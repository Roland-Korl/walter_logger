/**
 * @file config.example.h
 * @brief Template for config.h - copy this to config.h and fill in your
 * real WiFi/OTA/MQTT credentials there. config.h is gitignored on purpose
 * (it holds real secrets); this file is the one that's committed.
 *
 * See config.h.example's comments inline below for what each setting
 * does - this file mirrors config.h exactly except real secrets are
 * replaced with placeholders.
 *
 * ---
 * Central configuration for the Walter Feels solar/LiFePO4 test node.
 *
 * This node is one of three identical test nodes described in the
 * "solar-panel-test-handover" document (2026-09-01). It measures a single
 * solar panel charging a 12.8V/20Ah LiFePO4 battery through the onboard
 * LTC4015, and publishes full telemetry every SLEEP_INTERVAL_S seconds.
 *
 * -----------------------------------------------------------------------
 * REQUIRED HARDWARE JUMPER CONFIGURATION (solder jumpers on Walter Feels)
 * -----------------------------------------------------------------------
 * Battery: 12.8V nominal LiFePO4 = 4 cells (4 x 3.2V)
 *   CELLS0 = Z (both open)
 *   CELLS1 = L (OFF jumper closed, ON jumper open)
 *   CELLS2 = L (OFF jumper closed, ON jumper open)
 *
 * Chemistry: LiFePO4 fixed fast charge
 *   CHEM0 = Z (both open)
 *   CHEM1 = H (ON jumper closed, OFF jumper open)
 *
 * The firmware reads back CHEM_CELLS at boot and refuses to enable
 * charging if these don't match - see ltc4015SafeInit() in the .ino.
 */

#ifndef CONFIG_H
#define CONFIG_H

/* ---------------------------------------------------------------------
 * Identity / node numbering
 *
 * Set this to 1, 2 or 3 for the three test nodes so the MQTT topic and
 * payload identify which panel supplier this node is testing. Also used
 * as a fallback ID if reading the MAC address ever fails.
 * ------------------------------------------------------------------- */
#define NODE_ID 1

/* ---------------------------------------------------------------------
 * Telemetry transport.
 *
 * 1 = WiFi + local HTTP server serving live telemetry as JSON (bring-up
 *     mode - no cellular SIM/coverage dependency, but the node must stay
 *     awake to answer requests, so deep sleep is NOT used in this mode).
 * 0 = cellular (Walter modem) + MQTT publish, deep-sleeping between
 *     samples - the original design for the actual winter field test.
 *     Revisit once WiFi bring-up is done and the field deployment (which
 *     won't have WiFi) is being prepared.
 * ------------------------------------------------------------------- */
#define TELEMETRY_TRANSPORT_WIFI 1

#if TELEMETRY_TRANSPORT_WIFI
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define HTTP_SERVER_PORT 80
#define WIFI_CONNECT_TIMEOUT_S 20

/* ---------------------------------------------------------------------
 * OTA firmware updates, reachable at http://<node-ip>/update (HTTP Basic
 * Auth, handled directly via <Update.h> in portal.cpp - not ElegantOTA,
 * which this project dropped in favor of the same dependency-free
 * handler the pv-logger-c3 sibling project already relies on).
 *
 * IMPORTANT: anyone who can authenticate here can push arbitrary
 * firmware to this board - set a real OTA_PASSWORD before mounting the
 * box somewhere you can't easily re-flash over USB. This is the only
 * thing standing between "anyone on your WiFi" and "can flash your
 * charge controller".
 * ------------------------------------------------------------------- */
#define OTA_USERNAME "walter"
#define OTA_PASSWORD "CHANGE-ME-before-wall-mount"

/* ---------------------------------------------------------------------
 * Optional: also publish telemetry to an MQTT broker over WiFi (separate
 * from - and in addition to - the local HTTP dashboard/API, which keeps
 * working even if this is disabled or the broker is unreachable).
 *
 * Intended for bridging into a backend (e.g. an "RW Core" style MQTT
 * collector) alongside other nodes on the same broker. Before relying on
 * this, confirm:
 *   - This node's WiFi network can actually reach MQTT_WIFI_HOST (no
 *     routing/VPN needed between separate subnets).
 *   - The topic/payload convention matches what your backend/dashboard
 *     actually expects - MQTT_WIFI_TOPIC_FMT below is just this node's
 *     own guess at a plausible topic, not a confirmed contract.
 * ------------------------------------------------------------------- */
#define MQTT_WIFI_ENABLED 1
#define MQTT_WIFI_HOST "YOUR_MQTT_BROKER_IP_OR_HOST"
#define MQTT_WIFI_PORT 1883
#define MQTT_WIFI_USERNAME "" /* empty = no auth */
#define MQTT_WIFI_PASSWORD ""
#define MQTT_WIFI_CLIENT_ID_FMT "walter-feels-node%d"
#define MQTT_WIFI_TOPIC_FMT "pv/pv03/telemetry"
/* Publishes on the same cadence as SLEEP_INTERVAL_S below - no separate
 * interval, to avoid two independently-drifting timers doing almost
 * the same thing. */
