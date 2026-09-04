#include <Wire.h>
#include <Arduino.h>
#include <math.h>
#include "ltc4015.h"

static const char* _logtag = "[LTC4015]";
static const char* bat_chem_str[] = { "Li Ion", "LiFePO4", "Lead Acid", "Invalid" };

/* NTC on Walter Feels: R12, 10k @ 25C, beta = 3380K (Walter Feels BOM).
 * NTCBIAS is internal to the LTC4015 with a nominal value equal to the
 * recommended thermistor value (10k) - see file header in ltc4015.h for
 * what is/isn't independently verified here. */
static const float NTC_R25_OHM = 10000.0f;
static const float NTC_BETA = 3380.0f;
static const float NTC_T0_K = 298.15f;
static const float NTC_RBIAS_OHM = 10000.0f;
static const float NTC_RATIO_FULL_SCALE = 21845.0f;

void LTC4015::initialize(unsigned char _Rsnsi, unsigned char _Rsnsb)
{
  _rsnsi = _Rsnsi;
  _rsnsb = _Rsnsb;

  /* Default-safe: never leave the charger enabled from a previous state
   * while we're still figuring out whether it's safe to charge. */
  suspend_charging();
  get_battery_info();
  printinfo();
}

void LTC4015::printinfo()
{
  ESP_LOGI(_logtag, "Battery (as read from CHEM_CELLS): %s, %u cells", bat_chem_str[_chemistry],
           _cell_count);
}

/* ---------------------------------------------------------------------
 * Current / voltage / limit setters
 * ------------------------------------------------------------------- */

void LTC4015::set_charge_current(float current_a)
{
  /* current = (code + 1) * 1mV / Rsnsb_ohms  =>  code = current * Rsnsb_ohms / 1mV - 1.
   * _rsnsb is stored as a milliohm-integer (e.g. 4 for 0.004R, matching
   * initialize()'s calling convention), i.e. _rsnsb == Rsnsb_ohms / 1mV
   * already - so the /1mV division is already baked into that
   * convention and must NOT be applied again here. This matches the
   * vendor driver's original (correct) arithmetic. */
  int32_t code = (int32_t) lroundf(current_a * _rsnsb) - 1;
  if(code < 0) {
    code = 0;
  }
  if(code > 31) {
    code = 31;
  }
  _write_word(LTC4015_REG_ICHARGE_TARGET, (unsigned short) code);
}

void LTC4015::set_charge_voltage(float voltage_v)
{
  /* Only meaningful for a *programmable* chemistry setting - with a
   * *fixed* CHEM pin setting (used in this project) the charge voltage
   * is selected by the CHEM pins in hardware and this register is
   * ignored by the IC. Kept for completeness / future use. */
  float v_cell = voltage_v / (float) _cell_count;
  int32_t code = 0;

  if(_chemistry == LI_ION) {
    code = (int32_t) lroundf((v_cell - 3.8125f) * 80.0f);
  } else if(_chemistry == LIFEPO4) {
    code = (int32_t) lroundf((v_cell - 3.4125f) * 80.0f);
  } else if(_chemistry == LEAD_ACID) {
    code = (int32_t) lroundf((v_cell - 2.0f) * 105.0f);
  } else {
    return;
  }

  int32_t max_code = (_chemistry == LEAD_ACID) ? 63 : 31;
  if(code < 0) {
    code = 0;
  }
  if(code > max_code) {
    code = max_code;
  }
  _write_word(LTC4015_REG_VCHARGE_SETTING, (unsigned short) code);
}

void LTC4015::set_input_current_max(float current_a)
{
  /* FIX vs. the vendor example: this must write IIN_LIMIT_SETTING
   * (0x15), not ICHARGE_TARGET - that part of the original driver was
   * wrong. Its arithmetic, however, was correct: current = (code + 1) *
   * 500uV / Rsnsi_ohms  =>  code = current*Rsnsi_ohms/500uV - 1, and
   * since _rsnsi is a milliohm-integer (== Rsnsi_ohms / 1mV), that
   * reduces to 2 * current * _rsnsi - 1 with no further scaling -
   * exactly the vendor's original expression, just aimed at the right
   * register now. */
  int32_t code = (int32_t) lroundf(2.0f * current_a * _rsnsi) - 1;
  if(code < 0) {
    code = 0;
  }
  if(code > 63) {
    code = 63;
  }
  _write_word(LTC4015_REG_IIN_LIMIT_SETTING, (unsigned short) code);
}

