// Host-side tests of the power logic that is not the settings: when the radio is on in "wifi_mode = sync"
// (radio_plan.h), which CPU clock the clock runs at (clock_policy.h), the picker across radio sessions
// (wifi_pick.h) and the frame planner after a clock change (frame_plan.h).  Built and run by run_tests.sh.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "clock_policy.h"
#include "frame_plan.h"
#include "radio_plan.h"
#include "settings.h"
#include "wifi_pick.h"

using namespace radioplan;

namespace {

// A planner driven by hand: `in` is what the network task would report.
struct Drive {
  Planner p;
  Inputs in;
  uint32_t t = 100000;
  explicit Drive(bool sync = true, uint32_t timeout = kConnectTimeoutMs) : p(sync, timeout) {}
  bool step(uint32_t ms) {
    t += ms;
    return p.update(t, in);
  }
};

void testSteps() {
  section("radio plan: one session at a time");
  {  // nothing to do: the radio stays off
    Drive d;
    for (int i = 0; i < 100; i++) CHECK(!d.step(1000));
    CHECK(d.p.state() == Planner::OFF && d.p.sessions() == 0 && d.p.radioOnMs(d.t) == 0);
  }
  {  // always mode: the radio is on whatever it is asked
    Drive d(false);
    CHECK(d.step(10) && d.p.state() == Planner::CONNECTING);
    d.in.online = true;
    CHECK(d.step(10) && d.p.state() == Planner::ONLINE);
    d.in.online = false;
    CHECK(d.step(10) && d.p.state() == Planner::CONNECTING);
  }
  {  // a session: starts at once, goes online, ends 2.5 s after the work is done
    Drive d;
    d.in.workDue = true;
    CHECK(d.step(100) && d.p.state() == Planner::CONNECTING);
    for (int i = 0; i < 30; i++) CHECK(d.step(100));  // 3 s of association
    d.in.online = true;
    CHECK(d.step(100) && d.p.state() == Planner::ONLINE);
    for (int i = 0; i < 50; i++) CHECK(d.step(100));  // 5 s of work
    d.in.workDue = false;
    const uint32_t tDone = d.t;
    bool on = true;
    while (on && d.t - tDone < 10000) on = d.step(100);
    CHECK(!on && d.p.state() == Planner::OFF);
    CHECK(d.t - tDone >= kSettleMs && d.t - tDone <= kSettleMs + 200);  // the settle time, no more, no less
    CHECK(d.p.sessions() == 1 && d.p.failures() == 0 && d.p.retryAtMs() == 0);
    CHECK(d.p.radioOnMs(d.t) > 8000 && d.p.radioOnMs(d.t) < 12000);
  }
  {  // a session holds while Spotify holds it, then settles
    Drive d;
    d.in.holdSpotify = true;
    d.step(100);
    d.in.online = true;
    for (int i = 0; i < 6000; i++) CHECK(d.step(100));  // ten minutes of music
    d.in.holdSpotify = false;
    const uint32_t t0 = d.t;
    bool on = true;
    while (on && d.t - t0 < 10000) on = d.step(100);
    CHECK(!on && d.t - t0 >= kSettleMs && d.t - t0 <= kSettleMs + 200);
  }
  {  // a dropped connection is joined again, with a fresh allowance of time
    Drive d;
    d.in.workDue = true;
    d.step(100);
    d.in.online = true;
    d.step(100);
    d.in.online = false;
    CHECK(d.step(100) && d.p.state() == Planner::CONNECTING);
    for (uint32_t waited = 0; waited < kConnectTimeoutMs - 1000; waited += 1000) CHECK(d.step(1000));
    CHECK(d.p.state() == Planner::CONNECTING && !d.p.takeGaveUp());
    bool on = true;
    for (int i = 0; i < 3 && on; i++) on = d.step(1000);
    CHECK(!on && d.p.takeGaveUp() && d.p.failures() == 1);
  }
}

void testFailures() {
  section("radio plan: a network that cannot be joined");
  Drive d;
  d.in.workDue = true;  // the weather is due and stays due: nothing ever gets done
  const uint32_t expectBackoff[] = {60000, 120000, 300000, 600000, 900000, 900000, 900000};
  for (int round = 0; round < 7; round++) {
    bool on = false;
    const uint32_t t0 = d.t;
    while (!on && d.t - t0 < 2000000) on = d.step(1000);  // (the first session starts at once, the others after the back-off)
    CHECK(on);
    const uint32_t tOn = d.t;
    while (on && d.t - tOn < 120000) on = d.step(1000);
    CHECK(!on && d.p.takeGaveUp());
    CHECK(d.t - tOn >= kConnectTimeoutMs - 1000 && d.t - tOn <= kConnectTimeoutMs + 1000);
    CHECK(d.p.failures() == round + 1);
    CHECK(d.p.retryAtMs() - d.t >= expectBackoff[round] - 1000 && d.p.retryAtMs() - d.t <= expectBackoff[round] + 1000);
    const uint32_t retryAt = d.p.retryAtMs();
    while ((int32_t)(retryAt - d.t) > 1500) CHECK(!d.step(1000));  // it stays off until then
  }
  {  // a key press starts a session in the middle of the back-off (the person wants an answer), and the count goes on
    Drive e;
    e.in.workDue = true;
    e.step(1000);
    bool on = true;
    while (on) on = e.step(1000);  // the first session failed
    CHECK(e.p.failures() == 1);
    e.step(5000);
    CHECK(!e.step(100));  // 5 s into a minute's back-off
    e.in.demand = true;
    CHECK(e.step(100) && e.p.state() == Planner::CONNECTING);
    e.in.demand = false;
    on = true;
    while (on) on = e.step(1000);  // it fails too: two in a row, so the back-off is two minutes
    CHECK(e.p.failures() == 2);
    CHECK(e.p.retryAtMs() - e.t >= 119000 && e.p.retryAtMs() - e.t <= 121000);
    CHECK(!e.step(30000));  // and it is off again, with the work still due
  }
  {  // joining resets the count
    Drive e;
    e.in.workDue = true;
    e.step(1000);
    bool on = true;
    while (on) on = e.step(1000);
    CHECK(e.p.failures() == 1);
    while (!e.step(1000)) {
    }
    e.in.online = true;
    e.step(100);
    CHECK(e.p.failures() == 0);
  }
  {  // work that never gets done, with a joined radio, is a failure after three minutes, not a radio that stays on for ever
    Drive e;
    e.in.workDue = true;
    e.step(100);
    e.in.online = true;
    bool on = true;
    const uint32_t t0 = e.t;
    while (on && e.t - t0 < 400000) on = e.step(1000);
    CHECK(!on && e.p.takeGaveUp());
    CHECK(e.t - t0 >= kSessionMaxMs - 1000 && e.t - t0 <= kSessionMaxMs + 1000);
  }
  {  // ... but not while music holds it
    Drive e;
    e.in.workDue = true;
    e.in.holdSpotify = true;
    e.step(100);
    e.in.online = true;
    for (uint32_t i = 0; i < 400; i++) CHECK(e.step(1000));
  }
}

// A model of the network task's rules in sync mode (net_task.cpp): what is due, how long each thing takes,
// and what a joined radio does about it.  One pass every 100 ms.
struct NetDay {
  Planner p;
  uint32_t connectMs = 3000;  // the router answers this long after the session started
  bool routerUp = true;
  bool internetUp = true;
  bool spotify = false;  // configured and linked
  bool playing = false;  // the music hold
  uint32_t t = 1000;
  uint32_t sessionStart = 0;
  bool radio = false, online = false;
  uint32_t busyUntil = 0;
  // the network task's timers
  bool ntpEver = false;
  uint32_t ntpAt = 0, weatherAt = 0, ntpRetryAt = 0;
  int weatherFails = 0;
  bool peekDone = false;
  uint32_t demandUntilMs = 0;
  // what happened
  uint32_t weatherFetches = 0, ntpSyncs = 0, peeks = 0;
  uint32_t lastWeather = 0, lastNtp = 0, maxWeatherGap = 0, maxNtpGap = 0;
  uint32_t longestSession = 0;
  explicit NetDay(uint32_t timeout = kConnectTimeoutMs) : p(true, timeout) {}

