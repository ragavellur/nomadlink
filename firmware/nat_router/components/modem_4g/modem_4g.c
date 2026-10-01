/*
 * A7670E 4G modem as a PPP uplink. See include/modem_4g.h for the wiring and
 * the two rules inherited from BUG-005.
 *
 * Structure: one FreeRTOS task owns the modem for the whole run. The HTTP layer
 * only ever posts a request into this task's queue and reads a snapshot out of
 * it. That keeps AT traffic and PPP frames strictly serialized, which matters
 * because both travel the same UART and interleaving them corrupts PPP.
 */

#include "modem_4g.h"

#include <inttypes.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "oled_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lwip/init.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "netif/ppp/pppos.h"

static const char *TAG = "modem4g";

/* 115200 is what the baseline sketch measured working on this wiring. The
 * A7670E will go faster, but a baud bump is a separate experiment with its own
 * evidence, not something to bundle into the first working dial. */
#define MODEM_4G_BAUD       115200
#define MODEM_4G_UART_BUF   (4096 * 2)
#define DIAL_TIMEOUT_MS     25000
#define NEGOTIATE_TIMEOUT_MS 45000
#define MODEM_BOOT_WAIT_MS  3000

typedef enum { CMD_DIAL, CMD_STOP, CMD_NONE } modem_cmd_t;

typedef struct {
    modem_cmd_t cmd;
    char apn[64];
} modem_msg_t;

static modem_4g_status_t s_status;
static QueueHandle_t     s_q;
static TaskHandle_t      s_task;
static ppp_pcb          *s_pcb;
static struct netif      s_ppp_netif;
static int               s_uart_installed;
static bool              s_in_data_mode;

const char *modem_4g_state_str(modem_4g_state_t s)
{
    switch (s) {
    case MODEM_4G_STATE_OFF:         return "off";
    case MODEM_4G_STATE_PROBING:     return "probing";
    case MODEM_4G_STATE_DIALING:     return "dialing";
    case MODEM_4G_STATE_NEGOTIATING: return "negotiating";
    case MODEM_4G_STATE_UP:          return "up";
    case MODEM_4G_STATE_ERROR:       return "error";
    default:                         return "unknown";
    }
}

static void set_state(modem_4g_state_t st)
{
    s_status.state = st;
    ESP_LOGI(TAG, "state -> %s", modem_4g_state_str(st));
}

static void fail(const char *why)
{
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s", why);
    set_state(MODEM_4G_STATE_ERROR);
}

/* ---- UART helpers ---------------------------------------------------- */

static void uart_drain(void)
{
    size_t _rxlen = 0;
    while (uart_get_buffered_data_len(MODEM_4G_UART_NUM, &_rxlen) == ESP_OK && _rxlen > 0) {
        uint8_t scratch[256];
        uart_read_bytes(MODEM_4G_UART_NUM, scratch, sizeof(scratch), 0);
    }
}

/* Read one CR/LF terminated line. Returns false on timeout. */
static bool at_read_line(char *out, size_t cap, uint32_t timeout_ms)
{
    size_t n = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        uint8_t c;
        int r = uart_read_bytes(MODEM_4G_UART_NUM, &c, 1, pdMS_TO_TICKS(20));
        if (r != 1) continue;
        if (c == '\r' || c == '\n') {
            if (n == 0) continue;         /* skip empty CRLF pairs */
            out[n] = '\0';
            return true;
        }
        if (n + 1 < cap) out[n++] = (char)c;
    }
    out[n] = '\0';
    return false;
}

static bool at_send(const char *cmd)
{
    ESP_LOGD(TAG, ">> %s", cmd);
    return uart_write_bytes(MODEM_4G_UART_NUM, cmd, strlen(cmd)) > 0 &&
           uart_write_bytes(MODEM_4G_UART_NUM, "\r\n", 2) > 0;
}

/* Send a command and collect lines until OK / ERROR. Result of the last
 * meaningful line is copied into reply when non-NULL. */
static bool at_cmd(const char *cmd, char *reply, size_t cap, uint32_t timeout_ms)
{
    char line[128];
    bool ok = false;
    if (reply && cap) reply[0] = '\0';
    uart_drain();
    if (!at_send(cmd)) return false;

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        if (!at_read_line(line, sizeof(line), 200)) continue;
        ESP_LOGD(TAG, "<< %s", line);
        if (!strncmp(line, "OK", 2)) { ok = true; break; }
        if (!strncmp(line, "ERROR", 5) || !strncmp(line, "NO CARRIER", 10)) break;
        /* A URC arriving mid-command must not be mistaken for the result, so
         * only bare numeric/+ lines are kept. */
        if (line[0] && (line[0] == '+' || (line[0] >= '0' && line[0] <= '9'))) {
            if (reply && cap) snprintf(reply, cap, "%s", line);
        }
    }
    return ok;
}

