#include <Wire.h>
#include "SparkFun_SCD30_Arduino_Library.h"
#include "i2cSensor.h"

namespace {
SCD30 co2Sensor;
}

bool beginI2CSensor() {
  Wire.begin();
  return co2Sensor.begin();
}

bool readI2CSensorCO2(uint16_t &co2Value) {
  if (!co2Sensor.readMeasurement()) {
    return false;
  }

  co2Value = co2Sensor.getCO2();
  return true;
}