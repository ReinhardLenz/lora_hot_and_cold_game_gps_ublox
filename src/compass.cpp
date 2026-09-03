#include "compass.h"

Compass::Compass(Adafruit_BNO08x& imu)
: m_imu(imu)
{
}

void Compass::setReports(Adafruit_BNO08x* imu, sh2_SensorId_t reportType, long report_interval)
{
  if (!imu->enableReport(reportType, report_interval)) {
    Serial.println("Could not enable report");
  }
}

void Compass::quaternionToEuler(float qr, float qi, float qj, float qk, euler_t* ypr, bool degrees)
{
  float sqr = qr * qr;
  float sqi = qi * qi;
  float sqj = qj * qj;
  float sqk = qk * qk;

  ypr->yaw   = atan2f(2.0f * (qi * qj + qk * qr), (sqi - sqj - sqk + sqr));
  //ypr->pitch = asinf(-2.0f * (qi * qk - qj * qr) / (sqi + sqj + sqk + sqr));
  //ypr->roll  = atan2f(2.0f * (qj * qk + qi * qr), (-sqi - sqj + sqk + sqr));

  if (degrees) {
    ypr->yaw   *= RAD_TO_DEG;
    //ypr->pitch *= RAD_TO_DEG;
    //ypr->roll  *= RAD_TO_DEG;
  }
}

void Compass::quaternionToEulerRV(sh2_RotationVectorWAcc_t* rv, euler_t* ypr, bool degrees)
{
  quaternionToEuler(rv->real, rv->i, rv->j, rv->k, ypr, degrees);
}





float Compass::getNorthDirection(float yaw)
{
  return (yaw < 0) ? (360.0f + yaw) : yaw;
}



float Compass::getYawNorthDeg() const 
{
   return getNorthDirection(ypr_1.yaw); 
}



void Compass::processSensor()
{

  if (m_imu.wasReset())
  {
    Serial.print("sensor was reset ");
    setReports(&m_imu, SH2_ROTATION_VECTOR, 100000);
  }

  if (!m_imu.getSensorEvent(&sensorValue_1))
  {
    return;
  }

  switch (sensorValue_1.sensorId)
  {
    case SH2_ROTATION_VECTOR:
      quaternionToEulerRV(&sensorValue_1.un.rotationVector, &ypr_1, true);
      break;

    default:
      // ignore other reports
      break;
  }

}