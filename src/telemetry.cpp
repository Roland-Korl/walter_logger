#include <ArduinoJson.h>
#include "telemetry.h"
#include "ltc4015.h"

/* Decodes the raw charger_state/charge_status/system_status registers
 * into human-readable booleans, using the bit macros from ltc4015.h.
 * Kept separate from the raw values (still included) rather than
 * replacing them, so nothing downstream has to re-derive the same
 * decode from the hex value. */
static void addDecodedFlags(JsonObject& chg, const telemetry_sample_t* s)
{
  JsonObject flags = chg["flags"].to<JsonObject>();

  /* CHARGER_STATE */
  flags["equalize_charge"] = (bool) (s->charger_state & equalize_charge);
  flags["absorb_charge"] = (bool) (s->charger_state & absorb_charge);
  flags["charger_suspended"] = (bool) (s->charger_state & charger_suspended);
  flags["precharge"] = (bool) (s->charger_state & precharge);
  flags["cc_cv_charge"] = (bool) (s->charger_state & cc_cv_charge);
  flags["ntc_pause"] = (bool) (s->charger_state & ntc_pause);
  flags["timer_term"] = (bool) (s->charger_state & timer_term);
  flags["c_over_x_term"] = (bool) (s->charger_state & c_over_x_term);
  flags["max_charge_time_fault"] = (bool) (s->charger_state & max_charge_time_fault);
  flags["bat_missing_fault"] = (bool) (s->charger_state & bat_missing_fault);
  flags["bat_short_fault"] = (bool) (s->charger_state & bat_short_fault);

  /* CHARGE_STATUS */
  flags["vin_uvcl_active"] = (bool) (s->charge_status & vin_uvcl_active);
  flags["iin_limit_active"] = (bool) (s->charge_status & iin_limit_active);
  flags["constant_current"] = (bool) (s->charge_status & constant_current);
  flags["constant_voltage"] = (bool) (s->charge_status & constant_voltage);

  /* SYSTEM_STATUS */
  flags["charger_enabled"] = (bool) (s->system_status & charger_enabled);
  flags["ok_to_charge"] = (bool) (s->system_status & ok_to_charge);
  flags["cell_count_error"] = (bool) (s->system_status & cell_count_error);
  flags["thermal_shutdown"] = (bool) (s->system_status & thermal_shutdown);
  flags["vin_ovlo"] = (bool) (s->system_status & vin_ovlo);
  flags["vin_gt_vbat"] = (bool) (s->system_status & vin_gt_vbat);
  flags["drvcc_good"] = (bool) (s->system_status & drvcc_good);
}

size_t telemetryToJson(const telemetry_sample_t* s, char* out_buf, size_t out_buf_size)
{
  JsonDocument doc;

  doc["node"] = s->node_id;
  doc["boot"] = s->boot_count;
  if(s->unix_time > 0) {
    doc["t"] = s->unix_time;
  }

  JsonObject chg = doc["chg"].to<JsonObject>();
  chg["vin"] = s->vin_v;
  chg["iin"] = s->iin_a;
  chg["vbat"] = s->vbat_v;
  chg["vbat_filt"] = s->vbat_filt_v;
  chg["ibat"] = s->ibat_a;
  chg["vsys"] = s->vsys_v;
  chg["qcount"] = s->qcount_raw;
  chg["icharge_dac"] = s->icharge_dac_a;
  chg["iin_limit_dac"] = s->iin_limit_dac_a;
  if(s->bsr_valid) {
    chg["bsr_raw"] = s->bsr_raw;
  }
  chg["die_c"] = s->die_temp_c;
  chg["ntc_c"] = s->ntc_valid ? s->ntc_temp_c : (float) NAN;
  chg["jeita_region"] = s->jeita_region_estimated;
  chg["state"] = s->charger_state;
  chg["status"] = s->charge_status;
  chg["sysstat"] = s->system_status;
  chg["cold_block"] = s->charging_blocked_cold;
  chg["cold_block_cycles"] = s->cold_block_cycles_since_boot;
  addDecodedFlags(chg, s);

  JsonObject temp = doc["temp"].to<JsonObject>();
  temp["ds18b20_c"] = s->ds18b20_valid ? s->ds18b20_temp_c : (float) NAN;
  temp["disagree"] = s->temp_sensors_disagree;

  if(s->env_valid) {
    JsonObject env = doc["env"].to<JsonObject>();
    env["temp_c"] = s->air_temp_c;
    env["hum_pct"] = s->humidity_pct;
    env["press_hpa"] = s->pressure_hpa;
  }

  if(s->co2_valid) {
    doc["co2_ppm"] = s->co2_ppm;
  }

  if(s->cell_info_valid) {
    JsonObject cell = doc["cell"].to<JsonObject>();
    cell["band"] = s->cell_band;
    cell["id"] = s->cell_id;
    cell["rsrp"] = s->rsrp;
    cell["rsrq"] = s->rsrq;
    cell["rat"] = s->rat;
  }

  if(s->wifi_valid) {
    doc["wifi_rssi"] = s->wifi_rssi_dbm;
  }

  /* ArduinoJson serializes NaN as the bare token `NaN`, which is not
   * valid JSON but is what we want here: an invalid-reading field is
   * visually obvious in the raw payload and most JSON parsers used for
   * log ingestion (e.g. Python's json module) accept it by default. If
   * your MQTT consumer uses a strict JSON parser, replace the two NAN
   * assignments above with `nullptr` instead. */
  return serializeJson(doc, out_buf, out_buf_size);
}
