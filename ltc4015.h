/**
 * @file ltc4015.h
 * @brief Driver for the Analog Devices LTC4015 battery charger/telemetry IC
 * on the Walter Feels carrier board.
 *
 * This is based on DPTechnics' official example driver
 * (examples/walter_feels/ltc4015.h in the walter-arduino repository), with
 * the following changes for this solar/LiFePO4 test application:
 *
 *   1. FIX: the original set_input_current_max() wrote to the wrong
 *      register (ICHARGE_TARGET instead of IIN_LIMIT_SETTING). Fixed here.
 *   2. FIX: set_input_voltage_min() was an empty stub. Implemented via
 *      VIN_UVCL_SETTING.
 *   3. ADD: JEITA temperature-region configuration. This is the hardware
 *      safety mechanism that blocks charging below/above configured
 *      temperatures directly inside the LTC4015 - it keeps working even
 *      if the ESP32 crashes, hangs, or is in deep sleep. See config.h for
 *      the threshold values and their derivation.
 *   4. ADD: chemistry/cell-count readback verification, used as a
 *      boot-time safety interlock (see ltc4015SafeInit() in the .ino).
 *   5. ADD: structured getters for charger/status registers instead of
 *      only ESP_LOG print helpers, so the telemetry module can log the
 *      raw values as well as decode them.
 *
 * IMPORTANT - what is and isn't independently verified against the real
 * ADI datasheet in this project (no local copy of the LTC4015 datasheet
 * was available while writing this; the facts below were cross-checked
 * against ADI documentation excerpts, but you should re-verify against
 * the full PDF before relying on this in the field):
 *
 *  - VERIFIED: JEITA region 1 (below JEITA_T1) forces the charger
 *    completely off in hardware ("CHARGER OFF"). This is the property
 *    the whole cold-charging safety design leans on.
 *  - VERIFIED: NTC_RATIO = round(21845 * Rntc / (Rntc + Rntcbias)), and
 *    Rntcbias is internal to the LTC4015 (no external bias resistor on
 *    Walter Feels - confirmed against the board's BOM/schematic, which
 *    has only the single 10k/beta=3380 NTC thermistor, R12) with a
 *    nominal value equal to the recommended thermistor value (10k).
 *  - VERIFIED: ICHARGE_TARGET is a 5-bit register (bits 4:0), formula
 *    current = (code + 1) * 1mV / RSNSB. This matches the vendor driver's
 *    existing implementation.
 *  - VERIFIED: for lithium chemistries with EN_JEITA=1, charge CURRENT is
 *    taken from ICHARGE_JEITA_6_5 / ICHARGE_JEITA_4_3_2 instead of
 *    ICHARGE_TARGET. Those two registers pack five-bit fields at bit
 *    offsets 0/5(/10) - taken directly from the vendor's own driver
 *    constants, which matches the 5-bit width of ICHARGE_TARGET.
 *  - NOT INDEPENDENTLY VERIFIED: the exact behaviour of VCHARGE_JEITA_*
 *    under a *fixed* chemistry profile (as opposed to programmable). This
 *    project uses "LiFePO4 fixed fast charge" (CHEM pins), where charge
 *    voltage is selected by the CHEM pins in hardware - so this driver
 *    deliberately does NOT touch VCHARGE_JEITA_* at all. If you switch to
 *    a *programmable* chemistry setting later, verify the VCHARGE_JEITA_*
 *    bit packing against the real datasheet before trusting it.
 *
 * @copyright Original driver (C) 2023, DPTechnics bv - see LICENSE section
 * in the upstream file. Modifications for this project follow the same
 * terms.
 */

#ifndef _LTC4015_H_
#define _LTC4015_H_

#include <inttypes.h>

/* The I2C address of the LTC4015 on Walter Feels. */
#define WFEELS_ADDR_LTC4015 0xD0

