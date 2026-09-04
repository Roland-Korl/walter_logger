/**
 * @file walter_feels_solar_test.ino
 * @brief Solar panel + LiFePO4 charging test node, built on Walter Feels.
 *
 * One of three identical nodes described in the "solar-panel-test-handover"
 * document: a single solar panel (30-50Wp) charges a 12.8V/20Ah LiFePO4
 * battery through the onboard LTC4015, a fixed 10 ohm / 10 W resistor on
 * the 5V rail stands in for the real end device's load, and this firmware
 * reads the LTC4015's telemetry plus the onboard sensors and an independent
 * DS18B20 once a minute.
 *
 * Transport is selected by TELEMETRY_TRANSPORT_WIFI in config.h:
 *  - WiFi mode (bring-up, current default): connects to a local WiFi
 *    network and serves the latest telemetry as JSON over HTTP. The node
 *    stays awake (no deep sleep) so it can answer requests.
 *  - Cellular mode: the original design for the actual winter field
 *    deployment (no WiFi there) - connects over the Walter modem and
 *    publishes to an MQTT broker, deep-sleeping between samples to avoid
 *    skewing the solar energy balance being measured.
 *
 * REQUIRED READING before flashing: config.h (jumper settings, transport
 * selection, JEITA thresholds) and ltc4015.h (what's independently
 * verified vs. what still needs re-checking against the full ADI
 * datasheet).
 *
 * Required libraries (Library Manager): ArduinoJson (v7), OneWire,
 * DallasTemperature. WalterModem is assumed installed as this project's
 * board support library. WiFi/WebServer ship with the esp32 Arduino core.
 */

#include <Arduino.h>
#include <Wire.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <math.h>

#include "WalterFeels.h"
#include "ltc4015.h"
#include "hdc1080.h"
#include "lps22hb.h"
#include "ds18b20.h"
#include "telemetry.h"
#include "config.h"

#if CO2_SENSOR_ENABLED
#include "scd30.h"
#endif

#if TELEMETRY_TRANSPORT_WIFI
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
/* ElegantOTA defaults to ESPAsyncWebServer - this project already uses
 * the synchronous WebServer for everything else, so pin it to that
 * instead of pulling in a second, parallel web server stack. This has
 * to be a real compiler -D flag (see build_opt.h in this sketch folder,
 * "-DELEGANTOTA_USE_ASYNC_WEBSERVER=0"), NOT just a #define here - the
 * library's own ElegantOTA.cpp is a separate translation unit and never
 * sees a #define made only in this .ino. (Also patched a genuine bug in
 * the installed ElegantOTA.h: it used to force this macro to 1
 * unconditionally before its own ifndef-guarded default could apply -
 * see the FIX comment near the top of that file.) */
#include <ElegantOTA.h>
#if MQTT_WIFI_ENABLED
#include <WiFiClient.h>
#include <PubSubClient.h>
#endif
#else
#include <HardwareSerial.h>
#include <WalterModem.h>
#endif

/* ---------------------------------------------------------------------
 * State that must survive deep sleep (RTC memory). Also just works as
 * plain persistent state in WiFi mode, where the node never resets
 * between samples.
 *
 * NOTE: this resets to zero on a full power loss (battery disconnect /
 * brownout), not just on a normal wake. cold_block_cycles is therefore a
 * "since last power-on" counter, useful as a quick at-a-glance signal in
 * the live telemetry stream - but the authoritative source for "how many
 * hours were lost to cold this winter" is a query over the logged
 * charger_state/ntc_temp_c history in the backend, not this counter.
 * Said explicitly here so nobody downstream mistakes it for ground truth
 * after a power cycle.
 * ------------------------------------------------------------------- */
RTC_DATA_ATTR static uint32_t rtc_boot_count = 0;
RTC_DATA_ATTR static uint32_t rtc_cold_block_cycles = 0;

static char json_buf[2048]; /* sized generously for chg.flags.* plus every optional section */
static telemetry_sample_t latest_sample;
static bool charger_ok = false;

static HDC1080 hdc1080;
static LPS22HB lps22hb(Wire);
#if CO2_SENSOR_ENABLED
static SCD30 scd30;
static bool co2_sensor_installed = false;
#endif