/* ---- PPP callbacks --------------------------------------------------- */

static u32_t ppp_write_to_uart(ppp_pcb *pcb, const void *data, u32_t len, void *ctx)
{
    (void)pcb; (void)ctx;
    const uint8_t *b = (const uint8_t *)data;
    u32_t sent = 0;
    while (sent < len) {
        int w = uart_write_bytes(MODEM_4G_UART_NUM, b + sent, len - sent);
        if (w <= 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
        sent += (u32_t)w;
    }
    s_status.tx_bytes += sent;
    return sent;
}

static void ppp_on_phase(ppp_pcb *pcb, u8_t phase, void *ctx)
{
    (void)pcb; (void)ctx;
    ESP_LOGI(TAG, "PPP phase %u", (unsigned)phase);
    if (phase == PPP_PHASE_RUNNING) {
        const ip4_addr_t *a = netif_ip4_addr(&s_ppp_netif);
        const ip4_addr_t *p = netif_ip4_gw(&s_ppp_netif);
        snprintf(s_status.local_ip, sizeof(s_status.local_ip), "%s", ip4addr_ntoa(a));
        snprintf(s_status.peer_ip, sizeof(s_status.peer_ip), "%s", ip4addr_ntoa(p));
        const ip_addr_t *dns1 = dns_getserver(0);
        if (dns1 && !ip_addr_isany(dns1)) {
            snprintf(s_status.dns_primary, sizeof(s_status.dns_primary), "%s", ipaddr_ntoa(dns1));
        }
        set_state(MODEM_4G_STATE_UP);
    }
}

static void ppp_on_link(ppp_pcb *pcb, int err_code, void *ctx)
{
    (void)pcb; (void)ctx;
    if (err_code) {
        ESP_LOGW(TAG, "PPP link error %d", err_code);
        fail("PPP link error");
    }
}

/* Create and start the PPP link. Must run in the tcpip thread. */
static void ppp_create_cb(void *arg)
{
    (void)arg;
    s_pcb = pppos_create(&s_ppp_netif, ppp_write_to_uart, ppp_on_link, NULL);
    if (s_pcb == NULL) {
        fail("pppos_create failed");
        return;
    }
    ppp_set_notify_phase_callback(s_pcb, ppp_on_phase);
    ppp_set_usepeerdns(s_pcb, 1);
    ppp_set_default(s_pcb);
    ppp_connect(s_pcb, 0);
}

static void ppp_start(void)
{
    /* This project is also a WiFi router, so esp_netif has already called
     * tcpip_init(). The baseline sketch had to call it explicitly because it
     * never touched WiFi; doing it again here would be a double init. */

    tcpip_callback(ppp_create_cb, NULL);
}

/* ---- bring-up -------------------------------------------------------- */

static void power_on_modem(void)
{
    /* Enable rail is asserted and then we wait. No PWRKEY pulse: GPIO42 is not
     * PWRKEY on this board, and a pulse on a live modem hangs up the call. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << MODEM_4G_ENABLE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(MODEM_4G_ENABLE_GPIO, 1);
    ESP_LOGI(TAG, "module enable asserted on GPIO%d, waiting %d ms",
             MODEM_4G_ENABLE_GPIO, MODEM_BOOT_WAIT_MS);
    vTaskDelay(pdMS_TO_TICKS(MODEM_BOOT_WAIT_MS));
}

/* Escape sequence: guard by 1 s of silence, send +++ , guard again. In data
 * mode the A7670E discards AT, so this is the only way to tell a stuck data
 * session from a dead modem. */
static bool escape_data_mode(void)
{
    char line[128];
    ESP_LOGI(TAG, "no AT reply; trying guarded +++ before any power action");
    vTaskDelay(pdMS_TO_TICKS(1200));
    uart_write_bytes(MODEM_4G_UART_NUM, "+++", 3);
    uart_write_bytes(MODEM_4G_UART_NUM, "\r\n", 2);
    vTaskDelay(pdMS_TO_TICKS(1200));
    uart_drain();
    if (at_send("AT")) {
        if (at_read_line(line, sizeof(line), 1500) && !strncmp(line, "OK", 2)) {
            ESP_LOGI(TAG, "'+++' escape accepted - modem was in data mode");
            return true;
        }
    }
    return false;
}

/* Telemetry is sampled here, in the one safe AT window before the dial.
 * Reading it later would mean sending AT over a data-mode UART. */
static void sample_telemetry(void)
{
    char r[128];

    if (at_cmd("AT+CSQ", r, sizeof(r), 3000)) {
        int v = 0;
        if (sscanf(r, "+CSQ: %d", &v) == 1) {
            s_status.rssi_dbm = (v == 99) ? -1 : (-113 + 2 * v);
        }
    }
    if (at_cmd("AT+CGSN", r, sizeof(r), 3000)) {
        if (!strncmp(r, "+CGSN:", 6)) snprintf(s_status.imei, sizeof(s_status.imei), "%.15s", r + 6);
    }
    if (at_cmd("AT+COPS?", r, sizeof(r), 5000)) {
        /* +COPS: 0,0,"Operator",7 -- the name is the third comma field. */
        char *p = r;
        int field = 0;
        while (*p && field < 2) { if (*p == ',') field++; p++; }
        if (*p) {
            char *q = strchr(p, '"');
            if (q) {
                q++;
                char *e = strchr(q, '"');
                if (e) snprintf(s_status.operator, sizeof(s_status.operator), "%.*s",
                                (int)(e - q), q);
            }
        }
    }
    ESP_LOGI(TAG, "telemetry: rssi=%d operator='%s' imei=%s",
             s_status.rssi_dbm, s_status.operator, s_status.imei[0] ? s_status.imei : "(none)");
}

static bool bring_up_and_dial(const char *apn)
{
    set_state(MODEM_4G_STATE_PROBING);
    s_in_data_mode = false;

    if (!at_cmd("AT", NULL, 0, 2000) && !escape_data_mode()) {
        fail("modem unreachable: no AT reply after guarded escape");
        return false;
    }

    at_cmd("ATE0", NULL, 0, 2000);                 /* no echo */
    at_cmd("AT+CMEE=2", NULL, 0, 2000);            /* verbose errors */
    if (!at_cmd("AT+CPIN?", NULL, 0, 4000)) {
        fail("AT+CPIN? failed");
        return false;
    }

    sample_telemetry();

    /* Attach to the packet data profile. APN only; the SIM decides auth. */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", apn);
    at_cmd(cmd, NULL, 0, 5000);
    at_cmd("AT+CGDATA=1,1", NULL, 0, 3000);        /* 3GPP TS 27.007 test/data */

    set_state(MODEM_4G_STATE_DIALING);
    ESP_LOGI(TAG, "dialing PPP on APN '%s'", apn);

    /* ATD*99#*AT# is the documented A7670E PPP dial string. After CONNECT the
     * UART carries PPP frames, so nothing AT-shaped may be sent again. */
    uart_drain();
    at_send("ATD*99#*AT#");

    int64_t deadline = esp_timer_get_time() + (int64_t)DIAL_TIMEOUT_MS * 1000;
    bool connected = false;
    char line[128];
    while (esp_timer_get_time() < deadline) {
        if (!at_read_line(line, sizeof(line), 500)) continue;
        ESP_LOGD(TAG, "<< %s", line);
        if (!strcmp(line, "CONNECT")) { connected = true; break; }
    }
    if (!connected) {
        fail("no CONNECT from ATD*99#*AT#");
        return false;
    }

    s_in_data_mode = true;
    set_state(MODEM_4G_STATE_NEGOTIATING);
    ESP_LOGI(TAG, "CONNECT received; handing UART to PPP");

    ppp_start();

    /* Wait for the state task to observe PPP_PHASE_RUNNING. */
    deadline = esp_timer_get_time() + (int64_t)NEGOTIATE_TIMEOUT_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        if (s_status.state == MODEM_4G_STATE_UP) return true;
        if (s_status.state == MODEM_4G_STATE_ERROR) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    fail("PPP did not reach RUNNING in time");
    return false;
}

static void ppp_teardown_cb(void *arg)
{
    (void)arg;
    if (s_pcb) {
        ppp_close(s_pcb, 0);
        ppp_free(s_pcb);
        s_pcb = NULL;
    }
    s_status.local_ip[0] = 0;
    s_status.peer_ip[0] = 0;
}

/* ---- task ------------------------------------------------------------ */

static void modem_task(void *arg)
{
    (void)arg;
    power_on_modem();

    modem_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_q, &msg, portMAX_DELAY) != pdTRUE) continue;

        if (msg.cmd == CMD_DIAL) {
            snprintf(s_status.apn, sizeof(s_status.apn), "%s", msg.apn);
            if (s_status.state == MODEM_4G_STATE_UP) {
                ESP_LOGI(TAG, "already up; ignoring duplicate dial");
                continue;
            }
            bring_up_and_dial(s_status.apn);
        } else if (msg.cmd == CMD_STOP) {
            if (s_pcb) tcpip_callback(ppp_teardown_cb, NULL);
            vTaskDelay(pdMS_TO_TICKS(100));
            if (s_in_data_mode) {
                /* Get back to a state where AT is understood, without touching
                 * power: hold the guard, send +++, wait for OK. */
                vTaskDelay(pdMS_TO_TICKS(1200));
                uart_write_bytes(MODEM_4G_UART_NUM, "+++", 3);
                vTaskDelay(pdMS_TO_TICKS(1200));
                at_cmd("ATH", NULL, 0, 3000);
                s_in_data_mode = false;
            }
            set_state(MODEM_4G_STATE_OFF);
        }
    }
}