/* --- LTC4015 register map --- */
#define LTC4015_REG_VBAT_LO_ALERT_LIMIT 0x01
#define LTC4015_REG_VBAT_HI_ALERT_LIMIT 0x02
#define LTC4015_REG_VIN_LO_ALERT_LIMIT 0x03
#define LTC4015_REG_VIN_HI_ALERT_LIMIT 0x04
#define LTC4015_REG_VSYS_LO_ALERT_LIMIT 0x05
#define LTC4015_REG_VSYS_HI_ALERT_LIMIT 0x06
#define LTC4015_REG_IIN_HI_ALERT_LIMIT 0x07
#define LTC4015_REG_IBAT_LO_ALERT_LIMIT 0x08
#define LTC4015_REG_DIE_TEMP_HI_ALERT_LIMIT 0x09
#define LTC4015_REG_BSR_HI_ALERT_LIMIT 0x0A
#define LTC4015_REG_NTC_RATIO_HI_ALERT_LIMIT 0x0B
#define LTC4015_REG_NTC_RATIO_LO_ALERT_LIMIT 0x0C
#define LTC4015_REG_EN_LIMIT_ALERTS 0x0D
#define LTC4015_REG_EN_CHARGER_STATE_ALERTS 0x0E
#define LTC4015_REG_EN_CHARGE_STATUS_ALERTS 0x0F
#define LTC4015_REG_QCOUNT_LO_ALERT_LIMIT 0x10
#define LTC4015_REG_QCOUNT_HI_ALERT_LIMIT 0x11
#define LTC4015_REG_QCOUNT_PRESCALE_FACTOR 0x12
#define LTC4015_REG_QCOUNT 0x13
#define LTC4015_REG_CONFIG_BITS 0x14
#define LTC4015_REG_IIN_LIMIT_SETTING 0x15
#define LTC4015_REG_VIN_UVCL_SETTING 0x16
#define LTC4015_REG_ARM_SHIP_MODE 0x19
#define LTC4015_REG_ICHARGE_TARGET 0x1A
#define LTC4015_REG_VCHARGE_SETTING 0x1B
#define LTC4015_REG_C_OVER_X_THRESHOLD 0x1C
#define LTC4015_REG_MAX_CV_TIME 0x1D
#define LTC4015_REG_MAX_CHARGE_TIME 0x1E
#define LTC4015_REG_JEITA_T1 0x1F
#define LTC4015_REG_JEITA_T2 0x20
#define LTC4015_REG_JEITA_T3 0x21
#define LTC4015_REG_JEITA_T4 0x22
#define LTC4015_REG_JEITA_T5 0x23
#define LTC4015_REG_JEITA_T6 0x24
#define LTC4015_REG_VCHARGE_JEITA_6_5 0x25
#define LTC4015_REG_VCHARGE_JEITA_4_3_2 0x26
#define LTC4015_REG_ICHARGE_JEITA_6_5 0x27
#define LTC4015_REG_ICHARGE_JEITA_4_3_2 0x28
#define LTC4015_REG_CHARGER_CONFIG_BITS 0x29
#define LTC4015_REG_VABSORB_DELTA 0x2A
#define LTC4015_REG_MAX_ABSORB_TIME 0x2B
#define LTC4015_REG_VEQUALIZE_DELTA 0x2C
#define LTC4015_REG_EQUALIZE_TIME 0x2D
#define LTC4015_REG_LIFEP04_RECHARGE_THRESHOLD 0x2E
#define LTC4015_REG_MAX_CHARGE_TIMER 0x30
#define LTC4015_REG_CV_TIMER 0x31
#define LTC4015_REG_ABSORB_TIMER 0x32
#define LTC4015_REG_EQUALIZE_TIMER 0x33
#define LTC4015_REG_CHARGER_STATE 0x34
#define LTC4015_REG_CHARGE_STATUS 0x35
#define LTC4015_REG_LIMIT_ALERTS 0x36
#define LTC4015_REG_CHARGER_STATE_ALERTS 0x37
#define LTC4015_REG_CHARGE_STATUS_ALERTS 0x38
#define LTC4015_REG_SYSTEM_STATUS 0x39
#define LTC4015_REG_VBAT 0x3A
#define LTC4015_REG_VIN 0x3B
#define LTC4015_REG_VSYS 0x3C
#define LTC4015_REG_IBAT 0x3D
#define LTC4015_REG_IIN 0x3E
#define LTC4015_REG_DIE_TEMP 0x3F
#define LTC4015_REG_NTC_RATIO 0x40
#define LTC4015_REG_BSR 0x41
#define LTC4015_REG_JEITA_REGION 0x42
#define LTC4015_REG_CHEM_CELLS 0x43
#define LTC4015_REG_ICHARGE_DAC 0x44
#define LTC4015_REG_VCHARGE_DAC 0x45
#define LTC4015_REG_IIN_LIMIT_DAC 0x46
#define LTC4015_REG_VBAT_FILT 0x47
#define LTC4015_REG_ICHARGE_BSR 0x48
#define LTC4015_REG_MEAS_SYS_VALID 0x4A

