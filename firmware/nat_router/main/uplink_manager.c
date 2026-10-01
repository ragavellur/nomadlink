/*
 * Uplink selection: WiFi STA, Ethernet, or the A7670E 4G PPP link.
 *
 * The base router hard-assumes the STA is the only uplink, which is why no
 * "uplink" concept existed to extend. Here the choice is explicit and persisted
 * in NVS so it survives a reboot, and the fallback order is deliberate:
 *
 *   4G   - if selected and PPP reports UP, and 4G is the only way out when
 *          there is no WiFi at all (the travel case)
 *   WiFi - if the STA holds an IP
 *   ETH  - if the Ethernet interface holds an IP
 *
 * Selecting "4G" does not disable WiFi. The AP and the web UI live on the STA
 * interface in this build, so tearing WiFi down would take the control plane
 * with it. Instead the 4G link becomes the default route for forwarding, and
 * WiFi keeps serving clients locally.
 */

#include "uplink_manager.h"
#include "led_strip_status.h"
#include "modem_4g.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "router_config.h"

static const char *TAG = "uplink";

static uplink_mode_t s_mode = UPLINK_WIFI;
static bool s_sta_have_ip;
static bool s_eth_have_ip;
static bool s_applied;

/* Persisted as a string so a future mode can be added without a migration
 * keyed on enum ordering. */
static const char *mode_to_str(uplink_mode_t m)
{
    switch (m) {
    case UPLINK_4G:    return "4g";
    case UPLINK_WIFI:  return "wifi";
    case UPLINK_ETH:   return "eth";
    default:           return "wifi";
    }
}

static uplink_mode_t str_to_mode(const char *s)
{
    if (!s) return UPLINK_WIFI;
    if (!strcmp(s, "4g"))  return UPLINK_4G;
    if (!strcmp(s, "eth")) return UPLINK_ETH;
    return UPLINK_WIFI;
}

static void load_persisted(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    char buf[16] = {0};
    size_t len = sizeof(buf);
    if (nvs_get_str(nvs, "uplink_mode", buf, &len) == ESP_OK) {
        s_mode = str_to_mode(buf);
        ESP_LOGI(TAG, "persisted uplink mode: %s", mode_to_str(s_mode));
    }
    nvs_close(nvs);
}

esp_err_t uplink_set_mode(uplink_mode_t m, bool persist)
{
    s_mode = m;
    if (persist) {
        nvs_handle_t nvs;
        esp_err_t err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
        if (err != ESP_OK) return err;
        err = nvs_set_str(nvs, "uplink_mode", mode_to_str(m));
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
        if (err != ESP_OK) return err;
    }
    ESP_LOGI(TAG, "uplink mode set to %s (persist=%d)", mode_to_str(m), persist);
    uplink_apply();
    return ESP_OK;
}

uplink_mode_t uplink_get_mode(void)
{
    return s_mode;
}

const char *uplink_mode_str(void)
{
    return mode_to_str(s_mode);
}

/* Which interface currently holds the address a chosen mode needs. */
static bool mode_ready(uplink_mode_t m)
{
    switch (m) {
    case UPLINK_4G:   return modem_4g_is_up();
    case UPLINK_WIFI: return s_sta_have_ip;
    case UPLINK_ETH:  return s_eth_have_ip;
    default:          return false;
    }
}

/* Drop the default route and metric-based preference of every candidate, then
 * raise the one that should win. Clearing first is what makes failover
 * deterministic: without it, lwIP keeps two routes of equal length and the
 * winner depends on insertion order. */
static void apply_default_route(uplink_mode_t want)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_t *eth = esp_netif_get_handle_from_ifkey("ETH_DEF");

    if (sta) {
        esp_netif_set_route_prio(sta, 20);
    }
    if (eth) {
        esp_netif_set_route_prio(eth, 30);
    }

    switch (want) {
    case UPLINK_4G:
        /* PPP over serial carries no esp_netif handle, so the route is set on
         * the ppp netif directly. modem_4g exposes nothing public for that yet;
         * until it does, the PPP stack's own ppp_set_default() (called at dial)
         * owns the route and this case is a no-op marker. */
        break;
    case UPLINK_WIFI:
        if (sta) {
            esp_netif_set_default_netif(sta);
            esp_netif_set_route_prio(sta, 10);
            ESP_LOGI(TAG, "default route -> WIFI_STA_DEF");
        }
        break;
    case UPLINK_ETH:
        if (eth) {
            esp_netif_set_default_netif(eth);
            esp_netif_set_route_prio(eth, 10);
            ESP_LOGI(TAG, "default route -> ETH_DEF");
        }
        break;
    }
}

