/*
 * USB-PPP probe for A7670E (Waveshare ESP32-S3-A7670E-4G)
 * M1.5: enumerate modem on USB-OTG (GPIO19/20), PPP dial, verify internet.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_modem_usb_c_api.h"
#include "esp_modem_usb_config.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

static const char *TAG = "usb_ppp";
static const char *TAG_AT = "at_dbg";

#define APN "airtelgprs.com"

#define MODEM_PWR_PIN GPIO_NUM_33

/* Modem transport.
 *  1 = USB-OTG / TinyUSB / PPP  (Waveshare's documented route: DIP USB OFF
 *      routes the module's USB to the ESP32-S3). Needs DIP 4G ON so the module
 *      and ESP32 cold-boot together; an ESP32-only reset orphans the module's
 *      USB enumeration because the rail stays powered and GPIO33 is inert.
 *  0 = UART1 on GPIO17(TX)/GPIO18(RX) @115200, no flow control. This is a
 *      separate PCB route from the USB DIP, and matches the pin mapping that
 *      was verified working on this board, so it sidesteps USB enumeration
 *      entirely and is far more deterministic. */
/* Transport: 1 = A7670E UART on GPIO17/18, 0 = TinyUSB CDC (USB).
 * Use 0. The official Waveshare examples for this board all talk to the
 * module over the module's USB interface, and on real hardware the UART path
 * never once answered a single AT command. */
#define MODEM_USE_UART 0

#define MODEM_UART_NUM   UART_NUM_1
#define MODEM_UART_TX    17
#define MODEM_UART_RX    18
#define MODEM_UART_BAUD  115200

esp_modem_dce_t *esp_modem_new_dev_usb_safe(esp_modem_dce_device_t module,
                                            const esp_modem_dte_config_t *dte_config,
                                            const esp_modem_dce_config_t *dce_config,
                                            esp_netif_t *netif);

/* NOTE: GPIO33 is only wired to the module rail when DIP 4G is OFF. With 4G ON
 * the rail is hardwired and these writes are inert. A DIP change only takes
 * effect after a full USB unplug/replug, which is also the only way to force
 * the module to re-enumerate (an ESP32-only reset orphans it). */

static esp_modem_dce_t *open_modem(esp_modem_dce_config_t *dce_config,
                                   const esp_modem_dte_config_t *dte_config,
                                   esp_netif_t *netif)
{
#if MODEM_USE_UART
    /* UART is a fixed, always-present link - no enumeration, no orphaning.
     * Only a couple of attempts are needed. */
    for (int attempt = 1; attempt <= 5; attempt++) {
        ESP_LOGI(TAG, "Opening modem on UART%d TX=%d RX=%d @%d (attempt %d/5)",
                 MODEM_UART_NUM, MODEM_UART_TX, MODEM_UART_RX, MODEM_UART_BAUD, attempt);
        esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, dte_config, dce_config, netif);
        if (dce) {
            ESP_LOGI(TAG, "Modem on UART (attempt %d)", attempt);
            return dce;
        }
        ESP_LOGW(TAG, "esp_modem_new_dev (UART) failed on attempt %d", attempt);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    return NULL;
#else
    /* With DIP 4G ON the module rail is hardwired, so an ESP32-only reset
     * orphans the module's USB enumeration and GPIO33 cannot rescue it. Be
     * patient instead: the module can take a while to come back, and retry for
     * several minutes so we catch it whenever it re-enumerates. */
    for (int attempt = 1; attempt <= 20; attempt++) {
        ESP_LOGI(TAG, "USB modem open attempt %d/20 (~20 s wait each)", attempt);
        esp_modem_dce_t *dce = esp_modem_new_dev_usb_safe(ESP_MODEM_DCE_SIM7600, dte_config, dce_config, netif);
        if (dce) {
            ESP_LOGI(TAG, "Modem on USB (attempt %d)", attempt);
            return dce;
        }
        ESP_LOGW(TAG, "esp_modem_new_dev_usb failed on attempt %d", attempt);
        if (attempt < 20)
            vTaskDelay(pdMS_TO_TICKS(15000));
    }
    return NULL;
#endif
}