#if TELEMETRY_TRANSPORT_WIFI
static WebServer server(HTTP_SERVER_PORT);
#if MQTT_WIFI_ENABLED
static WiFiClient mqttTcpClient;
static PubSubClient mqttClient(mqttTcpClient);
static char mqtt_wifi_topic[64];
static char mqtt_wifi_client_id[48];
#endif
#else
static WalterModem modem;
static char mqtt_topic[64];
static char mqtt_client_id[48];
#endif

/* ---------------------------------------------------------------------
 * In-memory sample history, for the dashboard's trend sparklines. Not
 * persisted (RAM only - fine, it's a UI nicety, not a data record; the
 * actual measurement record of the test lives wherever the JSON gets
 * logged, MQTT broker or otherwise). One entry per collectTelemetry()
 * call, i.e. one per SLEEP_INTERVAL_S - HISTORY_LEN=120 is 2 hours of
 * trend at the default 60s interval.
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

static void historyPush(const telemetry_sample_t* s)
{
  history_entry_t* e = &history[history_head];
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
 * LTC4015 boot-time safety sequence.
 *
 * This implements the handover doc's requirement literally:
 *  - "Zakaz nabijania musi byt predvoleny stav po resete"     -> we call
 *    suspend_charging() as the very first thing, before touching
 *    anything else.
 *  - "Povolit az po uspesnom precitani teploty"                -> we only
 *    clear the suspend bit after confirming a valid NTC reading AND
 *    (belt-and-suspenders) after JEITA is configured and enabled, so the
 *    LTC4015's own hardware state machine - not this function's return
 *    path - is what ultimately decides whether current flows.
 *  - Cell-count/chemistry mismatch (wrong solder jumpers) is treated the
 *    same as an invalid temperature reading: stay suspended.
 * ------------------------------------------------------------------- */
static bool ltc4015SafeInit()
{
  /* Sense resistor values go in as milliohms (matches the vendor driver's
   * own convention, e.g. its example calls initialize(3, 4) for the same
   * 0.003R/0.004R sense resistors used on this board). */
  LTC4015::initialize((unsigned char) lroundf(RSNSI_OHM * 1000),
                      (unsigned char) lroundf(RSNSB_OHM * 1000));

  LTC4015::suspend_charging(); /* default-safe, explicit and redundant with initialize() */

  if(!LTC4015::verify_chemistry_and_cells(BATTERY_EXPECTED_CHEM, BATTERY_EXPECTED_CELL_COUNT)) {
    Serial.println("FATAL: battery chemistry/cell-count jumpers do not match config.h - "
                    "charging stays disabled.");
    return false;
  }

  LTC4015::enable_force_telemetry();
  /* The measurement/ADC subsystem needs real time to complete its first
   * conversion cycle after force_meas_sys_on is set - but exactly how
   * long is apparently not fixed (a flat 300ms delay here passed
   * reliably across many boots, then failed once with no other change).
   * Poll for readiness with a timeout instead of gambling on one
   * fixed-delay snapshot - this removes the race entirely rather than
   * trading one magic number for a bigger one. */
  const uint32_t MEAS_SYS_READY_TIMEOUT_MS = 1000;
  uint32_t meas_wait_start = millis();
  bool meas_sys_ready = false;
  while(millis() - meas_wait_start < MEAS_SYS_READY_TIMEOUT_MS) {
    if(LTC4015::is_meas_sys_valid()) {
      meas_sys_ready = true;
      break;
    }
    delay(25);
  }

  if(!meas_sys_ready) {
    Serial.printf("FATAL: LTC4015 measurement subsystem / NTC reading not valid %ums after "
                   "boot - charging stays disabled.\r\n",
                   (unsigned) (millis() - meas_wait_start));
    return false;
  }

  ltc4015_jeita_profile_t jeita = {
    .t1_c = JEITA_T1_DEG_C, .t2_c = JEITA_T2_DEG_C, .t3_c = JEITA_T3_DEG_C,
    .t4_c = JEITA_T4_DEG_C, .t5_c = JEITA_T5_DEG_C, .t6_c = JEITA_T6_DEG_C,
  };
  LTC4015::configure_jeita(&jeita, CHARGE_CURRENT_TARGET_A);
  LTC4015::enable_jeita();

  LTC4015::set_charge_current(CHARGE_CURRENT_TARGET_A);
  LTC4015::set_input_current_max(INPUT_CURRENT_LIMIT_A);

  LTC4015::enable_coulomb_counter();
  LTC4015::enable_mppt();

  /* Only now clear the suspend bit. Even if the NTC reading were to
   * become invalid a moment later, JEITA region 1 independently forces
   * the charger off in hardware - this call is what lets charging start
   * under normal (warm enough, jumpers correct) conditions. */
  LTC4015::start_charging();

  return true;
}