void LTC4015::set_input_voltage_min(float voltage_v)
{
  /* VIN_UVCL_SETTING: undervoltage control loop threshold. Left at the
   * chip's power-on default (lowest MPPT operating point) unless a
   * specific value is needed; implemented for completeness. The exact
   * VIN_UVCL_SETTING code-to-volts formula was not independently
   * re-verified in this project, so this function is intentionally not
   * wired into the boot sequence - call it explicitly if you need it,
   * after checking the code-to-volts formula against the datasheet. */
  (void) voltage_v;
}

/* ---------------------------------------------------------------------
 * JEITA hardware temperature safety
 * ------------------------------------------------------------------- */

uint16_t LTC4015::_ntc_ratio_from_celsius(float t_c)
{
  float t_k = t_c + 273.15f;
  float r = NTC_R25_OHM * expf(NTC_BETA * (1.0f / t_k - 1.0f / NTC_T0_K));
  float ratio = r / (r + NTC_RBIAS_OHM);
  int32_t code = (int32_t) lroundf(NTC_RATIO_FULL_SCALE * ratio);
  if(code < 0) {
    code = 0;
  }
  if(code > 21845) {
    code = 21845;
  }
  return (uint16_t) code;
}

float LTC4015::_celsius_from_ntc_ratio(uint16_t ratio)
{
  if(ratio == 0 || ratio >= (uint16_t) NTC_RATIO_FULL_SCALE) {
    return NAN;
  }
  float r = NTC_RBIAS_OHM * ((float) ratio / (NTC_RATIO_FULL_SCALE - (float) ratio));
  float inv_t = (1.0f / NTC_T0_K) + (1.0f / NTC_BETA) * logf(r / NTC_R25_OHM);
  return (1.0f / inv_t) - 273.15f;
}

void LTC4015::configure_jeita(const ltc4015_jeita_profile_t* profile, float icharge_warm_a)
{
  /* Region boundaries: since the NTC has a negative temperature
   * coefficient, JEITA_T1 (coldest breakpoint) has the HIGHEST register
   * code and JEITA_T6 (warmest) the LOWEST - the datasheet calls this
   * out explicitly, so this is not just "ascending by temperature". */
  _write_word(LTC4015_REG_JEITA_T1, _ntc_ratio_from_celsius(profile->t1_c));
  _write_word(LTC4015_REG_JEITA_T2, _ntc_ratio_from_celsius(profile->t2_c));
  _write_word(LTC4015_REG_JEITA_T3, _ntc_ratio_from_celsius(profile->t3_c));
  _write_word(LTC4015_REG_JEITA_T4, _ntc_ratio_from_celsius(profile->t4_c));
  _write_word(LTC4015_REG_JEITA_T5, _ntc_ratio_from_celsius(profile->t5_c));
  _write_word(LTC4015_REG_JEITA_T6, _ntc_ratio_from_celsius(profile->t6_c));

  /* Charge current for the "warm enough to charge normally" regions
   * (2..6). All five regions get the same conservative target - see the
   * ltc4015.h file header for why this driver doesn't try to taper
   * current per-region (residual uncertainty in the exact per-region
   * bit packing vs. a single, safely-bounded value for all regions).
   * Same milliohm-integer convention as set_charge_current() - see that
   * function's comment. */
  int32_t code = (int32_t) lroundf(icharge_warm_a * _rsnsb) - 1;
  if(code < 0) {
    code = 0;
  }
  if(code > 31) {
    code = 31;
  }
  uint16_t c = (uint16_t) code;

  uint16_t reg_6_5 = (uint16_t) ((c << icharge_jeita_6) | (c << icharge_jeita_5));
  uint16_t reg_4_3_2 =
      (uint16_t) ((c << icharge_jeita_4) | (c << icharge_jeita_3) | (c << icharge_jeita_2));

  _write_word(LTC4015_REG_ICHARGE_JEITA_6_5, reg_6_5);
  _write_word(LTC4015_REG_ICHARGE_JEITA_4_3_2, reg_4_3_2);

  /* VCHARGE_JEITA_* is deliberately left untouched here - see the
   * ltc4015.h file header ("NOT INDEPENDENTLY VERIFIED"). This project
   * uses a *fixed* CHEM pin voltage profile, which should make these
   * registers a don't-care on this hardware. */
}

void LTC4015::enable_jeita()
{
  _set_bit(LTC4015_REG_CHARGER_CONFIG_BITS, en_jeita);
}

