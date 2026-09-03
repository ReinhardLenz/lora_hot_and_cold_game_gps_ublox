#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "config.h"
#include "SendOwnInfo.h"
#include "AXP2101.h"
#include "U-blox-helper.h"
#include "NavigationMath.h"
#include <math.h>
#include <Adafruit_BNO08x.h>
#include <compass.h>
#include "LED.h"
#include <FastLED.h>

// ============================================================
// T-BEAM SX1262 GPS <-> LoRa communication
// RECEIVE-STATE RACE FIX
// ============================================================

//#define INITIATING_NODE

// --------------------
// BNO085 UART
// --------------------
Adafruit_BNO08x bno08x(PIN_BNO_RESET);
Compass compass(bno08x);


// LED ring module
LedRing ledRing(LED_COUNT, static_cast<uint8_t>(PIN_LED_RING));


// ============================================================
// NEW: Potentiometer correction (global, persistent)
// ============================================================
static float PotentiometerCorrection = 0.0f;   // set in setup() for now

// Helper: normalize degrees to [0, 360)
static float normalizeDeg360(float deg) {
  deg = fmodf(deg, 360.0f);
  if (deg < 0.0f) deg += 360.0f;
  return deg;
}

// ============================================================
// Navigation
// ============================================================

double d = 0.0;
double b = 0.0;   // bearingDegrees(...) stored here when computed

// NEW: store companion bearing (relative to corrected yaw)
double CompanionBearing = 0.0;

GpsInfo companion;
bool haveCompanionFix = false;

// ============================================================
// GPS UART
// ============================================================

HardwareSerial GPSSerial(1);

// ============================================================
// SX1262
// ============================================================

SX1262 radio = SX1262(
  new Module(
    LORA_NSS,
    LORA_DIO1,
    LORA_RST,
    LORA_BUSY
  )
);

// ============================================================
// Radio state
// ============================================================

enum RadioOperation : uint8_t
{
  RADIO_IDLE = 0,
  RADIO_RX   = 1,
  RADIO_TX   = 2
};

static volatile RadioOperation radioOperation = RADIO_IDLE;

// ============================================================
// IRQ event handling
// ============================================================

static volatile uint32_t irqEventCount = 0;
static volatile RadioOperation irqEventOperation = RADIO_IDLE;

static portMUX_TYPE radioStateMux =
    portMUX_INITIALIZER_UNLOCKED;

// ============================================================
// TX state
// ============================================================

int transmissionState = RADIOLIB_ERR_NONE;
bool transmitFlag = false;

// ============================================================
// Link maintenance
// ============================================================

static constexpr uint32_t
  LINK_RECOVERY_INTERVAL_INITIATOR_MS = 912;

static constexpr uint32_t
  LINK_RECOVERY_INTERVAL_OTHER_MS = 1201;

static constexpr uint32_t
  LINK_ACTIVITY_GRACE_MS = 3000;

static uint32_t lastValidPacketMs = 0;
static uint32_t nextMaintenanceTxMs = 0;

// ============================================================
// Radio health
// ============================================================

static constexpr uint32_t
  RADIO_HEALTH_CHECK_INTERVAL_MS = 3000;

static constexpr uint8_t
  RADIO_MAX_HEALTH_FAILURES = 3;

static uint32_t nextHealthCheckMs = 0;
static uint8_t radioHealthFailures = 0;

enum class ErrorCode
{
    None,
    UART,
    BNO_NotFound,
    EnableReport,
    ProductID,
};

void fatalError(ErrorCode code)
{
    Serial.println();
    Serial.println("========== FATAL ERROR ==========");

    switch (code)
    {
        case ErrorCode::UART:
            Serial.println("Unable to communicate with BNO085.");
            break;

        case ErrorCode::BNO_NotFound:
            Serial.println("BNO085 not detected.");
            break;

        case ErrorCode::EnableReport:
            Serial.println("Could not enable report.");
            break;

        default:
            Serial.println("Unknown error.");
            break;
    }

    while (true)
    {
        digitalWrite(PIN_STATUS_LED, HIGH);
        delay(BLINK_DELAY);
        digitalWrite(PIN_STATUS_LED, LOW);
        delay(BLINK_DELAY);
    }
}

