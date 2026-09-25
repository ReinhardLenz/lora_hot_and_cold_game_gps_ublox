#pragma once

#include <Arduino.h>

// Initializes Serial, FastLED, and ADC settings
void InitPotentiometerLedMapper();

// Reads ADC pin, updates LED ring position, prints degrees, returns degrees (0.00..360.00)
int UpdatePotentiometerCorrectionDegrees(uint8_t adcPin);