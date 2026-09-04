/**
 * @file ds18b20.h
 * @brief Thin wrapper around a DS18B20 battery temperature sensor.
 *
 * This is the INDEPENDENT cross-check sensor from the handover doc - the
 * PRIMARY, always-on safety mechanism is the LTC4015's own hardware JEITA
 * cutoff using its onboard NTC (see ltc4015.h/.cpp). The DS18B20 is a
 * second, physically separate sensor used to sanity-check the NTC reading
 * and to have an independent data source in the telemetry log.
 *
 * Requires the "OneWire" (Jim Studt / Paul Stoffregen) and
 * "DallasTemperature" (Miles Burton) Arduino libraries - install both via
 * Library Manager before building.
 *
 * Wiring: DATA -> DS18B20_PIN (config.h) with a 4.7k pull-up to 3V3,
 * VDD -> 3V3 (switched via WalterFeels::set3v3()), GND -> GND.
 */

#ifndef DS18B20_H
#define DS18B20_H

#include <stdint.h>

class DS18B20Sensor
{
public:
  /**
   * @brief Initialize the 1-Wire bus on the given pin.
   */
  static void begin(uint8_t pin);

  /**
   * @brief Trigger a conversion and read back the temperature.
   *
   * @param[out] temperature_c Set to the read temperature on success.
   * @return true if a sensor answered and returned a plausible reading,
   * false if no sensor was found or the reading looks invalid (e.g. the
   * DS18B20 power-on/disconnected sentinel value of 85C, or -127C for a
   * CRC/communication failure).
   */
  static bool read(float* temperature_c);

private:
  static inline bool _initialized = false;
};

#endif