void LTC4015::disable_jeita()
{
  _clr_bit(LTC4015_REG_CHARGER_CONFIG_BITS, en_jeita);
}

/* ---------------------------------------------------------------------
 * Charger control
 * ------------------------------------------------------------------- */

void LTC4015::suspend_charging()
{
  _set_bit(LTC4015_REG_CONFIG_BITS, suspend_charger);
}

void LTC4015::start_charging()
{
  _clr_bit(LTC4015_REG_CONFIG_BITS, suspend_charger);
}

void LTC4015::enable_mppt()
{
  _set_bit(LTC4015_REG_CONFIG_BITS, mppt_en_i2c);
}

void LTC4015::disable_mppt()
{
  _clr_bit(LTC4015_REG_CONFIG_BITS, mppt_en_i2c);
}

void LTC4015::enable_force_telemetry()
{
  _set_bit(LTC4015_REG_CONFIG_BITS, force_meas_sys_on);
}

void LTC4015::disable_force_telemetry()
{
  _clr_bit(LTC4015_REG_CONFIG_BITS, force_meas_sys_on);
}

void LTC4015::enable_coulomb_counter()
{
  _set_bit(LTC4015_REG_CONFIG_BITS, en_qcount);
}

void LTC4015::disable_coulomb_counter()
{
  _clr_bit(LTC4015_REG_CONFIG_BITS, en_qcount);
}

void LTC4015::config(unsigned short setting)
{
  _write_word(LTC4015_REG_CONFIG_BITS, setting);
}

void LTC4015::arm_ship_mode()
{
  _write_word(LTC4015_REG_ARM_SHIP_MODE, 0x534D);
}

/* ---------------------------------------------------------------------
 * Battery configuration readback
 * ------------------------------------------------------------------- */

void LTC4015::get_battery_info()
{
  unsigned short tmp = read_word(LTC4015_REG_CHEM_CELLS);
  unsigned char t_chem = (tmp >> 8) & 0x0F;
  _cell_count = tmp & 0x0F;

  if(t_chem < 4) {
    _chemistry = LI_ION;
  } else if(t_chem < 7) {
    _chemistry = LIFEPO4;
  } else if(t_chem < 9) {
    _chemistry = LEAD_ACID;
  } else {
    _chemistry = INVALID;
  }
}

bool LTC4015::verify_chemistry_and_cells(bat_chem expected_chem, unsigned char expected_cells)
{
  get_battery_info();
  bool ok = (_chemistry == expected_chem) && (_cell_count == expected_cells);
  if(!ok) {
    ESP_LOGE(_logtag,
             "Chemistry/cell mismatch: expected %s/%u cells, read %s/%u cells from CHEM_CELLS "
             "(check solder jumpers) - refusing to enable charging",
             bat_chem_str[expected_chem], expected_cells, bat_chem_str[_chemistry], _cell_count);
  }
  return ok;
}

/* ---------------------------------------------------------------------
 * Telemetry getters
 * ------------------------------------------------------------------- */

float LTC4015::get_input_voltage()
{
  return (float) ((signed short) read_word(LTC4015_REG_VIN) * 0.001648f);
}

float LTC4015::get_input_current()
{
  return (float) ((signed short) read_word(LTC4015_REG_IIN) * 0.00146487f / _rsnsi);
}

float LTC4015::get_system_voltage()
{
  return (float) ((signed short) read_word(LTC4015_REG_VSYS) * 0.001648f);
}

float LTC4015::get_battery_voltage()
{
  short v_bat_cell = (signed short) read_word(LTC4015_REG_VBAT);
  float v_bat;
  if(_chemistry == LEAD_ACID) {
    v_bat = v_bat_cell * 0.000128176f;
  } else {
    v_bat = v_bat_cell * 0.0001922f;
  }
  return v_bat * _cell_count;
}

float LTC4015::get_charge_current()
{
  return (float) ((signed short) read_word(LTC4015_REG_IBAT) * 0.00146487f / _rsnsb);
}

float LTC4015::get_battery_voltage_filtered()
{
  /* Same per-cell LSB as VBAT (0x3A) - VBAT_FILT (0x47) is the filtered
   * version of the same ADC channel, same units. */
  short v_bat_cell = (signed short) read_word(LTC4015_REG_VBAT_FILT);
  float v_bat;
  if(_chemistry == LEAD_ACID) {
    v_bat = v_bat_cell * 0.000128176f;
  } else {
    v_bat = v_bat_cell * 0.0001922f;
  }
  return v_bat * _cell_count;
}