/* ---------------------------------------------------------------------
 * Telemetry collection
 * ------------------------------------------------------------------- */

static void collectTelemetry(telemetry_sample_t* s)
{
  memset(s, 0, sizeof(*s));

  s->node_id = NODE_ID;
  s->boot_count = rtc_boot_count;

#if !TELEMETRY_TRANSPORT_WIFI
  WalterModemRsp clock_rsp = {};
  if(modem.getClock(&clock_rsp)) {
    s->unix_time = clock_rsp.data.clock.epochTime;
  }
#endif

  s->vin_v = LTC4015::get_input_voltage();
  s->iin_a = LTC4015::get_input_current();
  s->vbat_v = LTC4015::get_battery_voltage();
  s->vbat_filt_v = LTC4015::get_battery_voltage_filtered();
  s->ibat_a = LTC4015::get_charge_current();
  s->vsys_v = LTC4015::get_system_voltage();
  s->qcount_raw = LTC4015::get_qcount();
  s->icharge_dac_a = LTC4015::get_icharge_dac_current();
  s->iin_limit_dac_a = LTC4015::get_iin_limit_dac_current();
  s->die_temp_c = LTC4015::get_die_temp();

  /* BSR (battery internal resistance) measurement is triggered
   * periodically, not every sample - it's a one-shot the IC has to
   * perform, and running it constantly isn't necessary for a slow-moving
   * trend metric. Each call reports whatever the last completed
   * measurement was; a fresh one is kicked off every BSR_SAMPLE_PERIOD
   * calls so it stays reasonably current without spamming it. */
  static const uint32_t BSR_SAMPLE_PERIOD = 30; /* ~30 min at a 60s sample interval */
  static uint32_t bsr_call_count = 0;
  if((bsr_call_count++ % BSR_SAMPLE_PERIOD) == 0) {
    LTC4015::trigger_bsr_measurement();
  }
  s->bsr_valid = LTC4015::is_meas_sys_valid();
  s->bsr_raw = LTC4015::get_bsr_raw();

  float ntc_c = LTC4015::get_ntc_temperature();
  s->ntc_valid = !isnan(ntc_c);
  s->ntc_temp_c = ntc_c;

  if(!s->ntc_valid) {
    s->jeita_region_estimated = 0;
  } else if(ntc_c < JEITA_T1_DEG_C) {
    s->jeita_region_estimated = 1;
  } else if(ntc_c < JEITA_T2_DEG_C) {
    s->jeita_region_estimated = 2;
  } else if(ntc_c < JEITA_T3_DEG_C) {
    s->jeita_region_estimated = 3;
  } else if(ntc_c < JEITA_T4_DEG_C) {
    s->jeita_region_estimated = 4;
  } else if(ntc_c < JEITA_T5_DEG_C) {
    s->jeita_region_estimated = 5;
  } else if(ntc_c < JEITA_T6_DEG_C) {
    s->jeita_region_estimated = 6;
  } else {
    s->jeita_region_estimated = 7;
  }

  s->charger_state = LTC4015::get_charger_state();
  s->charge_status = LTC4015::get_charge_status();
  s->system_status = LTC4015::get_system_status();
  s->charging_blocked_cold = (s->charger_state & ntc_pause) != 0;

  if(!charger_ok || s->charging_blocked_cold) {
    rtc_cold_block_cycles++;
  }
  s->cold_block_cycles_since_boot = rtc_cold_block_cycles;

#if DS18B20_ENABLED
  float ds_c = 0;
  s->ds18b20_valid = DS18B20Sensor::read(&ds_c);
  s->ds18b20_temp_c = ds_c;
  if(s->ntc_valid && s->ds18b20_valid) {
    s->temp_sensors_disagree =
        fabsf(s->ntc_temp_c - s->ds18b20_temp_c) > TEMP_SENSOR_DISAGREEMENT_WARN_C;
  }
#endif

  float t = hdc1080.readTemperature();
  float h = hdc1080.readHumidity();
  /* readPressure() defaults to KILOPASCAL - explicitly request MILLIBAR
   * (numerically == hPa) to match telemetry's pressure_hpa field. */
  float p = lps22hb.readPressure(MILLIBAR);
  if(!isnan(t) && !isnan(h)) {
    s->env_valid = true;
    s->air_temp_c = t;
    s->humidity_pct = h;
    s->pressure_hpa = p;
  }

#if CO2_SENSOR_ENABLED
  if(co2_sensor_installed) {
    s->co2_valid = true;
    s->co2_ppm = scd30.getCO2();
  }
#endif

#if !TELEMETRY_TRANSPORT_WIFI
  WalterModemRsp cell_rsp = {};
  if(modem.getCellInformation(WALTER_MODEM_SQNMONI_REPORTS_SERVING_CELL, &cell_rsp)) {
    s->cell_info_valid = true;
    s->cell_band = cell_rsp.data.cellInformation.band;
    s->cell_id = cell_rsp.data.cellInformation.cid;
    s->rsrp = cell_rsp.data.cellInformation.rsrp;
    s->rsrq = cell_rsp.data.cellInformation.rsrq;
  }
  WalterModemRsp rat_rsp = {};
  if(modem.getRAT(&rat_rsp)) {
    s->rat = (uint8_t) rat_rsp.data.rat;
  }
#else
  if(WiFi.status() == WL_CONNECTED) {
    s->wifi_valid = true;
    s->wifi_rssi_dbm = (int8_t) WiFi.RSSI();
  }
#endif

  historyPush(s);
}

