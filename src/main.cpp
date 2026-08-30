#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "config.h"
#include "SendOwnInfo.h"
#include "AXP2101.h"
#include "U-blox-helper.h"
#include "NavigationMath.h"
#include <math.h>

double d = 0.0;
double b = 0.0;

GpsInfo companion;
bool   haveCompanionFix = false;

// GPS UART
HardwareSerial GPSSerial(1);

// --------------------
// LoRa (SX1262)
// --------------------
SX1262 radio = SX1262(
  new Module(
    LORA_NSS,
    LORA_DIO1,
    LORA_RST,
    LORA_BUSY
  )
);

int transmissionState = RADIOLIB_ERR_NONE;
bool transmitFlag = false;
volatile bool operationDone = false;

//#define INITIATING_NODE

// --------------------
// ✅ Recovery tuning
// --------------------
static constexpr uint32_t RADIO_STALL_TIMEOUT_MS = 4000;   // if no IRQ for this long -> recover
static constexpr uint8_t  RADIO_MAX_STALLS_BEFORE_REINIT = 3;

static uint32_t lastIrqMs = 0;
static uint8_t  stallCount = 0;

void setFlag(void) {
  operationDone = true;
  lastIrqMs = millis();
}

static void radioRecoverStartReceive() {
  // best-effort: stop any ongoing op and restart RX
  radio.standby();
  delay(5);
  radio.startReceive();
  transmitFlag = false;
}

static void radioHardReinit() {
  Serial.println("⚠️ Radio hard re-init...");
  radio.standby();
  delay(10);

  int state = radio.begin(LORA_FREQ);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("❌ radio.begin() failed, code = ");
    Serial.println(state);
    return;
  }

  Serial.println("✅ Radio re-init OK");
  radio.setDio1Action(setFlag);
  radio.startReceive();
  transmitFlag = false;
}

void setup() {
  Serial.begin(115200);

  // Power GPS via AXP2101
  AXP2101_beginAndEnableGPSPower();

  // GPS UART
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  delay(200);

  UbloxHelper_begin(GPSSerial);

  bool ok = UbloxHelper_configureUbxOnlyNavPvt();
  if (!ok) {
    Serial.println("⚠️ u-blox config: NAV-PVT enable did not ACK (continuing anyway).");
  }

  // LoRa init
  Serial.println("SX126x PingPong starting...");
  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, SPI_SS_PIN);

  int state = radio.begin(LORA_FREQ);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("radio.begin() failed, code = ");
    Serial.println(state);
    while (true) { delay(1000); }
  }
  Serial.println("✅ Radio init OK");

// ✅ LoRa link settings (MUST match on both devices)
radio.setSpreadingFactor(10);     // or 11 (more range, slower)
radio.setBandwidth(125.0);        // kHz
radio.setCodingRate(7);           // 4/7
radio.setOutputPower(14);         // dBm (⚠️ check legal limits)

Serial.println("✅ Radio init OK");


  SendOwnInfo_begin(GPSSerial);

  radio.setDio1Action(setFlag);

  lastIrqMs = millis();

#if defined(INITIATING_NODE)
  Serial.print(F("[SX1262] Sending first packet ... "));
  transmissionState = radio.startTransmit("start transmitting");
  transmitFlag = true;
#else
  Serial.print(F("[SX1262] Starting to listen ... "));
  state = radio.startReceive();
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println(F("success!"));
  } else {
    Serial.print(F("failed, code "));
    Serial.println(state);
    while (true) { delay(10); }
  }
#endif
}

void loop() {
  // ✅ Always keep parsing GPS in the background (important!)
  // This prevents GPS UART buffers from overflowing and keeps your own fix fresh.
  // (Your SendOwnInfo parser reads from GPSSerial when you transmit, but doing it
  // continuously is safer.)
  // If you want, you can expose a SendOwnInfo_poll() later; for now we keep it simple.

  // ✅ RADIO STALL WATCHDOG:
  // If no IRQ happens for too long, force RX restart / reinit.
  uint32_t now = millis();
  if (!operationDone && (now - lastIrqMs) > RADIO_STALL_TIMEOUT_MS) {
    stallCount++;
    Serial.print("⚠️ Radio stall detected. Recovering RX. stallCount=");
    Serial.println(stallCount);

    radioRecoverStartReceive();
    lastIrqMs = now;

    if (stallCount >= RADIO_MAX_STALLS_BEFORE_REINIT) {
      stallCount = 0;
      radioHardReinit();
      lastIrqMs = millis();
    }
  }

  if (!operationDone) {
    return; // nothing to do until IRQ or watchdog triggers
  }

  // we have an IRQ event
  operationDone = false;
  stallCount = 0;

  if (transmitFlag) {
    // previous operation was transmission
    if (transmissionState != RADIOLIB_ERR_NONE) {
      Serial.print(F("TX failed, code "));
      Serial.println(transmissionState);
    }

    // listen for response
    radio.startReceive();
    transmitFlag = false;

  } else {
    // previous operation was reception
    String str;
    int state = radio.readData(str);

    if (state == RADIOLIB_ERR_NONE) {
      if (UbloxHelper_parseGpsPayload(str, companion)) {
        haveCompanionFix = true;
      } else {
        Serial.println("❌ Failed to parse companion payload");
      }
    } else {
      // If readData fails, restart RX so we don't get stuck
      Serial.print("⚠️ readData failed, code ");
      Serial.println(state);
      radioRecoverStartReceive();
    }

    delay(1000);

    // send own info
    GpsInfo own = prepareAndSendOwnInfo(radio, transmissionState, transmitFlag);

    if (haveCompanionFix && companion.hasData) {
      d = distanceMeters(own.lat, own.lon, companion.lat, companion.lon);
      b = bearingDegrees(own.lat, own.lon, companion.lat, companion.lon);

      Serial.print(d, 6);
      Serial.print(", ");
      Serial.print(b, 6);
      Serial.print(", ");
      Serial.print(own.fixType);
      Serial.print(", ");
      Serial.print(companion.fixType);
      Serial.print(", ");
      Serial.print(own.valid ? "true" : "false");
      Serial.print(", ");
      Serial.println(companion.valid ? "true" : "false");
    } else {
      Serial.println("Companion: (no data yet)");
    }
  }
}