static void dump_diagnostics(esp_modem_dce_t *dce)
{
    const char *queries[] = {
        "AT+CGSN", "AT+CIMI", "AT+CPIN?", "AT+CFUN?", "AT+CSQ", "AT+COPS?",
        "AT+CEREG?", "AT+CGREG?", "AT+CREG?", "AT+CGATT?", "AT+CGDCONT?", "AT+CGACT?", "AT+CGCONTRDP"
    };
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        char buf[ESP_MODEM_C_API_STR_BUF_SIZE];
        esp_err_t err = esp_modem_at(dce, queries[i], buf, 10000);
        if (err == ESP_OK) {
            buf[sizeof(buf) - 1] = 0;
            ESP_LOGI(TAG_AT, "%s => %.*s", queries[i], (int)sizeof(buf) - 1, buf);
        } else {
            ESP_LOGW(TAG_AT, "%s => err %s", queries[i], esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    /* NOTE: AT+COPS=? is deliberately NOT run here. The scan takes longer than
     * any sane command timeout, so its reply arrives late and gets consumed as
     * the response to the NEXT command (observed: it desynced the APN set and
     * CGACT). Only run a scan when nothing else is pending. */
}

static EventGroupHandle_t event_group;
#define CONNECT_BIT BIT0
#define DISCONNECT_BIT BIT1
#define USB_GONE_BIT BIT3

static void usb_terminal_error_handler(esp_modem_terminal_error_t err)
{
    if (err == ESP_MODEM_TERMINAL_DEVICE_GONE) {
        ESP_LOGI(TAG, "USB modem disconnected");
        if (event_group) xEventGroupSetBits(event_group, USB_GONE_BIT);
    }
}

static void on_ppp_changed(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    ESP_LOGI(TAG, "PPP event %" PRIu32 " (state=%d)", event_id, (int)event_id);
}

static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_netif_dns_info_t dns;
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
        ESP_LOGI(TAG, "IP       : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Netmask  : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway  : " IPSTR, IP2STR(&event->ip_info.gw));
        if (esp_netif_get_dns_info(event->esp_netif, 0, &dns) == ESP_OK)
            ESP_LOGI(TAG, "DNS1     : " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
        xEventGroupSetBits(event_group, CONNECT_BIT);
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGI(TAG, "PPP IP lost");
        xEventGroupSetBits(event_group, DISCONNECT_BIT);
    }
}

static int do_http_probe(void)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    ESP_LOGI(TAG, "resolving example.com...");
    int rc = getaddrinfo("example.com", "80", &hints, &res);
    if (rc != 0 || !res) {
        ESP_LOGE(TAG, "getaddrinfo failed: rc=%d", rc);
        return -1;
    }
    ESP_LOGI(TAG, "resolved to %s", inet_ntoa(((struct sockaddr_in *)res->ai_addr)->sin_addr));

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket failed");
        freeaddrinfo(res);
        return -1;
    }
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "connect failed: %s", strerror(errno));
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    ESP_LOGI(TAG, "TCP connected");
    freeaddrinfo(res);

    const char *req =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Connection: close\r\n"
        "\r\n";
    if (send(fd, req, strlen(req), 0) < 0) {
        ESP_LOGE(TAG, "send failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    char buf[512];
    int n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    if (n <= 0) {
        ESP_LOGE(TAG, "recv failed: %s", strerror(errno));
        return -1;
    }
    buf[n] = 0;
    char *nl = strstr(buf, "\r\n");
    if (nl) *nl = 0;
    ESP_LOGI(TAG, "HTTP reply: %s", buf);
    return 0;
}

static int reg_stat_from_reply(const char *reply)
{
    /* Parse <stat> out of "+CGREG: <mode>,<stat>" / "+CEREG: <mode>,<stat>". */
    const char *p = strchr(reply, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return -1;
    while (*p >= '0' && *p <= '9') p++;
    while (*p == ' ') p++;
    if (*p != ',') return -1;
    p++;
    while (*p == ' ') p++;
    int stat = 0;
    while (*p >= '0' && *p <= '9') { stat = stat * 10 + (*p - '0'); p++; }
    return stat;
}

static int is_registered_reply(const char *reply)
{
    int s = reg_stat_from_reply(reply);
    return (s == 1 || s == 5);
}

static int looks_like_ipv4(const char *s)
{
    int dots = 0;
    for (const char *p = s; *p; p++) if (*p == '.') dots++;
    return (dots == 3 && !strstr(s, "0.0.0.0") && !strstr(s, "ERROR"));
}

/* Reference (zbotic/Waveshare Jio flow): use the modem's BUILT-IN TCP/IP
 * stack (NETOPEN -> IPADDR -> HTTP*) instead of PPP. Probe it; return 1 if a
 * usable IP is obtained over the built-in stack. */
static int probe_builtin_stack(esp_modem_dce_t *dce)
{
    char buf[ESP_MODEM_C_API_STR_BUF_SIZE];
    esp_err_t e;

    e = esp_modem_at(dce, "AT+NETOPEN", buf, 25000);
    ESP_LOGI(TAG_AT, "NETOPEN => %s rep=%.110s", esp_err_to_name(e), buf);
    vTaskDelay(pdMS_TO_TICKS(3000));

    e = esp_modem_at(dce, "AT+IPADDR", buf, 10000);
    ESP_LOGI(TAG_AT, "IPADDR => %s rep=%.110s", esp_err_to_name(e), buf);
    int got_ip = (e == ESP_OK) && looks_like_ipv4(buf);
    if (!got_ip) {
        ESP_LOGW(TAG, "Built-in stack did not return an IP; will fall back to PPP.");
        esp_modem_at(dce, "AT+NETCLOSE", buf, 15000);
        return 0;
    }
    ESP_LOGI(TAG, "Built-in stack got an IP: %.40s", buf);

    /* Now verify actual data flow over the built-in stack: HTTP GET.
     * HTTPREAD returns +ERROR until the transfer is complete, so poll the
     * session status first and give the radio time to finish. */
    e = esp_modem_at(dce, "AT+HTTPINIT", buf, 10000);
    ESP_LOGI(TAG_AT, "HTTPINIT => %s rep=%.60s", esp_err_to_name(e), buf);
    vTaskDelay(pdMS_TO_TICKS(1000));
    e = esp_modem_at(dce, "AT+HTTPPARA=\"URL\",\"http://example.com\"", buf, 10000);
    ESP_LOGI(TAG_AT, "HTTPPARA => %s rep=%.60s", esp_err_to_name(e), buf);
    vTaskDelay(pdMS_TO_TICKS(1000));
    e = esp_modem_at(dce, "AT+HTTPACTION=0", buf, 45000);
    ESP_LOGI(TAG_AT, "HTTPACTION(GET) => %s rep=%.60s", esp_err_to_name(e), buf);

    int http_ok = 0;
    for (int i = 1; i <= 10 && !http_ok; i++) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        e = esp_modem_at(dce, "AT+HTTPREAD", buf, 30000);
        buf[sizeof(buf) - 1] = 0;
        ESP_LOGI(TAG_AT, "HTTPREAD #%d => %s rep=%.110s", i, esp_err_to_name(e), buf);
        if (e == ESP_OK && strstr(buf, "+HTTPREAD") && !strstr(buf, "ERROR"))
            http_ok = 1;
    }
    if (http_ok) {
        ESP_LOGI(TAG, "SUCCESS: fetched a page over the built-in stack - 4G data path is live.");
    } else {
        ESP_LOGW(TAG, "HTTP body not retrieved; IP is up but the transfer is unverified.");
        esp_modem_at(dce, "AT+HTTPTERM", buf, 10000);
    }
    esp_modem_at(dce, "AT+HTTPTERM", buf, 10000);
    esp_modem_at(dce, "AT+NETCLOSE", buf, 15000);
    return 1;
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL));

    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(APN);
    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *esp_netif = esp_netif_new(&netif_ppp_config);
    assert(esp_netif);

    event_group = xEventGroupCreate();

    gpio_config_t pwr = {
        .pin_bit_mask = 1ULL << MODEM_PWR_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr);
    gpio_set_level(MODEM_PWR_PIN, 1);

