#include "U-blox-helper.h"

// We keep a pointer to the GPS serial provided by sender.cpp
static HardwareSerial* s_gps = nullptr;

// ------------------------------------------------------------
// UBX helpers (private to this translation unit)
// ------------------------------------------------------------
static void sendUBX(const uint8_t* msg, uint16_t len) {
  if (!s_gps) return;
  s_gps->write(msg, len);
  s_gps->flush();
}

static void ubxChecksum(const uint8_t* data, uint16_t len, uint8_t &ckA, uint8_t &ckB) {
  ckA = 0; ckB = 0;
  for (uint16_t i = 0; i < len; i++) {
    ckA = ckA + data[i];
    ckB = ckB + ckA;
  }
}

static AckResult waitForAck(uint8_t cls, uint8_t id, uint32_t timeoutMs) {
  if (!s_gps) return ACK_TIMEOUT;

  uint8_t buf[10];
  uint8_t idx = 0;
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    while (s_gps->available()) {
      uint8_t b = (uint8_t)s_gps->read();

      if (idx == 0 && b != 0xB5) continue;
      if (idx == 1 && b != 0x62) { idx = 0; continue; }

      buf[idx++] = b;

      if (idx == 10) {
        idx = 0;

        if (buf[0] != 0xB5 || buf[1] != 0x62 || buf[2] != 0x05) continue;
        if (!((buf[3] == 0x01) || (buf[3] == 0x00))) continue;
        if (buf[4] != 0x02 || buf[5] != 0x00) continue;

        uint8_t ckA, ckB;
        ubxChecksum(&buf[2], 6, ckA, ckB);
        if (ckA != buf[8] || ckB != buf[9]) continue;

        if (buf[6] == cls && buf[7] == id) {
          return (buf[3] == 0x01) ? ACK_OK : ACK_NAK;
        }
      }
    }
    delay(1);
  }
  return ACK_TIMEOUT;
}

static void sendUBX_CFG_MSG(uint8_t targetMsgClass, uint8_t targetMsgId, uint8_t rateUART1) {
  uint8_t payload[8] = {
    targetMsgClass, targetMsgId,
    0,         // rateI2C
    rateUART1, // rateUART1
    0,         // rateUART2
    0,         // rateUSB
    0,         // rateSPI
    0          // reserved
  };

  uint8_t msg[16];
  msg[0] = 0xB5; msg[1] = 0x62;
  msg[2] = 0x06; msg[3] = 0x01; // CFG-MSG
  msg[4] = 0x08; msg[5] = 0x00; // length=8
  memcpy(&msg[6], payload, 8);

  uint8_t ckA, ckB;
  ubxChecksum(&msg[2], 12, ckA, ckB); // class..payload
  msg[14] = ckA; msg[15] = ckB;

  sendUBX(msg, sizeof(msg));
}

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------
void UbloxHelper_begin(HardwareSerial& gpsSerial) {
  s_gps = &gpsSerial;
}

void UbloxHelper_flushGpsInput(uint32_t ms) {
  if (!s_gps) return;

  uint32_t start = millis();
  while (millis() - start < ms) {
    while (s_gps->available()) (void)s_gps->read();
    delay(1);
  }
}

bool UbloxHelper_configureUbxOnlyNavPvt() {
  if (!s_gps) return false;

  // Configure u-blox: disable NMEA, enable UBX-NAV-PVT on UART1
  UbloxHelper_flushGpsInput(200);

  // Disable NMEA sentences on UART1 (best-effort)
  sendUBX_CFG_MSG(0xF0, 0x00, 0); waitForAck(0x06, 0x01, 2000); UbloxHelper_flushGpsInput(200); // GGA
  sendUBX_CFG_MSG(0xF0, 0x01, 0); waitForAck(0x06, 0x01, 2000); UbloxHelper_flushGpsInput(200); // GLL
  sendUBX_CFG_MSG(0xF0, 0x02, 0); waitForAck(0x06, 0x01, 2000); UbloxHelper_flushGpsInput(200); // GSA
  sendUBX_CFG_MSG(0xF0, 0x03, 0); waitForAck(0x06, 0x01, 2000); UbloxHelper_flushGpsInput(200); // GSV
  sendUBX_CFG_MSG(0xF0, 0x04, 0); waitForAck(0x06, 0x01, 2000); UbloxHelper_flushGpsInput(200); // RMC
  sendUBX_CFG_MSG(0xF0, 0x05, 0); waitForAck(0x06, 0x01, 2000); UbloxHelper_flushGpsInput(200); // VTG

  // Enable NAV-PVT (class 0x01 id 0x07) at rate 1 on UART1
  sendUBX_CFG_MSG(0x01, 0x07, 1);
  AckResult r = waitForAck(0x06, 0x01, 2000);
  UbloxHelper_flushGpsInput(200);

  return (r == ACK_OK);
}

bool UbloxHelper_parseGpsPayload(const String& str, GpsInfo& in) {
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