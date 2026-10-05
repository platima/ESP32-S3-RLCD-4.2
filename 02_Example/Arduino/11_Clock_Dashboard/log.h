#pragma once

// Minimal serial logging: LOGF("tag", "format %d", value)

#include <Arduino.h>

#define LOGF(tag, fmt, ...) Serial.printf("[%8lu] %-8s " fmt "\n", (unsigned long)millis(), tag, ##__VA_ARGS__)
