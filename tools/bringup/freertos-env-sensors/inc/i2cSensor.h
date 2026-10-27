#pragma once

#include <cstdint>

bool beginI2CSensor();
bool readI2CSensorCO2(uint16_t &co2Value);