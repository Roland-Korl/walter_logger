#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "ds18b20.h"

static OneWire* _oneWire = nullptr;
static DallasTemperature* _sensors = nullptr;

void DS18B20Sensor::begin(uint8_t pin)
{
  _oneWire = new OneWire(pin);
  _sensors = new DallasTemperature(_oneWire);
  _sensors->begin();
  _sensors->setWaitForConversion(true);
  _initialized = (_sensors->getDeviceCount() > 0);

  if(!_initialized) {
    ESP_LOGW("[DS18B20]", "No DS18B20 found on pin %u", pin);
  }
}

bool DS18B20Sensor::read(float* temperature_c)
{
  if(!_initialized || _sensors == nullptr) {
    return false;
  }

  _sensors->requestTemperatures();
  float t = _sensors->getTempCByIndex(0);

  /* DEVICE_DISCONNECTED_C is -127 in DallasTemperature; 85.0 is the
   * DS18B20's power-on reset value returned when a conversion never
   * actually completed (e.g. bus timing issue) - both indicate an
   * invalid reading, not a real temperature. */
  if(t == DEVICE_DISCONNECTED_C || t == 85.0f) {
    return false;
  }

  *temperature_c = t;
  return true;
}