// ============================================================
// ISR
// ============================================================

void IRAM_ATTR setFlag(void)
{
  portENTER_CRITICAL_ISR(&radioStateMux);

  irqEventOperation = radioOperation;
  irqEventCount++;

  portEXIT_CRITICAL_ISR(&radioStateMux);
}

// ============================================================
// Read pending IRQ event atomically
// ============================================================

static bool takeRadioEvent(RadioOperation &operation)
{
  bool haveEvent = false;

  portENTER_CRITICAL(&radioStateMux);

  if (irqEventCount != 0) {

    irqEventCount--;

    operation = irqEventOperation;

    haveEvent = true;
  }

  portEXIT_CRITICAL(&radioStateMux);

  return haveEvent;
}

// ============================================================
// Check whether an IRQ event is pending
// ============================================================

static bool radioEventPending()
{
  bool pending;

  portENTER_CRITICAL(&radioStateMux);

  pending = (irqEventCount != 0);

  portEXIT_CRITICAL(&radioStateMux);

  return pending;
}

// ============================================================
// Atomically set radio operation state
// ============================================================

static void setRadioOperation(RadioOperation state)
{
  portENTER_CRITICAL(&radioStateMux);

  radioOperation = state;

  portEXIT_CRITICAL(&radioStateMux);
}

// ============================================================
// Read radio operation state
// ============================================================

static RadioOperation getRadioOperation()
{
  RadioOperation state;

  portENTER_CRITICAL(&radioStateMux);

  state = radioOperation;

  portEXIT_CRITICAL(&radioStateMux);

  return state;
}

// ============================================================
// Clear pending radio events
// ============================================================

static void clearRadioEvents()
{
  portENTER_CRITICAL(&radioStateMux);

  irqEventCount = 0;

  portEXIT_CRITICAL(&radioStateMux);
}

// ============================================================
// Configure SX1262
// ============================================================

static bool configureRadio()
{
  int state;

  state = radio.setSpreadingFactor(10);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("❌ setSpreadingFactor failed, code = ");
    Serial.println(state);
    return false;
  }

  state = radio.setBandwidth(125.0);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("❌ setBandwidth failed, code = ");
    Serial.println(state);
    return false;
  }

  state = radio.setCodingRate(7);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("❌ setCodingRate failed, code = ");
    Serial.println(state);
    return false;
  }

  state = radio.setOutputPower(14);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("❌ setOutputPower failed, code = ");
    Serial.println(state);
    return false;
  }

  return true;
}

// ============================================================
// Start RX
// ============================================================

static int startReceiveSafely()
{
  int state;

  if (radioEventPending()) {
    Serial.println("⚠️ Cannot start RX: radio IRQ event still pending.");
    return RADIOLIB_ERR_UNKNOWN;
  }

  setRadioOperation(RADIO_RX);
  transmitFlag = false;

  state = radio.startReceive();

  if (state != RADIOLIB_ERR_NONE) {
    setRadioOperation(RADIO_IDLE);
    Serial.print("⚠️ radio.startReceive() failed, code = ");
    Serial.println(state);
    return state;
  }

  return RADIOLIB_ERR_NONE;
}

// ============================================================
// Prepare transition from RX to TX
// ============================================================

static bool prepareForTransmit()
{
  RadioOperation current = getRadioOperation();

  if (current == RADIO_TX) {
    Serial.println("⚠️ TX requested while already transmitting.");
    return false;
  }

  if (current == RADIO_RX) {
    int state = radio.finishReceive();

    if (state != RADIOLIB_ERR_NONE) {
      Serial.print("⚠️ finishReceive() returned ");
      Serial.println(state);
      setRadioOperation(RADIO_IDLE);
      return false;
    }

    setRadioOperation(RADIO_IDLE);
  }

  clearRadioEvents();
  return true;
}

// ============================================================
// Start own GPS transmission
// ============================================================