void uplink_apply(void)
{
    uplink_mode_t eff = s_mode;

    if (!mode_ready(eff)) {
        /* Selected mode is not up. Fall back rather than leave the router with
         * no way out; report the fallback rather than hiding it. */
        if (s_mode == UPLINK_4G && s_sta_have_ip) {
            eff = UPLINK_WIFI;
            ESP_LOGW(TAG, "4G selected but not up; falling back to WiFi");
        } else if (s_mode == UPLINK_4G && s_eth_have_ip) {
            eff = UPLINK_ETH;
            ESP_LOGW(TAG, "4G selected but not up; falling back to ETH");
        }
    }

    apply_default_route(eff);
    s_applied = true;
    ESP_LOGI(TAG, "uplink applied: %s (selected %s)", mode_to_str(eff), mode_to_str(s_mode));
}

bool uplink_is_active(uplink_mode_t m)
{
    if (!s_applied) return false;
    /* mode_ready() mirrors apply_default_route(): WiFi/ETH need a handle,
     * 4G is owned by PPP's own ppp_set_default(). */
    return mode_ready(m);
}

void uplink_on_interface_event(int32_t event_id, void *event_data)
{
    (void)event_data;

    switch (event_id) {
    case IP_EVENT_STA_GOT_IP:
        s_sta_have_ip = true;
        ESP_LOGI(TAG, "STA got IP");
        break;
    case IP_EVENT_STA_LOST_IP:
        s_sta_have_ip = false;
        ESP_LOGI(TAG, "STA lost IP");
        break;
    case IP_EVENT_ETH_GOT_IP:
        s_eth_have_ip = true;
        ESP_LOGI(TAG, "ETH got IP");
        break;
    case IP_EVENT_ETH_LOST_IP:
        s_eth_have_ip = false;
        ESP_LOGI(TAG, "ETH lost IP");
        break;
    default:
        return;  /* AP assignments, TX/RX, etc. are not uplink events */
    }
    uplink_apply();
}

static void uplink_esp_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    uplink_on_interface_event(id, data);
}

static void uplink_poll_task(void *arg)
{
    (void)arg;
    bool was_up = modem_4g_is_up();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        bool is_up = modem_4g_is_up();
        if (is_up != was_up) {
            was_up = is_up;
            ESP_LOGI(TAG, "4G PPP %s", is_up ? "came up" : "went down");
            uplink_apply();
        }

        /* RGB indicator, per docs/product/led-signals.md.
         *
         * Precedence: a live 4G link wins (fast green), then a live non-4G
         * uplink (slow green), then "4G selected and failing" (fast red). While
         * the modem is mid-bring-up we publish NONE and let the base colouring
         * show, rather than blinking a fault that has not happened yet. */
        if (is_up) {
            led_strip_set_uplink_state(LED_UPLINK_4G_UP);
        } else if (s_mode != UPLINK_4G && (s_sta_have_ip || s_eth_have_ip)) {
            led_strip_set_uplink_state(LED_UPLINK_WIFI_UP);
        } else if (s_mode == UPLINK_4G) {
            modem_4g_status_t st;
            modem_4g_get_status(&st);
            bool dialing = (st.state == MODEM_4G_STATE_PROBING ||
                            st.state == MODEM_4G_STATE_DIALING ||
                            st.state == MODEM_4G_STATE_NEGOTIATING);
            led_strip_set_uplink_state(dialing ? LED_UPLINK_NONE
                                               : LED_UPLINK_NO_NETWORK);
        } else {
            led_strip_set_uplink_state(LED_UPLINK_NONE);
        }
    }
}

