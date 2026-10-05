#pragma once

// Screen renderer.  Pure drawing code: it only uses the U8g2 C API and libc,
// so the same file runs on the ESP32 and in the host-side preview tool
// (tools/ui_preview) that renders frames to PNG for layout work.

#include "app_model.h"

#include <clib/u8g2.h>

// Logical drawing surface: 400 x 300, 1 = ink (black), 0 = paper (white).
#define UI_WIDTH 400
#define UI_HEIGHT 300

// Draws one complete frame described by `model`.  The caller clears the buffer
// beforehand and sends it to the panel afterwards.
void uiDraw(u8g2_t *u8g2, const UiModel &model);

// The "battery empty" screen shown when the clock switches itself off to protect the cell: how
// far the battery is, the level it restarts at, and (offerOverride) the hold-KEY way round it.
void uiDrawBatteryEmpty(u8g2_t *u8g2, float volts, float restartVolts, bool offerOverride);

// The firmware-update screen: a chip, a heading, what is going on, a progress bar and the file.
void uiDrawFirmwareUpdate(u8g2_t *u8g2, const UiFwScreen &screen);
