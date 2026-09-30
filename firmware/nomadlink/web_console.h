/* NomadLink Console — the on-device admin page.
 *
 * Deliberate constraints, all of them learned the hard way:
 *
 *  - NO external assets. No CDN, no web fonts, no remote CSS/JS. A travel
 *    router is frequently the only route a phone has, so a page that needs the
 *    internet to render is a page that does not render. Everything is inline.
 *
 *  - The page lives at http://192.168.4.1 and http://nomadlink.local. There is
 *    no captive-portal popup: this build advertises public resolvers to DHCP
 *    clients because lwIP's DNS server is compiled out (see startSoftAP()), so
 *    an OS probe reaches the real internet and the portal never triggers. See
 *    TASK-101 AC3 / REQ-001, amended to match.
 *
 *  - Every value is served from /api/status, which reports only what the device
 *    actually measured. Fields that are not integrated say NOT INTEGRATED with
 *    the owning TASK id. Nothing here is ever a placeholder number: a plausible
 *    zero is how this project shipped two false "unsupported" verdicts once.
 */
#ifndef NOMADLINK_WEB_CONSOLE_H
#define NOMADLINK_WEB_CONSOLE_H

static const char CONSOLE_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>NomadLink Console</title>
<style>
  :root{
    --bg:#0e1116; --card:#161b22; --line:#232a34; --fg:#e6edf3; --dim:#8b949e;
    --ok:#3fb950; --warn:#d29922; --bad:#f85149; --acc:#58a6ff;
  }
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--fg);
       font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
  header{padding:14px 16px;border-bottom:1px solid var(--line);
         display:flex;align-items:baseline;gap:10px;flex-wrap:wrap;position:sticky;top:0;
         background:var(--bg);z-index:2}
  h1{font-size:16px;margin:0;font-weight:600;letter-spacing:.2px}
  .sub{color:var(--dim);font-size:12px}
  .wrap{padding:12px;max-width:900px;margin:0 auto}
  .grid{display:grid;gap:10px;grid-template-columns:repeat(auto-fit,minmax(210px,1fr))}
  .card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px}
  .card h2{font-size:11px;text-transform:uppercase;letter-spacing:.7px;
           color:var(--dim);margin:0 0 8px;font-weight:600}
  .kv{display:flex;justify-content:space-between;gap:10px;padding:2px 0;font-size:13px}
  .kv span:first-child{color:var(--dim)}
  .kv span:last-child{font-variant-numeric:tabular-nums;text-align:right;word-break:break-all}
  .pill{display:inline-block;padding:1px 7px;border-radius:10px;font-size:11px;font-weight:600}
  .up{background:rgba(63,185,80,.15);color:var(--ok)}
  .down{background:rgba(248,81,73,.15);color:var(--bad)}
  .na{background:rgba(139,148,158,.15);color:var(--dim)}
  table{width:100%;border-collapse:collapse;font-size:12.5px;font-variant-numeric:tabular-nums}
  th{text-align:left;color:var(--dim);font-weight:500;padding:5px 6px;
     border-bottom:1px solid var(--line);font-size:11px;text-transform:uppercase;letter-spacing:.5px}
  td{padding:5px 6px;border-bottom:1px solid rgba(35,42,52,.55)}
  .mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
  .ni{color:var(--dim);font-style:italic}
  .foot{color:var(--dim);font-size:11.5px;text-align:center;padding:18px 12px 28px}
  .bar{height:5px;background:var(--line);border-radius:3px;overflow:hidden;margin-top:6px}
  .bar>i{display:block;height:100%;background:var(--acc)}
  nav{display:flex;gap:6px;overflow-x:auto;padding:8px 12px;border-bottom:1px solid var(--line);
      position:sticky;top:46px;background:var(--bg);z-index:2;-webkit-overflow-scrolling:touch}
  nav button{flex:0 0 auto;padding:6px 11px;border:1px solid var(--line);border-radius:6px;
             background:var(--card);color:var(--dim);font:inherit;font-size:12.5px;cursor:pointer}
  nav button.on{color:var(--fg);border-color:var(--acc);background:rgba(88,166,255,.12)}
  .sec{display:none}
  .sec.on{display:block}
  .note{color:var(--dim);font-size:11.5px;line-height:1.45;margin:8px 0 0}
  .note b{color:var(--fg);font-weight:600}
