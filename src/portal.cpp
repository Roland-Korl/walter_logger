#include "portal.h"

#include <ArduinoJson.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "settings.h"

static WebServer server(HTTP_SERVER_PORT);
static bool otaBusy = false;

static telemetry_sample_t latest_sample;
static char json_buf[2048]; /* sized generously for chg.flags.* plus every optional section */

/* ---------------------------------------------------------------------
 * In-memory sample history, for the dashboard's trend sparklines. Not
 * persisted (RAM only) - one entry per portalPushSample() call, i.e. one
 * per collectTelemetry(). HISTORY_LEN=120 is 2 hours of trend at the
 * default 60s sample interval.
 * ------------------------------------------------------------------- */
#define HISTORY_LEN 120
typedef struct {
  float vin_v;
  float iin_a;
  float vbat_v;
  float ibat_a;
  float ntc_c;
  bool ntc_valid;
} history_entry_t;

static history_entry_t history[HISTORY_LEN];
static uint16_t history_count = 0; /* number of valid entries, caps at HISTORY_LEN */
static uint16_t history_head = 0;  /* index the NEXT entry will be written to */

bool portalOtaInProgress() { return otaBusy; }

void portalPushSample(const telemetry_sample_t *s)
{
  latest_sample = *s;

  history_entry_t *e = &history[history_head];
  e->vin_v = s->vin_v;
  e->iin_a = s->iin_a;
  e->vbat_v = s->vbat_v;
  e->ibat_a = s->ibat_a;
  e->ntc_c = s->ntc_temp_c;
  e->ntc_valid = s->ntc_valid;

  history_head = (history_head + 1) % HISTORY_LEN;
  if(history_count < HISTORY_LEN) {
    history_count++;
  }
}

/* ---------------------------------------------------------------------
 * Dashboard page - static shell (dark, card-based, inspired by
 * https://pv.rw-portal.eu/), all values filled in client-side via
 * fetch() against /telemetry and /history. Served once from flash
 * (PROGMEM) - no per-request string building on the ESP32 side.
 *
 * Unchanged from the original walter_feels_solar_test.ino - only moved
 * here so main.cpp stays transport-orchestration-only.
 * ------------------------------------------------------------------- */