static bool startOwnTransmission()
{
  if (!prepareForTransmit()) {
    return false;
  }

  setRadioOperation(RADIO_TX);
  transmitFlag = true;

  GpsInfo own =
      prepareAndSendOwnInfo(
        radio,
        transmissionState,
        transmitFlag
      );

  if (transmissionState != RADIOLIB_ERR_NONE) {
    Serial.print("❌ startTransmit failed, code = ");
    Serial.println(transmissionState);

    setRadioOperation(RADIO_IDLE);
    transmitFlag = false;
    return false;
  }

  // Calculate navigation only when both sides have data
  if (
    own.hasData &&
    haveCompanionFix &&
    companion.hasData
  ) {

    d = distanceMeters(
      own.lat,
      own.lon,
      companion.lat,
      companion.lon
    );

    b = bearingDegrees(
      own.lat,
      own.lon,
      companion.lat,
      companion.lon
    );
    // NOTE: b is stored globally and remains valid until next update
  }

  return true;
}

// ============================================================
// Schedule next maintenance TX
// ============================================================

static void scheduleMaintenanceTransmission()
{
#if defined(INITIATING_NODE)
  nextMaintenanceTxMs = millis() + LINK_RECOVERY_INTERVAL_INITIATOR_MS;
#else
  nextMaintenanceTxMs = millis() + LINK_RECOVERY_INTERVAL_OTHER_MS;
#endif
}

// ============================================================
// HARD RADIO REINITIALIZATION
// ============================================================

static bool hardRadioReinit()
{
  Serial.println("⚠️ SX1262 hard recovery...");

  portENTER_CRITICAL(&radioStateMux);
  radioOperation = RADIO_IDLE;
  irqEventCount = 0;
  portEXIT_CRITICAL(&radioStateMux);

  transmitFlag = false;

  int state = radio.reset();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("⚠️ radio.reset() returned ");
    Serial.println(state);
  }

  delay(50);

  state = radio.begin(LORA_FREQ);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("❌ radio.begin() failed, code = ");
    Serial.println(state);
    return false;
  }

  if (!configureRadio()) {
    Serial.println("❌ LoRa configuration after recovery failed.");
    return false;
  }

  radio.setDio1Action(setFlag);

  delay(10);

  clearRadioEvents();

  state = startReceiveSafely();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("❌ RX restart after hard recovery failed, code = ");
    Serial.println(state);
    return false;
  }

  radioHealthFailures = 0;
  nextHealthCheckMs = millis() + RADIO_HEALTH_CHECK_INTERVAL_MS;

  Serial.println("✅ SX1262 hard recovery successful.");
  return true;
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);
  ledRing.begin(LED_BRIGHTNESS);
  // NEW: initialize correction (later replace with analogRead)
  PotentiometerCorrection = 0.0f;

  // --------------------
  // start BNO085 UART
  // --------------------
  Serial2.begin(BNO_BAUD, SERIAL_8N1, PIN_BNO_RX, PIN_BNO_TX);

  while (!Serial) delay(RESET_TIME_MS);

  Serial.println("start Adafruit BNO08x test!");

  if (!bno08x.begin_UART(&Serial2))
  {
    fatalError(ErrorCode::BNO_NotFound);
  }

  Serial.println("BNO08x Found!");

  Compass::setReports(&bno08x, SH2_ROTATION_VECTOR, 100000);

  Serial.println("Reading events");
  delay(100);
  // --------------------
  // end BNO085 UART
  // --------------------

  // ==========================================================
  // GPS POWER
  // ==========================================================

  AXP2101_beginAndEnableGPSPower();

  // ==========================================================
  // GPS UART
  // ==========================================================

  GPSSerial.begin(
    GPS_BAUD,
    SERIAL_8N1,
    GPS_RX_PIN,
    GPS_TX_PIN
  );

  delay(200);

  UbloxHelper_begin(GPSSerial);

  bool gpsConfigOK =
      UbloxHelper_configureUbxOnlyNavPvt();

  if (!gpsConfigOK) {
    Serial.println(
      "⚠️ u-blox config: NAV-PVT enable did not ACK "
      "(continuing anyway)."
    );
  }

  // ==========================================================
  // SPI
  // ==========================================================

  Serial.println("SX126x PingPong starting...");

  SPI.begin(
    SPI_SCK_PIN,
    SPI_MISO_PIN,
    SPI_MOSI_PIN,
    SPI_SS_PIN
  );

  // ==========================================================
  // SX1262
  // ==========================================================

  int state =
      radio.begin(LORA_FREQ);

  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("radio.begin() failed, code = ");
    Serial.println(state);

    while (true) {
      delay(1000);
    }
  }

  Serial.println("✅ Radio init OK");

  // ==========================================================
  // LoRa settings
  // ==========================================================

  if (!configureRadio()) {
    Serial.println("❌ LoRa configuration failed.");

    while (true) {
      delay(1000);
    }
  }

  Serial.println("✅ LoRa settings: SF10, BW125, CR4/7, TX14dBm");

  // ==========================================================
  // GPS sender
  // ==========================================================

  SendOwnInfo_begin(GPSSerial);

  // ==========================================================
  // DIO1
  // ==========================================================

  radio.setDio1Action(setFlag);

  // ==========================================================
  // Initial timers
  // ==========================================================

  lastValidPacketMs = millis();

  nextHealthCheckMs =
      millis() +
      RADIO_HEALTH_CHECK_INTERVAL_MS;

  // ==========================================================
  // INITIAL RADIO STATE
  // ==========================================================

