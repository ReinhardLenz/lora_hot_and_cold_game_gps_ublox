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





// --- UBX framing helpers you likely already have ---
// sendUBX(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len);
// waitForAck(uint8_t cls, uint8_t id, uint32_t timeoutMs);
// UbloxHelper_flushGpsInput(uint32_t ms);
// sendUBX_CFG_MSG(uint8_t msgClass, uint8_t msgId, uint8_t rate);

// If you don't have a generic sendUBX(), you must adapt sendUBX_CFG_GNSS_*()
// to your existing UBX send routine.

static void sendUBX(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len)
{
  // Minimal UBX packet writer: 0xB5 0x62 CLS ID LEN_L LEN_H PAYLOAD CK_A CK_B
  uint8_t ckA = 0, ckB = 0;

  auto upd = [&](uint8_t b) { ckA = ckA + b; ckB = ckB + ckA; };

  s_gps->write(0xB5);
  s_gps->write(0x62);

  s_gps->write(cls); upd(cls);
  s_gps->write(id);  upd(id);

  uint8_t lenL = (uint8_t)(len & 0xFF);
  uint8_t lenH = (uint8_t)(len >> 8);
  s_gps->write(lenL); upd(lenL);
  s_gps->write(lenH); upd(lenH);

  for (uint16_t i = 0; i < len; i++) {
    uint8_t b = payload ? payload[i] : 0;
    s_gps->write(b);
    upd(b);
  }

  s_gps->write(ckA);
  s_gps->write(ckB);
}

// UBX-CFG-CFG (0x06 0x09): save current configuration
// payload: clearMask(4) saveMask(4) loadMask(4) deviceMask(1)
static void sendUBX_CFG_CFG_save()
{
  // Save everything to BBR + Flash (deviceMask bits: 0=BBR, 1=Flash, 2=EEPROM, 4=SPI Flash)
  // Many NEO-M8N boards support BBR; some also support Flash. If Flash isn't present, it may NAK.
  // You can change deviceMask to 0x01 (BBR only) if you want to be conservative.
  uint8_t payload[13];

  // clearMask
  payload[0] = 0x00; payload[1] = 0x00; payload[2] = 0x00; payload[3] = 0x00;

  // saveMask = 0x0000FFFF (save "all" commonly used config sections)
  payload[4] = 0xFF; payload[5] = 0xFF; payload[6] = 0x00; payload[7] = 0x00;

  // loadMask
  payload[8]  = 0x00; payload[9]  = 0x00; payload[10] = 0x00; payload[11] = 0x00;

  // deviceMask: BBR + Flash
  payload[12] = 0x03;

  sendUBX(0x06, 0x09, payload, sizeof(payload));
}

// UBX-CFG-GNSS (0x06 0x3E) for u-blox M8
// This payload enables GPS + Galileo + GLONASS, disables BeiDou.
// It keeps SBAS/QZSS enabled (common defaults) but you can change flags.
//
// NOTE: This exact block layout is for typical M8 firmware with 7 blocks:
// GPS, SBAS, Galileo, BeiDou, IMES, QZSS, GLONASS.
// If your firmware differs, you must adjust numConfigBlocks and blocks accordingly.
static void sendUBX_CFG_GNSS_GPS_GAL_GLO()
{
  // Header (4 bytes):
  // version(1)=0x00, numTrkChHw(1)=0x00 (read-only in poll), numTrkChUse(1)=0x00, numConfigBlocks(1)
  //
  // Then N blocks of 8 bytes each:
  // gnssId(1), resTrkCh(1), maxTrkCh(1), reserved1(1),
  // flags(4) little-endian
  //
  // flags bits (M8):
  // bit0: enable
  // bit1: sigCfgMask (not used much on M8)
  // bit16..: other flags; we keep them 0 except enable.
  //
  // Channel allocations below are conservative and commonly work:
  // GPS:     max 16
  // Galileo: max 8
  // GLONASS: max 8
  // SBAS/QZSS small
  //
  // Total maxTrkCh should not exceed typical M8 tracking resources (often 32).
  // Here: GPS16 + GAL8 + GLO8 = 32 plus SBAS/QZSS/IMES set to 0/1 -> keep low.

  uint8_t payload[4 + 7 * 8] = {0};

  payload[0] = 0x00; // version
  payload[1] = 0x00; // numTrkChHw (ignored when setting)
  payload[2] = 0x00; // numTrkChUse (ignored when setting)
  payload[3] = 7;    // numConfigBlocks

  auto putBlock = [&](int idx, uint8_t gnssId, uint8_t resTrkCh, uint8_t maxTrkCh, bool enable) {
    int o = 4 + idx * 8;
    payload[o + 0] = gnssId;
    payload[o + 1] = resTrkCh;
    payload[o + 2] = maxTrkCh;
    payload[o + 3] = 0x00; // reserved1

    uint32_t flags = 0;
    if (enable) flags |= 0x00000001UL; // enable bit
    // leave other bits 0
    payload[o + 4] = (uint8_t)(flags & 0xFF);
    payload[o + 5] = (uint8_t)((flags >> 8) & 0xFF);
    payload[o + 6] = (uint8_t)((flags >> 16) & 0xFF);
    payload[o + 7] = (uint8_t)((flags >> 24) & 0xFF);
  };

  // gnssId values for M8:
  // 0=GPS, 1=SBAS, 2=Galileo, 3=BeiDou, 4=IMES, 5=QZSS, 6=GLONASS
/*
  putBlock(0, 0, 8, 16, true);  // GPS enabled
  putBlock(1, 1, 1,  3, true);  // SBAS enabled (optional)
  putBlock(2, 2, 4,  8, true);  // Galileo enabled
  putBlock(3, 3, 0,  0, false); // BeiDou disabled
  putBlock(4, 4, 0,  0, false); // IMES disabled
  putBlock(5, 5, 0,  3, true);  // QZSS enabled (optional)
  putBlock(6, 6, 4,  8, true);  // GLONASS enabled
*/
  putBlock(0, 0, 8, 32, true);  // GPS enabled
  putBlock(1, 1, 1,  3, true);  // SBAS enabled (optional)
  putBlock(2, 2, 8,  8, true);  // Galileo enabled
  putBlock(3, 3, 8,  32, true); // BeiDou disabled
  putBlock(4, 4, 0,  0, false); // IMES disabled
  putBlock(5, 5, 0,  3, true);  // QZSS enabled (optional)
  putBlock(6, 6, 8,  32, true);  // GLONASS enabled


  sendUBX(0x06, 0x3E, payload, sizeof(payload));
}

