/* A7670E 4G fallback link for the ESP32 itself (never a client uplink).
 * See modem_fallback.c for the exact scope and the reasons it is this narrow. */
#pragma once

/* Start the fallback monitor task. Call at the END of app_main, once the SoftAP
 * and web server are up, so a modem fault can never prevent the AP coming up. */
void modem_fallback_init(void);