/* Charger control bits (CONFIG_BITS, 0x14) */
#define suspend_charger (1 << 8)
#define run_bsr (1 << 5)
#define force_meas_sys_on (1 << 4)
#define mppt_en_i2c (1 << 3)
#define en_qcount (1 << 2)

/* JEITA current/voltage sub-register bit offsets (5-bit fields), from the
 * vendor driver - see file header note on verification status. */
#define vcharge_jeita_6 5
#define vcharge_jeita_5 0
#define vcharge_jeita_4 10
#define vcharge_jeita_3 5
#define vcharge_jeita_2 0
#define icharge_jeita_6 5
#define icharge_jeita_5 0
#define icharge_jeita_4 10
#define icharge_jeita_3 5
#define icharge_jeita_2 0

/* Charger configuration bits (CHARGER_CONFIG_BITS, 0x29) */
#define en_c_over_x_term (1 << 2)
#define en_lead_acid_temp_comp (1 << 1)
#define en_jeita (1 << 0)

/* Charger state bits (CHARGER_STATE, 0x34) */
#define equalize_charge (1 << 10)
#define absorb_charge (1 << 9)
#define charger_suspended (1 << 8)
#define precharge (1 << 7)
#define cc_cv_charge (1 << 6)
#define ntc_pause (1 << 5)
#define timer_term (1 << 4)
#define c_over_x_term (1 << 3)
#define max_charge_time_fault (1 << 2)
#define bat_missing_fault (1 << 1)
#define bat_short_fault (1 << 0)

/* Charge status bits (CHARGE_STATUS, 0x35) */
#define vin_uvcl_active (1 << 3)
#define iin_limit_active (1 << 2)
#define constant_current (1 << 1)
#define constant_voltage (1 << 0)

/* System status bits (SYSTEM_STATUS, 0x39) */
#define charger_enabled (1 << 13)
#define mppt_en_pin (1 << 11)
#define equalize_req (1 << 10)
#define drvcc_good (1 << 9)
#define cell_count_error (1 << 8)
#define ok_to_charge (1 << 6)
#define no_rt (1 << 5)
#define thermal_shutdown (1 << 4)
#define vin_ovlo (1 << 3)
#define vin_gt_vbat (1 << 2)
#define intvcc_gt_4p3v (1 << 1)
#define intvcc_gt_2p8v (1 << 0)

enum bat_chem { LI_ION, LIFEPO4, LEAD_ACID, INVALID };

/**
 * @brief One breakpoint of the JEITA temperature profile, expressed in
 * degrees Celsius. configure_jeita() converts these to NTC_RATIO register
 * codes using the board's actual NTC (10k, beta 3380K, per the Walter
 * Feels BOM) at the point of use.
 */