void uplink_init(void)
{
    load_persisted();
    ESP_LOGI(TAG, "uplink manager ready, selected mode = %s", mode_to_str(s_mode));

    /* Register our own handlers rather than editing the base's event handler:
     * esp_event supports several handlers per event, and the base's routing and
     * reconnect logic must not be touched. */
    static const int32_t evs[] = { IP_EVENT_STA_GOT_IP, IP_EVENT_STA_LOST_IP,
                                   IP_EVENT_ETH_GOT_IP, IP_EVENT_ETH_LOST_IP };
    for (size_t i = 0; i < sizeof(evs) / sizeof(evs[0]); i++) {
        /* Signature is (base, id, handler, handler_arg, instance). Passing NULL
         * as the handler and the callback as the argument compiles fine -- both
         * are pointers -- and then trips assert(event_handler) in esp_event.c,
         * which is what bricked 0.2.0-4g.1 and looped 0.3.0-4g.2. */
        esp_err_t herr = esp_event_handler_instance_register(IP_EVENT, evs[i],
                                                              uplink_esp_event_cb, NULL, NULL);
        if (herr != ESP_OK) {
            ESP_LOGE(TAG, "cannot watch %s event %d: %s",
                     (i < 2 ? "STA" : "ETH"), evs[i], esp_err_to_name(herr));
        }
    }

    /* PPP here is raw lwIP, not an esp_netif interface, so it posts no
     * IP_EVENT_PPP_GOT_IP. Polling is therefore the only way to notice the
     * link going up or down. 2 s is well inside the link's own timeouts and
     * costs nothing when idle. */
    xTaskCreate(uplink_poll_task, "uplinkpoll", 3072, NULL, 3, NULL);

    if (s_mode == UPLINK_4G) {
        esp_err_t ierr = modem_4g_init();
        if (ierr != ESP_OK) {
            /* Do not abort the router over a modem that failed to start: the
             * AP, DHCP and web UI must still come up so the user can fix it. */
            ESP_LOGE(TAG, "modem_4g_init failed: %s -- WiFi uplink still available",
                     esp_err_to_name(ierr));
            return;
        }
        /* APN default matches the Airtel SIM measured in TASK-020 stage 6.
         * A persisted apn4g overrides it; until there is one this is the only
         * value we have evidence for. */
        char apn[64] = {0};
        nvs_handle_t nvs;
        if (nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            size_t len = sizeof(apn);
            nvs_get_str(nvs, "apn4g", apn, &len);
            nvs_close(nvs);
        }
        if (!apn[0]) snprintf(apn, sizeof(apn), "airtelgprs.com");

        ESP_LOGI(TAG, "uplink is 4G: starting PPP on APN '%s'", apn);
        esp_err_t err = modem_4g_start(apn);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "modem_4g_start failed: %s", esp_err_to_name(err));
        }
    }
}
/* ---- UI bridge (strong definitions of the weak stubs in http_server.c) ----
 *
 * The web layer cannot include this file, so everything it needs leaves through
 * plain C strings and ints. No modem-owned struct crosses the boundary.
 */
bool uplink_ui_get(char *mode, size_t mode_cap,
                   char *apn, size_t apn_cap,
                   int  *state, char *ip, size_t ip_cap,
                   int  *rssi, char *op, size_t op_cap)
{
    if (mode) snprintf(mode, mode_cap, "%s", mode_to_str(s_mode));

    /* APN: persisted value if present, else the same default uplink_init dials. */
    char apnbuf[64] = {0};
    nvs_handle_t nvs;
    if (nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(apnbuf);
        nvs_get_str(nvs, "apn4g", apnbuf, &len);
        nvs_close(nvs);
    }
    if (!apnbuf[0]) snprintf(apnbuf, sizeof(apnbuf), "airtelgprs.com");
    if (apn) snprintf(apn, apn_cap, "%s", apnbuf);

    modem_4g_status_t st;
    memset(&st, 0, sizeof(st));
    modem_4g_get_status(&st);

    if (state) *state = (int)st.state;
    if (ip)    snprintf(ip, ip_cap, "%s", st.local_ip[0] ? st.local_ip : "-");
    if (rssi)  *rssi  = st.rssi_dbm;
    if (op)    snprintf(op, op_cap, "%s", st.operator[0] ? st.operator : "-");
    return true;
}

esp_err_t uplink_ui_set(const char *mode, const char *apn)
{
    uplink_mode_t want = str_to_mode(mode);

    /* Persist the APN first so that if we are switching to 4G, uplink_init and
     * a later reboot dial the value the user just typed. */
    if (apn && apn[0]) {
        nvs_handle_t nvs;
        if (nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_str(nvs, "apn4g", apn);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }

    esp_err_t err = uplink_set_mode(want, true);   /* persists "uplink_mode" */
    if (err != ESP_OK) return err;

    /* Bring the modem up or down to match. A failed modem start must not take
     * the router with it -- the AP and this page stay reachable so the user can
     * recover, which is the whole reason the base owns the AP. */
    if (want == UPLINK_4G) {
        char apnbuf[64] = {0};
        nvs_handle_t nvs;
        if (nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            size_t len = sizeof(apnbuf);
            nvs_get_str(nvs, "apn4g", apnbuf, &len);
            nvs_close(nvs);
        }
        if (!apnbuf[0]) snprintf(apnbuf, sizeof(apnbuf), "airtelgprs.com");

        /* modem_4g_init is idempotent-by-refusal: it returns INVALID_STATE if
         * the task already exists. That is success for our purposes -- the
         * user may be switching WiFi -> 4G for the second time -- so only a
         * real failure should stop us from dialing. */
        esp_err_t ierr = modem_4g_init();
        if (ierr == ESP_OK || ierr == ESP_ERR_INVALID_STATE) {
            esp_err_t derr = modem_4g_start(apnbuf);
            if (derr != ESP_OK) ESP_LOGE(TAG, "4G start failed: %s", esp_err_to_name(derr));
        } else {
            ESP_LOGE(TAG, "modem_4g_init failed on switch to 4G: %s", esp_err_to_name(ierr));
        }
    } else {
        modem_4g_stop();
    }

    uplink_apply();
    ESP_LOGI(TAG, "uplink set from UI: %s (apn '%s')", mode_to_str(want), apn ? apn : "");
    return ESP_OK;
}
