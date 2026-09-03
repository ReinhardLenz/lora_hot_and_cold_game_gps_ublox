#include "LED.h"
#include "config.h"

LedRing::LedRing(uint16_t ledCount, uint8_t dataPin)
: m_ledCount(ledCount),
  m_dataPin(dataPin)
{
}

void LedRing::begin(uint8_t brightness)
{
  if (m_leds == nullptr)
  {
    m_leds = new CRGB[m_ledCount];
  }

  // Use compile-time pin from config.h (recommended with FastLED)
  FastLED.addLeds<WS2812B, PIN_LED_RING, GRB>(m_leds, m_ledCount);
  FastLED.setBrightness(brightness);

  fill_solid(m_leds, m_ledCount, CRGB::Black);
  FastLED.show();
}

void LedRing::setBrightness(uint8_t brightness)
{
  FastLED.setBrightness(brightness);
}

void LedRing::clear()
{
  if (!m_leds) return;
  fill_solid(m_leds, m_ledCount, CRGB::Black);
  FastLED.show();
}

uint16_t LedRing::yawToIndex(float yawNorthDeg) const
{
  while (yawNorthDeg < 0.0f)    yawNorthDeg += 360.0f;
  while (yawNorthDeg >= 360.0f) yawNorthDeg -= 360.0f;

  return static_cast<uint16_t>((yawNorthDeg * m_ledCount) / 360.0f) % m_ledCount;
}

void LedRing::showDirection(float yawNorthDeg, const CRGB& color)
{
  if (!m_leds) return;

  const uint16_t idx = yawToIndex(yawNorthDeg);

  fill_solid(m_leds, m_ledCount, CRGB::Black);
  m_leds[idx] = color;

  FastLED.show();
}