#else
/* ---------------------------------------------------------------------
 * MQTT broker (cellular mode).
 *
 * TEMPORARY for bring-up verification: broker.emqx.io is a public,
 * unauthenticated test broker (same one used in WalterModem's own mqtt
 * example) - fine for confirming the publish path works end to end, but
 * anyone can read this topic. Switch to a real broker with credentials
 * before leaving this running unattended for the winter test.
 * ------------------------------------------------------------------- */
#define MQTT_HOST "broker.emqx.io"
#define MQTT_PORT 1883
#define MQTT_USERNAME NULL /* or "user" */
#define MQTT_PASSWORD NULL /* or "pass" */
#define MQTT_TLS_PROFILE 0 /* 0 = no TLS, 1-6 = configured TLS profile */
#define MQTT_TOPIC_FMT "walter/feels/node%d/telemetry"
#define MQTT_CLIENT_ID_FMT "walter-feels-node%d"
#define MQTT_QOS 1

/* ---------------------------------------------------------------------
 * Cellular
 * ------------------------------------------------------------------- */
#define CELLULAR_APN "hologram" /* required APN for a Hologram SIM, else "" */
#define RADIO_TECHNOLOGY WALTER_MODEM_RAT_NBIOT
#define LTE_CONNECT_TIMEOUT_S 300
#endif

/* ---------------------------------------------------------------------
 * Timing
 * ------------------------------------------------------------------- */
/* Handover doc: 1 minute logging interval. In WiFi mode this is the
 * re-sample period while the node stays awake serving HTTP; in cellular
 * mode it's the deep-sleep interval between wake+publish cycles. */
#define SLEEP_INTERVAL_S 60

/* ---------------------------------------------------------------------
 * Battery / charger parameters (LTC4015)
 * ------------------------------------------------------------------- */
#define BATTERY_EXPECTED_CELL_COUNT 4
#define BATTERY_EXPECTED_CHEM LIFEPO4

/* Sense resistors, from the Walter Feels BOM (R13 = IIN, R14 = IBAT). */
#define RSNSI_OHM 0.003f
#define RSNSB_OHM 0.004f

/* Charge current ceiling. A single 30-50Wp panel can never push more than
 * ~1.5A into the battery, so this only needs to sit comfortably above
 * that so the panel (via MPPT/IIN limit), not this setting, is always the
 * bottleneck. 5A = 0.25C for a 20Ah LiFePO4 cell - well within safe
 * continuous charge current for that chemistry. */
#define CHARGE_CURRENT_TARGET_A 5.0f

/* Input current ceiling on the MPPT input. Generous headroom for a
 * single panel in this test phase (see handover doc: multi-panel
 * parallel operation is a later phase, not this one). */
#define INPUT_CURRENT_LIMIT_A 3.0f

/* ---------------------------------------------------------------------
 * JEITA temperature safety thresholds
 *
 * These gate charging in HARDWARE inside the LTC4015 itself, so they
 * stay in effect even if the ESP32 crashes or is asleep - unlike a
 * pure-software cutoff. See ltc4015.h/.cpp for the derivation of the
 * register codes (computed from the board's actual NTC: R12, 10k,
 * beta=3380K, per the Walter Feels BOM).
 *
 * Per the handover doc: "Pre test nastavit prisne, blokovat pod +2 C" -
 * so JEITA_T1 (the coldest breakpoint, below which the LTC4015 forces
 * CHARGER OFF unconditionally) is set to +2C, not 0C.
 * ------------------------------------------------------------------- */
#define JEITA_T1_DEG_C 2.0f  /* charging forced OFF below this          */
#define JEITA_T2_DEG_C 5.0f
#define JEITA_T3_DEG_C 10.0f
#define JEITA_T4_DEG_C 20.0f
#define JEITA_T5_DEG_C 35.0f
#define JEITA_T6_DEG_C 50.0f /* charging forced OFF above this          */

/* ---------------------------------------------------------------------
 * Independent DS18B20 battery temperature sensor (cross-check only -
 * NOT the primary safety mechanism, see ltc4015SafeInit()).
 *
 * Not a dedicated connector on Walter Feels: wire it to one of the two
 * free GPIO pins broken out on the board. Confirm/adjust if wired
 * differently.
 * ------------------------------------------------------------------- */
#define DS18B20_ENABLED 1
#define DS18B20_PIN WFEELS_PIN_GPIO_A

/* If the DS18B20 and the LTC4015's own NTC disagree by more than this
 * many degrees, it's logged as a fault flag in the telemetry payload
 * (charging is not blocked on disagreement alone, only on the colder
 * of the two readings breaching JEITA_T1 - see ltc4015SafeInit()). */
#define TEMP_SENSOR_DISAGREEMENT_WARN_C 8.0f

/* ---------------------------------------------------------------------
 * CO2 sensor (Sensirion SCD30) - optional, present on some builds.
 * Set to 0 if not installed on this node to skip the boot delay.
 * ------------------------------------------------------------------- */
#define CO2_SENSOR_ENABLED 1

#endif
