/* Position service: cellular LBS first, GNSS upgraded in when a real fix arrives.
 *
 * One record, always with an explicit source. LBS is available within seconds of
 * boot and is coarse; GNSS takes minutes and is accurate. ADR-006 hierarchy: LBS
 * immediately, then replace it the moment quality >= 1 and sats_used > 0. The
 * engine is never power-cycled mid-session, because a cold GNSS needs one
 * uninterrupted power-on to download an almanac.
 *
 * Proven command sequences on this board (A7670E-FASE, firmware V1.11.1), from
 * firmware/baseline/position_service which measured working:
 *   LBS   AT+CGDCONT/AT+CGATT/AT+CGACT=1,1, AT+SIMEI from AT+CGSN,
 *         then AT+CLBS=1,1 (the CID is mandatory; AT+CLBS=1 fails).
 *         Answers +CLBS: 0,<lon>,<lat>,<acc>. lon comes FIRST -- that is not a typo.
 *         The command returns OK BEFORE the async +CLBS: URC, so a parser that
 *         stops on OK sees nothing. That is the single biggest trap here.
 *   GNSS  AT+CGNSSPWR=1, wait for "+CGNSSPWR: READY!", then
 *         AT+CGNSSPORTSWITCH=0,1 (1,1 returns ERROR on this rev), then
 *         AT+CGNSSTST=1 to stream NMEA on the same UART.
 *
 * Two rules carried over from the regression work:
 *   - The module enable rail is GPIO33, active HIGH. GPIO42 is NOT PWRKEY --
 *     that net does not reach this board -- so no PWRKEY pulse is ever sent.
 *     A pulse toggles power on an off modem and hangs up on a live one.
 *   - In data mode the A7670E discards AT by design, so "no reply" is ambiguous.
 *     Guarded '+++' is tried before concluding anything.
 *
 * AT and NMEA share one UART, so the NMEA stream is muted while the LBS exchange
 * runs and re-enabled afterwards.
 */

#include "position.h"
#include "nmea.h"

#include <string.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "soc/uart_struct.h"

static const char *TAG = "position";

/* --- board wiring, fixed by schematic --------------------------------- */
#define POS_UART_NUM   UART_NUM_1
#define POS_UART_RX    17 /* ESP32 RX <- modem TX */
#define POS_UART_TX    18 /* ESP32 TX -> modem RX */
#define POS_ENABLE     33 /* module enable rail, active HIGH */
#define POS_BAUD       115200

/* How long to keep looking for a GNSS fix before settling for LBS forever. The
 * sketch used 15 min; we keep retrying in the background rather than giving up,
 * so this only controls the first attempt's patience. */
#define GNSS_FIRST_WAIT_MS  (15UL * 60UL * 1000UL)

/* LBS ret_codes that are transient and worth retrying, not "unsupported":
 * 8 busy, 9 open net error, 10 close net error, 11 op timeout, 12 DNS error.
 * Retrying is the difference between publishing nothing and publishing late. */
static bool lbs_code_is_transient(int code)
{
    return code == 8 || code == 9 || code == 10 || code == 11 || code == 12;
}

/* --- the single published record --------------------------------------- */
static void position_publish(position_source_t src, double lat, double lon,
                            int accuracy_m, int sats, const char *detail);

static position_t s_pos;
static SemaphoreHandle_t s_lock;

/* --- UART plumbing ------------------------------------------------------ */
static SemaphoreHandle_t s_uart_lock;

static void uart_put(const char *s)
{
    int n = strlen(s);
    uart_write_bytes(POS_UART_NUM, s, n);
    uart_write_bytes(POS_UART_NUM, "\r\n", 2);
}

/* Read one CRLF/LF-terminated line. Returns false on timeout. '$' mid-line means
 * an NMEA sentence arrived while we expected AT: drop the partial line and
 * restart, otherwise AT replies get prefixed with NMEA garbage. */
static bool read_line(char *out, size_t cap, uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t len = 0;

    while (esp_timer_get_time() < deadline) {
        size_t avail = 0;
        uart_get_buffered_data_len(POS_UART_NUM, &avail);
        if (!avail) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        uint8_t c;
        if (uart_read_bytes(POS_UART_NUM, &c, 1, 0) != 1) continue;

        if (c == '$') {           /* NMEA sentence: abandon this line */
            len = 0;
            continue;
        }
        if (c == '\n') {
            if (len == 0) continue;
            out[len] = '\0';
            return true;
        }
        if (c == '\r') continue;
        if (len < cap - 1) out[len++] = (char)c;
    }
    out[len] = '\0';
    return false;
}