#if defined(INITIATING_NODE)

  Serial.print(F("[SX1262] Sending first packet ... "));

  if (!startOwnTransmission()) {

    Serial.println(F("failed"));

    if (!startReceiveSafely()) {
      hardRadioReinit();
    }

  } else {
    Serial.println(F("started"));
  }

#else

  Serial.print(F("[SX1262] Starting to listen ... "));

  state = startReceiveSafely();

  if (state == RADIOLIB_ERR_NONE) {
    Serial.println(F("success!"));
  } else {
    Serial.print(F("failed, code "));
    Serial.println(state);

    if (!hardRadioReinit()) {
      while (true) {
        delay(1000);
      }
    }
  }

#endif

  // ==========================================================
  // Maintenance transmission schedule
  // ==========================================================

#if defined(INITIATING_NODE)
  nextMaintenanceTxMs = millis() + LINK_RECOVERY_INTERVAL_INITIATOR_MS;
#else
  nextMaintenanceTxMs = millis() + LINK_RECOVERY_INTERVAL_OTHER_MS;
#endif
}

// ============================================================
// HANDLE TX COMPLETE
// ============================================================

static void handleTxEvent()
{
  setRadioOperation(RADIO_IDLE);

  int state =
      radio.finishTransmit();

  transmitFlag = false;

  if (state != RADIOLIB_ERR_NONE) {

    Serial.print("⚠️ finishTransmit() failed, code = ");
    Serial.println(state);

    clearRadioEvents();

    if (startReceiveSafely() != RADIOLIB_ERR_NONE) {
      Serial.println("⚠️ RX after TX failure failed.");
      hardRadioReinit();
    }

    return;
  }

  clearRadioEvents();

  state = startReceiveSafely();

  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("⚠️ RX after TX failed, code = ");
    Serial.println(state);
    hardRadioReinit();
  }
}

// ============================================================
// HANDLE RX COMPLETE
// ============================================================

