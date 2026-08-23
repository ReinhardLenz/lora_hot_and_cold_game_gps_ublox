#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "config.h"          // 
#include "SendOwnInfo.h"
#include "AXP2101.h"
#include "U-blox-helper.h"
#include "NavigationMath.h"  // 
#include <math.h>

double d = 0.0;
double b = 0.0;

GpsInfo companion;                 // last parsed companion data (persists)
bool   haveCompanionFix = false;   // indicates we have valid parsed data at least once

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

// flag to indicate transmission or reception state
bool transmitFlag = false;
// flag to indicate that a packet was sent or received
volatile bool operationDone = false;

//#define INITIATING_NODE

void setFlag(void) {
  operationDone = true;
}

// Parse received payload into the unified struct
bool parseGpsPayload(const String& str, GpsInfo& in) {
  char buf[128];
  str.toCharArray(buf, sizeof(buf));

  char validStr[6] = {0}; // "true" or "false" (+ null)

  int n = sscanf(
    buf,
    "LAT=%lf LON=%lf valid=%5s fixType=%hhu",
    &in.lat,
    &in.lon,
    validStr,
    &in.fixType
  );

  if (n != 4) {
    return false;
  }

  in.hasData = true;
  in.valid = (strcmp(validStr, "true") == 0);
  return true;
}

void setup() {
  Serial.begin(115200);

  // Power GPS via AXP2101
  AXP2101_beginAndEnableGPSPower();

  // GPS UART (✅ uses config.h: GPS_BAUD, GPS_RX_PIN, GPS_TX_PIN)
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  delay(200);

  // Provide GPS serial to u-blox helper module and configure receiver output
  UbloxHelper_begin(GPSSerial);

  bool ok = UbloxHelper_configureUbxOnlyNavPvt();
  if (!ok) {
    Serial.println("⚠️ u-blox config: NAV-PVT enable did not ACK (continuing anyway).");
  }

  // LoRa init
  Serial.println("SX126x Sender starting...");
  SPI.begin(5, 19, 27, 18);

  // ✅ uses config.h: LORA_FREQ
  int state = radio.begin(LORA_FREQ);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("radio.begin() failed, code = ");
    Serial.println(state);
    while (true) { delay(1000); }
  }
  Serial.println("✅ Radio init OK");

  // Tell SendOwnInfo which serial to parse UBX from
  SendOwnInfo_begin(GPSSerial);

  radio.setDio1Action(setFlag);

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
  if (operationDone) {
    operationDone = false;

    if (transmitFlag) {
      // previous operation was transmission
      if (transmissionState == RADIOLIB_ERR_NONE) {
        //Serial.println(F("transmission finished!"));
      } else {
        Serial.print(F("failed, code "));
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
        if (parseGpsPayload(str, companion)) {
          haveCompanionFix = true;
        } else {
          Serial.println("❌ Failed to parse companion payload");
        }
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
}