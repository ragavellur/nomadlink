/*
 * Exception-safe wrapper around esp_modem_new_dev_usb.
 *
 * The esp_modem USB DTE throws a C++ esp_exception when the modem device
 * does not appear within usb_config.timeout_ms. That exception would
 * propagate uncaught through the C API and abort() the app (boot loop).
 * This wrapper catches it and returns NULL so the caller can retry
 * (e.g. after a modem power-cycle via GPIO33).
 */
#include "esp_modem_usb_c_api.h"

extern "C" esp_modem_dce_t *esp_modem_new_dev_usb_safe(
    esp_modem_dce_device_t module, const esp_modem_dte_config_t *dte_config,
    const esp_modem_dce_config_t *dce_config, esp_netif_t *netif)
{
    try {
        return esp_modem_new_dev_usb(module, dte_config, dce_config, netif);
    } catch (...) {
        return nullptr;
    }
}