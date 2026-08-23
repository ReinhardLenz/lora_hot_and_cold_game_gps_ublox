#pragma once
#include <Arduino.h>

// Great-circle distance (Haversine), returns meters
double distanceMeters(double lat1, double lon1, double lat2, double lon2);

// Initial bearing from point 1 to point 2, returns degrees in [0, 360)
double bearingDegrees(double lat1, double lon1, double lat2, double lon2);