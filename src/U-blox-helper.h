#pragma once
#include <Arduino.h>
#include "SendOwnInfo.h"   // for GpsInfo

// Result of UBX ACK waiting
enum AckResult { ACK_OK, ACK_NAK, ACK_TIMEOUT };

// Provide the GPS serial instance to this helper module (call once in setup)
void UbloxHelper_begin(HardwareSerial& gpsSerial);

// Flush pending bytes from GPS UART for a given time window
void UbloxHelper_flushGpsInput(uint32_t ms);

// Configure u-blox: disable NMEA sentences and enable UBX-NAV-PVT on UART1
// Returns true if NAV-PVT enable got ACK_OK (best-effort; NMEA disables are also attempted).
bool UbloxHelper_configureUbxOnlyNavPvt();

// Parse received LoRa payload into the unified struct (GpsInfo)
bool UbloxHelper_parseGpsPayload(const String& str, GpsInfo& in);