#if TELEMETRY_TRANSPORT_WIFI
/* ---------------------------------------------------------------------
 * WiFi + local HTTP server (bring-up mode)
 * ------------------------------------------------------------------- */

static bool wifiConnect()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to WiFi '%s'", WIFI_SSID);
  uint32_t start = millis();
  while(WiFi.status() != WL_CONNECTED) {
    if(millis() - start > (uint32_t) WIFI_CONNECT_TIMEOUT_S * 1000) {
      Serial.println();
      return false;
    }
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.printf("Connected to WiFi, IP address: %s\r\n", WiFi.localIP().toString().c_str());
  return true;
}

/* ---------------------------------------------------------------------
 * Dashboard page - static shell (dark, card-based, inspired by
 * https://pv.rw-portal.eu/), all values filled in client-side via
 * fetch() against /telemetry and /history. Served once from flash
 * (PROGMEM) - no per-request string building on the ESP32 side.
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
<header><h1><span class="dot" id="live-dot"></span>Walter Feels &mdash; solar/LiFePO4 test</h1><span class="sub"><span id="boot-label"></span> &middot; <a href="/update" style="color:var(--muted)">OTA update</a></span></header>
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
    $('boot-label').textContent='Node '+t.node+' - boot #'+t.boot;

    const [stateText,stateCls]=chargeStateText(t.chg.flags);
    $('charge-state-big').innerHTML=(t.chg.vin*t.chg.iin).toFixed(1)+'<small>W z panela</small>';
    $('charge-state-label').textContent='stav nabijania: '+stateText;

    const badges=$('badges');badges.innerHTML='';
    badges.appendChild(badge(stateText,stateCls));
    if(t.chg.cold_block)badges.appendChild(badge('Blok. zima ('+t.chg.cold_block_cycles+'x)','off'));
    if(t.wifi_rssi!==undefined)badges.appendChild(badge('WiFi '+t.wifi_rssi+' dBm',t.wifi_rssi>-70?'on':'warn'));
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
#endif

/* ---------------------------------------------------------------------
 * Cellular LTE + MQTT (field deployment mode) - adapted from the
 * official mqtt.ino / walter_feels.ino examples (DPTechnics).
 * ------------------------------------------------------------------- */