</style>
</head>
<body>
<header>
  <h1>NomadLink Console</h1>
  <span class="sub" id="hdr">connecting…</span>
</header>

<nav>
  <button class="on" data-s="0">Dashboard</button>
  <button data-s="1">Network</button>
  <button data-s="2">Camera</button>
  <button data-s="3">Location</button>
  <button data-s="4">Cloud</button>
  <button data-s="5">SMS</button>
  <button data-s="6">Telephony</button>
</nav>

<div class="wrap">

<!-- 1 — DASHBOARD =================================================== -->
<section class="sec on" id="s0">
  <div class="grid">
    <div class="card">
      <h2>Access point</h2>
      <div class="kv"><span>SSID</span><span id="ssid" class="mono">–</span></div>
      <div class="kv"><span>AP address</span><span id="apip" class="mono">–</span></div>
      <div class="kv"><span>mDNS</span><span id="mdns" class="mono">–</span></div>
      <div class="kv"><span>DHCP pool</span><span id="dhcp" class="mono">–</span></div>
      <div class="kv"><span>Resolvers sent to clients</span><span id="dns" class="mono">–</span></div>
    </div>

    <div class="card">
      <h2>Cellular uplink</h2>
      <div class="kv"><span>PPP link</span><span id="ppp">–</span></div>
      <div class="kv"><span>Local / peer</span><span id="pips" class="mono">–</span></div>
      <div class="kv"><span>Signal (CSQ)</span><span id="csq">–</span></div>
      <div class="kv"><span>Operator</span><span id="op" class="mono">–</span></div>
      <div class="kv"><span>Sample age</span><span id="csqage">–</span></div>
    </div>

    <div class="card">
      <h2>Routing</h2>
      <div class="kv"><span>NAPT on SoftAP</span><span id="napt">–</span></div>
      <div class="kv"><span>Bytes out / in</span><span id="bytes" class="mono">–</span></div>
      <div class="kv"><span>Forwarded packets</span><span id="tap" class="mono">–</span></div>
      <div class="kv"><span>Default route</span><span id="def" class="mono">–</span></div>
    </div>

    <div class="card">
      <h2>System</h2>
      <div class="kv"><span>Uptime</span><span id="up" class="mono">–</span></div>
      <div class="kv"><span>Free heap</span><span id="heap" class="mono">–</span></div>
      <div class="kv"><span>Min ever free heap</span><span id="minheap" class="mono">–</span></div>
      <div class="kv"><span>PSRAM free</span><span id="psram" class="mono">–</span></div>
      <div class="kv"><span>Flash used</span><span id="flash" class="mono">–</span></div>
      <div class="bar"><i id="flashbar" style="width:0%"></i></div>
    </div>
  </div>

  <div class="card" style="margin-top:10px">
    <h2>Associated clients</h2>
    <table>
      <thead><tr><th>#</th><th>MAC</th><th>IP</th><th>Since</th></tr></thead>
      <tbody id="clients"><tr><td colspan="4" class="ni">none associated</td></tr></tbody>
    </table>
    <p class="note">A client that has associated but has not yet been leased shows
      <span class="mono">no address yet</span>. That is the core reporting no
      address, not a guess.</p>
  </div>
</section>