float LTC4015::get_icharge_dac_current()
{
  /* ICHARGE_DAC (0x44) mirrors the live DAC code driving the charge
   * current servo loop right now - same 5-bit encoding/formula as
   * ICHARGE_TARGET (verified: current = (code + 1) * 1mV / Rsnsb_ohms,
   * and _rsnsb is the milliohm-integer form of Rsnsb_ohms - see
   * set_charge_current()). Under JEITA this can differ from what was
   * commanded if the IC is throttling for temperature. */
  uint16_t code = read_word(LTC4015_REG_ICHARGE_DAC) & 0x1F;
  return (float) (code + 1) / (float) _rsnsb;
}

float LTC4015::get_iin_limit_dac_current()
{
  /* IIN_LIMIT_DAC (0x46) mirrors the live input current limit DAC code -
   * same formula as IIN_LIMIT_SETTING (verified: current = (code + 1) *
   * 500uV / Rsnsi_ohms, reducing to (code+1)/(2*_rsnsi) with the
   * milliohm-integer convention - see set_input_current_max()). */
  uint16_t code = read_word(LTC4015_REG_IIN_LIMIT_DAC) & 0x3F;
  return (float) (code + 1) / (2.0f * (float) _rsnsi);
}

void LTC4015::trigger_bsr_measurement()
{
  _set_bit(LTC4015_REG_CONFIG_BITS, run_bsr);
}

uint16_t LTC4015::get_bsr_raw()
{
  return read_word(LTC4015_REG_BSR);
}

float LTC4015::get_die_temp()
{
  short rawdie = (signed short) read_word(LTC4015_REG_DIE_TEMP);
  return (float) ((rawdie - 12010) / 45.6f);
}

float LTC4015::get_ntc_temperature()
{
  if(!is_meas_sys_valid()) {
    return NAN;
  }
  uint16_t ratio = read_word(LTC4015_REG_NTC_RATIO);
  return _celsius_from_ntc_ratio(ratio);
}

bool LTC4015::is_meas_sys_valid()
{
  uint16_t sys_status = read_word(LTC4015_REG_SYSTEM_STATUS);
  /* meas_sys_valid lives in LIMIT_ALERTS (bit 15) per the register map,
   * but that register only latches when the alert is enabled. The
   * measurement subsystem being active/valid is more directly implied
   * by drvcc_good plus a non-degenerate NTC_RATIO reading, so check
   * both drvcc_good and that NTC_RATIO isn't stuck at 0 or full scale. */
  if(!(sys_status & drvcc_good)) {
    return false;
  }
  uint16_t ratio = read_word(LTC4015_REG_NTC_RATIO);
  return ratio > 0 && ratio < (uint16_t) NTC_RATIO_FULL_SCALE;
}

uint16_t LTC4015::get_qcount()
{
  return read_word(LTC4015_REG_QCOUNT);
}

uint16_t LTC4015::get_charger_state()
{
  return read_word(LTC4015_REG_CHARGER_STATE);
}

uint16_t LTC4015::get_charge_status()
{
  return read_word(LTC4015_REG_CHARGE_STATUS);
}

uint16_t LTC4015::get_system_status()
{
  return read_word(LTC4015_REG_SYSTEM_STATUS);
}

/* ---------------------------------------------------------------------
 * Low-level I2C access
 * ------------------------------------------------------------------- */

void LTC4015::_write_word(unsigned char sub_address, unsigned short word)
{
  Wire.beginTransmission(WFEELS_ADDR_LTC4015 >> 1);
  Wire.write(sub_address);
  Wire.write(word & 0xFF);
  Wire.write((word >> 8) & 0xFF);
  Wire.endTransmission();
}

unsigned short LTC4015::read_word(unsigned char sub_address)
{
  unsigned short word = 0;
  Wire.beginTransmission(WFEELS_ADDR_LTC4015 >> 1);
  Wire.write(sub_address);
  Wire.endTransmission();

  Wire.requestFrom(WFEELS_ADDR_LTC4015 >> 1, 2);
  word = (Wire.read() | (Wire.read() << 8));
  return word;
}

void LTC4015::_clr_bit(unsigned char sub_address, unsigned short new_word)
{
  unsigned short old_word = read_word(sub_address);
  old_word &= ~new_word;
  _write_word(sub_address, old_word);
}

void LTC4015::_set_bit(unsigned char sub_address, unsigned short new_word)
{
  unsigned short old_word = read_word(sub_address);
  old_word |= new_word;
  _write_word(sub_address, old_word);
}
