#include "NavigationMath.h"
#include <math.h>

static inline double deg2rad(double deg) { return deg * (M_PI / 180.0); }
static inline double rad2deg(double rad) { return rad * (180.0 / M_PI); }

double distanceMeters(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371000.0; // Earth radius in meters

  double phi1 = deg2rad(lat1);
  double phi2 = deg2rad(lat2);
  double dphi = deg2rad(lat2 - lat1);
  double dlambda = deg2rad(lon2 - lon1);

  double a = sin(dphi / 2.0) * sin(dphi / 2.0) +
             cos(phi1) * cos(phi2) *
             sin(dlambda / 2.0) * sin(dlambda / 2.0);

  double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
  return R * c;
}

double bearingDegrees(double lat1, double lon1, double lat2, double lon2) {
  double phi1 = deg2rad(lat1);
  double phi2 = deg2rad(lat2);
  double dlambda = deg2rad(lon2 - lon1);

  double y = sin(dlambda) * cos(phi2);
  double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dlambda);

  double theta = atan2(y, x);
  double brng = rad2deg(theta);

  // normalize to [0, 360)
  brng = fmod((brng + 360.0), 360.0);
  return brng;
}