  void press() { demandUntilMs = demandUntil(t); }
  // wakes the radio on its own only when it is overdue by the grace time
  bool ntpDue() const { return (!ntpEver || due(t, ntpAt + kNtpGraceMs)) && due(t, ntpRetryAt); }
  // asked in a session that is going on anyway: due within ten minutes counts
  bool ntpNear() const { return (!ntpEver || due(t, ntpAt - kNtpEarlyMs)) && due(t, ntpRetryAt); }
  bool weatherDue() const { return due(t, weatherAt); }
  bool peekDue() const { return spotify && !peekDone; }

  void tick() {
    t += 100;
    online = radio && routerUp && (uint32_t)(t - sessionStart) >= connectMs;
    Inputs in;
    in.online = online;
    in.workDue = ntpDue() || weatherDue() || peekDue();
    in.holdSpotify = spotify && playing;
    in.demand = demandActive(t, demandUntilMs);
    const bool was = radio;
    radio = p.update(t, in);
    if (radio && !was) {
      sessionStart = t;
      peekDone = false;
    }
    if (!radio && was && (uint32_t)(t - sessionStart) > longestSession) longestSession = t - sessionStart;
    if (!radio) {
      online = false;
      return;
    }
    // what the task does once joined: the work of one loop, in the order of net_task.cpp
    if (online && t >= busyUntil) {
      if (ntpNear()) {
        busyUntil = t + 400;
        if (internetUp) {
          ntpEver = true;
          ntpAt = t + 3600000UL;
          if (lastNtp && t - lastNtp > maxNtpGap) maxNtpGap = t - lastNtp;
          lastNtp = t;
          ntpSyncs++;
        } else {  // no reply: wait twenty seconds, try again in five minutes
          busyUntil = t + 20000;
          ntpRetryAt = t + 20000 + 300000;
        }
      } else if (weatherDue()) {
        busyUntil = t + 2500;
        if (internetUp) {
          weatherAt = t + 15 * 60000UL;
          weatherFails = 0;
          if (lastWeather && t - lastWeather > maxWeatherGap) maxWeatherGap = t - lastWeather;
          lastWeather = t;
          weatherFetches++;
        } else {
          weatherAt = t + retryAfterMs(++weatherFails);  // gently: the radio is dear
        }
      } else if (peekDue()) {
        busyUntil = t + 3000;
        peekDone = true;
        peeks++;
      }
    }
  }
  void run(uint32_t ms) {
    const uint32_t end = t + ms;
    while ((int32_t)(end - t) > 0) tick();
  }
};

void testDay() {
  section("radio plan: a simulated day");
  {  // a quiet day with a good network: a session every quarter of an hour, the radio on for a sliver of it
    NetDay d;
    d.run(24 * 3600000UL);
    const double duty = d.p.radioOnMs(d.t) / (24.0 * 3600000.0);
    if (duty > 0.015) printf("  radio on %.2f %% of the day\n", duty * 100);
    printf("  (a quiet day: radio on %.2f %%, %u sessions, longest %.1f s)\n", duty * 100, (unsigned)d.p.sessions(), d.longestSession / 1000.0);
    CHECK(duty < 0.015);
    CHECK(d.weatherFetches >= 94 && d.weatherFetches <= 98);
    CHECK(d.ntpSyncs >= 23 && d.ntpSyncs <= 26);  // about every hour, in a session that is going on anyway
    CHECK(d.p.sessions() >= 94 && d.p.sessions() <= 104);  // the time sync does not need sessions of its own
    CHECK(d.maxWeatherGap <= 15 * 60000UL + 15000);  // never later than a quarter of an hour and a few seconds
    CHECK(d.maxNtpGap <= 3600000UL + kNtpGraceMs + 30000);  // an hour, and a few minutes when no session comes by
    CHECK(d.longestSession <= 20000);  // a session is seconds, not minutes
    CHECK(d.p.failures() == 0);
  }
  {  // with Spotify linked, each session also looks at it once: a few more seconds, nothing more
    NetDay d;
    d.spotify = true;
    d.run(24 * 3600000UL);
    const double duty = d.p.radioOnMs(d.t) / (24.0 * 3600000.0);
    if (duty >= 0.025 || d.peeks < 94 || d.peeks > 125) printf("  radio on %.2f %%, %u peeks, %u sessions\n", duty * 100, (unsigned)d.peeks, (unsigned)d.p.sessions());
    printf("  (with Spotify linked: radio on %.2f %%, %u sessions)\n", duty * 100, (unsigned)d.p.sessions());
    CHECK(duty < 0.025 && d.peeks >= 94 && d.peeks <= 125);  // (a few more sessions than weather fetches: the hourly time sync on its own)
    CHECK(d.longestSession <= 25000);
  }
  {  // three hours without the router: a try after 1, 2, 5, 10 minutes and then every quarter of an hour, the
     // radio on for the time it takes to give up each time; and the first good session within a quarter of an
     // hour of its return
    NetDay d(25000);  // (a clock with one network: 25 s is long enough to give up)
    d.run(2 * 3600000UL);
    d.routerUp = false;
    const uint32_t onBefore = d.p.radioOnMs(d.t);
    d.run(3 * 3600000UL);
    const uint32_t onOutage = d.p.radioOnMs(d.t) - onBefore;
    const uint32_t tries = (uint32_t)d.p.failures();
    printf("  (three hours without the router: %u failed sessions, radio on %.1f %% of the time)\n", (unsigned)tries, onOutage / (3 * 36000.0));
    if (tries < 14 || tries > 18) printf("  %u failed sessions in three hours\n", (unsigned)tries);
    CHECK(tries >= 14 && tries <= 18);
    CHECK(onOutage <= (tries + 1) * 26000UL);
    CHECK(onOutage < 0.04 * 3 * 3600000.0);
    const uint32_t weatherBefore = d.weatherFetches;
    d.routerUp = true;
    const uint32_t back = d.t;
    while (d.weatherFetches == weatherBefore && d.t - back < 20 * 60000UL) d.tick();
    CHECK(d.weatherFetches > weatherBefore && d.t - back <= 15 * 60000UL + 30000);
    CHECK(d.p.failures() == 0);
  }
  {  // the router is there, the internet is not: the weather and the time are retried gently, nothing stays on
    NetDay d;
    d.run(3600000UL);
    d.internetUp = false;
    const uint32_t onBefore = d.p.radioOnMs(d.t);
    d.run(2 * 3600000UL);
    const double on = (double)(d.p.radioOnMs(d.t) - onBefore) / (2 * 3600000.0);
    if (on > 0.15) printf("  radio on %.1f %% with no internet\n", on * 100);
    printf("  (router up, no internet: radio on %.1f %%)\n", on * 100);
    CHECK(on < 0.05);  // retried after 1, 2, 5, 10 minutes and then every 15 (and the time every 5)
    CHECK(d.longestSession <= 60000);
  }
  {  // key presses: each gets a joined radio in seconds, and the radio goes off again some 45 s after the last
    NetDay d;
    d.run(3600000UL);
    for (int press = 0; press < 12; press++) {
      d.run(7 * 60000UL + press * 13000UL);  // (some of them close to the quarter of an hour sessions)
      const uint32_t t0 = d.t;
      d.press();
      bool joined = false;
      while (!joined && d.t - t0 < 20000) {
        d.tick();
        joined = d.online;
      }
      CHECK(joined && d.t - t0 <= d.connectMs + 800);
      d.run(kDemandMs - 3000 - (d.t - t0));
      CHECK(d.online);  // still held
      d.run(3000 + kSettleMs + 2000);
      CHECK(!d.online || d.t - t0 < kDemandMs + 6000);
    }
  }
  {  // music: the radio stays on for as long as it plays and goes off a few seconds after
    NetDay d;
    d.spotify = true;
    d.run(3600000UL);
    d.playing = true;
    d.run(2 * 3600000UL);
    CHECK(d.radio && d.online);
    d.playing = false;
    const uint32_t t0 = d.t;
    while (d.radio && d.t - t0 < 30000) d.tick();
    CHECK(!d.radio && d.t - t0 <= kSettleMs + 3500);  // (a look at Spotify may still be under way)
    const uint32_t onAfterMusic = d.p.radioOnMs(d.t);
    d.run(4 * 3600000UL);
    CHECK(d.p.radioOnMs(d.t) - onAfterMusic < 0.03 * 4 * 3600000.0);
  }
}

void testClockPolicy() {
  section("CPU clock policy");
  using namespace clockpolicy;
  Needs none, radio, usb, poke;
  radio.radio = true;
  usb.usbHost = true;
  poke.poked = true;
  CHECK(target(none, 80, 20) == 20);
  CHECK(target(radio, 80, 20) == 80 && target(usb, 80, 20) == 80 && target(poke, 80, 20) == 80);
  Needs all;
  all.radio = all.usbHost = all.poked = true;
  CHECK(target(all, 80, 20) == 80);
  // no idle clock, or one that is not below the working clock: always the working clock
  CHECK(target(none, 80, 0) == 80 && target(none, 80, 80) == 80 && target(none, 80, 160) == 80);
  CHECK(target(none, 240, 80) == 80 && target(none, 160, 10) == 10);
  // up at once, down only when quiet, nothing to do when it is there already
  CHECK(mayChange(20, 80, false) && mayChange(20, 80, true));
  CHECK(!mayChange(80, 20, false) && mayChange(80, 20, true));
  CHECK(!mayChange(80, 80, true) && !mayChange(20, 20, false));

  // the settings give the idle clock: never above the working one
  Settings st;
  st.cpuIdle = CPUIDLE_20;
  CHECK(target(none, st.cpuMhz(), st.cpuIdleMhz()) == 20);
  st.cpuIdle = CPUIDLE_80;  // = cpu_mhz = 80: nothing to do
  CHECK(target(none, st.cpuMhz(), st.cpuIdleMhz()) == 80);
}

void testPicker() {
  section("radio plan: the picker across sessions");
  using namespace wifipick;
  Picker pk;
  pk.configure(true, true);
  CHECK(pk.nextAttempt() == NET_MAIN);
  pk.joined(NET_MAIN, 1000);
  pk.radioOff();
  CHECK(pk.nextAttempt() == NET_MAIN && pk.current() == NET_MAIN);  // on the main network: back to it
  // on the backup: the next session starts there, not with a failed attempt on the main one
  Picker bk;
  bk.configure(true, true);
  bk.nextAttempt();
  bk.nextAttempt();
  bk.joined(NET_BACKUP, 5000);
  CHECK(bk.onBackup());
  bk.radioOff();
  CHECK(bk.nextAttempt() == NET_BACKUP && bk.onBackup());
  CHECK(bk.nextAttempt() == NET_MAIN);  // if that fails, the other one, as always
  // ... and the look for the main network goes on by the clock
  bk.joined(NET_BACKUP, 10000);
  bk.radioOff();
  CHECK(!bk.timeToLookForMain(10000 + 5 * 60000UL) && bk.timeToLookForMain(10000 + 10 * 60000UL));
  // no backup: nothing changes
  Picker one;
  one.configure(true, false);
  one.nextAttempt();
  one.joined(NET_MAIN, 1);
  one.radioOff();
  CHECK(one.nextAttempt() == NET_MAIN);
}

// After the clock changes the frame planner expects the costs of the new clock.  A model of the UI loop at
// 80 MHz and at 20, where a frame takes four times as long, and a switch between them in the middle of the run.
void testFramePlanClock() {
  section("frame plan: after a clock change");
  FramePlanner p(20000);
  CHECK(p.mhz() == 80 && p.drawCostUs() == 30000 && p.sendCostUs() == 15000);
  p.clockChanged(80, 20);
  CHECK(p.mhz() == 20 && p.drawCostUs() == 120000 && p.sendCostUs() == 60000);
  p.clockChanged(20, 80);
  CHECK(p.drawCostUs() == 30000 && p.sendCostUs() == 15000);
  p.clockChanged(80, 80);  // no change
  CHECK(p.drawCostUs() == 30000);
  p.clockChanged(0, 20);   // nonsense is ignored
  p.clockChanged(80, 0);
  CHECK(p.drawCostUs() == 30000 && p.mhz() == 80);
  p.clockChanged(80, 10);
  CHECK(p.drawCostUs() == 240000 && p.sendCostUs() == 120000);  // more than the 150 ms ceiling of the fast clocks

  // a long stall must not stretch the plan, at 20 MHz as at 80: a measurement is capped at 150 ms * 80 / MHz
  FramePlanner q(20000);
  q.drew(100, 400000, true);
  CHECK(q.drawCostUs() == (30000u * 3 + 150000u) / 4);
  FramePlanner slow(20000);
  slow.clockChanged(80, 20);
  for (int i = 0; i < 40; i++) slow.drew(100 + i, 5000000, true);  // always stalled: the ceiling is 600 ms here
  CHECK(slow.drawCostUs() > 590000u && slow.drawCostUs() <= 600000u);

  // the first frame after a switch is planned with the new costs: a simulated second-by-second loop, the
  // clock dropping from 80 to 20 MHz at second 50 and coming back at second 150, with draws and sends that
  // take 4 times as long while it is low; a frame is late if it goes out after its second has begun
  FramePlanner pl(20000);
  int64_t t = 0;
  int lateFrames = 0, frames = 0;
  int mhz = 80;
  auto cost = [&](int64_t us) { return mhz == 80 ? us : us * 4; };
  time_t lastSent = 0;
  time_t prepared = 0;
  bool havePrepared = false;
  const int64_t end = 300 * 1000000LL;
  while (t < end) {
    const time_t sec = (time_t)(t / 1000000);
    const int sec2 = (int)sec;
    if ((sec2 == 50 && mhz == 80 && !pl.prepared()) || (sec2 == 150 && mhz == 20 && !pl.prepared())) {
      const int to = mhz == 80 ? 20 : 80;
      pl.clockChanged((uint32_t)mhz, (uint32_t)to);
      mhz = to;
    }
    const FramePlanner::Step st = pl.next(sec, (int32_t)(t % 1000000), false);
    switch (st.action) {
      case FramePlanner::DRAW_NOW:
        t += cost(30000);
        pl.drew(st.sec, (uint32_t)cost(30000), false);
        t += cost(15000);
        pl.sent((uint32_t)cost(15000));
        lastSent = st.sec;
        break;
      case FramePlanner::PREPARE:
        t += cost(30000);
        pl.drew(st.sec, (uint32_t)cost(30000), true);
        prepared = st.sec;
        havePrepared = true;
        break;
      case FramePlanner::SEND:
        t += st.waitUs;
        t += cost(15000);
        if (havePrepared && sec2 > 5) {
          frames++;
          // on the glass 20 ms (the panel) after it was sent: late if that is after its second began
          if ((t + 20000) > (int64_t)prepared * 1000000LL + 30000) lateFrames++;  // 30 ms of grace
        }
        pl.sent((uint32_t)cost(15000));
        lastSent = prepared;
        break;
      case FramePlanner::SLEEP:
        t += 4000;
        break;
      default:
        t += 4000;
        break;
    }
  }
  (void)lastSent;
  printf("  (%d of %d frames late around the two clock changes)\n", lateFrames, frames);
  CHECK(frames > 250);
  CHECK(lateFrames <= 2);  // a frame or two at the moment of a switch is what it costs; the rest is on time
}

}  // namespace

int main() {
  testSteps();
  testFailures();
  testDay();
  testClockPolicy();
  testPicker();
  testFramePlanClock();
  printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
