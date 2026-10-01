/* Uplink / 4G page templates.
 *
 * The WiFi-uplink half of uplink selection already existed in the base
 * (/scan and /config carry the ssid/password). This page adds the thing that
 * did not exist: an explicit uplink choice and the A7670E 4G controls.
 */
#include "router_config.h"

#define MODEM_PAGE "<!DOCTYPE html>\
<html>\
<head>\
<meta name='viewport' content='width=device-width, initial-scale=1, maximum-scale=1, user-scalable=0'>\
<meta charset='UTF-8'>\
<title>Uplink / 4G</title>\
<link rel='icon' href='favicon.png'>\
</head>\
<style>\
* { box-sizing: border-box; margin: 0; padding: 0; }\
body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Arial, sans-serif; background: linear-gradient(135deg, #1a1a2e 0%%, #16213e 100%%); color: #e0e0e0; padding: 1rem; min-height: 100vh; line-height: 1.6; }\
h1 { font-size: 1.5rem; font-weight: 600; color: #00d9ff; margin-bottom: 1rem; }\
h2 { font-size: 1.1rem; font-weight: 500; color: #00d9ff; margin: 1.5rem 0 0.75rem; padding-bottom: 0.5rem; border-bottom: 1px solid rgba(0,217,255,0.2); }\
#container { max-width: 560px; margin: 0 auto; padding: 1.5rem; background: rgba(30,30,46,0.9); border-radius: 16px; box-shadow: 0 8px 32px rgba(0,0,0,0.4); }\
table { width: 100%%; border-collapse: collapse; }\
td { padding: 0.6rem 0; vertical-align: middle; }\
td:first-child { color: #888; font-size: 0.9rem; padding-right: 0.75rem; width: 40%%; }\
input[type='text'], select { width: 100%%; background: rgba(22,33,62,0.6); border: 1px solid rgba(0,217,255,0.2); border-radius: 8px; color: #e0e0e0; padding: 0.7rem; font-size: 0.95rem; }\
input[type='text']:focus, select:focus { outline: none; border-color: #00d9ff; }\
select option { background: #16213e; color: #e0e0e0; }\
button { border: none; border-radius: 8px; padding: 0.75rem 1.25rem; font-size: 0.95rem; font-weight: 600; cursor: pointer; background: linear-gradient(135deg, #667eea 0%%, #764ba2 100%%); color: #fff; margin-top: 0.75rem; width: 100%%; }\
.status { background: rgba(22,33,62,0.6); border-radius: 12px; padding: 1rem; border: 1px solid rgba(0,217,255,0.1); margin-bottom: 1rem; }\
.status td { padding: 0.5rem; font-size: 0.95rem; border-bottom: 1px solid rgba(255,255,255,0.05); }\
.status tr:last-child td { border-bottom: none; }\
.status td:last-child { font-weight: 500; text-align: right; }\
.pill { display: inline-block; padding: 0.2rem 0.7rem; border-radius: 999px; font-size: 0.8rem; font-weight: 600; }\
.up { background: rgba(76,175,80,0.2); color: #81c784; }\
.down { background: rgba(244,67,54,0.2); color: #e57373; }\
.busy { background: rgba(255,193,7,0.2); color: #ffd54f; }\
.warn { color: #888; font-size: 0.85rem; margin-top: 0.5rem; }\
</style>\
<body>\
<div id='container'>\
<div style='display:flex; align-items:center; margin-bottom:0.5rem;'>\
<a href='/' style='margin-right:1rem;'><img src='/favicon.png' alt='Home' style='width:56px;height:56px;border:none;'></a>\
<h1 style='margin:0;'>Uplink / 4G</h1>\
</div>\
<h2>Uplink status</h2>\
<div class='status'><table>%s</table></div>\
<h2>Select uplink</h2>\
<form method='GET' action='/modem'>\
<table>\
<tr><td>Uplink</td><td><select name='uplink_mode'>%s</select></td></tr>\
<tr><td>4G APN</td><td><input type='text' name='apn4g' value='%s' placeholder='airtelgprs.com'></td></tr>\
</table>\
<button type='submit'>Apply</button>\
</form>\
<p class='warn'>Selecting 4G starts a PPP dial. The WiFi AP stays up either way, so this page remains reachable. If 4G has no network the RGB LED blinks fast red.</p>\
<div style='margin-top:2rem; text-align:center;'>\
<a href='/' style='padding:0.75rem 2rem; background:linear-gradient(135deg,#667eea 0%%,#764ba2 100%%); color:#fff; border-radius:8px; text-decoration:none; font-size:0.95rem; font-weight:600;'>Home</a>\
</div>\
</div>\
</body>\
</html>\
"

#define MODEM_OPT_WIFI_SEL   "<option value='wifi' selected>WiFi (another SSID)</option><option value='4g'>4G (A7670E)</option>"
#define MODEM_OPT_4G_SEL     "<option value='wifi'>WiFi (another SSID)</option><option value='4g' selected>4G (A7670E)</option>"
#define MODEM_OPT_ETH_SEL    "<option value='wifi'>WiFi (another SSID)</option><option value='4g'>4G (A7670E)</option><option value='eth' selected>Ethernet</option>"

/* One status row. */
#define MODEM_ROW "<tr><td>%s</td><td>%s</td></tr>"