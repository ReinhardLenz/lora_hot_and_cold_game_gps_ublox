#include "U-blox-helper.h"

// We keep a pointer to the GPS serial provided by sender.cpp
static HardwareSerial* s_gps = nullptr;

// ------------------------------------------------------------
// UBX helpers (private to this translation unit)
// ------------------------------------------------------------
static void sendUBXraw(const uint8_t* msg, uint16_t len) {
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

  sendUBXraw(msg, sizeof(msg));
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
// sendUBXpacket(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len);
// waitForAck(uint8_t cls, uint8_t id, uint32_t timeoutMs);
// UbloxHelper_flushGpsInput(uint32_t ms);
// sendUBX_CFG_MSG(uint8_t msgClass, uint8_t msgId, uint8_t rate);

// If you don't have a generic sendUBXpacket(), you must adapt sendUBX_CFG_GNSS_*()
// to your existing UBX send routine.

static void sendUBXpacket(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len)
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

// ------------------------------------------------------------
// Poll UBX-CFG-GNSS and print the receiver's current configuration
// ------------------------------------------------------------

void UbloxHelper_pollAndPrintCFG_GNSS()
{
  if (!s_gps) {
    Serial.println(F("[u-blox] ERROR: no GPS serial for CFG-GNSS poll"));
    return;
  }

  Serial.println();
  Serial.println(F("========== POLL UBX-CFG-GNSS =========="));

  // UBX-CFG-GNSS poll request:
  //
  // B5 62 06 3E 00 00 CK_A CK_B
  //
  // Class = 06
  // ID    = 3E
  // Length = 0

  uint8_t msg[8];

  msg[0] = 0xB5;
  msg[1] = 0x62;
  msg[2] = 0x06;
  msg[3] = 0x3E;
  msg[4] = 0x00;
  msg[5] = 0x00;

  uint8_t ckA, ckB;
  ubxChecksum(&msg[2], 4, ckA, ckB);

  msg[6] = ckA;
  msg[7] = ckB;

  Serial.print(F("[u-blox] Sending CFG-GNSS poll: "));

  for (uint8_t i = 0; i < sizeof(msg); i++) {
    if (msg[i] < 0x10) Serial.print('0');
    Serial.print(msg[i], HEX);
    Serial.print(' ');
  }

  Serial.println();

  // Clear old data first
  UbloxHelper_flushGpsInput(100);

  // Send poll request
  s_gps->write(msg, sizeof(msg));
  s_gps->flush();

  Serial.println(F("[u-blox] Waiting for CFG-GNSS response..."));

  // ----------------------------------------------------------
  // Receive UBX message
  // ----------------------------------------------------------

  uint8_t state = 0;

  uint8_t cls = 0;
  uint8_t id  = 0;

  uint16_t len = 0;
  uint16_t index = 0;

  uint8_t payload[128];

  uint8_t ckA_rx = 0;
  uint8_t ckB_rx = 0;

  uint8_t ckA_calc = 0;
  uint8_t ckB_calc = 0;

  uint32_t start = millis();

  while (millis() - start < 3000) {

    if (!s_gps->available()) {
      delay(1);
      continue;
    }

    uint8_t b = (uint8_t)s_gps->read();

    switch (state) {

      case 0:
        if (b == 0xB5) {
          state = 1;
        }
        break;


      case 1:
        if (b == 0x62) {
          state = 2;
        }
        else {
          state = 0;
        }
        break;


      case 2:
        cls = b;
        state = 3;
        break;


      case 3:
        id = b;
        state = 4;
        break;


      case 4:
        len = b;
        state = 5;
        break;


      case 5:
        len |= ((uint16_t)b << 8);

        if (len > sizeof(payload)) {

          Serial.print(F("[u-blox] ERROR: CFG-GNSS response too large: "));
          Serial.println(len);

          return;
        }

        index = 0;
        state = (len == 0) ? 7 : 6;
        break;


      case 6:
        payload[index++] = b;

        if (index >= len) {
          state = 7;
        }
        break;


      case 7:
        ckA_rx = b;
        state = 8;
        break;


      case 8:
        ckB_rx = b;

        // Calculate checksum over
        // class + id + length + payload


          ckA_calc = 0;
          ckB_calc = 0;

          uint8_t header[4];

          header[0] = cls;
          header[1] = id;
          header[2] = (uint8_t)(len & 0xFF);
          header[3] = (uint8_t)(len >> 8);

          ubxChecksum(header, 4, ckA_calc, ckB_calc);

          for (uint16_t i = 0; i < len; i++) {
            ckA_calc = ckA_calc + payload[i];
            ckB_calc = ckB_calc + ckA_calc;
          }


        if (ckA_calc != ckA_rx || ckB_calc != ckB_rx) {

          Serial.println(F("[u-blox] ERROR: CFG-GNSS checksum incorrect."));
          return;
        }


        Serial.print(F("[u-blox] Received UBX message: class=0x"));
        if (cls < 0x10) Serial.print('0');
        Serial.print(cls, HEX);

        Serial.print(F(" id=0x"));
        if (id < 0x10) Serial.print('0');
        Serial.print(id, HEX);

        Serial.print(F(" length="));
        Serial.println(len);


        // Is it really CFG-GNSS?
        if (cls != 0x06 || id != 0x3E) {

          Serial.println(F("[u-blox] Received another UBX message, not CFG-GNSS."));
          return;
        }


        // ----------------------------------------------------
        // CFG-GNSS response structure
        // ----------------------------------------------------

        if (len < 4) {

          Serial.println(F("[u-blox] ERROR: CFG-GNSS response too short."));
          return;
        }


        uint8_t version       = payload[0];
        uint8_t numTrkChHw    = payload[1];
        uint8_t numTrkChUse   = payload[2];
        uint8_t numBlocks     = payload[3];


        Serial.println();
        Serial.println(F("[u-blox] CURRENT RECEIVER CONFIGURATION"));
        Serial.println(F("----------------------------------------"));

        Serial.print(F("Protocol version : "));
        Serial.println(version);

        Serial.print(F("Hardware channels: "));
        Serial.println(numTrkChHw);

        Serial.print(F("Channels in use  : "));
        Serial.println(numTrkChUse);

        Serial.print(F("Config blocks    : "));
        Serial.println(numBlocks);

        Serial.println();


        // ----------------------------------------------------
        // Decode each GNSS block
        // ----------------------------------------------------

        const char* names[] = {
          "GPS",
          "SBAS",
          "Galileo",
          "BeiDou",
          "IMES",
          "QZSS",
          "GLONASS"
        };


        for (uint8_t i = 0; i < numBlocks; i++) {

          uint16_t o = 4 + i * 8;

          if (o + 8 > len) {

            Serial.println(F("[u-blox] ERROR: block exceeds response length."));
            return;
          }


          uint8_t gnssId    = payload[o + 0];
          uint8_t resTrkCh  = payload[o + 1];
          uint8_t maxTrkCh  = payload[o + 2];

          uint32_t flags =
              ((uint32_t)payload[o + 4]) |
              ((uint32_t)payload[o + 5] << 8) |
              ((uint32_t)payload[o + 6] << 16) |
              ((uint32_t)payload[o + 7] << 24);


          bool enabled = (flags & 0x00000001UL) != 0;

          uint8_t sigCfgMask =
              (uint8_t)((flags >> 16) & 0xFF);


          Serial.print(F("GNSS ID "));
          Serial.print(gnssId);

          Serial.print(F("  "));

          if (gnssId < 7) {
            Serial.print(names[gnssId]);
          }
          else {
            Serial.print(F("UNKNOWN"));
          }

          Serial.println();

          Serial.print(F("  enabled      : "));
          Serial.println(enabled ? F("YES") : F("NO"));

          Serial.print(F("  resTrkCh      : "));
          Serial.println(resTrkCh);

          Serial.print(F("  maxTrkCh      : "));
          Serial.println(maxTrkCh);

          Serial.print(F("  sigCfgMask    : 0x"));

          if (sigCfgMask < 0x10) Serial.print('0');

          Serial.println(sigCfgMask, HEX);

          Serial.print(F("  flags         : 0x"));

          if (flags < 0x10000000UL) Serial.print('0');

          Serial.println(flags, HEX);

          Serial.println();
        }


        Serial.println(F("========== END CFG-GNSS POLL =========="));

        return;
    }
  }

  Serial.println(F("[u-blox] ERROR: Timeout waiting for CFG-GNSS response."));
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

  sendUBXpacket(0x06, 0x09, payload, sizeof(payload));
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
  // ------------------------------------------------------------
  // u-blox M8 UBX-CFG-GNSS
  //
  // Configure:
  //   GPS      enabled
  //   Galileo  enabled
  //   GLONASS  enabled
  //
  // Disable:
  //   SBAS
  //   BeiDou
  //   IMES
  //   QZSS
  //
  // M8 supports concurrent reception of up to 3 GNSS systems.
  //
  // Payload:
  //   byte 0 : msgVer
  //   byte 1 : numTrkChHw
  //   byte 2 : numTrkChUse
  //   byte 3 : numConfigBlocks
  //
  // Each block = 8 bytes:
  //   byte 0 : gnssId
  //   byte 1 : resTrkCh
  //   byte 2 : maxTrkCh
  //   byte 3 : reserved
  //   byte 4-7 : flags
  // ------------------------------------------------------------

  constexpr uint8_t NUM_BLOCKS = 7;

  uint8_t payload[4 + NUM_BLOCKS * 8] = {0};

  // ------------------------------------------------------------
  // Header
  // ------------------------------------------------------------

  payload[0] = 0x00;   // msgVer

  // NEO-M8 typically has 72 hardware tracking channels,
  // but numTrkChUse is the number of channels allocated for
  // GNSS tracking. We use 32 here.
  payload[1] = 72;     // numTrkChHw
  payload[2] = 32;     // numTrkChUse

  payload[3] = NUM_BLOCKS;


  // ------------------------------------------------------------
  // Helper
  // ------------------------------------------------------------

  auto putBlock =
    [&](uint8_t index,
        uint8_t gnssId,
        uint8_t resTrkCh,
        uint8_t maxTrkCh,
        bool enable,
        uint8_t sigCfgMask)
  {
    const uint8_t offset = 4 + index * 8;

    payload[offset + 0] = gnssId;
    payload[offset + 1] = resTrkCh;
    payload[offset + 2] = maxTrkCh;
    payload[offset + 3] = 0x00;   // reserved

    uint32_t flags = 0;

    // Bit 0 = enable
    if (enable) {
      flags |= 0x00000001UL;
    }

    // Bits 16..23 = sigCfgMask
    flags |= ((uint32_t)sigCfgMask << 16);

    payload[offset + 4] = (uint8_t)(flags & 0xFF);
    payload[offset + 5] = (uint8_t)((flags >> 8) & 0xFF);
    payload[offset + 6] = (uint8_t)((flags >> 16) & 0xFF);
    payload[offset + 7] = (uint8_t)((flags >> 24) & 0xFF);
  };


  // ------------------------------------------------------------
  // GNSS IDs for u-blox M8
  //
  // 0 = GPS
  // 1 = SBAS
  // 2 = Galileo
  // 3 = BeiDou
  // 4 = IMES
  // 5 = QZSS
  // 6 = GLONASS
  // ------------------------------------------------------------


  // GPS
  //
  // res = 8
  // max = 16
  // signal = GPS L1 C/A
  //
  putBlock(
    0,
    0,
    8,
    16,
    true,
    0x01
  );


  // SBAS disabled
  putBlock(
    1,
    1,
    0,
    3,
    false,
    0x01
  );


  // Galileo
  //
  // res = 4
  // max = 8
  // signal = Galileo E1
  //
  putBlock(
    2,
    2,
    4,
    8,
    true,
    0x01
  );


  // BeiDou disabled
  putBlock(
    3,
    3,
    0,
    8,
    false,
    0x01
  );


  // IMES disabled
  putBlock(
    4,
    4,
    0,
    0,
    false,
    0x01
  );


  // QZSS disabled
  putBlock(
    5,
    5,
    0,
    3,
    false,
    0x01
  );


  // GLONASS
  //
  // res = 4
  // max = 8
  // signal = GLONASS L1
  //
  putBlock(
    6,
    6,
    4,
    8,
    true,
    0x01
  );


  // ------------------------------------------------------------
  // Diagnostic output
  // ------------------------------------------------------------

  Serial.println(F("[u-blox] CFG-GNSS payload:"));

  for (uint16_t i = 0; i < sizeof(payload); i++) {

    if (payload[i] < 0x10) {
      Serial.print('0');
    }

    Serial.print(payload[i], HEX);
    Serial.print(' ');
  }

  Serial.println();


  // ------------------------------------------------------------
  // Send UBX-CFG-GNSS
  // ------------------------------------------------------------

  sendUBXpacket(
    0x06,
    0x3E,
    payload,
    sizeof(payload)
  );
}


bool UbloxHelper_configureUbxOnlyNavPvt()
{
  Serial.println();
  Serial.println(F("========== u-blox configuration START =========="));

  // ------------------------------------------------------------
  // Check GPS serial pointer
  // ------------------------------------------------------------
  if (!s_gps) {
    Serial.println(F("[u-blox] ERROR: s_gps == nullptr"));
    Serial.println(F("[u-blox] UbloxHelper_begin() was probably not called."));
    Serial.println(F("========== u-blox configuration FAILED =========="));
    return false;
  }

  Serial.println(F("[u-blox] GPS serial pointer OK"));

  // ------------------------------------------------------------
  // Flush anything already waiting in the GPS UART
  // ------------------------------------------------------------
  Serial.println(F("[u-blox] Flushing GPS input buffer..."));
  UbloxHelper_flushGpsInput(200);
  Serial.println(F("[u-blox] GPS input buffer flushed"));


  // ============================================================
  // STEP 1. Disable NMEA messages on UART1
  // ============================================================

  AckResult ack;


  // ------------------------------------------------------------
  // 1.1 GGA
  // ------------------------------------------------------------
  Serial.println();
  Serial.println(F("[u-blox] STEP 1.1: Disable NMEA-GGA (F0 00)"));

  sendUBX_CFG_MSG(0xF0, 0x00, 0);

  ack = waitForAck(0x06, 0x01, 2000);

  if (ack == ACK_OK) {
    Serial.println(F("[u-blox] GGA disable: ACK_OK"));
  }
  else if (ack == ACK_NAK) {
    Serial.println(F("[u-blox] GGA disable: ACK_NAK"));
    Serial.println(F("[u-blox] ERROR: Receiver explicitly rejected CFG-MSG GGA."));
    return false;
  }
  else {
    Serial.println(F("[u-blox] GGA disable: ACK_TIMEOUT"));
    Serial.println(F("[u-blox] ERROR: No ACK received for CFG-MSG GGA."));
    return false;
  }

  UbloxHelper_flushGpsInput(50);


  // ------------------------------------------------------------
  // 1.2 GLL
  // ------------------------------------------------------------
  Serial.println();
  Serial.println(F("[u-blox] STEP 1.2: Disable NMEA-GLL (F0 01)"));

  sendUBX_CFG_MSG(0xF0, 0x01, 0);

  ack = waitForAck(0x06, 0x01, 2000);

  if (ack == ACK_OK) {
    Serial.println(F("[u-blox] GLL disable: ACK_OK"));
  }
  else if (ack == ACK_NAK) {
    Serial.println(F("[u-blox] GLL disable: ACK_NAK"));
    Serial.println(F("[u-blox] ERROR: Receiver explicitly rejected CFG-MSG GLL."));
    return false;
  }
  else {
    Serial.println(F("[u-blox] GLL disable: ACK_TIMEOUT"));
    Serial.println(F("[u-blox] ERROR: No ACK received for CFG-MSG GLL."));
    return false;
  }

  UbloxHelper_flushGpsInput(50);


  // ------------------------------------------------------------
  // 1.3 GSA
  // ------------------------------------------------------------
  Serial.println();
  Serial.println(F("[u-blox] STEP 1.3: Disable NMEA-GSA (F0 02)"));

  sendUBX_CFG_MSG(0xF0, 0x02, 0);

  ack = waitForAck(0x06, 0x01, 2000);

  if (ack == ACK_OK) {
    Serial.println(F("[u-blox] GSA disable: ACK_OK"));
  }
  else if (ack == ACK_NAK) {
    Serial.println(F("[u-blox] GSA disable: ACK_NAK"));
    Serial.println(F("[u-blox] ERROR: Receiver explicitly rejected CFG-MSG GSA."));
    return false;
  }
  else {
    Serial.println(F("[u-blox] GSA disable: ACK_TIMEOUT"));
    Serial.println(F("[u-blox] ERROR: No ACK received for CFG-MSG GSA."));
    return false;
  }

  UbloxHelper_flushGpsInput(50);


  // ------------------------------------------------------------
  // 1.4 GSV
  // ------------------------------------------------------------
  Serial.println();
  Serial.println(F("[u-blox] STEP 1.4: Disable NMEA-GSV (F0 03)"));

  sendUBX_CFG_MSG(0xF0, 0x03, 0);

  ack = waitForAck(0x06, 0x01, 2000);

  if (ack == ACK_OK) {
    Serial.println(F("[u-blox] GSV disable: ACK_OK"));
  }
  else if (ack == ACK_NAK) {
    Serial.println(F("[u-blox] GSV disable: ACK_NAK"));
    Serial.println(F("[u-blox] ERROR: Receiver explicitly rejected CFG-MSG GSV."));
    return false;
  }
  else {
    Serial.println(F("[u-blox] GSV disable: ACK_TIMEOUT"));
    Serial.println(F("[u-blox] ERROR: No ACK received for CFG-MSG GSV."));
    return false;
  }

  UbloxHelper_flushGpsInput(50);


  // ------------------------------------------------------------
  // 1.5 RMC
  // ------------------------------------------------------------
  Serial.println();
  Serial.println(F("[u-blox] STEP 1.5: Disable NMEA-RMC (F0 04)"));

  sendUBX_CFG_MSG(0xF0, 0x04, 0);

  ack = waitForAck(0x06, 0x01, 2000);

  if (ack == ACK_OK) {
    Serial.println(F("[u-blox] RMC disable: ACK_OK"));
  }
  else if (ack == ACK_NAK) {
    Serial.println(F("[u-blox] RMC disable: ACK_NAK"));
    Serial.println(F("[u-blox] ERROR: Receiver explicitly rejected CFG-MSG RMC."));
    return false;
  }
  else {
    Serial.println(F("[u-blox] RMC disable: ACK_TIMEOUT"));
    Serial.println(F("[u-blox] ERROR: No ACK received for CFG-MSG RMC."));
    return false;
  }

  UbloxHelper_flushGpsInput(50);


  // ------------------------------------------------------------
  // 1.6 VTG
  // ------------------------------------------------------------
  Serial.println();
  Serial.println(F("[u-blox] STEP 1.6: Disable NMEA-VTG (F0 05)"));

  sendUBX_CFG_MSG(0xF0, 0x05, 0);

  ack = waitForAck(0x06, 0x01, 2000);

  if (ack == ACK_OK) {
    Serial.println(F("[u-blox] VTG disable: ACK_OK"));
  }
  else if (ack == ACK_NAK) {
    Serial.println(F("[u-blox] VTG disable: ACK_NAK"));
    Serial.println(F("[u-blox] ERROR: Receiver explicitly rejected CFG-MSG VTG."));
    return false;
  }
  else {
    Serial.println(F("[u-blox] VTG disable: ACK_TIMEOUT"));
    Serial.println(F("[u-blox] ERROR: No ACK received for CFG-MSG VTG."));
    return false;
  }

  UbloxHelper_flushGpsInput(50);


  // ============================================================
  // STEP 2. Enable UBX-NAV-PVT
  // ============================================================

  Serial.println();
  Serial.println(F("[u-blox] STEP 2: Enable UBX-NAV-PVT (01 07)"));

  sendUBX_CFG_MSG(0x01, 0x07, 1);

  ack = waitForAck(0x06, 0x01, 2000);

  if (ack == ACK_OK) {

    Serial.println(F("[u-blox] NAV-PVT enable: ACK_OK"));

  }
  else if (ack == ACK_NAK) {

    Serial.println(F("[u-blox] NAV-PVT enable: ACK_NAK"));
    Serial.println(F("[u-blox] ERROR: Receiver explicitly rejected CFG-MSG NAV-PVT."));
    Serial.println(F("[u-blox] THIS is the NAV-PVT configuration failure."));

    return false;

  }
  else {

    Serial.println(F("[u-blox] NAV-PVT enable: ACK_TIMEOUT"));
    Serial.println(F("[u-blox] ERROR: No ACK received for CFG-MSG NAV-PVT."));
    Serial.println(F("[u-blox] THIS is the NAV-PVT configuration failure."));

    return false;
  }

  UbloxHelper_flushGpsInput(100);


  // ============================================================
  // STEP 3. Configure GNSS constellations
  // ============================================================

  Serial.println();
  Serial.println(F("[u-blox] STEP 3: Configure GPS + Galileo + GLONASS"));

  sendUBX_CFG_GNSS_GPS_GAL_GLO();

  ack = waitForAck(0x06, 0x3E, 3000);

  if (ack == ACK_OK) {

    Serial.println(F("[u-blox] CFG-GNSS: ACK_OK"));

  }
  else if (ack == ACK_NAK) {

    Serial.println(F("[u-blox] CFG-GNSS: ACK_NAK"));
    Serial.println(F("[u-blox] ERROR: Receiver explicitly rejected CFG-GNSS."));
    Serial.println(F("[u-blox] NAV-PVT was accepted; failure is in GNSS configuration."));

    return false;

  }
  else {

    Serial.println(F("[u-blox] CFG-GNSS: ACK_TIMEOUT"));
    Serial.println(F("[u-blox] ERROR: No ACK received for CFG-GNSS."));
    Serial.println(F("[u-blox] NAV-PVT was accepted; failure is in GNSS configuration."));

    return false;
  }

  UbloxHelper_flushGpsInput(100);


  // ============================================================
  // STEP 4. Save configuration
  // ============================================================

  Serial.println();
  Serial.println(F("[u-blox] STEP 4: Save configuration using UBX-CFG-CFG"));

  sendUBX_CFG_CFG_save();

  ack = waitForAck(0x06, 0x09, 3000);

  if (ack == ACK_OK) {

    Serial.println(F("[u-blox] CFG-CFG save: ACK_OK"));

  }
  else if (ack == ACK_NAK) {

    Serial.println(F("[u-blox] CFG-CFG save: ACK_NAK"));
    Serial.println(F("[u-blox] ERROR: Receiver explicitly rejected CFG-CFG save."));
    Serial.println(F("[u-blox] Try deviceMask = 0x01 (BBR only)."));

    return false;

  }
  else {

    Serial.println(F("[u-blox] CFG-CFG save: ACK_TIMEOUT"));
    Serial.println(F("[u-blox] ERROR: No ACK received for CFG-CFG save."));
    Serial.println(F("[u-blox] Try deviceMask = 0x01 (BBR only)."));

    return false;
  }

  UbloxHelper_flushGpsInput(100);


  // ============================================================
  // SUCCESS
  // ============================================================

  Serial.println();
  Serial.println(F("========== u-blox configuration SUCCESS =========="));
  Serial.println(F("[u-blox] NMEA disabled"));
  Serial.println(F("[u-blox] NAV-PVT enabled"));
  Serial.println(F("[u-blox] GPS + Galileo + GLONASS configured"));
  Serial.println(F("[u-blox] Configuration saved"));

  return true;
}