typedef struct {
  float t1_c; /* charging forced OFF below this temperature   */
  float t2_c;
  float t3_c;
  float t4_c;
  float t5_c;
  float t6_c; /* charging forced OFF above this temperature   */
} ltc4015_jeita_profile_t;

class LTC4015
{
public:
  static void initialize(unsigned char _Rsnsi, unsigned char _Rsnsb);

  static void printinfo();

  /* --- charger current/voltage/limits --- */
  static void set_input_current_max(float current_a);
  static void set_input_voltage_min(float voltage_v);
  static void set_charge_current(float current_a);
  static void set_charge_voltage(float voltage_v);

  /* --- JEITA hardware temperature safety --- */
  static void configure_jeita(const ltc4015_jeita_profile_t* profile, float icharge_warm_a);
  static void enable_jeita();
  static void disable_jeita();

  /* --- charger control --- */
  static void suspend_charging();
  static void start_charging();
  static void enable_mppt();
  static void disable_mppt();
  static void enable_force_telemetry();
  static void disable_force_telemetry();
  static void enable_coulomb_counter();
  static void disable_coulomb_counter();
  static void config(unsigned short setting);
  static void arm_ship_mode();

  /* --- battery configuration readback (boot-time safety interlock) --- */
  static void get_battery_info();
  static bool verify_chemistry_and_cells(bat_chem expected_chem, unsigned char expected_cells);

  /* --- telemetry getters (physical units) --- */
  static float get_input_voltage();
  static float get_input_current();
  static float get_system_voltage();
  static float get_battery_voltage();
  static float get_battery_voltage_filtered(); /* less noisy than get_battery_voltage() */
  static float get_charge_current();
  static float get_die_temp();
  static float get_ntc_temperature();  /* NAN if measurement invalid    */
  static uint16_t get_qcount();
  static bool is_meas_sys_valid();

  /* --- live DAC readback: what the charger is ACTUALLY doing right now,
   * as opposed to what was commanded via set_charge_current() etc. Under
   * JEITA control the IC can throttle these on its own (e.g. reduced
   * current in a cooler region), so this is the ground truth. --- */
  static float get_icharge_dac_current();  /* actual live charge current, A */
  static float get_iin_limit_dac_current(); /* actual live input current limit, A */

  /* --- battery series resistance (internal resistance) ---
   * Triggers a one-shot measurement and returns the RAW register value.
   * NOT converted to ohms here - the LTC4015_REG_BSR-to-ohms formula was
   * not independently verified against the full datasheet in this
   * project (see ltc4015.h file header). The raw value is still useful:
   * it trends with actual internal resistance (rises in the cold for
   * LiFePO4), which is exactly what the winter test wants to see, even
   * before it's been calibrated to real ohms. */
  static void trigger_bsr_measurement();
  static uint16_t get_bsr_raw();

  /* --- telemetry getters (raw status/state registers, for logging) --- */
  static uint16_t get_charger_state();
  static uint16_t get_charge_status();
  static uint16_t get_system_status();

  static unsigned short read_word(uint8_t sub_address);

private:
  static inline bat_chem _chemistry;
  static inline unsigned char _cell_count;
  static inline unsigned char _rsnsi;
  static inline unsigned char _rsnsb;

  static void _write_word(unsigned char sub_address, unsigned short word);
  static void _clr_bit(unsigned char sub_address, unsigned short new_word);
  static void _set_bit(unsigned char sub_address, unsigned short new_word);

  /* Converts a temperature in Celsius to the NTC_RATIO code the board's
   * actual NTC (10k, beta 3380K) would report at that temperature. Used
   * both for JEITA_Tn thresholds and for decoding the live NTC_RATIO
   * register back into a temperature for telemetry. */
  static uint16_t _ntc_ratio_from_celsius(float t_c);
  static float _celsius_from_ntc_ratio(uint16_t ratio);
};

#endif