/* Send an AT command and accumulate the reply. stop_on_ok must be FALSE for
 * AT+CLBS, which answers OK *before* the +CLBS: URC we actually want. */
static bool at_send(const char *cmd, uint32_t wait_ms, char *out, size_t cap,
                    bool stop_on_ok)
{
    size_t used = 0;
    out[0] = '\0';

    /* Drain first: stale bytes from a previous session are the classic source of
     * a reply that looks like it belongs to this command. */
    uart_flush_input(POS_UART_NUM);
    uart_put(cmd);

    int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    char line[192];

    while (esp_timer_get_time() < deadline) {
        if (!read_line(line, sizeof(line), 200)) break;
        if (used + strlen(line) + 2 < cap) {
            used += snprintf(out + used, cap - used, "%s\n", line);
        }
        if (strstr(line, "ERROR")) break;
        if (stop_on_ok && !strncmp(line, "OK", 2)) break;
        if (strstr(line, "+CGNSSPWR:")) break;
        if (strstr(line, "+CLBS:")) break;
        if (!strncmp(line, "+CGNSSTST:", 10)) break;
    }
    return used > 0;
}

/* V.250 escape into command mode. Harmless if already in command mode, and the
 * only safe way to find out: on this modem an absent AT reply is ambiguous. */
static bool try_escape_to_command_mode(void)
{
    uart_flush_input(POS_UART_NUM);
    vTaskDelay(pdMS_TO_TICKS(1100));      /* 1.1 s guard before */
    uart_write_bytes(POS_UART_NUM, "+++", 3);
    vTaskDelay(pdMS_TO_TICKS(1100));      /* and after */

    char line[64];
    if (read_line(line, sizeof(line), 800) && strstr(line, "OK")) {
        ESP_LOGI(TAG, "modem was in data mode; escaped to command mode");
        return true;
    }
    return false;
}

/* --- NMEA --------------------------------------------------------------- */

/* Split on commas into a bounded field array. Deliberately table-driven with a
 * hard cap and a hard field count: an index-poking parser is how an old build
 * reported satsUsed=99, which is not a value any satellite can produce. */
static void nmea_handle_gga(char *body)
{
    nmea_fix_t fix;
    if (!nmea_parse_gga(body, &fix)) {
        /* No fix, bad checksum, or an implausible field: all rejected upstream.
         * Silence here is correct -- a dropped sentence is not a fault. */
        return;
    }

    int acc_m = 0;
    if (fix.hdop > 0.0 && fix.hdop < 99.0) acc_m = (int)(fix.hdop * 5.0);

    position_publish(POSITION_SOURCE_GNSS, fix.lat, fix.lon, acc_m, fix.sats,
                     "GPS (GNSS)");
}

/* --- record publication ------------------------------------------------- */

static void position_publish(position_source_t src, double lat, double lon,
                      int accuracy_m, int sats, const char *detail)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    s_pos.source      = src;
    s_pos.lat         = lat;
    s_pos.lon         = lon;
    s_pos.accuracy_m  = accuracy_m;
    s_pos.sats        = sats;
    s_pos.updated_ms  = esp_timer_get_time() / 1000;
    s_pos.has_fix     = true;
    if (detail) snprintf(s_pos.detail, sizeof(s_pos.detail), "%s", detail);
    else        s_pos.detail[0] = '\0';

    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "position: %s lat=%.6f lon=%.6f acc=%dm sats=%d",
             position_source_str(src), lat, lon, accuracy_m, sats);
}

void position_get(position_t *out)
{
    if (!out) return;
    if (!s_lock) { memset(out, 0, sizeof(*out)); return; }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_pos;
    xSemaphoreGive(s_lock);
}