<!-- 2 — NETWORK ===================================================== -->
<section class="sec" id="s1">
  <div class="grid">
    <div class="card">
      <h2>SoftAP configuration</h2>
      <div class="kv"><span>SSID</span><span id="n_ssid" class="mono">–</span></div>
      <div class="kv"><span>Address</span><span id="n_apip" class="mono">–</span></div>
      <div class="kv"><span>DHCP pool</span><span id="n_dhcp" class="mono">–</span></div>
      <div class="kv"><span>Resolvers</span><span id="n_dns" class="mono">–</span></div>
      <div class="kv"><span>mDNS name</span><span id="n_mdns" class="mono">–</span></div>
      <div class="kv"><span>Leasable addresses</span><span class="mono">11</span></div>
      <p class="note">The pool is <b>11 addresses</b>, not the 253 a phone
      expects. This core is built with
      <span class="mono">CONFIG_LWIP_DHCPS_MAX_STATION_NUM=8</span>, so a 9th
      client can associate but will not be served. Widening the pool is a core
        rebuild, not a configuration change.</p>
    </div>

    <div class="card">
      <h2>Uplink</h2>
      <div class="kv"><span>Type</span><span class="mono">LTE PPP (A7670E)</span></div>
      <div class="kv"><span>PPP link</span><span id="n_ppp">–</span></div>
      <div class="kv"><span>Local / peer</span><span id="n_pips" class="mono">–</span></div>
      <div class="kv"><span>Default route</span><span id="n_def" class="mono">–</span></div>
      <div class="kv"><span>NAPT</span><span id="n_napt">–</span></div>
      <div class="kv"><span>PPP bytes out / in</span><span id="n_bytes" class="mono">–</span></div>
      <div class="kv"><span>Forwarded packets</span><span id="n_tap" class="mono">–</span></div>
    </div>

    <div class="card">
      <h2>Not integrated yet</h2>
      <div class="kv"><span>Wi-Fi uplink scan &amp; credentials</span><span class="ni">TASK-102, TASK-105</span></div>
      <div class="kv"><span>WAN failover state machine</span><span class="ni">TASK-108</span></div>
      <div class="kv"><span>Per-client traffic accounting</span><span class="ni">TASK-110</span></div>
      <div class="kv"><span>Config persistence in NVS</span><span class="ni">TASK-402</span></div>
      <div class="kv"><span>Portal authentication</span><span class="ni">TASK-404</span></div>
    </div>

    <div class="card">
      <h2>Access</h2>
      <p class="note"><b>No captive portal.</b> This build advertises public
      resolvers to DHCP clients, so an OS connectivity probe reaches the real
      internet and no portal popup appears. Browse to
      <span class="mono">http://192.168.4.1</span> or
      <span class="mono">http://nomadlink.local</span> directly.</p>
      <p class="note"><b>No password on this page.</b> Authentication is tracked
      as TASK-404 and is deliberately deferred. This console is a test surface,
      not a production one — do not expose it to an untrusted radio.</p>
    </div>
  </div>
</section>

<!-- 3 — CAMERA ====================================================== -->
<section class="sec" id="s2">
  <div class="card">
    <h2>Camera</h2>
    <div class="kv"><span>Driver &amp; sensor control</span><span class="ni">TASK-201</span></div>
    <div class="kv"><span>Snapshot capture</span><span class="ni">TASK-202</span></div>
    <div class="kv"><span>Live MJPEG stream</span><span class="ni">TASK-203</span></div>
    <p class="note">The OV2640 is physically verified working
    (<span class="mono">cam_diag</span> reports non-zero luma variance) but is
    not yet driven by product firmware, so this page has no sensor access.
    Streaming will need concurrent camera + PPP load, which is why it is a
    separate task rather than a line in this page.</p>
  </div>
</section>

