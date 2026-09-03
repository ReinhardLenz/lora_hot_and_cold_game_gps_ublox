#ifndef LED_H
#define LED_H

#include <Arduino.h>
#include <FastLED.h>

class LedRing
{
public:
  LedRing(uint16_t ledCount, uint8_t dataPin);
  
  void begin(uint8_t brightness);

  // Show a single "direction" pixel based on yaw in degrees (0..360)
  void showDirection(float yawNorthDeg, const CRGB& color = CRGB::White);

  // Optional helpers
  void clear();
  void setBrightness(uint8_t brightness);

private:
  uint16_t m_ledCount;
  uint8_t  m_dataPin;

  CRGB* m_leds = nullptr;

  uint16_t yawToIndex(float yawNorthDeg) const;
};

#endif