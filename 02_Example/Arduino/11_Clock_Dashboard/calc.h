#pragma once

// Pure calculations used by the sensor code.  Header-only and free of Arduino
// dependencies so tools/tests can check them on a PC.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

namespace calc {

// ---------------------------------------------------------------------------
// Battery: single-cell Li-ion / 18650 resting voltage -> state of charge.
// Piecewise-linear through a typical discharge curve; far better than a
// straight line because the curve is flat in the middle and falls off a cliff
// near empty.
// ---------------------------------------------------------------------------
struct BatteryPoint {
  float v;
  int pct;
};
inline const BatteryPoint *batteryCurve(int *count) {
  static const BatteryPoint kCurve[] = {
      {4.20f, 100}, {4.15f, 95}, {4.11f, 90}, {4.08f, 85}, {4.02f, 80}, {3.98f, 75}, {3.95f, 70},
      {3.91f, 65},  {3.87f, 60}, {3.85f, 55}, {3.84f, 50}, {3.82f, 45}, {3.80f, 40}, {3.79f, 35},
      {3.77f, 30},  {3.75f, 25}, {3.73f, 20}, {3.71f, 15}, {3.69f, 10}, {3.61f, 5},  {3.27f, 0}};
  *count = (int)(sizeof(kCurve) / sizeof(kCurve[0]));
  return kCurve;
}

// The same, not rounded: the battery-life estimate needs the fractions.
inline float batteryPercentF(float volts) {
  int n;
  const BatteryPoint *c = batteryCurve(&n);
  if (volts >= c[0].v) return 100.0f;
  for (int i = 1; i < n; i++) {
    if (volts >= c[i].v) {
      float span = c[i - 1].v - c[i].v;
      float frac = (volts - c[i].v) / span;
      return c[i].pct + frac * (c[i - 1].pct - c[i].pct);
    }
  }
  return 0.0f;
}

inline int batteryPercent(float volts) { return (int)lroundf(batteryPercentF(volts)); }

// The curve backwards: the voltage at which a cell reads `pct` percent (0..100).
inline float batteryVoltsForPercent(float pct) {
  int n;
  const BatteryPoint *c = batteryCurve(&n);
  if (pct >= 100.0f) return c[0].v;
  if (pct <= 0.0f) return c[n - 1].v;
  for (int i = 1; i < n; i++) {
    if (pct >= (float)c[i].pct) {
      float frac = (pct - (float)c[i].pct) / (float)(c[i - 1].pct - c[i].pct);
      return c[i].v + frac * (c[i - 1].v - c[i].v);
    }
  }
  return c[n - 1].v;
}

// ---------------------------------------------------------------------------
// SHTC3 (Sensirion): CRC-8, polynomial 0x31, init 0xFF.
// Datasheet check value: crc8({0xBE, 0xEF}) == 0x92.
// ---------------------------------------------------------------------------
inline uint8_t crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

// Decodes a 6 byte "temperature first" measurement: T msb, T lsb, crc, RH msb,
// RH lsb, crc.  Returns false on a CRC mismatch.
inline bool shtc3Decode(const uint8_t b[6], float *tempC, float *rhPercent) {
  if (crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) return false;
  uint16_t rawT = (uint16_t)((b[0] << 8) | b[1]);
  uint16_t rawH = (uint16_t)((b[3] << 8) | b[4]);
  *tempC = -45.0f + 175.0f * (float)rawT / 65536.0f;
  *rhPercent = 100.0f * (float)rawH / 65536.0f;
  return true;
}

// ---------------------------------------------------------------------------
// Humidity correction.  Relative humidity depends on temperature: if the sensor
// is warmer than the room, it under-reports RH.  Keeping the water vapour
// pressure fixed (Magnus formula) gives the RH the room air really has.
// ---------------------------------------------------------------------------
inline float saturationVapourPressure(float tempC) {  // hPa
  return 6.112f * expf(17.62f * tempC / (243.12f + tempC));
}

inline float humidityAtTemperature(float rhMeasured, float measuredC, float actualC) {
  float rh = rhMeasured * saturationVapourPressure(measuredC) / saturationVapourPressure(actualC);
  if (rh < 0.0f) rh = 0.0f;
  if (rh > 100.0f) rh = 100.0f;
  return rh;
}

// ---------------------------------------------------------------------------
// Calendar <-> epoch (UTC) without relying on timegm().
// ---------------------------------------------------------------------------
// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
inline int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

inline int64_t epochFromUtc(int year, int month, int day, int hour, int minute, int second) {
  return daysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

// POSIX TZ string for a fixed UTC offset, e.g. +28800 -> "<+08>-8",
// +19800 -> "<+0530>-5:30", -12600 -> "<-0330>3:30".
inline void posixFromUtcOffset(int offsetSec, char *out, size_t cap) {
  int a = offsetSec < 0 ? -offsetSec : offsetSec;
  int hh = (a / 3600) % 100, mm = (a % 3600) / 60;
  char sign = offsetSec < 0 ? '-' : '+';
  const char *posixSign = offsetSec > 0 ? "-" : "";  // POSIX counts hours *west* of UTC
  if (mm) {
    snprintf(out, cap, "<%c%02d%02d>%s%d:%02d", sign, hh, mm, posixSign, hh, mm);
  } else {
    snprintf(out, cap, "<%c%02d>%s%d", sign, hh, posixSign, hh);
  }
}

}  // namespace calc
