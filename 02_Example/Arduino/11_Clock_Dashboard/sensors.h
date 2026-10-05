#pragma once

// Board sensors: SHTC3 temperature/humidity, PCF85063 real-time clock and the
// battery voltage divider.  Everything here is called from the UI task only
// (it owns the I2C bus).

#include <Arduino.h>
#include <time.h>

struct IndoorReading {
  bool valid = false;
  float tempC = 0;     // corrected for self-heating (INDOOR_TEMP_OFFSET_C)
  float rh = 0;        // corrected to match
  float rawTempC = 0;  // as measured
};

struct BatteryReading {
  bool present = false;
  float volts = 0;
  int percent = 0;
};

// Starts the I2C bus.  Returns true when the SHTC3 answered.
bool sensorsBegin();

// One blocking measurement (about 15 ms).  Returns false on a bus or CRC error.
bool readIndoor(IndoorReading &out);

// Average of several ADC samples, converted through the 1:3 divider.
BatteryReading readBattery();

// Hardware RTC, kept in UTC.  rtcReadUtc() is false when the clock has never
// been set or lost power (the chip's oscillator-stop flag).
bool rtcReadUtc(time_t *utc);
bool rtcWriteUtc(time_t utc);