<!-- 4 — LOCATION ==================================================== -->
<section class="sec" id="s3">
  <div class="card">
    <h2>Location</h2>
    <div class="kv"><span>GNSS fix &amp; satellites</span><span class="ni">TASK-301</span></div>
    <div class="kv"><span>Position service</span><span class="ni">TASK-302</span></div>
    <div class="kv"><span>Cellular LBS fallback</span><span class="ni">TASK-303</span></div>
    <p class="note">Not integrated into product firmware yet, and deliberately
    so: both GNSS and LBS have open regressions (BUG-010 GNSS, BUG-009 LBS)
    scheduled for re-verification on 2026-10-01. Reporting a position here now
    would mean publishing a number this project has not re-measured.</p>
  </div>
</section>

<!-- 5 — CLOUD ======================================================= -->
<section class="sec" id="s4">
  <div class="card">
    <h2>Cloud telemetry</h2>
    <div class="kv"><span>Broker connection state</span><span class="ni">TASK-403</span></div>
    <div class="kv"><span>Publish queue &amp; backoff</span><span class="ni">TASK-403</span></div>
    <p class="note">The raw broker socket path is proven
    (<span class="mono">tracker</span> reaches
    <span class="mono">+CIPOPEN: 0,10</span> and an independent subscriber has
    received a payload). The MQTT client on top of it is not built yet, so
    there is nothing honest to display here.</p>
  </div>
</section>

<!-- 6 — SMS ========================================================== -->
<section class="sec" id="s5">
  <div class="card">
    <h2>SMS</h2>
    <div class="kv"><span>Outbox</span><span class="ni">TASK-501</span></div>
    <div class="kv"><span>Inbox / command parser</span><span class="ni">TASK-502</span></div>
    <p class="note">Outbound SMS is proven
    (<span class="mono">+CMGS: &lt;ref&gt;</span> plus a user-confirmed
    delivery) but the manager does not exist yet, so there is no message store
    to list.</p>
  </div>
</section>

<!-- 7 — TELEPHONY =================================================== -->
<section class="sec" id="s6">
  <div class="card">
    <h2>Telephony</h2>
    <div class="kv"><span>Call state &amp; dialpad</span><span class="ni">TASK-601</span></div>
    <div class="kv"><span>Audio hardware population</span><span class="ni">RISK-003 / TASK-021</span></div>
    <p class="note">Outbound voice is proven
    (<span class="mono">+VOICE CALL: BEGIN</span>) but audio hardware is still
    unverified, so a dialpad here could place a call nobody can hear. Tracked
    as RISK-003.</p>
  </div>
</section>

  <div class="foot">
    Every figure above is measured on the device. Nothing is simulated.
    Sections marked not integrated name the TASK that owns them.
  </div>
</div>

<script>
function fmtBytes(b){
  if(b<1024) return b+" B";
  if(b<1048576) return (b/1024).toFixed(1)+" KB";
  if(b<1073741824) return (b/1048576).toFixed(2)+" MB";
  return (b/1073741824).toFixed(2)+" GB";
}
function fmtUptime(s){
  var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
  if(d) return d+"d "+h+"h "+m+"m";
  if(h) return h+"h "+m+"m";
  return m+"m "+Math.floor(s%60)+"s";
}
function pill(t,cls){ return '<span class="pill '+cls+'">'+t+'</span>'; }
function set(id,v){ var e=document.getElementById(id); if(e) e.innerHTML=v; }

