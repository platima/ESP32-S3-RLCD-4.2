#pragma once

// When to draw the coming second and when to send it to the panel.
//
// Drawing a frame takes tens of milliseconds and the time varies (cache misses, the
// WiFi stack taking the memory bus, a flash write on the other core), while sending
// it is short.  So the coming second is drawn into the display buffer early, with
// room to spare, and sent just before it starts: the draw may run long without the
// digits changing late.  The panel refreshes itself at about 25 Hz, so a frame shows
// up 0-39 ms after it has been sent; the send is timed to end `latencyUs` before the
// second starts, which puts the average error at zero.
//
// Pure logic with no Arduino dependency: the UI loop asks next() whenever it wakes up
// and does what it says; tools/tests drives it with simulated clocks and stalls.

#include <stdint.h>
#include <time.h>

class FramePlanner {
 public:
  enum Action : uint8_t {
    IDLE,      // nothing to do yet; slow work may run if quiet() allows
    DRAW_NOW,  // draw `sec`, then send it at once (something has to be on screen now)
    PREPARE,   // draw `sec` into the buffer, but do not send it yet
    SEND,      // wait `waitUs` (at most kSpinUs), then send the prepared frame
    SLEEP,     // the prepared frame is not due for `waitUs`: sleep a little and ask again
  };
  struct Step {
    Action action = IDLE;
    time_t sec = 0;
    int32_t waitUs = 0;
  };

  static constexpr int32_t kSpinUs = 5000;     // waits shorter than this are spun, longer ones slept
  static constexpr int32_t kMarginUs = 60000;  // spare time between a frame being drawn and being due
  static constexpr int32_t kQuietUs = 300000;  // slow work only with more than this left before the tick

  explicit FramePlanner(int32_t latencyUs) : latencyUs_(latencyUs) {}

  // `sec` / `usec`: the clock now.  `force`: something has to be on the glass immediately
  // (a key press, a toast, a page change).
  Step next(time_t sec, int32_t usec, bool force) {
    const int32_t usToNext = 1000000 - usec;
    const int32_t sendLead = (int32_t)sendCostUs_ + latencyUs_;
    const int32_t drawLead = sendLead + (int32_t)drawCostUs_ + kMarginUs;
    Step st;

    if (prepared_ && preparedSec_ > sec + 1) prepared_ = false;  // the clock went backwards: start over

    if (force) {
      // A frame drawn and sent now is on the glass `cost` from now.  Close to a tick there would
      // be no time left afterwards to prepare the next second the usual way, so show the second
      // that is about to start instead (up to about 65 ms early beats a late second).
      const int32_t cost = (int32_t)(drawCostUs_ + sendCostUs_);
      st.action = DRAW_NOW;
      st.sec = usToNext <= 2 * cost + latencyUs_ ? sec + 1 : sec;
      return st;
    }
    if (prepared_) {
      const int32_t waitUs = preparedSec_ > sec ? usToNext - sendLead : 0;  // already due if its second has begun
      st.waitUs = waitUs > 0 ? waitUs : 0;
      st.action = waitUs > kSpinUs ? SLEEP : SEND;
      return st;
    }
    if (lastSentSec_ != sec && lastSentSec_ != sec + 1) {  // behind (start-up, a long stall): catch up at once
      st.action = DRAW_NOW;
      st.sec = sec;
      return st;
    }
    if (usToNext <= drawLead && lastSentSec_ != sec + 1) {
      st.action = PREPARE;
      st.sec = sec + 1;
      return st;
    }
    return st;  // IDLE
  }

  // May slow work (I2C sensors, battery) run now?  Never close to a tick.
  bool quiet(int32_t usec) const { return !prepared_ && 1000000 - usec > kQuietUs; }

  // The caller reports what it did.  `scheduled`: the frame is meant to go out at its precise
  // moment (as opposed to a forced redraw), so its timing is worth recording.
  void drew(time_t sec, uint32_t costUs, bool scheduled) {
    drawCostUs_ = (drawCostUs_ * 3 + clampCost(costUs)) / 4;
    prepared_ = true;
    preparedSec_ = sec;
    preparedScheduled_ = scheduled;
  }
  void sent(uint32_t costUs) {
    sendCostUs_ = (sendCostUs_ * 3 + clampCost(costUs)) / 4;
    lastSentSec_ = preparedSec_;
    prepared_ = false;
  }

  bool prepared() const { return prepared_; }
  time_t preparedSec() const { return preparedSec_; }
  bool preparedScheduled() const { return preparedScheduled_; }
  time_t lastSentSec() const { return lastSentSec_; }
  uint32_t drawCostUs() const { return drawCostUs_; }
  uint32_t sendCostUs() const { return sendCostUs_; }
  int32_t latencyUs() const { return latencyUs_; }

 private:
  // One freak measurement (a stall in the middle of a draw) must not move the schedule for long.
  static uint32_t clampCost(uint32_t us) { return us > 150000 ? 150000 : us; }

  int32_t latencyUs_;
  bool prepared_ = false;
  bool preparedScheduled_ = false;
  time_t preparedSec_ = 0;
  time_t lastSentSec_ = 0;
  uint32_t drawCostUs_ = 30000;
  uint32_t sendCostUs_ = 15000;
};
