/* Uplink selection: WiFi STA, Ethernet, or the A7670E 4G PPP link.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif_types.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UPLINK_WIFI = 0,   /* STA associates with another SSID (the base's only mode) */
    UPLINK_4G,         /* A7670E over UART PPP                                   */
    UPLINK_ETH,        /* wired                                                  */
} uplink_mode_t;

/* Load the persisted selection from NVS. Call once, after nvs_flash_init(). */
void uplink_init(void);

/* Apply the current selection (and any necessary fallback) to the routing
 * table. Safe to call repeatedly; called on interface IP events. */
void uplink_apply(void);

/* Change the selected mode. persist=true writes it to NVS so it survives a
 * reboot. Either way the change is applied to routing immediately. */
esp_err_t uplink_set_mode(uplink_mode_t mode, bool persist);

uplink_mode_t uplink_get_mode(void);

/* "wifi" / "4g" / "eth" */
const char *uplink_mode_str(void);

/* True when this mode is the one currently carrying the default route. */
bool uplink_is_active(uplink_mode_t mode);

/* Call from the app's IP_EVENT handler for the events that mean an uplink
 * address appeared or disappeared, so selection is re-run on failover. */
void uplink_on_interface_event(int32_t event_id, void *event_data);

/* ---- UI bridge -------------------------------------------------------
 *
 * http_server is a component and must not depend on main, so the web layer
 * reaches the uplink state through these two functions instead. They are the
 * strong definitions of the weak stubs declared in http_server.c.
 */

/* Fill the web page's status fields. Returns false if no modem is present. */
bool uplink_ui_get(char *mode, size_t mode_cap,
                   char *apn, size_t apn_cap,
                   int  *state, char *ip, size_t ip_cap,
                   int  *rssi, char *op, size_t op_cap);

/* Apply a new selection from the web form: persist, then start or stop the
 * modem to match. */
esp_err_t uplink_ui_set(const char *mode, const char *apn);

#ifdef __cplusplus
}
#endif