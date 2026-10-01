/* A7670E 4G fallback: a link for the ESP32 itself, never a client uplink.
 *
 * Scope, deliberately narrow, after the 4G routing work was withdrawn:
 *
 *   - 4G is NOT a router uplink. Nothing here installs a default route, enables
 *     NAPT, or changes route priority, so SoftAP clients can never egress over
 *     the modem. This is the whole point of the rewrite.
 *   - It only dials when the WiFi STA has had no address for a sustained period,
 *     i.e. there is no SSID to reach. With WiFi working -- the normal case --
 *     the modem stays completely dormant and this task costs one get_handle
 *     call every few seconds.
 *   - Even when it does come up, PPP is left as a plain netif. Whether the
 *     ESP32's *own* traffic should prefer it is a routing decision that is not
 *     made here, because the base enables NAPT globally and any default-route
 *     change would drag clients along with it. That needs an explicit decision
 *     and a separate, deliberate change.
 *
 * Nothing in this file may run before the SoftAP is up, so it is started from the
 * end of app_main (see modem_fallback_init).
 */

#include "modem_fallback.h"
#include "modem_4g.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "modem_fb";

/* How often to look at the STA, and how many consecutive misses before we
 * conclude there is genuinely no WiFi. 5 s x 3 = 15 s of no address before the
 * modem is touched at all. Long enough that a WiFi blip never dials a modem
 * call that costs money. */
#define POLL_MS         5000
#define MISSES_TO_DIAL  3

/* APN used when none is stored. This is the only value with any evidence
 * behind it (the Airtel SIM used in TASK-020). */
#define APN_FALLBACK_DEFAULT "airtelgprs.com"

static bool sta_has_address(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(sta, &ip) != ESP_OK) return false;
    return ip.ip.addr != 0;
}

static void load_apn(char *out, size_t cap)
{
    /* Read straight from NVS so no router_config dependency is needed and the
     * fallback cannot be broken by a change in the base's param helpers. */
    nvs_handle_t nvs;
    size_t len = cap;
    out[0] = '\0';
    if (nvs_open("esp32_nat", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_str(nvs, "apn4g", out, &len);
        nvs_close(nvs);
    }
    if (!out[0]) strncpy(out, APN_FALLBACK_DEFAULT, cap - 1);
    out[cap - 1] = '\0';
}

static void modem_fallback_task(void *arg)
{
    int misses = 0;
    bool dialled = false;

    /* Give the base time to associate the STA before judging it. */
    vTaskDelay(pdMS_TO_TICKS(10000));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        if (sta_has_address()) {
            misses = 0;
            if (dialled) {
                ESP_LOGI(TAG, "WiFi address is back; dropping the 4G link");
                modem_4g_stop();
                dialled = false;
            }
            continue;
        }

        if (++misses < MISSES_TO_DIAL) continue;

        if (dialled) continue;   /* already up; leave it alone */

        char apn[64];
        load_apn(apn, sizeof(apn));
        ESP_LOGW(TAG, "no WiFi address after %d checks -- bringing up 4G on '%s' "
                      "(ESP32 use only; clients are not routed over it)",
                 misses, apn);

        esp_err_t err = modem_4g_init();
        /* INVALID_STATE just means the task already exists from an earlier
         * round; that is success for our purposes, not a failure. */
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "modem_4g_init failed: %s -- staying on WiFi-only",
                     esp_err_to_name(err));
            misses = 0;             /* do not spin retrying every 5 s */
            continue;
        }
        if (modem_4g_start(apn) == ESP_OK) {
            dialled = true;
            misses = 0;
        } else {
            ESP_LOGE(TAG, "modem_4g_start failed");
            misses = 0;
        }
    }
}

void modem_fallback_init(void)
{
    xTaskCreate(modem_fallback_task, "modemfb", 4096, NULL, 3, NULL);
}