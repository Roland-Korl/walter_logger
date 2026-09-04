#include "settings.h"

#include <Preferences.h>

#include "config.h"

Settings settings;

static Preferences prefs;
static const char *NS = "walterlog";

void settingsLoad()
{
  // First boot: the namespace doesn't exist yet, and opening it
  // read-only would fail with "NOT_FOUND" - create it first.
  prefs.begin(NS, false);
  prefs.end();

  prefs.begin(NS, true); // read-only from here on

  /* No DEFAULT_DEVICE_ID macro exists (walter_logger's only prior
   * identity concept was the compile-time NODE_ID int) - derive a
   * reasonable seed programmatically instead of adding a new macro
   * whose default would immediately be stale once NVS takes over.
   * "charger0<N>" matches the naming already agreed for these test
   * nodes (distinct from pv-logger-c3's pv01/pv02/pv03 Victron units). */
  String defaultDeviceId = "charger0" + String(NODE_ID);
  settings.deviceId = prefs.getString("dev", defaultDeviceId);

  settings.wifiSsid = prefs.getString("wssid", WIFI_SSID);
  settings.wifiPass = prefs.getString("wpass", WIFI_PASSWORD);

  settings.useStaticIp = prefs.getBool("sfix", false);
  settings.ip = prefs.getString("sip", "");
  settings.gw = prefs.getString("sgw", "");
  settings.mask = prefs.getString("smask", "255.255.255.0");
  settings.dns = prefs.getString("sdns", "1.1.1.1");

  settings.otaUser = prefs.getString("otau", OTA_USERNAME);
  settings.otaPass = prefs.getString("otap", OTA_PASSWORD);

#if MQTT_WIFI_ENABLED
  settings.mqttHost = prefs.getString("mhost", MQTT_WIFI_HOST);
  settings.mqttPort = prefs.getUShort("mport", MQTT_WIFI_PORT);
  settings.mqttUser = prefs.getString("muser", MQTT_WIFI_USERNAME);
  settings.mqttPass = prefs.getString("mpass", MQTT_WIFI_PASSWORD);
#else
  settings.mqttHost = prefs.getString("mhost", "");
  settings.mqttPort = prefs.getUShort("mport", 1883);
  settings.mqttUser = prefs.getString("muser", "");
  settings.mqttPass = prefs.getString("mpass", "");
#endif

  /* Not read from a config.h macro on purpose - CELLULAR_APN/
   * RADIO_TECHNOLOGY only exist in the (currently unused) cellular
   * branch of config.h, and RADIO_TECHNOLOGY's type comes from
   * WalterModem.h, which isn't linked into this build yet (see
   * platformio.ini). Plain, self-contained seed values instead; Phase 5
   * revisits this once the cellular library is back and dual-stack
   * support is wired in. */
  settings.apn = prefs.getString("apn", "hologram");
  settings.rat = (uint8_t) prefs.getUChar("rat", 0);

  // Defaults OFF - a new firmware version must never turn power-saving
  // on by itself for an already-provisioned device.
  settings.usporny = prefs.getBool("usp", false);
  settings.periodaS = prefs.getUInt("per", 60);
  settings.oknoOtaS = prefs.getUInt("ota", 600);

  prefs.end();
}

void settingsSave()
{
  prefs.begin(NS, false);
  prefs.putString("dev", settings.deviceId);
  prefs.putString("wssid", settings.wifiSsid);
  prefs.putString("wpass", settings.wifiPass);
  prefs.putBool("sfix", settings.useStaticIp);
  prefs.putString("sip", settings.ip);
  prefs.putString("sgw", settings.gw);
  prefs.putString("smask", settings.mask);
  prefs.putString("sdns", settings.dns);
  prefs.putString("otau", settings.otaUser);
  prefs.putString("otap", settings.otaPass);
  prefs.putString("mhost", settings.mqttHost);
  prefs.putUShort("mport", settings.mqttPort);
  prefs.putString("muser", settings.mqttUser);
  prefs.putString("mpass", settings.mqttPass);
  prefs.putString("apn", settings.apn);
  prefs.putUChar("rat", settings.rat);
  prefs.putBool("usp", settings.usporny);
  prefs.putUInt("per", settings.periodaS);
  prefs.putUInt("ota", settings.oknoOtaS);
  prefs.end();
}

void settingsFactoryReset()
{
  prefs.begin(NS, false);
  prefs.clear();
  prefs.end();
}
