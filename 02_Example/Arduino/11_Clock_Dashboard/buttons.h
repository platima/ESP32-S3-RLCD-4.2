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
    // time does not run backwards (a level read just after an interrupt recorded a later change, see edge())
    if (seen_ && (int32_t)(nowMs - lastMs_) < 0) nowMs = lastMs_;
    seen_ = true;
    lastMs_ = nowMs;
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

  // A change of the level that happened at `atMs`, recorded by an interrupt while the loop was too busy to
  // look (a slow frame at the idle clock): time passes with the level as it was, and then it changes.  So a
  // tap that began and ended between two looks still counts, with its real length.
  ClickEvent edge(bool pressed, uint32_t atMs) {
    const ClickEvent before = update(raw_, atMs);
    const ClickEvent after = update(pressed, atMs);
    return before != CLICK_NONE ? before : after;
  }

  // The debounced state of the button, and for how long it has been down (0 when it is up), by the time
  // of the last update.
  bool isDown() const { return stable_; }
  uint32_t heldMs() const { return stable_ ? (uint32_t)(lastMs_ - pressStartMs_) : 0; }

 private:
  uint32_t gapMs_, longMs_, debounceMs_;
  bool seen_ = false;
  uint32_t lastMs_ = 0;
  bool raw_ = false;
  bool stable_ = false;
  bool longFired_ = false;
  uint8_t clicks_ = 0;
  uint32_t rawSinceMs_ = 0;
  uint32_t pressStartMs_ = 0;
  uint32_t lastReleaseMs_ = 0;
};

// A very long hold that acts when the button is let go: "armed" once it has been down for `holdMs` (the moment
// to say so on the screen), "fired" when it then comes up.  The clock restarts on this, and it must not restart
// with the button still down: KEY held while the clock starts means something else.  Feed it the debounced
// state of a ClickDetector (isDown(), heldMs()).
class HoldRelease {
 public:
  enum Event : uint8_t { NONE = 0, ARMED, FIRED };

  explicit HoldRelease(uint32_t holdMs) : holdMs_(holdMs) {}

  Event update(bool down, uint32_t heldMs) {
    if (down) {
      if (armed_ || heldMs < holdMs_) return NONE;
      armed_ = true;
      return ARMED;
    }
    if (!armed_) return NONE;
    armed_ = false;
    return FIRED;
  }

  bool armed() const { return armed_; }

 private:
  uint32_t holdMs_;
  bool armed_ = false;
};