static void handleRxEvent()
{
  setRadioOperation(RADIO_IDLE);

  String str;

  int state =
      radio.readData(str);

  if (state == RADIOLIB_ERR_NONE) {

    lastValidPacketMs = millis();
    radioHealthFailures = 0;

    if (UbloxHelper_parseGpsPayload(str, companion)) {
      haveCompanionFix = true;
    } else {
      Serial.println("❌ Failed to parse companion payload");
    }

  } else {

    Serial.print("⚠️ readData failed, code = ");
    Serial.println(state);
  }

  clearRadioEvents();

  state = startReceiveSafely();

  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("⚠️ RX restart failed, code = ");
    Serial.println(state);
    hardRadioReinit();
  }
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
  uint32_t now = millis();

  // ----------------------------------------------------------
  // 0) Compass + per-iteration corrected yaw
  // ----------------------------------------------------------
  compass.processSensor();

  // NEW: per-iteration variable (valid only during this loop iteration)
  float CorrectedYaw =
      normalizeDeg360(
        PotentiometerCorrection-compass.getYawNorthDeg() 
      );

  // NEW: per-iteration companion bearing relative to corrected yaw
  // b is global and remains stored even if not updated this iteration
  CompanionBearing =
      normalizeDeg360(
        (float)b - CorrectedYaw
      );

  // Optional debug prints
  Serial.print("YawNorth=");
  Serial.print(compass.getYawNorthDeg(), 3);
  Serial.print(" PotCorr=");
  Serial.print(PotentiometerCorrection, 3);
  Serial.print(" CorrectedYaw=");
  Serial.print(CorrectedYaw, 3);
  Serial.print(" b=");
  Serial.print(b, 3);
  Serial.print(" CompanionBearing=");
  Serial.println(CompanionBearing, 3);

  ledRing.showDirection(CompanionBearing, CRGB::White);

  // ==========================================================
  // 1. RADIO IRQ EVENTS
  // ==========================================================

  RadioOperation eventOperation;

  if (takeRadioEvent(eventOperation)) {

    if (eventOperation == RADIO_TX) {
      handleTxEvent();
    }
    else if (eventOperation == RADIO_RX) {
      handleRxEvent();
    }
    else {
      Serial.println("⚠️ Radio IRQ received while state was IDLE.");

      setRadioOperation(RADIO_IDLE);
      clearRadioEvents();

      if (startReceiveSafely() != RADIOLIB_ERR_NONE) {
        hardRadioReinit();
      }
    }
  }

  // ==========================================================
  // 2. LINK MAINTENANCE TRANSMISSION
  // ==========================================================

  if (
    getRadioOperation() == RADIO_RX &&
    !radioEventPending() &&
    (int32_t)(now - nextMaintenanceTxMs) >= 0
  ) {

    uint32_t silentTime = now - lastValidPacketMs;

    if (silentTime < LINK_ACTIVITY_GRACE_MS) {

      scheduleMaintenanceTransmission();

    } else {

      if (!startOwnTransmission()) {

        Serial.println("⚠️ Maintenance TX could not be started.");

        if (getRadioOperation() == RADIO_IDLE) {

          clearRadioEvents();

          if (startReceiveSafely() != RADIOLIB_ERR_NONE) {
            hardRadioReinit();
          }
        }
      }

      scheduleMaintenanceTransmission();
    }
  }

  // ==========================================================
  // 3. REAL SX1262 HEALTH CHECK
  // ==========================================================

  if (
    getRadioOperation() == RADIO_RX &&
    !radioEventPending() &&
    (int32_t)(now - nextHealthCheckMs) >= 0
  ) {

    nextHealthCheckMs =
        now + RADIO_HEALTH_CHECK_INTERVAL_MS;

    uint32_t irqFlags = radio.getIrqFlags();

    if (irqFlags == 0xFFFFFFFFUL) {

      radioHealthFailures++;

      Serial.print("⚠️ SX1262 health check failed: ");
      Serial.println(radioHealthFailures);

      if (radioHealthFailures >= RADIO_MAX_HEALTH_FAILURES) {
        radioHealthFailures = 0;
        hardRadioReinit();
      }

    } else {
      radioHealthFailures = 0;
    }
  }

  // ==========================================================
  // 4. IDLE SAFETY NET
  // ==========================================================

  if (
    getRadioOperation() == RADIO_IDLE &&
    !radioEventPending()
  ) {

    delay(1);

    if (startReceiveSafely() != RADIOLIB_ERR_NONE) {
      Serial.println("⚠️ Idle -> RX failed.");
      hardRadioReinit();
    }
  }

  // ==========================================================
  // 5. Yield
  // ==========================================================

  delay(1);
}