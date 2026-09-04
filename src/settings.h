/**
 * @file settings.h
 * @brief Runtime configuration, stored in NVS (ESP32 Preferences).
 *
 * Named "settings" rather than "config" specifically to avoid colliding
 * with config.h/config.example.h, which hold the compile-time seed
 * defaults (WiFi/OTA/MQTT placeholders) AND the charging-safety
 * constants (JEITA thresholds, chemistry, sense resistors) that
 * deliberately stay compile-time forever - this file is the other half:
 * the values a runtime web form (/config, /factory in portal.cpp) is
 * allowed to change.
 *
 * Same purpose as pv-logger-c3's src/config.h/.cpp: one compiled binary
 * serves every physical node - what tells them apart (device ID, WiFi,
 * MQTT broker) is set once via the web page and survives OTA updates.
 * At first boot (empty NVS) or after a factory reset, values fall back
 * to the compile-time seed defaults in config.h.
 *
 * Deliberately NOT here: BATTERY_EXPECTED_CHEM/CELL_COUNT, JEITA_T1..T6,
 * RSNSI_OHM/RSNSB_OHM, CHARGE_CURRENT_TARGET_A, INPUT_CURRENT_LIMIT_A -
 * that's charging-safety-critical LTC4015 configuration and stays
 * exclusively in config.h/.example.h as plain #define - making it
 * remotely changeable would be a real safety regression, not a
 * convenience.
 */
#pragma once

#include <Arduino.h>

struct Settings {
  // identity - Phase 4 builds the MQTT topic charger/<deviceId>/... from this
  String deviceId;

  // WiFi
  String wifiSsid;
  String wifiPass;

  // Static IP - defaults OFF (device stays on DHCP until network
  // placement is finalized). Same rationale as pv-logger-c3: the MQTT
  // broker sits in 10.10.10.0/24, which DHCP from a different network
  // may not route to.
  bool   useStaticIp;
  String ip;
  String gw;
  String mask;
  String dns;

  // OTA (/update page) HTTP Basic Auth
  String otaUser;
  String otaPass;

  // MQTT-over-WiFi bridge (optional, gated by MQTT_WIFI_ENABLED in config.h)
  String mqttHost;
  uint16_t mqttPort;
  String mqttUser;
  String mqttPass;

  // Cellular (Walter modem) - not wired into the firmware yet (Phase 5
  // adds the dual-stack refactor, Phase 8 actually enables it), but
  // stored from the start so a future SIM/APN change doesn't need a
  // recompile.
  String apn;
  uint8_t rat;

  // Power-save (Phase 7/9) - defaults OFF, only ever turned on
  // deliberately (via /config or a remote command), never by a new
  // firmware version changing behavior on its own.
  bool     usporny;
  uint32_t periodaS;  // how often the radio/cellular link wakes
  uint32_t oknoOtaS;  // how long to stay reachable after an OTA command
};

extern Settings settings;

void settingsLoad();
void settingsSave();
void settingsFactoryReset();