#if MODEM_USE_UART
    /* The esp_modem default UART pins (TX 25 / RX 26) belong to other boards;
     * this board wires the module UART to GPIO17/18. No RTS/CTS is used. */
    esp_modem_dte_config_t dte_config_transport = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config_transport.uart_config.port_num = MODEM_UART_NUM;
    dte_config_transport.uart_config.tx_io_num = MODEM_UART_TX;
    dte_config_transport.uart_config.rx_io_num = MODEM_UART_RX;
    dte_config_transport.uart_config.rts_io_num = -1;
    dte_config_transport.uart_config.cts_io_num = -1;
    dte_config_transport.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
    dte_config_transport.uart_config.baud_rate = MODEM_UART_BAUD;

    ESP_LOGI(TAG, "Opening A7670E on UART%d (TX=%d RX=%d @%d)...",
             MODEM_UART_NUM, MODEM_UART_TX, MODEM_UART_RX, MODEM_UART_BAUD);
#else
    struct esp_modem_usb_term_config usb_config = ESP_MODEM_A7670_USB_CONFIG();
    usb_config.timeout_ms = 20000;
    const esp_modem_dte_config_t dte_config_transport = ESP_MODEM_DTE_DEFAULT_USB_CONFIG(usb_config);

    ESP_LOGI(TAG, "Waiting for A7670E on USB (VID 0x1E0E PID 0x9011, iface 4/5)...");
    ESP_LOGI(TAG, "Board DIP must be 4G ON, USB OFF, HUB ON (per Waveshare PPP firmware). "
                  "With 4G ON the module rail is hardwired and GPIO33 is inert, so the module "
                  "gets a true cold boot at board power-up and re-reads the SIM. "
                  "A DIP change only takes effect after a full USB unplug/replug.");
