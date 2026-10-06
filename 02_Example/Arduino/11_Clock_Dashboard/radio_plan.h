#pragma once

// When the radio is on, in "wifi_mode = sync" (and the CPU clock policy that goes with it).
//
// In sync mode the radio is off between "sessions".  A session starts when something needs the
// network (the time, the place, the weather, a look at Spotify, a key press), lasts while there
// is something to do, and ends a moment after the last thing was done.  The network task works out
// what is due and tells the planner; the planner says whether the radio should be on, and keeps
// the books on failures: a network that cannot be joined is retried after 1, 2, 5, 10 and then 15
// minutes, so a clock in a house with no WiFi does not scan all day.  A key press may start a
// session anyway (the person wants an answer): it holds the radio for 45 seconds, which bounds the effort.
//
// Pure logic with no Arduino dependency: the times are passed in, tools/tests drives it with a
// simulated day.

#include <stdint.h>

namespace radioplan {

const uint32_t kConnectTimeoutMs = 45000;  // a session that is not online by now is given up (main 20 s, backup 20 s, and a margin)
const uint32_t kSettleMs = 2500;           // the radio stays on this long after the last thing was done (sockets close, a command is confirmed)
const uint32_t kDemandMs = 45000;          // a key press holds the radio for this long (follow-up presses, the confirming poll)
const uint32_t kSessionMaxMs = 180000;     // work that is still "due" after this long, with nothing holding the radio, ends the session as a failure
const int kBackoffSteps = 5;
const uint32_t kBackoffMs[kBackoffSteps] = {60000UL, 120000UL, 300000UL, 600000UL, 900000UL};  // after the 1st, 2nd, ... failed session in a row

// The times a person's key press holds the radio (`netWake()` records `demandUntil(now)`).
inline uint32_t demandUntil(uint32_t nowMs) { return nowMs + kDemandMs; }
inline bool demandActive(uint32_t nowMs, uint32_t untilMs) { return untilMs != 0 && (int32_t)(untilMs - nowMs) > 0; }
// How long to wait before trying a piece of work again (the weather) after it failed `streak` times in a
// row while the radio was joined: 1, 2, 5, 10 minutes and then every 15.
inline uint32_t retryAfterMs(int streak) {
  const int step = streak < 1 ? 0 : (streak > kBackoffSteps ? kBackoffSteps - 1 : streak - 1);
  return kBackoffMs[step];
}

// A session that is going on anyway also asks for the network time when it is within kNtpEarlyMs of being
// due, and the time only starts a session of its own when it is kNtpGraceMs overdue: the sessions come every
// quarter of an hour for the weather, and a separate session just for the time would cost more than a
// few minutes early or late.
const uint32_t kNtpEarlyMs = 10UL * 60UL * 1000UL;
const uint32_t kNtpGraceMs = 5UL * 60UL * 1000UL;

// Is a time (millis) due?  Wrap-safe.
inline bool due(uint32_t nowMs, uint32_t atMs) { return (int32_t)(nowMs - atMs) >= 0; }

// What the network task knows, every time round its loop.
struct Inputs {
  bool online = false;       // associated, with an address
  bool workDue = false;      // the time, the place, the weather or a look at Spotify is due
  bool holdSpotify = false;  // music is playing, a command waits, it was playing a minute ago, the Now Playing page is open
  bool demand = false;       // a key press (or a refresh) asked for the radio and its time has not run out
};

class Planner {
 public:
  enum State : uint8_t { OFF, CONNECTING, ONLINE };

  // `sync` false: the radio is always on and the planner only mirrors it.  `connectTimeoutMs`: how long a
  // session may take to join a network before it is given up (long enough to try both networks if there
  // are two, see kConnectTimeoutMs).
  explicit Planner(bool sync = true, uint32_t connectTimeoutMs = kConnectTimeoutMs) : sync_(sync), connectTimeoutMs_(connectTimeoutMs) {
    if (!sync) state_ = CONNECTING;
  }

  // Should the radio be on?  Call it every time round the loop.
  bool update(uint32_t nowMs, const Inputs &in) {
    accountOnTime(nowMs);
    if (!sync_) {
      state_ = in.online ? ONLINE : CONNECTING;
      return true;
    }
    const bool wants = in.workDue || in.holdSpotify || in.demand;
    switch (state_) {
      case OFF:
        if (wants) {
          const bool backingOff = (int32_t)(nowMs - retryAtMs_) < 0;
          if (!backingOff || in.demand) start(nowMs);  // (a key press does not wait for the back-off)
        }
        break;
      case CONNECTING:
        if (in.online) {
          state_ = ONLINE;
          onlineSinceMs_ = nowMs;
          lastActiveMs_ = nowMs;
          failures_ = 0;
        } else if ((uint32_t)(nowMs - sessionStartMs_) >= connectTimeoutMs_) {
          giveUp(nowMs);
        }
        break;
      case ONLINE:
        if (!in.online) {  // dropped: join again (the time allowed starts over)
          state_ = CONNECTING;
          sessionStartMs_ = nowMs;
          break;
        }
        if (wants) lastActiveMs_ = nowMs;
        if (!in.holdSpotify && !in.demand && (uint32_t)(nowMs - sessionStartMs_) >= kSessionMaxMs && in.workDue) {
          giveUp(nowMs);  // work that never gets done must not keep the radio on for ever
        } else if ((uint32_t)(nowMs - lastActiveMs_) >= kSettleMs) {
          state_ = OFF;  // everything is done
          retryAtMs_ = 0;
          sessions_++;
        }
        break;
    }
    return state_ != OFF;
  }

  State state() const { return state_; }
  bool radioWanted() const { return state_ != OFF; }

  // True once after a session was given up (the radio was not joined in time, or work never finished).
  bool takeGaveUp() {
    const bool g = gaveUp_;
    gaveUp_ = false;
    return g;
  }
  // When the next session may start although something is due (0 = now); only means something while OFF.
  uint32_t retryAtMs() const { return retryAtMs_; }
  int failures() const { return failures_; }
  uint32_t sessions() const { return sessions_; }

  // Milliseconds the radio was on (CONNECTING and ONLINE) up to `nowMs`.
  uint32_t radioOnMs(uint32_t nowMs) {
    accountOnTime(nowMs);
    return onMs_;
  }

 private:
  void start(uint32_t nowMs) {
    state_ = CONNECTING;
    sessionStartMs_ = nowMs;
  }
  void giveUp(uint32_t nowMs) {
    if (failures_ < 1000) failures_++;
    const int step = failures_ > kBackoffSteps ? kBackoffSteps - 1 : failures_ - 1;
    retryAtMs_ = nowMs + kBackoffMs[step];
    if (retryAtMs_ == 0) retryAtMs_ = 1;
    state_ = OFF;
    gaveUp_ = true;
  }
  void accountOnTime(uint32_t nowMs) {
    if (started_ && state_ != OFF) onMs_ += (uint32_t)(nowMs - lastTickMs_);
    lastTickMs_ = nowMs;
    started_ = true;
  }

  bool sync_;
  uint32_t connectTimeoutMs_;
  State state_ = OFF;
  uint32_t sessionStartMs_ = 0, onlineSinceMs_ = 0, lastActiveMs_ = 0;
  uint32_t retryAtMs_ = 0;
  bool gaveUp_ = false;
  int failures_ = 0;
  uint32_t sessions_ = 0;
  uint32_t onMs_ = 0, lastTickMs_ = 0;
  bool started_ = false;
};

}  // namespace radioplan
