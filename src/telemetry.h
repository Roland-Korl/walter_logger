/**
 * @file telemetry.h
 * @brief Telemetry payload struct + JSON serialization for MQTT publish.
 *
 * Field list follows the handover doc's "Co logovat cez I2C z LTC4015"
 * section (VIN/IIN, VBAT/IBAT/VSYS, QCOUNT, die + NTC temperature, charger
 * state/status), plus the onboard environmental sensors, the independent
 * DS18B20 cross-check, and cellular signal quality (useful context for a
 * trackside node, and free since the modem is awake to publish anyway).
 *
 * Requires the "ArduinoJson" (Benoit Blanchon) library, v7.x.
 */

#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stddef.h> /* size_t - previously relied on transitive inclusion via
                      * Arduino.h always being included first by whatever
                      * pulled this header in; broke once portal.cpp started
                      * including telemetry.h more directly. */
#include <stdint.h>

/* Compile-time build stamp, included in every telemetry JSON payload
 * ("fw" field) and shown on the dashboard - makes it obvious at a
 * glance which build is actually running on a given node, useful for
 * exactly the kind of "is this the binary I think it is" question that
 * comes up during OTA testing. __DATE__/__TIME__ are standard C, no
 * extra tooling or git integration needed. */
#define FIRMWARE_BUILT (__DATE__ " " __TIME__)

typedef struct {
  /* --- identity --- */
  uint32_t node_id;
  uint32_t boot_count;
  int64_t unix_time; /* 0 if the modem clock could not be read */

  /* --- LTC4015: power rails --- */
  float vin_v;
  float iin_a;
  float vbat_v;
  float vbat_filt_v; /* filtered/smoothed VBAT, less noisy */
  float ibat_a;
  float vsys_v;
  uint16_t qcount_raw;

  /* --- LTC4015: live DAC readback - what the charger is ACTUALLY doing
   * right now, which can differ from what was commanded if JEITA is
   * throttling for temperature. --- */
  float icharge_dac_a;
  float iin_limit_dac_a;

  /* --- LTC4015: battery internal resistance (BSR) ---
   * Raw register value, NOT yet converted to ohms (see ltc4015.h) - but
   * still useful as a trend: rises in the cold for LiFePO4, which is
   * exactly what the winter test cares about. Only refreshed every
   * BSR_SAMPLE_INTERVAL cycles (measurement is disruptive-ish to charge
   * regulation, so it isn't run every single sample). */
  bool bsr_valid;
  uint16_t bsr_raw;

  /* --- LTC4015: temperature --- */
  float die_temp_c;
  float ntc_temp_c;   /* NAN if the measurement was invalid */
  bool ntc_valid;

  /* --- LTC4015: JEITA region the charger is currently operating in.
   * 1 = coldest (charging forced off, below JEITA_T1), 7 = hottest
   * (charging forced off, above JEITA_T6), 0 = unknown/NTC invalid.
   * Computed in firmware from the live NTC reading against the
   * configured JEITA_T1..T6 thresholds (config.h) - not read from an
   * LTC4015 region register, since that register's exact bit layout
   * wasn't independently verified (see ltc4015.h). Since both this and
   * the configured thresholds go through the same verified NTC
   * beta-equation, comparing them in software is self-consistent. */
  uint8_t jeita_region_estimated;

  /* --- LTC4015: charger state (raw registers, for offline decoding -
   * see telemetry.cpp for the human-readable decoded flags added at
   * serialization time) --- */
  uint16_t charger_state;
  uint16_t charge_status;
  uint16_t system_status;
  bool charging_blocked_cold; /* derived: ntc_pause bit set in charger_state */

  /* --- independent temperature cross-check --- */
  bool ds18b20_valid;
  float ds18b20_temp_c;
  bool temp_sensors_disagree; /* |ntc_temp_c - ds18b20_temp_c| over threshold */

  /* --- environmental sensors --- */
  bool env_valid;
  float air_temp_c;
  float humidity_pct;
  float pressure_hpa;
  bool co2_valid;
  uint16_t co2_ppm;

  /* --- cellular (best-effort, zeroed if unavailable) --- */
  bool cell_info_valid;
  uint16_t cell_band;
  uint32_t cell_id;
  float rsrp;
  float rsrq;
  uint8_t rat;

  /* --- WiFi (bring-up transport only) --- */
  bool wifi_valid;
  int8_t wifi_rssi_dbm;

  /* --- accumulated safety metric ---
   * Running count of wake cycles (roughly SLEEP_INTERVAL_S seconds each)
   * since last power-on where charging was found blocked by cold. Per
   * the handover doc, this is more important than the panel comparison
   * itself. Resets on power loss - see the RTC_DATA_ATTR note in the
   * .ino for why this is a supplementary metric, not the source of
   * truth (that's charger_state/ntc_temp_c in the logged history). */
  uint32_t cold_block_cycles_since_boot;
} telemetry_sample_t;

/**
 * @brief Serialize a telemetry sample to a compact JSON string.
 *
 * @param[in] sample The sample to serialize.
 * @param[out] out_buf Destination buffer.
 * @param[in] out_buf_size Size of out_buf in bytes.
 * @return Number of bytes written (excluding null terminator), or 0 on
 * failure (e.g. buffer too small).
 */
size_t telemetryToJson(const telemetry_sample_t* sample, char* out_buf, size_t out_buf_size);

#endif
