#include "power.h"

#include <Adafruit_INA219.h>
#include <Wire.h>

#include "battery_curve.h"
#include "config.h"

namespace Power {

namespace {
Adafruit_INA219 ina219(INA219_I2C_ADDRESS);
bool sensorAvailable = false;
uint16_t lastMillivolts = 0;
uint32_t lastReadMs = 0;
bool hasEverRead = false;
// Latched by isLow() with hysteresis (BATTERY_LOW_MV / BATTERY_LOW_CLEAR_MV).
bool lowLatched = false;
} // namespace

bool begin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  sensorAvailable = ina219.begin(&Wire);
  if (!sensorAvailable) {
    Serial.println("Power: INA219 nicht gefunden -- Akkuanzeige deaktiviert.");
  }
  return sensorAvailable;
}

bool available() { return sensorAvailable; }

uint16_t batteryMillivolts() {
  if (!sensorAvailable) {
    return 0;
  }
  // Throttled: this is called every loop() iteration (display refresh) plus
  // every heartbeat -- without the cache each call would be a full I2C
  // transaction on the bus shared with the OLED.
  uint32_t now = millis();
  if (!hasEverRead || now - lastReadMs >= BATTERY_READ_INTERVAL_MS) {
    float busVoltage = ina219.getBusVoltage_V(); // load-side voltage ~= battery voltage
    lastMillivolts = static_cast<uint16_t>(busVoltage * 1000.0f);
    lastReadMs = now;
    hasEverRead = true;
  }
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

} // namespace Power
