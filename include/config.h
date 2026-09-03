#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// -------------------------------
// UART GPS PIN
// -------------------------------
static constexpr int GPS_RX_PIN = 34;     // GPS TX -> ESP32 RX
static constexpr int GPS_TX_PIN = 12;     // GPS RX -> ESP32 TX
static constexpr uint32_t GPS_BAUD = 9600;

// --------------------
// LoRa (SX1262) pins (T-Beam)
// --------------------
static constexpr int LORA_NSS  = 18;      // CS
static constexpr int LORA_DIO1 = 33;      // IRQ
static constexpr int LORA_RST  = 23;      // RESET
static constexpr int LORA_BUSY = 32;      // BUSY

static constexpr float LORA_FREQ = 868.0; // must match receiver

// --------------------
// SPI bus pins (ESP32 -> SX1262 on T-Beam V1.2)
// SPI.begin(SCK, MISO, MOSI, SS)
// --------------------
static constexpr int SPI_SCK_PIN  = 5;
static constexpr int SPI_MISO_PIN = 19;
static constexpr int SPI_MOSI_PIN = 27;
// You can reuse LORA_NSS as the SPI "SS" default pin:
static constexpr int SPI_SS_PIN   = LORA_NSS;



// -------------------------------
// UART
// -------------------------------
constexpr uint32_t PC_BAUD  = 115200;
constexpr uint32_t BNO_BAUD = 3000000;

// -------------------------------
// ESP32 GPIO (UART to BNO085)
// -------------------------------
constexpr gpio_num_t PIN_BNO_TX = GPIO_NUM_14;
constexpr gpio_num_t PIN_BNO_RX = GPIO_NUM_15;

// -------------------------------
// BNO reset pin (⚠️ must NOT conflict with LED ring pin)
// -------------------------------
constexpr gpio_num_t PIN_BNO_RESET = GPIO_NUM_12;   // <-- adjust to your wiring

constexpr uint32_t RESET_TIME_MS = 10;

// -------------------------------
// WS2812 LED ring
// -------------------------------
constexpr gpio_num_t PIN_LED_RING = GPIO_NUM_13;    // <-- your LED data pin
constexpr uint16_t   LED_COUNT    = 45;
constexpr uint8_t    LED_BRIGHTNESS = 50;

// -------------------------------
// Status LED pin for fatal error blinking
// -------------------------------
constexpr gpio_num_t PIN_STATUS_LED = GPIO_NUM_4;   // <-- adjust to your board/wiring

constexpr uint32_t BLINK_DELAY   = 200;

#endif