/* --- GNSS NMEA pump ----------------------------------------------------- */
static void nmea_task(void *arg)
{
    char line[128];
    size_t len = 0;

    for (;;) {
        size_t avail = 0;
        uart_get_buffered_data_len(POS_UART_NUM, &avail);
        if (!avail) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        uint8_t c;
        if (uart_read_bytes(POS_UART_NUM, &c, 1, 10) != 1) continue;

        if (c == '$') { len = 0; continue; }
        if (c == '\n') {
            if (len > 8 && line[len - 1] == '*') continue;  /* truncated */
            if (len > 0) {
                line[len] = '\0';
                if (nmea_checksum_ok(line)) {
                    /* Strip the checksum tail before splitting so it cannot be
                     * mistaken for a field. */
                    char *star = strrchr(line, '*');
                    if (star) *star = '\0';
                    if (strstr(line, "GGA")) {
                        char *body = strchr(line, ',');
                        if (body) nmea_handle_gga(body + 1);
                    }
                } else {
                    ESP_LOGD(TAG, "NMEA checksum failed, sentence dropped");
                }
            }
            len = 0;
            continue;
        }
        if (c == '\r') continue;
        if (len < sizeof(line) - 1) line[len++] = (char)c;
    }
}

/* --- acquisition -------------------------------------------------------- */

static bool uart_and_power_up(void)
{
    gpio_config_t en = {
        .pin_bit_mask = 1ULL << POS_ENABLE,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&en));
    gpio_set_level(POS_ENABLE, 1);      /* enable rail active HIGH */
    vTaskDelay(pdMS_TO_TICKS(500));

    uart_config_t cfg = {
        .baud_rate = POS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(POS_UART_NUM, 2048, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }
    if (uart_param_config(POS_UART_NUM, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed");
        return false;
    }
    uart_set_rx_timeout(POS_UART_NUM, 2);
    return true;
}

/* Populate AT+SIMEI, without which AT+CLBS returns +CLBS: 12 (DNS error).
 * The IMEI itself is never logged or stored -- it is a live identifier. */
static void ensure_simei(void)
{
    char reply[512];
    if (!at_send("AT+SIMEI?", 5000, reply, sizeof(reply), false)) return;
    if (strstr(reply, "+SIMEI: 0")) {          /* 0 == not set */
        if (!at_send("AT+CGSN", 5000, reply, sizeof(reply), true)) return;

        char digits[24];
        size_t n = 0;
        for (const char *p = reply; *p && n + 1 < sizeof(digits); p++) {
            if (*p >= '0' && *p <= '9') digits[n++] = *p;
        }
        digits[n] = '\0';
        if (n != 15) {
            ESP_LOGW(TAG, "AT+CGSN gave %zu digits, not 15; leaving SIMEI alone", n);
            return;
        }
        char cmd[40];
        snprintf(cmd, sizeof(cmd), "AT+SIMEI=%s", digits);
        at_send(cmd, 8000, reply, sizeof(reply), true);
        ESP_LOGI(TAG, "AT+SIMEI populated from AT+CGSN (value not logged)");
    }
}

/* One AT+CLBS attempt. Returns true on a usable position. */
static bool lbs_try_once(int *code_out)
{
    char reply[512];
    if (code_out) *code_out = -1;

    at_send("AT+CSQ", 5000, reply, sizeof(reply), true);

    /* stop_on_ok = false. AT+CLBS echoes OK before the +CLBS: URC, and that URC
     * is the payload. Breaking on OK is why earlier attempts saw nothing. */
    if (!at_send("AT+CLBS=1,1", 30000, reply, sizeof(reply), false)) return false;
    ESP_LOGI(TAG, "AT+CLBS raw: %s", reply);

    const char *p = strstr(reply, "+CLBS: ");
    if (!p) return false;
    p += 7;

    int code = atoi(p);
    if (code_out) *code_out = code;
    if (code != 0) {
        ESP_LOGW(TAG, "AT+CLBS ret_code=%d", code);
        return false;
    }

    /* +CLBS: 0,<lon>,<lat>,<acc> -- longitude first. */
    char *end;
    double lon = strtod(p + 1, &end);      /* skip the code and its comma */
    if (*end != ',') return false;
    double lat = strtod(end + 1, &end);
    int acc = (*end == ',') ? atoi(end + 1) : 0;

    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        ESP_LOGW(TAG, "AT+CLBS returned out-of-range lat/lon; discarding");
        return false;
    }
    position_publish(POSITION_SOURCE_LBS, lat, lon, acc, 0, "Cellular LBS");
    return true;
}