#if !TELEMETRY_TRANSPORT_WIFI

static bool lteConnected()
{
  WalterModemNetworkRegState regState = modem.getNetworkRegState();
  return (regState == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME ||
          regState == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING);
}

static bool waitForNetwork(int timeout_sec)
{
  Serial.print("Connecting to the network...");
  int time = 0;
  while(!lteConnected()) {
    Serial.print(".");
    delay(1000);
    time++;
    if(time > timeout_sec) {
      Serial.println();
      return false;
    }
  }
  Serial.println();
  Serial.println("Connected to the network");
  return true;
}

static bool lteConnect()
{
  if(!modem.setOpState(WALTER_MODEM_OPSTATE_NO_RF)) {
    Serial.println("Error: could not set opstate to NO_RF");
    return false;
  }
  if(!modem.definePDPContext(1, CELLULAR_APN)) {
    Serial.println("Error: could not create PDP context");
    return false;
  }
  if(!modem.setOpState(WALTER_MODEM_OPSTATE_FULL)) {
    Serial.println("Error: could not set opstate to FULL");
    return false;
  }
  if(!modem.setNetworkSelectionMode(WALTER_MODEM_NETWORK_SEL_MODE_AUTOMATIC)) {
    Serial.println("Error: could not set network selection mode");
    return false;
  }
  return waitForNetwork(LTE_CONNECT_TIMEOUT_S);
}

#endif // !TELEMETRY_TRANSPORT_WIFI

/* ---------------------------------------------------------------------
 * Shared bring-up: power rails, I2C, charger safety init, sensors.
 * ------------------------------------------------------------------- */

static void bringUpPowerAndSensors()
{
  WalterFeels::set3v3(true);
  WalterFeels::setI2cBusPower(true);
  /* Small settle margin for the 3.3V rail's soft-start fade (set3v3()
   * returns before the fade completes). NOTE: this was NOT the fix for
   * the ESP_ERR_INVALID_STATE failures seen during bring-up on esp32
   * core 3.3.x - that turned out to be a real regression in that core's
   * new I2C driver (see https://github.com/espressif/arduino-esp32/issues/11374),
   * worked around by building against esp32 core 3.1.3 instead. Kept
   * here anyway since it's cheap and doesn't hurt. */
  delay(150);

  if(!Wire.begin(WFEELS_PIN_I2C_SDA, WFEELS_PIN_I2C_SCL, 100000)) {
    Serial.println("Error: I2C init failed, restarting");
    delay(2000);
    ESP.restart();
  }

  charger_ok = ltc4015SafeInit();
  if(!charger_ok) {
    Serial.println("Charger left SUSPENDED due to a failed safety check (see above). "
                    "Telemetry will still be collected and served/published.");
  }

  hdc1080.begin();
  lps22hb.begin();

#if DS18B20_ENABLED
  DS18B20Sensor::begin(DS18B20_PIN);
#endif

#if CO2_SENSOR_ENABLED
  Wire1.begin(WFEELS_PIN_CO2_SDA, WFEELS_PIN_CO2_SCL);
  delay(100);
  co2_sensor_installed = scd30.begin(Wire1);
  for(int i = 0; i < 5 && !co2_sensor_installed; i++) {
    delay(500);
    co2_sensor_installed = scd30.begin(Wire1);
  }
  if(!co2_sensor_installed) {
    Serial.println("Warning: no SCD30 CO2 sensor found");
  }
#endif
}

/* ---------------------------------------------------------------------
 * Setup / loop
 * ------------------------------------------------------------------- */

void setup()
{
  Serial.begin(115200);
  delay(200);

#if TELEMETRY_TRANSPORT_WIFI
  /* If the bootloader's app-rollback safety net is active, an OTA'd
   * image stays "unconfirmed" (and gets auto-reverted to the previous
   * one on the next reset) until something in the new firmware calls
   * this. Getting this far in setup() is a reasonable "looks alive"
   * signal. Harmless no-op if rollback isn't enabled in this build. */
  esp_ota_mark_app_valid_cancel_rollback();
#endif

  rtc_boot_count++;

  Serial.printf("\r\n=== Walter Feels solar/LiFePO4 test node %d (boot #%u) ===\r\n", NODE_ID,
                rtc_boot_count);

  uint8_t mac[6] = { 0 };
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  Serial.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n", mac[0], mac[1], mac[2], mac[3], mac[4],
                mac[5]);

  bringUpPowerAndSensors();