/* ---- public API ------------------------------------------------------ */

esp_err_t modem_4g_init(void)
{
    if (s_task) return ESP_ERR_INVALID_STATE;

    /* The OLED component's ESP32-S3 defaults are SDA=17 / SCL=18 -- the very
     * pins this modem uses for UART. It is disabled by default, but if anyone
     * turns it on it will fight us for both lines and the symptom would be a
     * modem that silently stops answering AT. Refuse rather than half-work. */
    bool oled_on = false;
    int sda = 0, scl = 0;
    oled_display_get_config(&oled_on, &sda, &scl);
    if (oled_on && (sda == MODEM_4G_UART_RX_GPIO || sda == MODEM_4G_UART_TX_GPIO ||
                    scl == MODEM_4G_UART_RX_GPIO || scl == MODEM_4G_UART_TX_GPIO)) {
        ESP_LOGE(TAG, "OLED claims GPIO%d/%d which are the modem UART; disable the OLED first",
                 sda, scl);
        return ESP_ERR_INVALID_STATE;
    }

    uart_config_t uc = {
        .baud_rate = MODEM_4G_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(MODEM_4G_UART_NUM, MODEM_4G_UART_BUF,
                                       MODEM_4G_UART_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(MODEM_4G_UART_NUM, &uc));
    ESP_ERROR_CHECK(uart_set_pin(MODEM_4G_UART_NUM, MODEM_4G_UART_TX_GPIO,
                                 MODEM_4G_UART_RX_GPIO, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    s_uart_installed = 1;

    memset(&s_status, 0, sizeof(s_status));
    s_status.rssi_dbm = -1;
    set_state(MODEM_4G_STATE_OFF);

    s_q = xQueueCreate(4, sizeof(modem_msg_t));
    if (!s_q) return ESP_ERR_NO_MEM;

    if (xTaskCreate(modem_task, "modem4g", 6144, NULL, 5, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t modem_4g_start(const char *apn)
{
    if (!s_q || !apn || !*apn) return ESP_ERR_INVALID_ARG;
    modem_msg_t m = { .cmd = CMD_DIAL };
    snprintf(m.apn, sizeof(m.apn), "%s", apn);
    return xQueueSend(s_q, &m, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t modem_4g_stop(void)
{
    if (!s_q) return ESP_ERR_INVALID_STATE;
    modem_msg_t m = { .cmd = CMD_STOP };
    return xQueueSend(s_q, &m, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

void modem_4g_get_status(modem_4g_status_t *out)
{
    if (!out) return;
    *out = s_status;
    if (s_pcb) {
        /* Read straight from the netif: state may still say NEGOTIATING while
         * IPCP has already written the address. */
        const ip4_addr_t *a = netif_ip4_addr(s_pcb->netif);
        if (a && !ip4_addr_isany_val(*a)) {
            snprintf(out->local_ip, sizeof(out->local_ip), "%s", ip4addr_ntoa(a));
        }
    }
}

bool modem_4g_is_up(void)
{
    return s_status.state == MODEM_4G_STATE_UP;
}