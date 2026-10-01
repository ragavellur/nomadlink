/*
 * A7670E 4G modem as a PPP uplink for the NomadLink router.
 *
 * Transport is the one thing that differs from Espressif's usb_cdc_4g_module
 * reference: this board wires the modem to UART GPIO17/18, not USB CDC. The
 * PPP/NAPT approach is unchanged. The bring-up sequence and the two rules that
 * BUG-005 taught us are carried over from firmware/baseline/ppp_client, which
 * measured working on this board:
 *
 *   1. The module enable rail is GPIO33, active HIGH. GPIO42 is NOT PWRKEY --
 *      that net does not reach this board -- so no PWRKEY pulse is ever sent.
 *      A pulse toggles power on an off modem and hangs up on a live one.
 *   2. Silence is ambiguous. In PPP data mode the A7670E discards AT by design,
 *      so "no AT reply" and "no power" look identical. We try the guarded '+++'
 *      escape before concluding anything.
 *
 * Once the modem is in data mode we never cut its power and never send AT again:
 * those bytes would be interpreted as PPP frames.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Board wiring, fixed by schematic. Not runtime configurable. */
#define MODEM_4G_UART_NUM      1
#define MODEM_4G_UART_RX_GPIO  17 /* ESP32 RX <- modem TX */
#define MODEM_4G_UART_TX_GPIO  18 /* ESP32 TX -> modem RX */
#define MODEM_4G_ENABLE_GPIO   33 /* module enable rail, active HIGH */
#define MODEM_4G_PPP_MTU       1500

typedef enum {
    MODEM_4G_STATE_OFF = 0,     /* never started, or stopped by request   */
    MODEM_4G_STATE_PROBING,     /* powering/AT probe, not yet dialled      */
    MODEM_4G_STATE_DIALING,     /* ATD*99# sent, waiting for CONNECT      */
    MODEM_4G_STATE_NEGOTIATING, /* CONNECT seen, LCP/IPCP running          */
    MODEM_4G_STATE_UP,          /* PPP running, IP assigned, NAPT can flow */
    MODEM_4G_STATE_ERROR        /* bring-up failed; see last_error         */
} modem_4g_state_t;

const char *modem_4g_state_str(modem_4g_state_t s);

typedef struct {
    modem_4g_state_t state;

    /* Assigned by IPCP. Empty strings until MODEM_4G_STATE_UP. */
    char local_ip[16];
    char peer_ip[16];
    char dns_primary[16];

    /* Modem telemetry, sampled in the AT window before the dial. Never read
     * from the data path, where an AT command would be read as PPP. */
    int  rssi_dbm;    /* 0..31 mapped from AT+CSQ; -1 if unknown        */
    char operator[32]; /* from AT+COPS? read mode; empty if unknown      */

    char apn[64];
    char imei[16];    /* from AT+CGSN; redacted in any user-facing output */

    /* Counters, so "it is up" is not the only thing a user can look at. */
    uint32_t tx_bytes;
    uint32_t rx_bytes;

    char last_error[96];
} modem_4g_status_t;

/* Installs the UART driver and starts the modem status task. Does not dial.
 * Safe to call once at startup; returns ESP_ERR_INVALID_STATE on repeat. */
esp_err_t modem_4g_init(void);

/* Bring the modem up if needed and dial PPP with the given APN.
 * Non-blocking: the state moves to DIALING immediately and to UP or ERROR
 * later. Returns ESP_ERR_INVALID_STATE if init() has not run. */
esp_err_t modem_4g_start(const char *apn);

/* Tear the PPP link down and return the modem to AT command mode.
 * Never pulses PWRKEY; the module stays powered. */
esp_err_t modem_4g_stop(void);

/* Snapshot of current state. Safe from any task. */
void modem_4g_get_status(modem_4g_status_t *out);

/* True once PPP has reached MODEM_4G_STATE_UP and holds an IP. This is the
 * signal the router uses to decide the 4G uplink is usable. */
bool modem_4g_is_up(void);

#ifdef __cplusplus
}
#endif