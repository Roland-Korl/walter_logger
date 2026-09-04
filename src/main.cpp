/**
 * @file main.cpp
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
 * The dashboard/JSON API/OTA-update web server lives in portal.cpp/.h, not
 * here - main.cpp only orchestrates sensor bring-up, telemetry collection,
 * and the WiFi/cellular transports.
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
#include "portal.h"
#include "settings.h"
#include "config.h"

#if CO2_SENSOR_ENABLED
#include "scd30.h"
#endif

#if TELEMETRY_TRANSPORT_WIFI
#include <WiFi.h>
#include <esp_ota_ops.h>
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

static telemetry_sample_t latest_sample;
static char json_buf[2048]; /* used for the cellular publish and the WiFi-MQTT bridge publish */
static bool charger_ok = false;

static HDC1080 hdc1080;
static LPS22HB lps22hb(Wire);
#if CO2_SENSOR_ENABLED
static SCD30 scd30;
static bool co2_sensor_installed = false;
#endif

#if TELEMETRY_TRANSPORT_WIFI
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
 *
 * UNCHANGED from the original firmware - this is charging-safety-critical
 * code and is explicitly out of scope for the OTA/config rework.
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

  portalPushSample(s);
}

#if TELEMETRY_TRANSPORT_WIFI
/* ---------------------------------------------------------------------
 * WiFi (bring-up mode)
 * ------------------------------------------------------------------- */

static bool wifiConnect()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPass.c_str());

  Serial.printf("Connecting to WiFi '%s'", settings.wifiSsid.c_str());
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
   * signal. Harmless no-op if rollback isn't enabled in this build.
   *
   * NOTE: this only proves "reached line X of setup()", not "WiFi/portal
   * actually came up" - Phase 6 of the walter_logger rework plan moves
   * this call to after a confirmed-successful connectivity check, to
   * close that gap. Left exactly as in the original firmware for this
   * phase (Phase 1 is toolchain/OTA-mechanism only). */
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

  /* Deliberately AFTER bringUpPowerAndSensors(), not before: nothing
   * before this point reads settings.* (WiFi/OTA/MQTT credentials are
   * only needed starting a few lines below), and ltc4015SafeInit() -
   * called from bringUpPowerAndSensors() - has its own tight, already
   * documented as occasionally-marginal timing window for the LTC4015's
   * measurement subsystem to report ready. That function is
   * charging-safety-critical and stays untouched; this is only a
   * call-ORDER change in main.cpp, so NVS access (flash I/O, non-zero
   * latency, worse on a namespace's very first use) can't insert itself
   * before the safety check and skew it. */
  settingsLoad();

#if TELEMETRY_TRANSPORT_WIFI
  if(!wifiConnect()) {
    Serial.println("Error: WiFi connect failed - will keep retrying in loop()");
  }

  collectTelemetry(&latest_sample);

  portalBegin();

#if MQTT_WIFI_ENABLED
  snprintf(mqtt_wifi_topic, sizeof(mqtt_wifi_topic), "%s", MQTT_WIFI_TOPIC_FMT);
  snprintf(mqtt_wifi_client_id, sizeof(mqtt_wifi_client_id), MQTT_WIFI_CLIENT_ID_FMT, NODE_ID);
  mqttClient.setServer(settings.mqttHost.c_str(), settings.mqttPort);
  mqttClient.setBufferSize(sizeof(json_buf)); /* default 256B is far too small for our payload */
  Serial.printf("MQTT-over-WiFi bridge target: %s:%d, topic '%s' (see /config - not yet "
                "confirmed reachable/correct, see the MQTT_WIFI_* comment in config.h)\r\n",
                settings.mqttHost.c_str(), settings.mqttPort, mqtt_wifi_topic);
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
         * check in portal.cpp's handleTelemetryJson(). */
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
  portalLoop();

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
        if(settings.mqttUser.length() > 0) {
          ok = mqttClient.connect(mqtt_wifi_client_id, settings.mqttUser.c_str(),
                                  settings.mqttPass.c_str());
        } else {
          ok = mqttClient.connect(mqtt_wifi_client_id);
        }
        Serial.printf("MQTT connect to %s:%d: %s\r\n", settings.mqttHost.c_str(), settings.mqttPort,
                      ok ? "OK" : "FAILED (broker unreachable or refused - see /config note "
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