static const char DASHBOARD_HTML[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html>
<html lang="sk"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Walter Feels</title>
<style>
:root{--bg:#0a0d12;--card:#12161d;--card2:#161b23;--border:#232a35;--text:#e8ecf1;--muted:#8a93a3;
--green:#3ecf8e;--blue:#4fa8f5;--orange:#f0955a;--red:#f0625a;--purple:#a78bfa;--yellow:#e8c14a;}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font-family:-apple-system,Segoe UI,Roboto,sans-serif;font-size:14px}
.wrap{max-width:1000px;margin:0 auto;padding:16px}
header{display:flex;align-items:center;justify-content:space-between;padding:8px 4px 20px}
header h1{font-size:17px;font-weight:600;margin:0;display:flex;align-items:center;gap:8px}
.dot{width:8px;height:8px;border-radius:50%;background:var(--muted)}
.dot.ok{background:var(--green);box-shadow:0 0 6px var(--green)}
.dot.bad{background:var(--red);box-shadow:0 0 6px var(--red)}
.row{display:flex;gap:14px;flex-wrap:wrap;margin-bottom:14px}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px;flex:1;min-width:260px}
.card h3{font-size:11px;letter-spacing:.06em;text-transform:uppercase;color:var(--muted);margin:0 0 12px;font-weight:600}
.big{font-size:34px;font-weight:700;line-height:1}
.big small{font-size:16px;color:var(--muted);font-weight:500;margin-left:2px}
.sub{color:var(--muted);margin-top:4px}
.badges{display:flex;gap:6px;margin-top:12px;flex-wrap:wrap}
.badge{padding:4px 10px;border-radius:7px;font-size:12px;font-weight:600;background:#1b2129;color:var(--muted);border:1px solid var(--border)}
.badge.on{background:rgba(62,207,142,.12);color:var(--green);border-color:rgba(62,207,142,.3)}
.badge.warn{background:rgba(240,197,74,.12);color:var(--yellow);border-color:rgba(232,193,74,.3)}
.badge.off{background:rgba(240,98,90,.12);color:var(--red);border-color:rgba(240,98,90,.3)}
.flow{display:flex;align-items:center;justify-content:space-around;text-align:center;padding:8px 0}
.flow .node{display:flex;flex-direction:column;align-items:center;gap:8px}
.flow .icon{width:46px;height:46px;border-radius:50%;background:var(--card2);display:flex;align-items:center;justify-content:center;border:1px solid var(--border)}
.flow .label{font-size:12px;color:var(--muted)}
.flow .val{font-size:13px;font-weight:600}
.flow .link{flex:1;height:0;border-top:2px dotted var(--border);margin:0 6px;position:relative;top:-19px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin-bottom:14px}
.stat{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:14px}
.stat h4{display:flex;align-items:center;gap:6px;font-size:11px;letter-spacing:.04em;text-transform:uppercase;color:var(--muted);margin:0 0 10px;font-weight:600}
.stat .v{font-size:22px;font-weight:700}
.stat .v small{font-size:13px;color:var(--muted);font-weight:500}
.stat svg{width:100%;height:26px;margin-top:8px;display:block}
.list{background:var(--card);border:1px solid var(--border);border-radius:12px;overflow:hidden}
.list .item{display:flex;justify-content:space-between;padding:11px 16px;border-bottom:1px solid var(--border);font-size:13px}
.list .item:last-child{border-bottom:none}
.list .item span:first-child{color:var(--muted)}
.list .item span:last-child{font-weight:600}
footer{text-align:center;color:var(--muted);font-size:12px;padding:18px 0 4px}
.err{background:rgba(240,98,90,.1);border:1px solid rgba(240,98,90,.35);color:var(--red);border-radius:10px;padding:10px 14px;margin-bottom:14px;display:none}
</style></head>
<body><div class="wrap">
<header><h1><span class="dot" id="live-dot"></span>Walter Feels &mdash; solar/LiFePO4 test</h1><span class="sub"><span id="boot-label"></span> &middot; <a href="/config" style="color:var(--muted)">Nastavenie</a> &middot; <a href="/update" style="color:var(--muted)">OTA update</a></span></header>
<div class="err" id="err-banner">Nepodarilo sa nacitat telemetriu - skusam znova...</div>

<div class="row">
  <div class="card">
    <h3>Stav nabijania</h3>
    <div class="big" id="charge-state-big">&ndash;</div>
    <div class="sub" id="charge-state-label">&ndash;</div>
    <div class="badges" id="badges"></div>
  </div>
  <div class="card">
    <h3>Tok energie</h3>
    <div class="flow">
      <div class="node"><div class="icon"><svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="#e8c14a" stroke-width="2"><circle cx="12" cy="12" r="4"/><path d="M12 2v3M12 19v3M4.2 4.2l2.1 2.1M17.7 17.7l2.1 2.1M2 12h3M19 12h3M4.2 19.8l2.1-2.1M17.7 6.3l2.1-2.1" stroke-linecap="round"/></svg></div><div class="label">Panel</div><div class="val" id="flow-panel">&ndash;</div></div>
      <div class="link"></div>
      <div class="node"><div class="icon"><svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="#3ecf8e" stroke-width="2"><rect x="2" y="7" width="17" height="10" rx="2"/><path d="M21 10v4" stroke-linecap="round"/></svg></div><div class="label">Batéria</div><div class="val" id="flow-batt">&ndash;</div></div>
      <div class="link"></div>
      <div class="node"><div class="icon"><svg width="22" height="22" viewBox="0 0 24 24" fill="#4fa8f5" stroke="none"><path d="M13 2 4 14h6l-1 8 9-12h-6l1-8z"/></svg></div><div class="label">Systém</div><div class="val" id="flow-sys">&ndash;</div></div>
    </div>
  </div>
</div>

<div class="grid" id="stat-grid"></div>

<div class="list" id="info-list"></div>

<footer id="footer">&ndash;</footer>
</div>
<script>
const $=id=>document.getElementById(id);
const STATS=[
  {key:'panel_w',label:'Vykon panela',unit:'W',hist:h=>h.map(e=>e.vin_v*e.iin_a),fmt:v=>v.toFixed(1)},
  {key:'vin',label:'Napatie panela',unit:'V',hist:h=>h.map(e=>e.vin_v),fmt:v=>v.toFixed(2)},
  {key:'vbat',label:'Napatie baterie',unit:'V',hist:h=>h.map(e=>e.vbat_v),fmt:v=>v.toFixed(3)},
  {key:'ibat_ma',label:'Prud do baterie',unit:'mA',hist:h=>h.map(e=>e.ibat_a*1000),fmt:v=>v.toFixed(0)},
  {key:'ntc',label:'Teplota NTC',unit:'&deg;C',hist:h=>h.map(e=>e.ntc_valid?e.ntc_c:null),fmt:v=>v.toFixed(1)},
  {key:'jeita',label:'JEITA region',unit:'/7',fmt:v=>v},
  {key:'bsr',label:'BSR (raw)',unit:'',fmt:v=>v},
  {key:'wifi',label:'Signal WiFi',unit:'dBm',fmt:v=>v},
];
let statNodes={};
function buildGrid(){
  const grid=$('stat-grid');
  STATS.forEach(s=>{
    const el=document.createElement('div');
    el.className='stat';
    el.innerHTML=`<h4>${s.label}</h4><div class="v" id="stat-${s.key}">&ndash;</div>${s.hist?`<svg id="spark-${s.key}" viewBox="0 0 100 26" preserveAspectRatio="none"></svg>`:''}`;
    grid.appendChild(el);
    statNodes[s.key]=el.querySelector('.v');
  });
}
function sparkline(svgId,values){
  const svg=$(svgId);
  if(!svg)return;
  const v=values.filter(x=>x!==null&&x!==undefined&&!isNaN(x));
  if(v.length<2){svg.innerHTML='';return;}
  const min=Math.min(...v),max=Math.max(...v),range=(max-min)||1;
  const pts=values.map((x,i)=>{
    const px=(i/(values.length-1))*100;
    if(x===null||x===undefined||isNaN(x))return null;
    const py=26-((x-min)/range)*24-1;
    return `${px.toFixed(1)},${py.toFixed(1)}`;
  }).filter(p=>p!==null).join(' ');
  svg.innerHTML=`<polyline points="${pts}" fill="none" stroke="currentColor" stroke-width="1.5" style="color:var(--blue)"/>`;
}
function badge(text,cls){const b=document.createElement('span');b.className='badge '+(cls||'');b.textContent=text;return b;}
function chargeStateText(f){
  if(f.charger_suspended)return['Pozastavene','warn'];
  if(f.ntc_pause)return['Blokovane (chlad/teplo)','off'];
  if(f.bat_missing_fault)return['Chyba: bateria chyba','off'];
  if(f.bat_short_fault)return['Chyba: skrat baterie','off'];
  if(f.precharge)return['Precharge','warn'];
  if(f.absorb_charge)return['Absorb','on'];
  if(f.cc_cv_charge)return['CC/CV nabijanie','on'];
  if(f.timer_term||f.c_over_x_term)return['Ukoncene (plna)','on'];
  return['Necinny','warn'];
}
async function refresh(){
  try{
    const [tRes,hRes]=await Promise.all([fetch('/telemetry'),fetch('/history')]);
    if(!tRes.ok)throw new Error('telemetry '+tRes.status);
    const t=await tRes.json();
    const h=hRes.ok?await hRes.json():[];
    $('err-banner').style.display='none';
    $('live-dot').className='dot ok';
    $('boot-label').textContent='Node '+t.node+' - boot #'+t.boot+(t.fw?' - fw '+t.fw:'');

    const [stateText,stateCls]=chargeStateText(t.chg.flags);
    $('charge-state-big').innerHTML=(t.chg.vin*t.chg.iin).toFixed(1)+'<small>W z panela</small>';
    $('charge-state-label').textContent='stav nabijania: '+stateText;

    const badges=$('badges');badges.innerHTML='';
    badges.appendChild(badge(stateText,stateCls));
    if(t.chg.cold_block)badges.appendChild(badge('Blok. zima ('+t.chg.cold_block_cycles+'x)','off'));
    if(t.wifi_rssi!==undefined)badges.appendChild(badge('WiFi '+t.wifi_rssi+' dBm',t.wifi_rssi>-70?'on':'warn'));
    if(t.mqtt!==undefined)badges.appendChild(badge(t.mqtt?'MQTT pripojene':'MQTT NEpripojene',t.mqtt?'on':'off'));
    if(!t.chg.flags.ok_to_charge)badges.appendChild(badge('NOT ok_to_charge','off'));

    $('flow-panel').textContent=(t.chg.vin*t.chg.iin).toFixed(1)+' W';
    $('flow-batt').textContent=t.chg.vbat.toFixed(2)+' V '+(t.chg.ibat_a>0.005?'&#8593;':'');
    $('flow-sys').textContent=t.chg.vsys.toFixed(2)+' V';

    const vals={panel_w:t.chg.vin*t.chg.iin,vin:t.chg.vin,vbat:t.chg.vbat,ibat_ma:t.chg.ibat*1000,
      ntc:t.chg.ntc_c,jeita:t.chg.jeita_region,bsr:t.chg.bsr_raw!==undefined?t.chg.bsr_raw:'-',
      wifi:t.wifi_rssi!==undefined?t.wifi_rssi:'-'};
    STATS.forEach(s=>{
      const v=vals[s.key];
      const node=statNodes[s.key];
      if(node)node.innerHTML=(typeof v==='number'?s.fmt(v):v)+(s.unit?` <small>${s.unit}</small>`:'');
      if(s.hist&&h.length)sparkline('spark-'+s.key,s.hist(h));
    });

    const info=$('info-list');info.innerHTML='';
    const items=[
      ['Chyby',(t.chg.flags.bat_missing_fault||t.chg.flags.bat_short_fault||t.chg.flags.max_charge_time_fault||t.chg.flags.thermal_shutdown)?'CHYBA - pozri raw JSON':'Bez chyby'],
      ['Charger safety init',t.chg.flags.charger_enabled?'OK':'SUSPENDED'],
      ['Cold-block cyklov',t.chg.cold_block_cycles],
      ['DS18B20',t.temp.ds18b20_c!==null?t.temp.ds18b20_c.toFixed(2)+' &deg;C':'nepripojeny'],
      ['Prostredie',t.env?t.env.temp_c.toFixed(1)+' &deg;C, '+t.env.hum_pct.toFixed(0)+'%, '+t.env.press_hpa.toFixed(0)+' hPa':'n/a'],
    ];
    items.forEach(([k,v])=>{
      const row=document.createElement('div');row.className='item';
      row.innerHTML=`<span>${k}</span><span>${v}</span>`;
      info.appendChild(row);
    });

    $('footer').textContent='Posledna aktualizacia: '+new Date().toLocaleTimeString('sk-SK');
  }catch(e){
    $('err-banner').style.display='block';
    $('live-dot').className='dot bad';
  }
}
buildGrid();
refresh();
setInterval(refresh,5000);
</script>
</body></html>
)HTMLPAGE";

static void handleRoot()
{
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

static void handleTelemetryJson()
{
  size_t len = telemetryToJson(&latest_sample, json_buf, sizeof(json_buf));
  /* serializeJson() truncates (not cleanly) if the buffer is too small -
   * treat "wrote right up to the edge of the buffer" as a failure too,
   * not just len==0, so a future field addition that overflows the
   * buffer fails loudly instead of silently serving corrupt JSON. */
  if(len == 0 || len >= sizeof(json_buf) - 1) {
    server.send(500, "application/json", "{\"error\":\"telemetry did not fit in buffer\"}");
    return;
  }
  server.send(200, "application/json", json_buf);
}

static void handleHistoryJson()
{
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  /* Emit oldest-to-newest so the client doesn't have to reorder. */
  uint16_t start = (history_count < HISTORY_LEN) ? 0 : history_head;
  for(uint16_t i = 0; i < history_count; i++) {
    uint16_t idx = (start + i) % HISTORY_LEN;
    JsonObject e = arr.add<JsonObject>();
    e["vin_v"] = history[idx].vin_v;
    e["iin_a"] = history[idx].iin_a;
    e["vbat_v"] = history[idx].vbat_v;
    e["ibat_a"] = history[idx].ibat_a;
    if(history[idx].ntc_valid) {
      e["ntc_c"] = history[idx].ntc_c;
    } else {
      e["ntc_c"] = nullptr;
    }
  }

  size_t len = serializeJson(doc, json_buf, sizeof(json_buf));
  if(len == 0 || len >= sizeof(json_buf) - 1) {
    server.send(500, "application/json", "[]");
    return;
  }
  server.send(200, "application/json", json_buf);
}

/* ---------------------------------------------------------------------
 * Shared CSS for /update and /config - both are simple form pages,
 * separate from DASHBOARD_HTML's own inline style.
 * ------------------------------------------------------------------- */
static const char STYLE_FORM[] PROGMEM = R"CSS(
<style>
:root{--bg:#0a0d12;--card:#12161d;--border:#232a35;--text:#e8ecf1;--muted:#8a93a3;--blue:#4fa8f5;--red:#f0625a}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:14px -apple-system,Segoe UI,Roboto,sans-serif;padding:16px}
.wrap{max-width:640px;margin:0 auto}
h1{font-size:17px;margin:0 0 6px}
h2{font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.05em;margin:20px 0 8px}
a{color:var(--blue)}
nav{display:flex;gap:8px;margin-bottom:18px;flex-wrap:wrap}
nav a{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:6px 12px;font-size:13px;text-decoration:none;color:var(--text)}
.note{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:12px 14px;margin-bottom:16px;color:var(--muted);line-height:1.6}
label{display:block;color:var(--muted);font-size:12px;margin:12px 0 4px;text-transform:uppercase;letter-spacing:.04em}
input[type=text],input[type=password],input[type=number],select{width:100%;background:#0d1117;color:var(--text);
border:1px solid var(--border);border-radius:8px;padding:9px 11px;font:14px inherit}
input[type=file]{width:100%;background:#0d1117;color:var(--text);border:1px solid var(--border);border-radius:8px;padding:10px}
input:focus,select:focus{outline:0;border-color:var(--blue)}
button{background:var(--blue);color:#06121f;border:0;border-radius:8px;padding:10px 18px;font:600 14px inherit;cursor:pointer;margin-top:12px}
button.danger{background:var(--red);color:#fff}
.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:18px}
progress{width:100%;height:9px;margin-top:14px;display:none}
.sub{color:var(--muted);margin-top:8px}
</style>
)CSS";

static const char PAGE_UPDATE[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="sk"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Walter Feels &mdash; aktualizacia</title>%STYLE%</head><body><div class="wrap">
<nav><a href="/">Prehlad</a><a href="/config">Nastavenie</a><a href="/update">Aktualizacia</a></nav>
<h1>Aktualizacia firmveru</h1>
<div class="note">Vyber <b>firmware.bin</b>. Zariadenie neodpajaj od napajania,
kym prebieha nahravanie. Po dokonceni sa samo restartuje, nastavenie
zostane zachovane.</div>
<form id="f" method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="update" accept=".bin" required>
<div><button type="submit">Nahrat</button> <a href="/">Spat</a></div>
</form>
<progress id="p" value="0" max="100"></progress>
<div id="s" class="sub"></div>
</div><script>
document.getElementById('f').onsubmit=function(e){
  e.preventDefault();
  var fd=new FormData(e.target), xhr=new XMLHttpRequest();
  var p=document.getElementById('p'), s=document.getElementById('s');
  p.style.display='block';
  xhr.upload.onprogress=function(ev){
    if(ev.lengthComputable){var pc=Math.round(ev.loaded/ev.total*100);
      p.value=pc;s.textContent='Nahravam... '+pc+' %';}};
  xhr.onload=function(){
    s.textContent=(xhr.status===200)?'Hotovo, zariadenie sa restartuje...':'Chyba: '+xhr.responseText;
    if(xhr.status===200)setTimeout(function(){location.href='/'},9000);};
  xhr.onerror=function(){s.textContent='Spojenie sa prerusilo.'};
  xhr.open('POST','/update');xhr.send(fd);
};
</script></body></html>
)HTML";

static bool requireAuth()
{
  if(!server.authenticate(settings.otaUser.c_str(), settings.otaPass.c_str())) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

/* ---------------------------------------------------------------------
 * /config, /factory - NVS-backed runtime settings, ported (in spirit)
 * from pv-logger-c3's src/portal.cpp handleConfigGet/Post/handleFactory.
 * No BSSID pinning field here (walter_logger doesn't need it - see the
 * walter_logger integration plan's resolved open questions). `rat`
 * (cellular radio access technology) isn't exposed here either: it's a
 * raw numeric code whose meaning depends on the WalterModem library,
 * which isn't linked into this build yet (Phase 5) - editing it blind
 * wouldn't be useful before then.
 * ------------------------------------------------------------------- */
static String esc(const String &s)
{
  String o = s;
  o.replace("&", "&amp;");
  o.replace("\"", "&quot;");
  o.replace("<", "&lt;");
  o.replace(">", "&gt;");
  return o;
}

static const char PAGE_CONFIG_HEAD[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="sk"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Walter Feels &mdash; nastavenie</title>%STYLE%</head><body><div class="wrap">
<nav><a href="/">Prehlad</a><a href="/config">Nastavenie</a><a href="/update">Aktualizacia</a></nav>
<h1>Nastavenie zariadenia</h1>
<div class="note">Identifikator zariadenia je kluc, pod ktorym bude lezat
historia na serveri (Faza 4). Zmena hesla k WiFi zariadenie na chvilu
odpoji od siete - uloz len vtedy, ked si si istý spravnymi udajmi.</div>
<form method="POST" action="/config">
)HTML";

static void handleConfigGet()
{
  if(!requireAuth()) {
    return;
  }

  String h = FPSTR(PAGE_CONFIG_HEAD);
  h.replace("%STYLE%", FPSTR(STYLE_FORM));

  h += F("<h2>Identita</h2><label>Device ID</label>"
         "<input type=\"text\" name=\"dev\" required pattern=\"[A-Za-z0-9_-]{1,32}\" value=\"");
  h += esc(settings.deviceId);
  h += F("\">");

  h += F("<h2>WiFi</h2><label>Siet (SSID)</label><input type=\"text\" name=\"wssid\" value=\"");
  h += esc(settings.wifiSsid);
  h += F("\"><label>Heslo (prazdne = nemenit)</label>"
         "<input type=\"password\" name=\"wpass\" placeholder=\"********\">");

  h += F("<h2>Siet</h2><label>Staticka adresa</label><select name=\"sfix\">");
  h += settings.useStaticIp ? F("<option value=\"1\" selected>ano</option><option value=\"0\">nie (DHCP)</option>")
                            : F("<option value=\"1\">ano</option><option value=\"0\" selected>nie (DHCP)</option>");
  h += F("</select><label>IP adresa</label><input type=\"text\" name=\"sip\" value=\"");
  h += esc(settings.ip);
  h += F("\"><label>Brana</label><input type=\"text\" name=\"sgw\" value=\"");
  h += esc(settings.gw);
  h += F("\"><label>Maska</label><input type=\"text\" name=\"smask\" value=\"");
  h += esc(settings.mask);
  h += F("\"><label>DNS</label><input type=\"text\" name=\"sdns\" value=\"");
  h += esc(settings.dns);
  h += F("\">");

  h += F("<h2>OTA (/update)</h2><label>Pouzivatel</label><input type=\"text\" name=\"otau\" value=\"");
  h += esc(settings.otaUser);
  h += F("\"><label>Heslo (prazdne = nemenit)</label>"
         "<input type=\"password\" name=\"otap\" placeholder=\"********\">");

  h += F("<h2>MQTT most (WiFi)</h2><label>Broker</label><input type=\"text\" name=\"mhost\" value=\"");
  h += esc(settings.mqttHost);
  h += F("\"><label>Port</label><input type=\"number\" name=\"mport\" min=\"1\" max=\"65535\" value=\"");
  h += String(settings.mqttPort);
  h += F("\"><label>Pouzivatel</label><input type=\"text\" name=\"muser\" value=\"");
  h += esc(settings.mqttUser);
  h += F("\"><label>Heslo (prazdne = nemenit)</label>"
         "<input type=\"password\" name=\"mpass\" placeholder=\"********\">");

  h += F("<h2>Celularne pripojenie (zatial nepouzite)</h2>"
         "<label>APN</label><input type=\"text\" name=\"apn\" value=\"");
  h += esc(settings.apn);
  h += F("\">");

  h += F("<h2>Uporny rezim (zatial nepouzity)</h2>"
         "<label>Zapnuty</label><select name=\"usp\">");
  h += settings.usporny ? F("<option value=\"1\" selected>ano</option><option value=\"0\">nie</option>")
                        : F("<option value=\"1\">ano</option><option value=\"0\" selected>nie</option>");
  h += F("</select><label>Perioda [s]</label><input type=\"number\" name=\"per\" min=\"20\" max=\"280\" value=\"");
  h += String(settings.periodaS);
  h += F("\"><label>Okno na OTA po prikaze [s]</label>"
         "<input type=\"number\" name=\"otaw\" min=\"60\" max=\"3600\" value=\"");
  h += String(settings.oknoOtaS);
  h += F("\">");

  h += F("<div class=\"actions\"><button type=\"submit\">Ulozit a restartovat</button>"
         "<a href=\"/\">Spat</a></div></form>"
         "<form method=\"POST\" action=\"/factory\" "
         "onsubmit=\"return confirm('Naozaj zmazat nastavenie a vratit sa k tovarenskym hodnotam?')\">"
         "<div class=\"actions\"><button class=\"danger\" type=\"submit\">Tovarenske nastavenia</button></div>"
         "</form></div></body></html>");
  server.send(200, "text/html; charset=utf-8", h);
}

static void handleConfigPost()
{
  if(!requireAuth()) {
    return;
  }

  if(server.hasArg("dev")) settings.deviceId = server.arg("dev");
  if(server.hasArg("wssid")) settings.wifiSsid = server.arg("wssid");
  // Empty password field means "leave unchanged" - otherwise every save
  // would wipe it, since the form never echoes real passwords back.
  if(server.hasArg("wpass") && server.arg("wpass").length()) settings.wifiPass = server.arg("wpass");
  if(server.hasArg("sfix")) settings.useStaticIp = (server.arg("sfix") == "1");
  if(server.hasArg("sip")) settings.ip = server.arg("sip");
  if(server.hasArg("sgw")) settings.gw = server.arg("sgw");
  if(server.hasArg("smask")) settings.mask = server.arg("smask");
  if(server.hasArg("sdns")) settings.dns = server.arg("sdns");
  if(server.hasArg("otau")) settings.otaUser = server.arg("otau");
  if(server.hasArg("otap") && server.arg("otap").length()) settings.otaPass = server.arg("otap");
  if(server.hasArg("mhost")) settings.mqttHost = server.arg("mhost");
  if(server.hasArg("mport")) settings.mqttPort = (uint16_t) server.arg("mport").toInt();
  if(server.hasArg("muser")) settings.mqttUser = server.arg("muser");
  if(server.hasArg("mpass") && server.arg("mpass").length()) settings.mqttPass = server.arg("mpass");
  if(server.hasArg("apn")) settings.apn = server.arg("apn");
  if(server.hasArg("usp")) settings.usporny = (server.arg("usp") == "1");
  if(server.hasArg("per")) {
    long v = server.arg("per").toInt();
    settings.periodaS = (uint32_t) constrain(v, 20, 280);
  }
  if(server.hasArg("otaw")) {
    settings.oknoOtaS = (uint32_t) constrain(server.arg("otaw").toInt(), 60, 3600);
  }

  settingsSave();
  server.send(200, "text/html; charset=utf-8",
              "<!DOCTYPE html><html lang=\"sk\"><head><meta charset=\"utf-8\">"
              "<meta http-equiv=\"refresh\" content=\"6;url=/\"></head>"
              "<body style=\"background:#0a0d12;color:#e8ecf1;font:15px system-ui;padding:28px\">"
              "Ulozene. Zariadenie sa restartuje a o chvilu ta presmeruje spat.</body></html>");
  delay(400);
  ESP.restart();
}

static void handleFactory()
{
  if(!requireAuth()) {
    return;
  }

  settingsFactoryReset();
  server.send(200, "text/html; charset=utf-8",
              "<!DOCTYPE html><html lang=\"sk\"><head><meta charset=\"utf-8\"></head>"
              "<body style=\"background:#0a0d12;color:#e8ecf1;font:15px system-ui;padding:28px\">"
              "Nastavenie zmazane, zariadenie sa restartuje s tovarenskymi hodnotami.</body></html>");
  delay(400);
  ESP.restart();
}

static void handleUpdateGet()
{
  if(!requireAuth()) {
    return;
  }
  String html = FPSTR(PAGE_UPDATE);
  html.replace("%STYLE%", FPSTR(STYLE_FORM));
  server.send(200, "text/html; charset=utf-8", html);
}

static void handleUpdateDone()
{
  bool ok = !Update.hasError();
  server.sendHeader("Connection", "close");
  server.send(ok ? 200 : 500, "text/plain; charset=utf-8", ok ? "OK" : "Nahratie zlyhalo");
  delay(400);
  if(ok) {
    ESP.restart();
  }
  otaBusy = false;
}

static void handleUpdateUpload()
{
  if(!server.authenticate(settings.otaUser.c_str(), settings.otaPass.c_str())) {
    /* Upload callback can't send a normal auth challenge mid-stream -
     * just refuse to write anything and let handleUpdateDone() report
     * the failure via Update.hasError(). */
    return;
  }

  HTTPUpload &up = server.upload();
  if(up.status == UPLOAD_FILE_START) {
    otaBusy = true;
    Serial.printf("OTA zo stranky: %s\r\n", up.filename.c_str());
    if(!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if(up.status == UPLOAD_FILE_WRITE) {
    if(Update.write(up.buf, up.currentSize) != up.currentSize) {
      Update.printError(Serial);
    }
  } else if(up.status == UPLOAD_FILE_END) {
    if(Update.end(true)) {
      Serial.printf("OTA hotova, %u B\r\n", (unsigned) up.totalSize);
    } else {
      Update.printError(Serial);
    }
  } else if(up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaBusy = false;
    Serial.println("OTA prerusena");
  }
}

void portalBegin()
{
  server.on("/", handleRoot);
  server.on("/telemetry", handleTelemetryJson);
  server.on("/history", handleHistoryJson);
  server.on("/config", HTTP_GET, handleConfigGet);
  server.on("/config", HTTP_POST, handleConfigPost);
  server.on("/factory", HTTP_POST, handleFactory);
  server.on("/update", HTTP_GET, handleUpdateGet);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.begin();

  Serial.printf("HTTP server listening on port %d (OTA at /update)\r\n", HTTP_SERVER_PORT);
  // Checked against the RUNTIME value (settings.otaPass), not the
  // compile-time OTA_PASSWORD macro - after a /config save these can
  // differ, and it's the runtime value that actually guards /update.
  if(settings.otaPass == "CHANGE-ME-before-wall-mount") {
    Serial.println("*** WARNING: OTA password is still the default placeholder - anyone on "
                    "the WiFi network can push firmware to this board. Change it via /config "
                    "before mounting it somewhere you can't easily re-flash over USB. ***");
  }
}

void portalLoop()
{
  server.handleClient();
}
