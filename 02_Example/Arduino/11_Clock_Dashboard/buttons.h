#pragma once

// Debounced multi-click / long-press detector for one active-low button.
// Pure logic with no Arduino dependency (tools/tests drives it with synthetic
// timestamps).  Feed update() the raw pressed/not-pressed level every few
// milliseconds; it returns a gesture once one is complete:
//
//   CLICK_1 / CLICK_2 / CLICK_3   that many clicks, reported once the button has
//                                 been idle for `multiClickGapMs` (3+ clicks = CLICK_3)
//   CLICK_LONG                    held for `longPressMs`, reported while still held

#include <stdint.h>

enum ClickEvent : uint8_t {
  CLICK_NONE = 0,
  CLICK_1 = 1,
  CLICK_2 = 2,
  CLICK_3 = 3,
  CLICK_LONG = 4
};

class ClickDetector {
 public:
  ClickDetector(uint32_t multiClickGapMs, uint32_t longPressMs, uint32_t debounceMs = 25)
      : gapMs_(multiClickGapMs), longMs_(longPressMs), debounceMs_(debounceMs) {}

  ClickEvent update(bool pressed, uint32_t nowMs) {
    // debounce: accept a level once it has been steady for debounceMs_
    if (pressed != raw_) {
      raw_ = pressed;
      rawSinceMs_ = nowMs;
    }
    if (raw_ != stable_ && (uint32_t)(nowMs - rawSinceMs_) >= debounceMs_) {
      stable_ = raw_;
      if (stable_) {  // pressed
        pressStartMs_ = nowMs;
        longFired_ = false;
      } else if (longFired_) {  // released after a long press: nothing more to report
        clicks_ = 0;
      } else {  // released after a short press: count it
        if (clicks_ < 255) clicks_++;
        lastReleaseMs_ = nowMs;
      }
    }

    if (stable_ && !longFired_ && (uint32_t)(nowMs - pressStartMs_) >= longMs_) {
      longFired_ = true;
      clicks_ = 0;
      return CLICK_LONG;
    }
    if (!stable_ && clicks_ > 0 && (uint32_t)(nowMs - lastReleaseMs_) >= gapMs_) {
      uint8_t c = clicks_;
      clicks_ = 0;
      return c >= 3 ? CLICK_3 : (c == 2 ? CLICK_2 : CLICK_1);
    }
    return CLICK_NONE;
  }

 private:
  uint32_t gapMs_, longMs_, debounceMs_;
  bool raw_ = false;
  bool stable_ = false;
  bool longFired_ = false;
  uint8_t clicks_ = 0;
  uint32_t rawSinceMs_ = 0;
  uint32_t pressStartMs_ = 0;
  uint32_t lastReleaseMs_ = 0;
};