function render(s){
  set("hdr", "uptime "+fmtUptime(s.uptime)+" · "+s.clients+" client"+(s.clients===1?"":"s"));
  set("ssid", s.ssid);
  set("apip", s.ap_ip);
  set("mdns", s.mdns_ok ? "nomadlink.local" : "unavailable");
  set("dhcp", s.dhcp_start+" – "+s.dhcp_end);
  set("dns", s.dns_prim+", "+s.dns_sec);

  set("ppp", s.ppp_up ? pill("UP","up") : pill("DOWN","down"));
  set("pips", s.ppp_up ? (s.ppp_local+" / "+s.ppp_peer) : "–");
  if(s.csq_valid){
    var pct = s.csq===99 ? null : (s.csq*2-113);
    set("csq", s.csq+(s.csq===99?" (unknown)":" ("+(pct<0?0:pct)+"%)"));
  } else set("csq", pill("not sampled","na"));
  set("op", s.operator && s.operator!="-" ? s.operator : "–");
  set("csqage", s.csq_age_s>=0 ? fmtUptime(s.csq_age_s)+" ago" : "–");

  set("napt", s.napt_on ? pill("ENABLED","up") : pill("OFF","down"));
  set("bytes", fmtBytes(s.tx_bytes)+" / "+fmtBytes(s.rx_bytes));
  set("tap", s.tap_pkts+" pkts · "+fmtBytes(s.tap_bytes));
  set("def", s.ppp_up ? s.ppp_peer : "–");

  set("up", fmtUptime(s.uptime));
  set("heap", fmtBytes(s.heap_free));
  set("minheap", fmtBytes(s.heap_min));
  set("psram", fmtBytes(s.psram_free)+" / "+fmtBytes(s.psram_total));
  var fp = Math.round(s.flash_used*100/s.flash_total);
  set("flash", fmtBytes(s.flash_used)+" / "+fmtBytes(s.flash_total)+"  ("+fp+"%)");
  document.getElementById("flashbar").style.width=fp+"%";

  /* Network view mirrors the same values; one /api/status, no second fetch. */
  set("n_ssid", s.ssid);
  set("n_apip", s.ap_ip);
  set("n_dhcp", s.dhcp_start+" – "+s.dhcp_end);
  set("n_dns", s.dns_prim+", "+s.dns_sec);
  set("n_mdns", s.mdns_ok ? "nomadlink.local" : "unavailable");
  set("n_ppp", s.ppp_up ? pill("UP","up") : pill("DOWN","down"));
  set("n_pips", s.ppp_up ? (s.ppp_local+" / "+s.ppp_peer) : "–");
  set("n_def", s.ppp_up ? s.ppp_peer : "–");
  set("n_napt", s.napt_on ? pill("ENABLED","up") : pill("OFF","down"));
  set("n_bytes", fmtBytes(s.tx_bytes)+" / "+fmtBytes(s.rx_bytes));
  set("n_tap", s.tap_pkts+" pkts · "+fmtBytes(s.tap_bytes));

  var tb=document.getElementById("clients");
  if(!s.client_list || s.client_list.length===0){
    tb.innerHTML='<tr><td colspan="4" class="ni">none associated</td></tr>';
  } else {
    var h="";
    for(var i=0;i<s.client_list.length;i++){
      var c=s.client_list[i];
      h+="<tr><td>"+(i+1)+'</td><td class="mono">'+c.mac+"</td><td class=\"mono\">"+c.ip+
         '</td><td class="mono">'+fmtUptime(c.since_s)+"</td></tr>";
    }
    tb.innerHTML=h;
  }
}

function poll(){
  fetch("/api/status",{cache:"no-store"})
    .then(function(r){ return r.json(); })
    .then(render)
    .catch(function(){
      set("hdr","disconnected — retrying");
    });
}
/* Section switching. The chosen tab is remembered, because re-opening the
 * console to check one thing should not drop you back on the dashboard. */
var btns=document.querySelectorAll("nav button");
function showSec(n){
  for(var i=0;i<btns.length;i++){
    btns[i].className=(i===n?"on":"");
    document.getElementById("s"+i).className="sec"+(i===n?" on":"");
  }
  try{ localStorage.setItem("nlsec",n); }catch(e){}
}
for(var i=0;i<btns.length;i++){
  btns[i].onclick=(function(n){ return function(){ showSec(n); }; })(i);
}
var start=0;
try{ start=parseInt(localStorage.getItem("nlsec")||"0",10)||0; }catch(e){}
if(start<0||start>=btns.length) start=0;
showSec(start);

poll();
setInterval(poll,3000);
</script>
</body>
</html>
)rawliteral";

#endif /* NOMADLINK_WEB_CONSOLE_H */