#if TELEMETRY_TRANSPORT_WIFI
  if(!wifiConnect()) {
    Serial.println("Error: WiFi connect failed - will keep retrying in loop()");
  }

  collectTelemetry(&latest_sample);

  server.on("/", handleRoot);
  server.on("/telemetry", handleTelemetryJson);
  server.on("/history", handleHistoryJson);
  server.begin();

  ElegantOTA.begin(&server, OTA_USERNAME, OTA_PASSWORD);
  Serial.printf("HTTP server listening on port %d (OTA at /update)\r\n", HTTP_SERVER_PORT);
  if(strcmp(OTA_PASSWORD, "CHANGE-ME-before-wall-mount") == 0) {
    Serial.println("*** WARNING: OTA_PASSWORD is still the default placeholder - anyone on "
                    "the WiFi network can push firmware to this board. Change it in config.h "
                    "before mounting it somewhere you can't easily re-flash over USB. ***");
  }

#if MQTT_WIFI_ENABLED
  snprintf(mqtt_wifi_topic, sizeof(mqtt_wifi_topic), "%s", MQTT_WIFI_TOPIC_FMT);
  snprintf(mqtt_wifi_client_id, sizeof(mqtt_wifi_client_id), MQTT_WIFI_CLIENT_ID_FMT, NODE_ID);
  mqttClient.setServer(MQTT_WIFI_HOST, MQTT_WIFI_PORT);
  mqttClient.setBufferSize(sizeof(json_buf)); /* default 256B is far too small for our payload */
  Serial.printf("MQTT-over-WiFi bridge target: %s:%d, topic '%s' (see config.h - not yet "
                "confirmed reachable/correct, see the MQTT_WIFI_* comment there)\r\n",
                MQTT_WIFI_HOST, MQTT_WIFI_PORT, mqtt_wifi_topic);
#endif
#else
  snprintf(mqtt_topic, sizeof(mqtt_topic), MQTT_TOPIC_FMT, NODE_ID);
  snprintf(mqtt_client_id, sizeof(mqtt_client_id), MQTT_CLIENT_ID_FMT, NODE_ID);

  if(!modem.begin(&Serial2)) {
    Serial.println("Error: could not initialize the modem, restarting");
    delay(2000);
    ESP.restart();
  }

  WalterModemRsp rat_check_rsp = {};
  if(modem.getRAT(&rat_check_rsp) && rat_check_rsp.data.rat != RADIO_TECHNOLOGY) {
    modem.setRAT(RADIO_TECHNOLOGY);
    Serial.println("Switched modem radio technology - restarting to apply");
    delay(500);
    ESP.restart();
  }

  /* Connect first (if it works, cell_info/RAT in collectTelemetry() get
   * filled in), but collect telemetry - and advance the cold-block
   * counter - regardless of whether connectivity or the MQTT publish
   * itself succeeds. A network outage must not silently erase this
   * cycle's cold-block accounting; only the *publish* is allowed to fail
   * without consequence for the counters. */
  bool lte_ok = lteConnect();
  if(!lte_ok) {
    Serial.println("Error: LTE connect failed - will retry next cycle");
  }

  collectTelemetry(&latest_sample);

  bool published = false;
  if(lte_ok) {
    if(modem.mqttConfig(mqtt_client_id, MQTT_USERNAME, MQTT_PASSWORD, MQTT_TLS_PROFILE)) {
      if(modem.mqttConnect(MQTT_HOST, MQTT_PORT)) {
        size_t len = telemetryToJson(&latest_sample, json_buf, sizeof(json_buf));
        /* len >= buffer size - 1 means serializeJson() truncated (not
         * cleanly) - treat that the same as len==0, see the identical
         * check in handleTelemetryJson(). */
        if(len > 0 && len < sizeof(json_buf) - 1) {
          Serial.printf("Publishing %u bytes to '%s':\r\n%s\r\n", (unsigned) len, mqtt_topic,
                        json_buf);
          published = modem.mqttPublish(mqtt_topic, (uint8_t*) json_buf, len, MQTT_QOS);
          if(!published) {
            Serial.println("Error: MQTT publish failed");
          }
        } else {
          Serial.println("Error: telemetry JSON did not fit in the buffer");
        }
        modem.mqttDisconnect();
      } else {
        Serial.println("Error: MQTT connect failed");
      }
    } else {
      Serial.println("Error: MQTT config failed");
    }
  }

  if(!published) {
    Serial.println("This cycle's telemetry was NOT published (see error above); "
                    "cold-block counter still advanced locally.");
  }

  WalterFeels::prepareDeepSleep();
  Serial.printf("Sleeping for %d seconds\r\n", SLEEP_INTERVAL_S);
  Serial.flush();
  modem.sleep(SLEEP_INTERVAL_S);