bool UbloxHelper_configureUbxOnlyNavPvt()
{
  if (!s_gps) return false;

  UbloxHelper_flushGpsInput(200);

  // 1) Disable NMEA sentences on UART1 (best-effort)
  sendUBX_CFG_MSG(0xF0, 0x00, 0); if (waitForAck(0x06, 0x01, 2000) != ACK_OK) return false; UbloxHelper_flushGpsInput(50); // GGA
  sendUBX_CFG_MSG(0xF0, 0x01, 0); if (waitForAck(0x06, 0x01, 2000) != ACK_OK) return false; UbloxHelper_flushGpsInput(50); // GLL
  sendUBX_CFG_MSG(0xF0, 0x02, 0); if (waitForAck(0x06, 0x01, 2000) != ACK_OK) return false; UbloxHelper_flushGpsInput(50); // GSA
  sendUBX_CFG_MSG(0xF0, 0x03, 0); if (waitForAck(0x06, 0x01, 2000) != ACK_OK) return false; UbloxHelper_flushGpsInput(50); // GSV
  sendUBX_CFG_MSG(0xF0, 0x04, 0); if (waitForAck(0x06, 0x01, 2000) != ACK_OK) return false; UbloxHelper_flushGpsInput(50); // RMC
  sendUBX_CFG_MSG(0xF0, 0x05, 0); if (waitForAck(0x06, 0x01, 2000) != ACK_OK) return false; UbloxHelper_flushGpsInput(50); // VTG

  // 2) Enable UBX-NAV-PVT (class 0x01 id 0x07) at rate 1 on UART1 , wait 2 SECONDS for ACK
  sendUBX_CFG_MSG(0x01, 0x07, 1);
  if (waitForAck(0x06, 0x01, 2000) != ACK_OK) 
  Serial.println(F("UBX-CFG-MSG failed! CFG-MSG payload layout/length doesn't match your firmware"));
    // If you hit this ❌, your CFG-MSG payload layout/length doesn't match your firmware.
  return false;
  UbloxHelper_flushGpsInput(100);

  // 3) Configure GNSS constellations: GPS + Galileo + GLONASS, wait 3 SECONDS for ACK
  sendUBX_CFG_GNSS_GPS_GAL_GLO();
  if (waitForAck(0x06, 0x3E, 3000) != ACK_OK) {
    Serial.println(F("UBX-CFG-GNSS failed! CFG-GNSS payload layout/length doesn't match your firmware"));
    // If you hit this ❌, your CFG-GNSS payload layout/length doesn't match your firmware.
    return false;
  }
  UbloxHelper_flushGpsInput(100);

  // 4) Save configuration to non-volatile memory (BBR/Flash) , wait 3 SECONDS for ACK
  sendUBX_CFG_CFG_save();
  if (waitForAck(0x06, 0x09, 3000) != ACK_OK) {
    Serial.println(F("UBX-CFG-CFG save failed! try deviceMask=0x01 (BBR only)."));
    // Some boards don't have Flash; if this fails, try deviceMask=0x01 (BBR only).
    return false;
  }
  UbloxHelper_flushGpsInput(100);

  return true;
}

/**
  
 //old  UbloxHelper_configureUbxOnlyNavPvt
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

*/