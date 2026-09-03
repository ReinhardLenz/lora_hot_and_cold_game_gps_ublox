#ifndef COMPASS_H
#define COMPASS_H

#include <Arduino.h>
#include <Adafruit_BNO08x.h>

struct euler_t {
  float yaw;
  float pitch;
  float roll;
};

class Compass
{
public:
  explicit Compass(Adafruit_BNO08x& imu);

  void processSensor();

  // Keep this helper since you're using it from main.cpp
  static void setReports(Adafruit_BNO08x* imu, sh2_SensorId_t reportType, long report_interval);

  // Make yaw available to main.cpp (degrees)
  float getYawDeg() const { return ypr_1.yaw; }
  float getYawNorthDeg() const;

  private:
euler_t ypr_1{};

  Adafruit_BNO08x& m_imu;
  sh2_SensorValue_t sensorValue_1{};

  static void quaternionToEulerRV(sh2_RotationVectorWAcc_t* rotational_vector, euler_t* ypr, bool degrees);
  static void quaternionToEuler(float qr, float qi, float qj, float qk, euler_t* ypr, bool degrees);

  static float getNorthDirection(float yaw);
};

#endif