#endif
}

void loop()
{
#if TELEMETRY_TRANSPORT_WIFI
  server.handleClient();
  ElegantOTA.loop();

  static uint32_t last_sample_ms = 0;
  uint32_t now = millis();
  if(now - last_sample_ms >= (uint32_t) SLEEP_INTERVAL_S * 1000) {
    last_sample_ms = now;
    collectTelemetry(&latest_sample);
    Serial.printf("[sample #%u] VIN=%.2fV IIN=%.3fA VBAT=%.2fV IBAT=%.3fA NTC=%s cold_block=%s\r\n",
                  latest_sample.boot_count, latest_sample.vin_v, latest_sample.iin_a,
                  latest_sample.vbat_v, latest_sample.ibat_a,
                  latest_sample.ntc_valid ? String(latest_sample.ntc_temp_c, 1).c_str() : "invalid",
                  latest_sample.charging_blocked_cold ? "yes" : "no");

#if MQTT_WIFI_ENABLED
    if(mqttClient.connected()) {
      size_t len = telemetryToJson(&latest_sample, json_buf, sizeof(json_buf));
      if(len > 0 && len < sizeof(json_buf) - 1) {
        bool ok = mqttClient.publish(mqtt_wifi_topic, (const uint8_t*) json_buf, len, false);
        Serial.printf("MQTT publish to '%s': %s\r\n", mqtt_wifi_topic, ok ? "OK" : "FAILED");
      }
    } else {
      Serial.println("MQTT not connected - skipping this cycle's bridge publish "
                      "(local dashboard/API are unaffected)");
    }
#endif
  }

  if(WiFi.status() != WL_CONNECTED) {
    static uint32_t last_reconnect_attempt_ms = 0;
    if(now - last_reconnect_attempt_ms >= 10000) {
      last_reconnect_attempt_ms = now;
      Serial.println("WiFi disconnected, retrying...");
      wifiConnect();
    }
  }

#if MQTT_WIFI_ENABLED
  if(WiFi.status() == WL_CONNECTED) {
    if(!mqttClient.connected()) {
      static uint32_t last_mqtt_attempt_ms = 0;
      /* Rate-limited retry, not blocking retry-with-delay() - a stuck
       * broker must not stall the local HTTP dashboard/OTA. */
      if(now - last_mqtt_attempt_ms >= 15000) {
        last_mqtt_attempt_ms = now;
        bool ok;
        if(strlen(MQTT_WIFI_USERNAME) > 0) {
          ok = mqttClient.connect(mqtt_wifi_client_id, MQTT_WIFI_USERNAME, MQTT_WIFI_PASSWORD);
        } else {
          ok = mqttClient.connect(mqtt_wifi_client_id);
        }
        Serial.printf("MQTT connect to %s:%d: %s\r\n", MQTT_WIFI_HOST, MQTT_WIFI_PORT,
                      ok ? "OK" : "FAILED (broker unreachable or refused - see config.h note "
                                   "about unconfirmed network routing/topic)");
      }
    }
    mqttClient.loop();
  }
#endif
#else
  /* Not used - cellular mode does everything once in setup() then deep
   * sleeps, which effectively "restarts" into setup() again on wake. */
#endif
}