#endif
    esp_modem_dce_t *dce = open_modem(&dce_config, &dte_config_transport, esp_netif);
    if (!dce) {
        ESP_LOGE(TAG, "Modem not found after all retries.");
        return;
    }
    esp_modem_set_error_cb(dce, usb_terminal_error_handler);
    /* Do not poke CNMP or CGATT before the camping wait: a bare AT interface
     * right after USB enumeration registers fastest. If it still will not camp,
     * sweep AT+CNMP below - the A7670E defaults to mode 2 and will ignore the
     * 2G cells Airtel broadcasts, so 38 (auto) is what finally camped. */
    ESP_LOGI(TAG, "Waiting 10 s for the module to finish booting.");
    vTaskDelay(pdMS_TO_TICKS(10000));

    /* Read SIM identity EARLY, before the long camping wait. If the module has
     * been power-cycled since the SIM swap this must show the new Airtel IMSI
     * (MCC/MNC 404-02/404-45/404-70). A 404-90 reading here means either the
     * modem never cold-booted (cached IMSI) or that SIM is not Airtel. */
    ESP_LOGI(TAG, "--- Early SIM identity check ---");
    {
        const char *ident[] = { "AT+CPIN?", "AT+CIMI", "AT+CCID", "ATI" };
        char idbuf[ESP_MODEM_C_API_STR_BUF_SIZE];
        for (size_t i = 0; i < sizeof(ident) / sizeof(ident[0]); i++) {
            esp_err_t e = esp_modem_at(dce, ident[i], idbuf, 10000);
            if (e == ESP_OK) {
                idbuf[sizeof(idbuf) - 1] = 0;
                ESP_LOGI(TAG_AT, "%s => %.*s", ident[i], 150, idbuf);
            } else {
                ESP_LOGW(TAG_AT, "%s => err %s", ident[i], esp_err_to_name(e));
            }
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }

    ESP_LOGW(TAG, "AT+COPS? is NOT a reliable registration indicator on this module: in "
                  "usbprobe33 the LED was blinking and CGPADDR handed out a real IP while "
                  "COPS? still reported +COPS: 0. So this is a bounded wait, never a gate - "
                  "we always proceed to attach + PDP and let CGPADDR be the judge.");
    /* Step 1: prove the AT link is alive. Every command timing out later is a
     * dead link, so test this first and loudly - with DIP 4G ON the module rail
     * is hardwired and only a full board power-cycle can revive it. */
    bool link_ok = false;
    for (int i = 1; i <= 15 && !link_ok; i++) {
        char probe[ESP_MODEM_C_API_STR_BUF_SIZE] = {0};
        if (esp_modem_at(dce, "AT", probe, 2000) == ESP_OK) {
            ESP_LOGI(TAG, "AT link is ALIVE (bare AT ok on try %d): %.*s", i, 20, probe);
            link_ok = true;
        } else {
            ESP_LOGW(TAG, "bare AT no response (try %d/15) - module may still be booting", i);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    if (!link_ok) {
        ESP_LOGE(TAG, "Module is not answering AT at all. The module rail is hardwired and the "
                      "18650 battery keeps it alive, so unplugging USB alone does NOT reset it. "
                      "Unplug USB AND flip the battery power switch OFF for 15 s, then ON and replug.");
    }

    int rssi = 0, ber = 0, reg_wait = 0, good = 0;
    char regbuf[ESP_MODEM_C_API_STR_BUF_SIZE];
    int camped = 0, registered = 0;

    /* Poll CSQ/COPS/CEREG until camped or `budget` seconds elapse. */
    #define POLL_FOR_CAMP(dce, regbuf, budget, camped, registered, rssi, ber, good)      \
        do {                                                                              \
            reg_wait = 0;                                                                 \
            while (reg_wait < (budget)) {                                                  \
                if (esp_modem_get_signal_quality((dce), &rssi, &ber) == ESP_OK) {         \
                    if (rssi != 99) { ++good; } else { good = 0; }                        \
                    ESP_LOGI(TAG, "CSQ: rssi=%d ber=%d good=%d camped=%d", rssi, ber, good, camped); \
                }                                                                          \
                if (esp_modem_at((dce), "AT+COPS?", (regbuf), 6000) == ESP_OK) {           \
                    ESP_LOGI(TAG, "COPS: %.*s", 60, (regbuf));                              \
                    if (strchr((regbuf), '"')) camped = 1;                                   \
                }                                                                          \
                if (esp_modem_at((dce), "AT+CEREG?", (regbuf), 6000) == ESP_OK &&          \
                    is_registered_reply((regbuf))) {                                        \
                    ESP_LOGI(TAG, "CEREG registered: %.120s", (regbuf));                    \
                    registered = 1;                                                         \
                }                                                                          \
                if (registered || camped) break;                                             \
                reg_wait += 10;                                                              \
                vTaskDelay(pdMS_TO_TICKS(10000));                                            \
            }                                                                               \
        } while (0)

    /* Only CNMP=38 and CNMP=2 are accepted by this module's firmware (1, 0 and
     * 42 are all rejected). 38 is the one that actually finds a cell here
     * (CSQ 11-15) while 2 sits at CSQ 99, so set 38 and leave it alone
     * instead of sweeping. */
    ESP_LOGI(TAG, "Setting AT+CNMP=38 (auto GSM+LTE) and waiting up to 120 s...");
    if (esp_modem_at(dce, "AT+CNMP=38", regbuf, 10000) == ESP_OK)
        ESP_LOGI(TAG, "AT+CNMP=38 accepted");
    else
        ESP_LOGW(TAG, "AT+CNMP=38 rejected");
    vTaskDelay(pdMS_TO_TICKS(10000));
    POLL_FOR_CAMP(dce, regbuf, 40, camped, registered, rssi, ber, good);

    if (camped || registered)
        ESP_LOGI(TAG, "CAMPED/registered (rssi good=%d).", good);
    else
        ESP_LOGW(TAG, "COPS still shows nothing (rssi good=%d) - continuing to attach anyway.", good);

    /* Force an attach. In usbprobe33 this is the step after which CGPADDR
     * finally handed out an IP even though COPS? still read +COPS: 0. */
    ESP_LOGI(TAG, "Forcing attach with AT+CGATT=1 ...");
    if (esp_modem_at(dce, "AT+CGATT=1", regbuf, 15000) == ESP_OK)
        ESP_LOGI(TAG, "AT+CGATT=1 => %.*s", 40, regbuf);
    else
        ESP_LOGW(TAG, "AT+CGATT=1 => err");

    ESP_LOGI(TAG, "Attaching packet service (CGATT=1)...");
    esp_modem_at(dce, "AT+CGATT=1", NULL, 8000);
    vTaskDelay(pdMS_TO_TICKS(10000));

    ESP_LOGI(TAG, "Dumping modem radio/SIM/PDP state...");
    dump_diagnostics(dce);

    esp_modem_PdpContext_t pdp = { 0, NULL, APN };
    esp_err_t perr = esp_modem_set_pdp_context(dce, &pdp);
    ESP_LOGI(TAG, "Set PDP context (APN %s): %s", APN, esp_err_to_name(perr));
    if (perr != ESP_OK) {
        /* A timeout here usually means the previous command's reply was still
         * in flight. Settle, then set the APN again and confirm it stuck. */
        ESP_LOGW(TAG, "PDP context set failed; settling and retrying once...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        perr = esp_modem_set_pdp_context(dce, &pdp);
        ESP_LOGI(TAG, "Set PDP context retry: %s", esp_err_to_name(perr));
    }
    char vfy[ESP_MODEM_C_API_STR_BUF_SIZE] = {0};
    if (esp_modem_at(dce, "AT+CGDCONT?", vfy, 10000) == ESP_OK) {
        vfy[sizeof(vfy) - 1] = 0;
        ESP_LOGI(TAG, "AT+CGDCONT? => %.*s", 120, vfy);
        if (!strstr(vfy, APN))
            ESP_LOGW(TAG, "APN '%s' is NOT in CGDCONT - module may be using its own default.", APN);
    }

    ESP_LOGI(TAG, "Manually activating PDP context (AT+CGACT=1,1)...");
    char cgact_buf[128] = {0};
    esp_err_t cgact_err = esp_modem_at(dce, "AT+CGACT=1,1", cgact_buf, 15000);
    ESP_LOGI(TAG, "CGACT => err=%s reply=%.*s", esp_err_to_name(cgact_err), (int)sizeof(cgact_buf), cgact_buf);

    /* CGAACT is the activate call the known-good Airtel sketch uses; the PDP
     * often takes a while to come up, so poll CGPADDR until an IP appears. */
    ESP_LOGI(TAG, "Activating with AT+CGAACT=1,1 and polling AT+CGPADDR=1 for an IP...");
    char cgaact_buf[128] = {0};
    esp_err_t cgaact_err = esp_modem_at(dce, "AT+CGAACT=1,1", cgaact_buf, 15000);
    ESP_LOGI(TAG, "CGAACT => err=%s reply=%.*s", esp_err_to_name(cgaact_err), (int)sizeof(cgaact_buf), cgaact_buf);

    char ip_buf[ESP_MODEM_C_API_STR_BUF_SIZE] = {0};
    for (int i = 1; i <= 24; i++) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (esp_modem_at(dce, "AT+CGPADDR=1", ip_buf, 10000) == ESP_OK) {
            ip_buf[sizeof(ip_buf) - 1] = 0;
            ESP_LOGI(TAG, "CGPADDR=1 #%d => %.*s", i, 80, ip_buf);
            if (looks_like_ipv4(ip_buf)) {
                ESP_LOGI(TAG, "PDP is up - Airtel assigned an IP.");
                break;
            }
        }
    }

    ESP_LOGI(TAG, "Probing built-in TCP/IP stack (Airtel reference path: NETOPEN/IPADDR/HTTP)...");
    if (probe_builtin_stack(dce)) {
        ESP_LOGI(TAG, "Data path verified over built-in stack; skipping PPP dial.");
        return;
    }

    for (int att = 1; att <= 3; att++) {
        ESP_LOGI(TAG, "Switching to DATA mode (PPP dial via ATD*99#), attempt %d/3", att);
        esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "set_mode(DATA) failed on attempt %d: %s", att, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Waiting for PPP IP (up to 90 s)...");
            xEventGroupClearBits(event_group, CONNECT_BIT | DISCONNECT_BIT | USB_GONE_BIT);
            EventBits_t bits = xEventGroupWaitBits(event_group, CONNECT_BIT | DISCONNECT_BIT | USB_GONE_BIT,
                                                   pdFALSE, pdFALSE, pdMS_TO_TICKS(90000));
            if (bits & USB_GONE_BIT) {
                ESP_LOGE(TAG, "USB modem gone during dial");
                return;
            }
            if (bits & DISCONNECT_BIT) {
                ESP_LOGW(TAG, "PPP DISCONNECT during attempt %d (bits=0x%X)", att, bits);
            }
            if (bits & CONNECT_BIT) {
                ESP_LOGI(TAG, "PPP up. Running internet probe...");
                for (int i = 0; i < 3; i++) {
                    ESP_LOGI(TAG, "--- probe attempt %d ---", i + 1);
                    if (do_http_probe() == 0) break;
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
                break;
            }
        }
        ESP_LOGW(TAG, "Attempt %d did not yield IP; back to COMMAND mode, retry in 15 s", att);
        esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}