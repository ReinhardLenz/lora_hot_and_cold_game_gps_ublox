#include "potentiometer.h"
#include <FastLED.h>

// -------------------- Hardware configuration --------------------
static constexpr int      ADC_MAX    = 4095;  // ESP32 12-bit ADC (0..4095)

void InitPotentiometerLedMapper() {
  delay(200);
  analogReadResolution(12);                   // 0..4095
  analogSetPinAttenuation(36, ADC_11db);      // ~0..3.3V range (pin is fixed on your board)
  Serial.println("ESP32 ADC->45-LED ring mapper started.");
}

int UpdatePotentiometerCorrectionDegrees(uint8_t adcPin) {
  const int raw = analogRead(adcPin); // 0..4095
  const int cdeg = (raw * 36000 + (ADC_MAX / 2)) / ADC_MAX; // 0..36000
  const int degrees =int(cdeg / 100.0f);
  return degrees;
}