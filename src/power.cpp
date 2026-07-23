#include "power.h"

#include <Adafruit_INA219.h>
#include <Wire.h>
#include <math.h>

#include "battery_curve.h"
#include "charging_decision.h"
#include "config.h"

namespace Power {

namespace {
Adafruit_INA219 ina219(INA219_I2C_ADDRESS);
bool sensorAvailable = false;
uint16_t lastMillivolts = 0;
int16_t lastCurrentMa = 0;
uint32_t lastReadMs = 0;
bool hasEverRead = false;
// Latched by isLow() with hysteresis (BATTERY_LOW_MV / BATTERY_LOW_CLEAR_MV).
bool lowLatched = false;
// Hysteresis + dwell-time latch for isCharging() -- see charging_decision.h.
ChargingDecisionState chargingState;

// Throttled to BATTERY_READ_INTERVAL_MS: batteryMillivolts()/
// chargeCurrentMilliamps() are called every loop() iteration (display
// refresh) plus every heartbeat -- without the cache each call would be a
// full I2C transaction on the bus shared with the OLED. Reads both values
// together so a single throttle window covers both, rather than doubling the
// I2C traffic for a second independent cache.
void refreshIfDue() {
  uint32_t now = millis();
  if (hasEverRead && now - lastReadMs < BATTERY_READ_INTERVAL_MS) {
    return;
  }
  float busVoltage = ina219.getBusVoltage_V(); // load-side voltage ~= battery voltage
  lastMillivolts = static_cast<uint16_t>(busVoltage * 1000.0f);
  // Adafruit_INA219::begin() calibrates for 32V/2A internally (see init()),
  // so getCurrent_mA() works out of the box -- no extra setCalibration_*()
  // needed for the small currents a USB charger delivers here.
  float currentMa = ina219.getCurrent_mA() * static_cast<float>(INA219_CURRENT_CHARGE_SIGN);
  lastCurrentMa = static_cast<int16_t>(lroundf(currentMa));
  lastReadMs = now;
  hasEverRead = true;
}
} // namespace

bool begin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  sensorAvailable = ina219.begin(&Wire);
  if (!sensorAvailable) {
    Serial.println("Power: INA219 not found -- battery display disabled.");
  }
  return sensorAvailable;
}

bool available() { return sensorAvailable; }

uint16_t batteryMillivolts() {
  if (!sensorAvailable) {
    return 0;
  }
  refreshIfDue();
  return lastMillivolts;
}

uint8_t batteryPercent() {
  uint16_t mv = batteryMillivolts();
  if (!sensorAvailable || mv == 0) {
    return 0;
  }
  return batteryPercentFromMillivolts(mv);
}

bool isLow() {
  uint16_t mv = batteryMillivolts();
  if (!sensorAvailable || mv == 0) {
    return false;
  }
  if (lowLatched) {
    if (mv > BATTERY_LOW_CLEAR_MV) {
      lowLatched = false;
    }
  } else if (mv < BATTERY_LOW_MV) {
    lowLatched = true;
  }
  return lowLatched;
}

int16_t chargeCurrentMilliamps() {
  if (!sensorAvailable) {
    return 0;
  }
  refreshIfDue();
  return lastCurrentMa;
}

bool isCharging() {
  if (!sensorAvailable) {
    return false;
  }
  return updateChargingDecision(chargingState, chargeCurrentMilliamps(), millis(),
                                 CHARGING_DETECT_ENTER_MA, CHARGING_DETECT_EXIT_MA,
                                 CHARGING_MIN_DWELL_MS);
}

} // namespace Power
