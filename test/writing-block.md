```cpp
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "config.h"
#include "SendOwnInfo.h"
#include "AXP2101.h"
#include "U-blox-helper.h"
#include "NavigationMath.h"
#include <math.h>


// ============================================================
// T-BEAM SX1262 GPS <-> LoRa communication
//
// RECEIVE-STATE RACE FIX
//
// The radio interrupt and the main loop are asynchronous.
// The old design used:
//
//     volatile bool operationDone
//     radioOperation
//
// as two independent variables.
//
// This can race:
//
//     ISR                         loop
//     ---                         ----
//     DIO1 interrupt
//                                 change RX -> IDLE
//                                 change IDLE -> TX
//     operationDone = true
//
// The main loop could then interpret an old RX interrupt as
// belonging to the TX operation.
//
// This version uses:
//
//   1. A protected radio state.
//   2. An IRQ event counter.
//   3. The operation state captured by the ISR.
//   4. Critical sections around state/event changes.
//   5. Explicit RX -> IDLE -> TX and TX -> IDLE -> RX
//      transitions.
//   6. No "no IRQ = radio failure" watchdog.
//
// A quiet channel is therefore treated as a normal condition.
//
// Only this main.cpp needs to be changed.
// ============================================================


//#define INITIATING_NODE


// ============================================================
// Navigation
// ============================================================

double d = 0.0;
double b = 0.0;

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


// This is accessed from both ISR and main loop.
static volatile RadioOperation radioOperation = RADIO_IDLE;


// ============================================================
// IRQ event handling
// ============================================================
//
// The old code:
//
//     volatile bool operationDone
//
// can lose an event.
//
// We instead count events.
//
// irqEventCount is incremented by the ISR.
//
// irqEventOperation stores the operation which was active when
// the interrupt occurred.
//
// On ESP32, access is protected with portMUX.
//
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
  LINK_RECOVERY_INTERVAL_INITIATOR_MS = 12000;

static constexpr uint32_t
  LINK_RECOVERY_INTERVAL_OTHER_MS = 16000;

static constexpr uint32_t
  LINK_ACTIVITY_GRACE_MS = 3000;

static uint32_t lastValidPacketMs = 0;
static uint32_t nextMaintenanceTxMs = 0;


// ============================================================
// Radio health
// ============================================================
//
// IMPORTANT:
//
// Absence of DIO1 is NOT a radio failure.
//
// We only declare the SX1262 unhealthy if SPI access itself
// fails repeatedly.
// ============================================================

static constexpr uint32_t
  RADIO_HEALTH_CHECK_INTERVAL_MS = 3000;

static constexpr uint8_t
  RADIO_MAX_HEALTH_FAILURES = 3;

static uint32_t nextHealthCheckMs = 0;
static uint8_t radioHealthFailures = 0;


// ============================================================
// ISR
// ============================================================
//
// The ISR does only two things:
//
//   - capture the operation state
//   - increment the event counter
//
// No Serial.
// No SPI.
// No RadioLib calls.
// No millis().
//
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
//
// Returns true when an event was pending.
//
// The event is consumed atomically.
//
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
//
// Used only when we have deliberately completed the old radio
// operation and are about to establish a new one.
//
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

    Serial.print(
      "❌ setSpreadingFactor failed, code = "
    );

    Serial.println(state);

    return false;
  }


  state = radio.setBandwidth(125.0);

  if (state != RADIOLIB_ERR_NONE) {

    Serial.print(
      "❌ setBandwidth failed, code = "
    );

    Serial.println(state);

    return false;
  }


  state = radio.setCodingRate(7);

  if (state != RADIOLIB_ERR_NONE) {

    Serial.print(
      "❌ setCodingRate failed, code = "
    );

    Serial.println(state);

    return false;
  }


  state = radio.setOutputPower(14);

  if (state != RADIOLIB_ERR_NONE) {

    Serial.print(
      "❌ setOutputPower failed, code = "
    );

    Serial.println(state);

    return false;
  }


  return true;
}


// ============================================================
// Start RX
// ============================================================
//
// IMPORTANT:
//
// radioOperation is changed to RX BEFORE startReceive().
//
// Therefore, if DIO1 fires immediately after startReceive(),
// the ISR records that event as an RX event.
//
// The transition is protected against the main-loop side of
// the race.
//
// ============================================================

static int startReceiveSafely()
{
  int state;


  // There must not be an old unprocessed event when we start
  // a new RX operation.
  //
  // The caller is expected to have processed the old operation.

  if (radioEventPending()) {

    Serial.println(
      "⚠️ Cannot start RX: radio IRQ event still pending."
    );

    return RADIOLIB_ERR_UNKNOWN;
  }


  // Put the logical state into RX BEFORE touching the radio.

  setRadioOperation(RADIO_RX);

  transmitFlag = false;


  state = radio.startReceive();


  if (state != RADIOLIB_ERR_NONE) {

    // startReceive failed, therefore RX is not active.

    setRadioOperation(RADIO_IDLE);

    Serial.print(
      "⚠️ radio.startReceive() failed, code = "
    );

    Serial.println(state);

    return state;
  }


  return RADIOLIB_ERR_NONE;
}


// ============================================================
// Prepare transition from RX to TX
// ============================================================
//
// This function first makes absolutely sure that RX is stopped.
//
// This is important because startTransmit() must never be
// launched while the SX1262 is still logically considered RX.
//
// ============================================================

static bool prepareForTransmit()
{
  RadioOperation current = getRadioOperation();


  if (current == RADIO_TX) {

    Serial.println(
      "⚠️ TX requested while already transmitting."
    );

    return false;
  }


  // ----------------------------------------------------------
  // If currently receiving, finish RX first.
  // ----------------------------------------------------------

  if (current == RADIO_RX) {

    int state = radio.finishReceive();

    if (state != RADIOLIB_ERR_NONE) {

      Serial.print(
        "⚠️ finishReceive() returned "
      );

      Serial.println(state);

      // Do not immediately assume total radio failure.
      //
      // Put the logical state into IDLE before attempting
      // recovery.

      setRadioOperation(RADIO_IDLE);

      return false;
    }

    setRadioOperation(RADIO_IDLE);
  }


  // ----------------------------------------------------------
  // There must be no old RX/TX event left over.
  // ----------------------------------------------------------

  clearRadioEvents();


  return true;
}


// ============================================================
// Start own GPS transmission
// ============================================================
//
// The important race fix here is:
//
//     RX
//      |
//      | finishReceive()
//      v
//     IDLE
//      |
//      | startTransmit()
//      v
//     TX
//
// The ISR can therefore never report a newly-created TX event
// as an RX event, or vice versa.
// ============================================================

static bool startOwnTransmission()
{
  if (!prepareForTransmit()) {

    return false;
  }


  // ----------------------------------------------------------
  // Tell the ISR that the upcoming radio operation is TX
  // BEFORE startTransmit() is called.
  // ----------------------------------------------------------

  setRadioOperation(RADIO_TX);

  transmitFlag = true;


  // ----------------------------------------------------------
  // Parse GPS and start transmission.
  //
  // prepareAndSendOwnInfo() calls radio.startTransmit().
  // ----------------------------------------------------------

  GpsInfo own =
      prepareAndSendOwnInfo(
        radio,
        transmissionState,
        transmitFlag
      );


  if (transmissionState != RADIOLIB_ERR_NONE) {

    Serial.print(
      "❌ startTransmit failed, code = "
    );

    Serial.println(transmissionState);


    // TX did not actually start.

    setRadioOperation(RADIO_IDLE);

    transmitFlag = false;

    return false;
  }


  // ----------------------------------------------------------
  // Calculate and print navigation information.
  // ----------------------------------------------------------

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


    Serial.print(d, 6);
    Serial.print(", ");

    Serial.print(b, 6);
    Serial.print(", ");

    Serial.print(own.fixType);
    Serial.print(", ");

    Serial.print(companion.fixType);
    Serial.print(", ");

    Serial.print(
      own.valid ? "true" : "false"
    );

    Serial.print(", ");

    Serial.println(
      companion.valid ? "true" : "false"
    );
  }


  return true;
}


// ============================================================
// Schedule next maintenance TX
// ============================================================

static void scheduleMaintenanceTransmission()
{
#if defined(INITIATING_NODE)

  nextMaintenanceTxMs =
      millis() +
      LINK_RECOVERY_INTERVAL_INITIATOR_MS;

#else

  nextMaintenanceTxMs =
      millis() +
      LINK_RECOVERY_INTERVAL_OTHER_MS;

#endif
}


// ============================================================
// HARD RADIO REINITIALIZATION
// ============================================================

static bool hardRadioReinit()
{
  Serial.println(
    "⚠️ SX1262 hard recovery..."
  );


  // ----------------------------------------------------------
  // Prevent ISR events from being interpreted during recovery.
  // ----------------------------------------------------------

  portENTER_CRITICAL(&radioStateMux);

  radioOperation = RADIO_IDLE;
  irqEventCount = 0;

  portEXIT_CRITICAL(&radioStateMux);

  transmitFlag = false;


  // ----------------------------------------------------------
  // Put chip into reset state.
  // ----------------------------------------------------------

  int state = radio.reset();

  if (state != RADIOLIB_ERR_NONE) {

    Serial.print(
      "⚠️ radio.reset() returned "
    );

    Serial.println(state);
  }


  delay(50);


  // ----------------------------------------------------------
  // Reinitialize chip.
  // ----------------------------------------------------------

  state = radio.begin(LORA_FREQ);

  if (state != RADIOLIB_ERR_NONE) {

    Serial.print(
      "❌ radio.begin() failed, code = "
    );

    Serial.println(state);

    return false;
  }


  // ----------------------------------------------------------
  // IMPORTANT:
  // radio.begin() restores the physical chip but our desired
  // LoRa configuration must be applied again.
  // ----------------------------------------------------------

  if (!configureRadio()) {

    Serial.println(
      "❌ LoRa configuration after recovery failed."
    );

    return false;
  }


  // ----------------------------------------------------------
  // Reinstall interrupt handler.
  // ----------------------------------------------------------

  radio.setDio1Action(setFlag);


  delay(10);


  // ----------------------------------------------------------
  // Clear any stale software event before RX starts.
  // ----------------------------------------------------------

  clearRadioEvents();


  // ----------------------------------------------------------
  // Start fresh RX.
  // ----------------------------------------------------------

  state = startReceiveSafely();

  if (state != RADIOLIB_ERR_NONE) {

    Serial.print(
      "❌ RX restart after hard recovery failed, code = "
    );

    Serial.println(state);

    return false;
  }


  radioHealthFailures = 0;

  nextHealthCheckMs =
      millis() +
      RADIO_HEALTH_CHECK_INTERVAL_MS;


  Serial.println(
    "✅ SX1262 hard recovery successful."
  );


  return true;
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);


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

  Serial.println(
    "SX126x PingPong starting..."
  );


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

    Serial.print(
      "radio.begin() failed, code = "
    );

    Serial.println(state);


    while (true) {
      delay(1000);
    }
  }


  Serial.println(
    "✅ Radio init OK"
  );


  // ==========================================================
  // LoRa settings
  // ==========================================================

  if (!configureRadio()) {

    Serial.println(
      "❌ LoRa configuration failed."
    );


    while (true) {
      delay(1000);
    }
  }


  Serial.println(
    "✅ LoRa settings: SF10, BW125, CR4/7, TX14dBm"
  );


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

  // ----------------------------------------------------------
  // Initiator sends first packet.
  // ----------------------------------------------------------

  Serial.print(
    F("[SX1262] Sending first packet ... ")
  );


  if (!startOwnTransmission()) {

    Serial.println(
      F("failed")
    );


    if (!startReceiveSafely()) {

      hardRadioReinit();
    }

  } else {

    Serial.println(
      F("started")
    );
  }


#else

  // ----------------------------------------------------------
  // Other node starts in RX.
  // ----------------------------------------------------------

  Serial.print(
    F("[SX1262] Starting to listen ... ")
  );


  state =
      startReceiveSafely();


  if (state == RADIOLIB_ERR_NONE) {

    Serial.println(
      F("success!")
    );

  } else {

    Serial.print(
      F("failed, code ")
    );

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

  nextMaintenanceTxMs =
      millis() +
      LINK_RECOVERY_INTERVAL_INITIATOR_MS;

#else

  nextMaintenanceTxMs =
      millis() +
      LINK_RECOVERY_INTERVAL_OTHER_MS;

#endif
}


// ============================================================
// HANDLE TX COMPLETE
// ============================================================
//
// Called only when the IRQ event was captured while the radio
// was in RADIO_TX.
//
// ============================================================

static void handleTxEvent()
{
  // ----------------------------------------------------------
  // First change logical state to IDLE.
  //
  // This prevents the ISR from associating another event with
  // TX while finishTransmit() is executing.
  // ----------------------------------------------------------

  setRadioOperation(RADIO_IDLE);


  int state =
      radio.finishTransmit();


  transmitFlag = false;


  if (state != RADIOLIB_ERR_NONE) {

    Serial.print(
      "⚠️ finishTransmit() failed, code = "
    );

    Serial.println(state);


    // The TX operation failed.
    //
    // First attempt normal RX restart.

    clearRadioEvents();


    if (
      startReceiveSafely()
      != RADIOLIB_ERR_NONE
    ) {

      Serial.println(
        "⚠️ RX after TX failure failed."
      );

      hardRadioReinit();
    }


    return;
  }


  // ----------------------------------------------------------
  // TX completed normally.
  // ----------------------------------------------------------

  clearRadioEvents();


  state =
      startReceiveSafely();


  if (state != RADIOLIB_ERR_NONE) {

    Serial.print(
      "⚠️ RX after TX failed, code = "
    );

    Serial.println(state);


    hardRadioReinit();
  }
}


// ============================================================
// HANDLE RX COMPLETE
// ============================================================
//
// Called only when the IRQ event was captured while the radio
// was in RADIO_RX.
//
// ============================================================

static void handleRxEvent()
{
  // ----------------------------------------------------------
  // Atomically move logical state away from RX before touching
  // the radio.
//
// This is the other important half of the race fix.
//
// Once we begin processing this RX event, a new interrupt cannot
// be interpreted as another RX operation simply because the old
// state was still RX.
// ----------------------------------------------------------

  setRadioOperation(RADIO_IDLE);


  // ----------------------------------------------------------
  // Read received packet.
  // ----------------------------------------------------------

  String str;


  int state =
      radio.readData(str);


  if (state == RADIOLIB_ERR_NONE) {

    // --------------------------------------------------------
    // REAL PACKET RECEIVED
    // --------------------------------------------------------

    lastValidPacketMs =
        millis();

    radioHealthFailures = 0;


    if (
      UbloxHelper_parseGpsPayload(
        str,
        companion
      )
    ) {

      haveCompanionFix = true;


      Serial.println(
        "✅ Companion packet received."
      );

    } else {

      Serial.println(
        "❌ Failed to parse companion payload"
      );
    }

  } else {

    // --------------------------------------------------------
    // RX ERROR
    //
    // This does NOT mean that the radio has stalled.
    //
    // It can simply be a bad packet/CRC/etc.
    // --------------------------------------------------------

    Serial.print(
      "⚠️ readData failed, code = "
    );

    Serial.println(state);
  }


  // ----------------------------------------------------------
  // Return to RX.
  // ----------------------------------------------------------

  clearRadioEvents();


  state =
      startReceiveSafely();


  if (state != RADIOLIB_ERR_NONE) {

    Serial.print(
      "⚠️ RX restart failed, code = "
    );

    Serial.println(state);


    hardRadioReinit();
  }
}


// ============================================================
// LOOP
// ============================================================

void loop()
{
  uint32_t now =
      millis();


  // ==========================================================
  // 1. RADIO IRQ EVENTS
  // ==========================================================
  //
  // Consume one event at a time.
  //
  // The operation associated with the IRQ was captured by the
  // ISR, so we do NOT infer it from the CURRENT radio state.
  //
  // This is the central race fix.
  //
  // ==========================================================

  RadioOperation eventOperation;


  if (takeRadioEvent(eventOperation)) {

    // --------------------------------------------------------
    // TX event
    // --------------------------------------------------------

    if (eventOperation == RADIO_TX) {

      // Only process as TX if this event really belongs to TX.
      handleTxEvent();
    }


    // --------------------------------------------------------
    // RX event
    // --------------------------------------------------------

    else if (eventOperation == RADIO_RX) {

      // Only process as RX if the ISR captured RX.
      handleRxEvent();
    }


    // --------------------------------------------------------
    // Unexpected event
    // --------------------------------------------------------

    else {

      Serial.println(
        "⚠️ Radio IRQ received while state was IDLE."
      );


      // Do not blindly interpret it.
      //
      // First ensure the radio has a clean RX state.

      setRadioOperation(RADIO_IDLE);

      clearRadioEvents();


      if (
        startReceiveSafely()
        != RADIOLIB_ERR_NONE
      ) {

        hardRadioReinit();
      }
    }
  }


  // ==========================================================
  // 2. LINK MAINTENANCE TRANSMISSION
  // ==========================================================
  //
  // This is NOT a radio watchdog.
  //
  // If both devices are sitting in RX because the link was lost,
  // eventually one device transmits again.
  //
  // This prevents the classic:
  //
  //     RX <----> RX
  //
  // deadlock.
  //
  // ==========================================================

  if (
    getRadioOperation() == RADIO_RX &&
    !radioEventPending() &&
    (int32_t)(
      now - nextMaintenanceTxMs
    ) >= 0
  ) {

    uint32_t silentTime =
        now - lastValidPacketMs;


    if (
      silentTime <
      LINK_ACTIVITY_GRACE_MS
    ) {

      // A packet arrived recently.
      // No need to transmit maintenance traffic.

      scheduleMaintenanceTransmission();

    } else {

      Serial.println(
        "ℹ️ Link maintenance TX: no recent packet."
      );


      if (!startOwnTransmission()) {

        Serial.println(
          "⚠️ Maintenance TX could not be started."
        );


        // Make sure RX is restored.

        if (
          getRadioOperation() ==
          RADIO_IDLE
        ) {

          clearRadioEvents();

          if (
            startReceiveSafely()
            != RADIOLIB_ERR_NONE
          ) {

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
  //
  // NO packet != radio failure.
  //
  // We only use this check to detect an SX1262 which no longer
  // responds correctly over SPI.
  //
  // ==========================================================

  if (
    getRadioOperation() == RADIO_RX &&
    !radioEventPending() &&
    (int32_t)(
      now - nextHealthCheckMs
    ) >= 0
  ) {

    nextHealthCheckMs =
        now +
        RADIO_HEALTH_CHECK_INTERVAL_MS;


    uint32_t irqFlags =
        radio.getIrqFlags();


    if (
      irqFlags ==
      0xFFFFFFFFUL
    ) {

      radioHealthFailures++;


      Serial.print(
        "⚠️ SX1262 health check failed: "
      );

      Serial.println(
        radioHealthFailures
      );


      if (
        radioHealthFailures >=
        RADIO_MAX_HEALTH_FAILURES
      ) {

        radioHealthFailures = 0;

        hardRadioReinit();
      }

    } else {

      // SX1262 responds over SPI.
      //
      // A silent channel is therefore not considered a failure.

      radioHealthFailures = 0;
    }
  }


  // ==========================================================
  // 4. IDLE SAFETY NET
  // ==========================================================
  //
  // The radio should normally never remain IDLE for long.
  //
  // However, if a transition failed, restore RX.
  //
  // ==========================================================

  if (
    getRadioOperation() == RADIO_IDLE &&
    !radioEventPending()
  ) {

    delay(1);


    if (
      startReceiveSafely()
      != RADIOLIB_ERR_NONE
    ) {

      Serial.println(
        "⚠️ Idle -> RX failed."
      );


      hardRadioReinit();
    }
  }


  // ==========================================================
  // 5. Yield
  // ==========================================================

  delay(1);
}
```