static void acquire_lbs(void)
{
    char reply[512];

    ensure_simei();

    at_send("AT+CNMP=38", 5000, reply, sizeof(reply), true);   /* 2G retired on Airtel */
    at_send("AT+CGDCONT=1,\"IP\",\"airtelgprs.com\"", 5000, reply, sizeof(reply), true);
    at_send("AT+CGATT=1", 8000, reply, sizeof(reply), true);
    vTaskDelay(pdMS_TO_TICKS(5000));
    at_send("AT+CGACT=1,1", 15000, reply, sizeof(reply), true);
    vTaskDelay(pdMS_TO_TICKS(6000));

    at_send("AT+CGPADDR=1", 8000, reply, sizeof(reply), true);
    ESP_LOGI(TAG, "AT+CGPADDR: %s", reply);

    for (int attempt = 1; attempt <= 3; attempt++) {
        int code = -1;
        if (lbs_try_once(&code)) {
            ESP_LOGI(TAG, "LBS position accepted on attempt %d", attempt);
            return;
        }
        /* Retry transient codes; never publish a code as if it were a verdict. */
        if (!lbs_code_is_transient(code) && code != -1) {
            ESP_LOGW(TAG, "AT+CLBS code %d is not transient; stopping retries", code);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    ESP_LOGW(TAG, "no LBS position; GNSS remains the only source");
}

/* Start the GNSS engine and stream NMEA. The engine is powered BEFORE the LBS
 * query in acquire_lbs' caller order so acquisition overlaps the LBS wait. */
static bool start_gnss(void)
{
    char reply[512];

    at_send("AT+CGNSSPWR=1", 10000, reply, sizeof(reply), false);
    if (!strstr(reply, "READY")) {
        ESP_LOGW(TAG, "AT+CGNSSPWR did not report READY: %s", reply);
    }
    /* 0,1 is the only variant that works; 1,1 returns ERROR on this firmware. */
    at_send("AT+CGNSSPORTSWITCH=0,1", 4000, reply, sizeof(reply), true);

    xTaskCreate(nmea_task, "nmea", 3072, NULL, 4, NULL);

    at_send("AT+CGNSSTST=1", 4000, reply, sizeof(reply), false);
    ESP_LOGI(TAG, "GNSS engine on, NMEA streaming");
    return true;
}

static void position_task(void *arg)
{
    /* Let the SoftAP finish coming up first. This task is spawned at the very end
     * of app_main, but a modem that stalls must still never cost us the AP. */
    vTaskDelay(pdMS_TO_TICKS(10000));

    if (!uart_and_power_up()) {
        ESP_LOGE(TAG, "modem UART/power unavailable; no position service");
        return;
    }

    try_escape_to_command_mode();

    /* AT sync. */
    bool synced = false;
    for (int i = 0; i < 6 && !synced; i++) {
        char reply[128];
        if (at_send("AT", 3000, reply, sizeof(reply), true) && strstr(reply, "OK")) synced = true;
        else vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!synced) {
        ESP_LOGE(TAG, "modem did not answer AT; no position service");
        return;
    }
    ESP_LOGI(TAG, "modem answering AT");

    /* Mute NMEA first so the LBS exchange runs on a clean UART. Stop the stream
     * only -- never power-cycle the engine. */
    char reply[256];
    at_send("AT+CGNSSTST=0", 4000, reply, sizeof(reply), true);

    /* Power the engine NOW so it acquires while LBS resolves, then mute again. */
    at_send("AT+CGNSSPWR=1", 10000, reply, sizeof(reply), false);
    at_send("AT+CGNSSPORTSWITCH=0,1", 4000, reply, sizeof(reply), true);

    acquire_lbs();

    /* LBS exchange done; let NMEA through. */
    start_gnss();

    /* Stay alive pumping nothing further: nmea_task owns the UART from here, and
     * the record is updated by the parser. */
    for (;;) vTaskDelay(pdMS_TO_TICKS(60000));
}

void position_init(void)
{
    if (s_lock) return;               /* already started */
    s_lock = xSemaphoreCreateMutex();
    s_uart_lock = xSemaphoreCreateMutex();
    memset(&s_pos, 0, sizeof(s_pos));

    xTaskCreate(position_task, "position", 6144, NULL, 3, NULL);
    ESP_LOGI(TAG, "position service starting (LBS first, GNSS upgrade)");
}

const char *position_source_str(position_source_t s)
{
    switch (s) {
    case POSITION_SOURCE_LBS:  return "Cellular LBS";
    case POSITION_SOURCE_GNSS: return "GPS (GNSS)";
    default:                   return "none";
    }
}