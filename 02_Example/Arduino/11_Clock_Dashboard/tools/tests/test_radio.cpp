// Host-side tests of the power logic that is not the settings: when the radio is on in "wifi_mode = sync"
// (radio_plan.h), which CPU clock the clock runs at (clock_policy.h), when the console is shut down
// (console_policy.h), the picker across radio sessions (wifi_pick.h) and the frame planner after a clock change
// (frame_plan.h).  Built and run by run_tests.sh.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "clock_policy.h"
#include "console_policy.h"
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
  // steps of `ms` until the radio is on (true) or `limitMs` has passed
  bool untilOn(uint32_t limitMs, uint32_t ms = 1000) {
    const uint32_t t0 = t;
    bool on = false;
    while (!on && t - t0 < limitMs) on = step(ms);
    return on;
  }
  bool untilOff(uint32_t limitMs, uint32_t ms = 1000) {
    const uint32_t t0 = t;
    bool on = true;
    while (on && t - t0 < limitMs) on = step(ms);
    return !on;
  }
};

void testSteps() {
  section("radio plan: one session at a time");
  {  // nothing to do: the radio stays off
    Drive d;
    for (int i = 0; i < 100; i++) CHECK(!d.step(1000));
    CHECK(d.p.state() == Planner::OFF && d.p.sessions() == 0 && d.p.radioOnMs(d.t) == 0);
    CHECK(d.p.lastOutcome() == Planner::NONE);
  }
  {  // always mode: the radio is on whatever it is asked
    Drive d(false);
    CHECK(d.step(10) && d.p.state() == Planner::CONNECTING);
    d.in.online = true;
    CHECK(d.step(10) && d.p.state() == Planner::ONLINE);
    d.in.online = false;
    CHECK(d.step(10) && d.p.state() == Planner::CONNECTING);
    d.in.absent = true;  // ... and "no such network" does not switch it off: that is sync mode's business
    for (int i = 0; i < 100; i++) CHECK(d.step(1000));
    CHECK(!d.p.takeGaveUp() && d.p.lastOutcome() == Planner::NONE);
    d.p.lookNow();
    CHECK(d.step(10));
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
    CHECK(d.p.lastOutcome() == Planner::DONE && !d.p.takeGaveUp());
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
    CHECK(!on && d.p.takeGaveUp() && d.p.failures() == 1 && d.p.lastOutcome() == Planner::FAILED);
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
    CHECK(!on && d.p.takeGaveUp() && d.p.lastOutcome() == Planner::FAILED);
    CHECK(d.t - tOn >= kConnectTimeoutMs - 1000 && d.t - tOn <= kConnectTimeoutMs + 1000);
    CHECK(d.p.failures() == round + 1 && d.p.awayLooks() == 0);
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
  {  // one press buys one attempt: when that is given up, the rest of the 45 seconds does not start another
     // (a clock with one network gives up after 25 s; the press would otherwise run the radio twice)
    Drive e(true, 25000);
    e.in.workDue = true;
    e.step(1000);
    CHECK(e.untilOff(60000) && e.p.failures() == 1);
    e.step(5000);
    e.in.demand = true;
    e.in.demandSeq = 7;
    CHECK(e.step(100) && e.p.state() == Planner::CONNECTING);
    CHECK(e.untilOff(60000, 100) && e.p.failures() == 2);  // given up after 25 s, the press still "active" for 20 more
    for (int i = 0; i < 190; i++) CHECK(!e.step(100));     // ... and nothing starts in those
    e.in.demandSeq = 8;                                     // a new press does
    CHECK(e.step(100) && e.p.state() == Planner::CONNECTING);
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
    CHECK(!on && e.p.takeGaveUp() && e.p.lastOutcome() == Planner::FAILED);
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

// One session's search for the networks: what "no such network" from the WiFi stack adds up to.
void testSearch() {
  section("radio plan: is the network there at all?");
  {  // one network, coming from a session that worked: two answers before it is believed
    Search s;
    s.begin(true, false, kAwayScansFirst);
    CHECK(!s.noneInRange());
    CHECK(!s.note(0, 0, 0) && !s.noneInRange());
    CHECK(!s.note(0, 1, 0) && !s.noneInRange());  // one scan can miss a router that is there
    CHECK(s.note(0, 2, 0) && s.noneInRange());
    CHECK(!s.note(0, 3, 0) && s.noneInRange());   // said once: the caller moves on once, not every time round
  }
  {  // away already: the first answer will do
    Search s;
    s.begin(true, false, kAwayScansAgain);
    CHECK(s.note(0, 1, 0) && s.noneInRange());
  }
  {  // two networks: both have to be missing
    Search s;
    s.begin(true, true, kAwayScansAgain);
    CHECK(s.note(0, 1, 0) && !s.noneInRange());   // the main one is not there: on to the backup at once
    CHECK(!s.note(1, 0, 0) && !s.noneInRange());
    CHECK(s.note(1, 1, 0) && s.noneInRange());
  }
  {  // a network that answers anything else (a wrong password) is there: the session is not "away"
    Search s;
    s.begin(true, true, kAwayScansAgain);
    CHECK(!s.note(0, 0, 1) && !s.noneInRange());
    CHECK(s.note(1, 1, 0));                       // the backup is missing ...
    CHECK(!s.noneInRange());                      // ... but the main one refused: that is a failure, with its back-off
    CHECK(!s.note(0, 5, 0) || !s.noneInRange());  // and it stays one for this session
    CHECK(!s.noneInRange());
  }
  {  // a mixed bag on one network (not found, then a refusal): not away
    Search s;
    s.begin(true, false, kAwayScansFirst);
    CHECK(!s.note(0, 1, 1) && !s.noneInRange());
    s.note(0, 2, 1);
    CHECK(!s.noneInRange());
  }
  {  // a connection came up or broke: start over
    Search s;
    s.begin(true, false, kAwayScansAgain);
    s.note(0, 0, 3);
    s.forget();
    CHECK(s.note(0, 1, 0) && s.noneInRange());
    s.forget();
    CHECK(!s.noneInRange());
  }
  {  // no network set at all is not "away" (the network task never gets this far), and a bad index is ignored
    Search s;
    s.begin(false, false, kAwayScansAgain);
    CHECK(!s.noneInRange());
    CHECK(!s.note(-1, 9, 0) && !s.note(2, 9, 0) && !s.noneInRange());
    s.begin(false, true, 0);  // (a count below one is one)
    CHECK(s.note(1, 1, 0) && s.noneInRange());
  }
  CHECK(kAwayScansFirst == 2 && kAwayScansAgain == 1 && kMinScanMs >= 500 && kMinScanMs < 1300);  // (a scan of 11 channels takes 1.3 s at the least)
}

void testAway() {
  section("radio plan: none of the networks in range");
  {  // not a failure: the session ends as soon as the answer is in, and the looks come every 2 minutes for the
     // first five, then every 5 minutes, for as long as it takes
    Drive d;
    d.in.workDue = true;
    const uint32_t expectWait[] = {120000, 120000, 120000, 120000, 120000, 300000, 300000, 300000, 300000};
    for (int look = 0; look < 9; look++) {
      CHECK(d.untilOn(400000, 100));
      const uint32_t tOn = d.t;
      for (int i = 0; i < 24; i++) CHECK(d.step(100));  // 2.4 s: the scan
      d.in.absent = true;
      CHECK(!d.step(100) && d.p.takeGaveUp() && d.p.lastOutcome() == Planner::AWAY);
      d.in.absent = false;
      CHECK(d.t - tOn <= 2600);
      CHECK(d.p.failures() == 0 && d.p.awayLooks() == look + 1);  // the failure count is not touched
      const uint32_t wait = d.p.retryAtMs() - d.t;
      if (wait != expectWait[look]) printf("  look %d: wait %u s\n", look + 1, (unsigned)(wait / 1000));
      CHECK(wait == expectWait[look]);
      const uint32_t retryAt = d.p.retryAtMs();
      while ((int32_t)(retryAt - d.t) > 150) CHECK(!d.step(100));  // off until then
    }
    // the network is back: joined at the next look, and the counts start over
    CHECK(d.untilOn(400000, 100));
    d.in.online = true;
    d.step(100);
    CHECK(d.p.state() == Planner::ONLINE && d.p.awayLooks() == 0);
    d.in.workDue = false;
    CHECK(d.untilOff(10000, 100) && d.p.lastOutcome() == Planner::DONE && d.p.retryAtMs() == 0);
  }
  CHECK(awayAfterMs(1) == kAwayQuickMs && awayAfterMs(kAwayQuickLooks) == kAwayQuickMs && awayAfterMs(kAwayQuickLooks + 1) == kAwaySlowMs &&
        awayAfterMs(1000) == kAwaySlowMs);
  CHECK(kAwaySlowMs < kBackoffMs[kBackoffSteps - 1]);  // the point of it: a network that is not there is looked for more often than one that refuses

  // (helper: one look that finds nothing; the radio is on for 2.5 s)
  auto lookAndMiss = [](Drive &d) {
    for (int i = 0; i < 24; i++) d.step(100);
    d.in.absent = true;
    const bool off = !d.step(100);
    d.in.absent = false;
    return off && d.p.takeGaveUp() && d.p.lastOutcome() == Planner::AWAY;
  };
  {  // a key press while away: one look at once, and only one for that press; the next press looks again
    Drive d;
    d.in.workDue = true;
    CHECK(d.step(100) && lookAndMiss(d));
    CHECK(!d.step(10000));  // ten seconds into the two minutes
    d.in.demand = true;
    d.in.demandSeq = 2;
    CHECK(d.step(100) && d.p.state() == Planner::CONNECTING);
    CHECK(lookAndMiss(d));
    for (int i = 0; i < 400; i++) CHECK(!d.step(100));  // the press is still "active" for 40 s: no loop of looks
    CHECK(d.p.awayLooks() == 1);                         // a look someone asked for is extra: the quick ones are not used up
    d.in.demandSeq = 3;
    CHECK(d.step(100) && lookAndMiss(d));
    CHECK(d.p.awayLooks() == 1);
    d.in.demand = false;
    // ... and when the hotspot is on at last, the press that finds it holds the radio as a press does
    d.in.demand = true;
    d.in.demandSeq = 4;
    CHECK(d.step(100));
    d.in.online = true;
    d.in.workDue = false;
    for (int i = 0; i < 300; i++) CHECK(d.step(100));  // 30 s on, held by the press
    d.in.demand = false;
    CHECK(d.untilOff(5000, 100) && d.p.lastOutcome() == Planner::DONE);
  }
  {  // the first look of an absence counts even when a key press started it (the next looks then take one answer)
    Drive d;
    d.in.demand = true;
    d.in.demandSeq = 2;
    CHECK(d.step(100) && lookAndMiss(d) && d.p.awayLooks() == 1);
  }
  {  // any button (BOOT has no business with the network, but it means someone is there): look now
    Drive d;
    d.in.workDue = true;
    CHECK(d.step(100) && lookAndMiss(d));
    CHECK(!d.step(30000));
    d.p.lookNow();
    CHECK(d.step(100) && d.p.state() == Planner::CONNECTING);
    d.p.lookNow();  // while a look is under way: nothing
    CHECK(lookAndMiss(d) && d.p.awayLooks() == 1);
    CHECK(d.p.retryAtMs() - d.t == kAwayQuickMs);  // and the looks by the clock go on from there
    CHECK(!d.step(60000));
  }
  {  // ... but only then: it does not cut a back-off short (the network is there and refuses), and it starts
     // nothing when all is well
    Drive d;
    d.in.workDue = true;
    d.step(100);
    CHECK(d.untilOff(60000) && d.p.lastOutcome() == Planner::FAILED);
    const uint32_t retryAt = d.p.retryAtMs();
    d.p.lookNow();
    CHECK(d.p.retryAtMs() == retryAt && !d.step(1000));
    Drive fine;
    fine.p.lookNow();
    for (int i = 0; i < 50; i++) CHECK(!fine.step(1000));
  }
  {  // away and failures keep separate books: a refusal after five looks waits a minute, not a quarter of an hour
    Drive d;
    d.in.workDue = true;
    for (int i = 0; i < 6; i++) {
      CHECK(d.untilOn(400000, 100));
      CHECK(lookAndMiss(d));
    }
    CHECK(d.p.awayLooks() == 6 && d.p.failures() == 0);
    CHECK(d.untilOn(400000, 100));
    CHECK(d.untilOff(60000, 100) && d.p.lastOutcome() == Planner::FAILED && d.p.failures() == 1);
    CHECK(d.p.retryAtMs() - d.t == kBackoffMs[0]);
  }
  {  // a connection that breaks and then finds the network gone (the router was switched off): away, in the same session
    Drive d;
    d.in.workDue = true;
    d.step(100);
    d.in.online = true;
    d.step(100);
    d.in.online = false;
    CHECK(d.step(100) && d.p.state() == Planner::CONNECTING);
    CHECK(lookAndMiss(d) && d.p.failures() == 0);
  }
}

// 32 bits of milliseconds wrap after 49.7 days, and a time more than 24.8 days back looks like one still to come.
void testWrap() {
  section("radio plan: after weeks of running");
  CHECK(!pending(1000, 0) && pending(1000, 1001) && !pending(1000, 1000) && !pending(1000, 999));
  CHECK(!pending(0x90000000u, 0) && !pending(0xFFFFFFFFu, 0));  // 0 is "none" at every age, not a time 25 days ahead
  CHECK(pending(0xFFFFFFF0u, 0x10u));                 // across the wrap: 32 ms to go
  CHECK(pending(1000, 1000 + 0x7FFFFFFFu));           // (the far end of what can be told)
  CHECK(demandUntil(5) != 0 && demandUntil(0u - kDemandMs) != 0);  // never the "none" value
  CHECK(demandActive(5, demandUntil(5)) && !demandActive(5 + kDemandMs + 2, demandUntil(5)));
  CHECK(due(1000, 1000) && due(1001, 1000) && !due(999, 1000) && due(5, 0xFFFFFFF0u));
  {  // keepDue: work that is due stays due, however long ago that was
    uint32_t at = 1000;
    uint32_t now = 1000;
    int flips = 0;
    for (uint64_t step = 0; step < 70ULL * 24 * 3600 * 1000; step += 3600000ULL) {  // 70 days, looked at every hour
      now = (uint32_t)(1000 + step);
      keepDue(now, &at);
      if (!due(now, at)) flips++;
    }
    CHECK(flips == 0);
    uint32_t bare = 1000;  // without it: "not due" for 24.8 of those days
    int bareFlips = 0;
    for (uint64_t step = 0; step < 70ULL * 24 * 3600 * 1000; step += 3600000ULL) {
      if (!due((uint32_t)(1000 + step), bare)) bareFlips++;
    }
    CHECK(bareFlips > 24 * 20);
    uint32_t ahead = now + 60000;  // a time still to come is left alone
    keepDue(now, &ahead);
    CHECK(ahead == now + 60000);
  }
  // the planner starts sessions at every age: at the start, past 2^31 ms (24.8 days) and across 2^32 (49.7 days)
  for (uint32_t start : {100000u, 0x7FFFF000u, 0x80000100u, 0xC0000000u, 0xFFFF0000u}) {
    Drive d;
    d.t = start;
    int sessions = 0;
    for (int n = 0; n < 12; n++) {  // twelve sessions ten minutes apart: two hours, which crosses the boundary in question
      d.in.workDue = true;
      const bool on = d.step(100);
      if (!on) printf("  no session at t = 0x%08x (start 0x%08x)\n", (unsigned)d.t, (unsigned)start);
      CHECK(on);
      d.in.online = true;
      d.step(3000);
      d.in.workDue = false;
      d.in.online = true;
      if (d.untilOff(10000, 100)) sessions++;
      d.in.online = false;
      for (int i = 0; i < 600; i++) d.step(1000);
    }
    CHECK(sessions == 12 && d.p.sessions() == 12);
  }
  {  // a wait that ran out long ago does not come round again: a give-up, then nothing due for 30 days, then work
    Drive d;
    d.in.workDue = true;
    d.step(100);
    CHECK(d.untilOff(60000));
    d.in.workDue = false;
    for (int day = 0; day < 30; day++) {
      for (int i = 0; i < 24; i++) CHECK(!d.step(3600000));
    }
    CHECK(d.p.retryAtMs() == 0);
    d.in.workDue = true;
    CHECK(d.step(100));
  }
}

void testSpotifyRules() {
  section("radio plan: what Spotify needs of the radio");
  SpotifyNeeds n;
  CHECK(!spotifyHolds(n));
  n.playing = true;
  CHECK(spotifyHolds(n));  // live (the default): music holds the radio
  n.live = false;
  CHECK(!spotifyHolds(n));  // spotify_live = off: it does not
  n.pausedRecently = true;
  CHECK(!spotifyHolds(n));
  n.live = true;
  n.playing = false;
  CHECK(spotifyHolds(n));  // paused a moment ago, live
  for (bool live : {false, true}) {  // a command that waits and the Now Playing page hold it either way
    SpotifyNeeds c;
    c.live = live;
    c.commandWaiting = true;
    CHECK(spotifyHolds(c));
    c.commandWaiting = false;
    c.pageOpen = true;
    CHECK(spotifyHolds(c));
  }
  // the next look: when the track should be over, and a moment more
  CHECK(trackLookAfterMs(210000, 0) == 210000 + kTrackEndSlackMs);
  CHECK(trackLookAfterMs(210000, 200000) == 10000 + kTrackEndSlackMs);
  CHECK(trackLookAfterMs(210000, 210000) == kTrackEndSlackMs);  // over already by Spotify's own figures: just the moment
  CHECK(trackLookAfterMs(210000, 250000) == kTrackEndSlackMs);
  CHECK(kTrackEndSlackMs < kSettleMs);  // a look that finds the track on its last second is repeated within the same session
}

// A model of the network task's rules in sync mode (net_task.cpp): what is due, how long each thing takes,
// what the WiFi stack answers and what a joined radio does about it.  One pass every 100 ms.
enum NetState { NET_NOT_SET, NET_UP, NET_ABSENT, NET_REFUSING };

struct NetDay {
  Planner p;
  wifipick::Picker pick;
  Search search;
  NetState net[2] = {NET_UP, NET_NOT_SET};  // the main network and the backup
  uint32_t connectMs = 3000;  // a network that is up is joined this long after the attempt on it began
  uint32_t scanMs = 2400;     // one that is not answers "no such network" (or refuses) this long after, and again every so long
  bool internetUp = true;
  bool spotify = false;  // configured and linked
  bool live = true;      // spotify_live
  bool playing = false;  // the phone is playing
  uint32_t trackMs = 210000;
  uint32_t t = 1000;
  // the session
  bool radio = false, online = false;
  uint32_t sessionStart = 0, lastBegin = 0, answerAt = 0, busyUntil = 0;
  int attemptNet = -1, joinedNet = -1;
  unsigned notFound = 0, other = 0;
  // the network task's timers
  bool ntpEver = false;
  uint32_t ntpAt = 0, weatherAt = 0, ntpRetryAt = 0;
  int weatherFails = 0;
  bool peekDone = false;
  uint32_t demandUntilMs = 0, demandSeq = 1;
  // the player, and what the clock believes of it
  uint32_t trackStart = 0, trackNo = 0;
  bool seenPlaying = false;
  uint32_t seenTrackNo = 0, lookAt = 0, nextPollAt = 0;
  // what happened
  uint32_t weatherFetches = 0, ntpSyncs = 0, peeks = 0, looks = 0, awayEnds = 0, failedEnds = 0;
  uint32_t lastWeather = 0, lastNtp = 0, maxWeatherGap = 0, maxNtpGap = 0;
  uint32_t longestSession = 0, staleMs = 0, staleRun = 0, maxStaleRun = 0;
  explicit NetDay(uint32_t timeout = 25000) : p(true, timeout) { pick.configure(true, false); }

  void setNets(NetState mainNet, NetState backup) {
    net[0] = mainNet;
    net[1] = backup;
    pick.configure(mainNet != NET_NOT_SET, backup != NET_NOT_SET);
  }
  void press() {
    demandSeq++;
    demandUntilMs = demandUntil(t);
  }
  void play(bool on) {
    playing = on;
    trackStart = t;
    trackNo++;
  }
  void skip() {
    trackStart = t;
    trackNo++;
  }
  // wakes the radio on its own only when it is overdue by the grace time
  bool ntpDue() const { return (!ntpEver || due(t, ntpAt + kNtpGraceMs)) && due(t, ntpRetryAt); }
  // asked in a session that is going on anyway: due within ten minutes counts
  bool ntpNear() const { return (!ntpEver || due(t, ntpAt - kNtpEarlyMs)) && due(t, ntpRetryAt); }
  bool weatherDue() const { return due(t, weatherAt); }
  bool peekDue() const { return spotify && !peekDone; }
  bool lookDue() const { return spotify && !live && seenPlaying && lookAt != 0 && due(t, lookAt); }

  void poll() {  // a look at the player
    seenPlaying = playing;
    seenTrackNo = trackNo;
    lookAt = playing ? ((t + trackLookAfterMs(trackMs, t - trackStart)) | 1u) : 0;
    nextPollAt = t + 10000;
  }

  void tick() {
    t += 100;
    if (playing && (uint32_t)(t - trackStart) >= trackMs) {  // the next track begins
      trackStart += trackMs;
      trackNo++;
    }
    if (seenPlaying && (seenTrackNo != trackNo || !playing)) {  // the screen shows something that is no longer so
      staleMs += 100;
      staleRun += 100;
      if (staleRun > maxStaleRun) maxStaleRun = staleRun;
    } else {
      staleRun = 0;
    }

    // what the WiFi stack does with the attempt under way
    if (radio && !online && attemptNet >= 0) {
      const NetState s = net[attemptNet];
      if (s == NET_UP && (uint32_t)(t - lastBegin) >= connectMs) {
        online = true;
        joinedNet = attemptNet;
        pick.joined(attemptNet ? wifipick::NET_BACKUP : wifipick::NET_MAIN, t);
        search.forget();
      } else if (s != NET_UP && due(t, answerAt)) {
        if (s == NET_ABSENT) notFound++;
        else other++;
        answerAt = t + scanMs;
      }
    }
    if (radio && online && net[joinedNet] != NET_UP) {  // the connection broke: the stack tries the same network again
      online = false;
      pick.lost();
      search.forget();
      lastBegin = t;
      notFound = other = 0;
      answerAt = t + scanMs;
    }

    Inputs in;
    in.online = radio && online;
    SpotifyNeeds needs;
    needs.playing = seenPlaying;
    needs.live = live;
    in.workDue = ntpDue() || weatherDue() || peekDue() || lookDue();
    in.holdSpotify = spotify && spotifyHolds(needs);
    in.demand = demandActive(t, demandUntilMs);
    in.demandSeq = demandSeq;
    in.absent = radio && search.noneInRange();
    const bool was = radio;
    radio = p.update(t, in);
    if (radio && !was) {
      sessionStart = t;
      peekDone = false;
      nextPollAt = t;
      search.begin(net[0] != NET_NOT_SET, net[1] != NET_NOT_SET, p.awayLooks() == 0 ? kAwayScansFirst : kAwayScansAgain);
      attemptNet = -1;
      lastBegin = 0;
      online = false;
    }
    if (!radio && was) {
      if ((uint32_t)(t - sessionStart) > longestSession) longestSession = t - sessionStart;
      pick.radioOff();
      online = false;
      if (p.takeGaveUp()) {
        if (p.lastOutcome() == Planner::AWAY) awayEnds++;
        else failedEnds++;
        seenPlaying = false;  // the network is gone: a track nobody can follow is taken off the screen
      }
    }
    if (!radio) return;

    if (!online) {  // the attempts: one network at a time, the other one at once when this one is not there
      const bool moveOn = attemptNet >= 0 && search.note(attemptNet, notFound, other) && !search.noneInRange();
      if (lastBegin == 0 || (uint32_t)(t - lastBegin) > 20000 || moveOn) {
        attemptNet = pick.nextAttempt() == wifipick::NET_BACKUP ? 1 : 0;
        lastBegin = t ? t : 1;
        notFound = other = 0;
        answerAt = t + scanMs;
      }
      return;
    }
    // what the task does once joined: the work of one loop, in the order of net_task.cpp
    if (t >= busyUntil) {
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
      } else if (peekDue()) {  // the session's first look at the player (TLS and all: three seconds)
        busyUntil = t + 3000;
        peekDone = true;
        peeks++;
        poll();
      } else if (spotify && due(t, nextPollAt) && (seenPlaying || lookDue())) {  // the ten second poll while it plays and the radio is on
        busyUntil = t + 500;
        looks++;
        poll();
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
    NetDay d(kConnectTimeoutMs);
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
    CHECK(d.p.failures() == 0 && d.awayEnds == 0 && d.failedEnds == 0);
  }
  {  // with Spotify linked, each session also looks at it once: a few more seconds, nothing more
    NetDay d(kConnectTimeoutMs);
    d.spotify = true;
    d.run(24 * 3600000UL);
    const double duty = d.p.radioOnMs(d.t) / (24.0 * 3600000.0);
    if (duty >= 0.025 || d.peeks < 94 || d.peeks > 125) printf("  radio on %.2f %%, %u peeks, %u sessions\n", duty * 100, (unsigned)d.peeks, (unsigned)d.p.sessions());
    printf("  (with Spotify linked: radio on %.2f %%, %u sessions)\n", duty * 100, (unsigned)d.p.sessions());
    CHECK(duty < 0.025 && d.peeks >= 94 && d.peeks <= 125);  // (a few more sessions than weather fetches: the hourly time sync on its own)
    CHECK(d.longestSession <= 25000);
  }
  {  // three hours with a router that is there and refuses the clock (a changed password): a try after 1, 2, 5,
     // 10 minutes and then every quarter of an hour, the radio on for the time it takes to give up each time;
     // and the first good session within a quarter of an hour of the password being put right
    NetDay d;  // (a clock with one network: 25 s is long enough to give up)
    d.run(2 * 3600000UL);
    d.net[0] = NET_REFUSING;
    const uint32_t onBefore = d.p.radioOnMs(d.t);
    d.run(3 * 3600000UL);
    const uint32_t onOutage = d.p.radioOnMs(d.t) - onBefore;
    const uint32_t tries = (uint32_t)d.p.failures();
    printf("  (three hours with a router that refuses: %u failed sessions, radio on %.1f %% of the time)\n", (unsigned)tries, onOutage / (3 * 36000.0));
    if (tries < 14 || tries > 18) printf("  %u failed sessions in three hours\n", (unsigned)tries);
    CHECK(tries >= 14 && tries <= 18);
    CHECK(d.awayEnds == 0 && d.failedEnds == tries);
    CHECK(onOutage <= (tries + 1) * 26000UL);
    CHECK(onOutage < 0.04 * 3 * 3600000.0);
    const uint32_t weatherBefore = d.weatherFetches;
    d.net[0] = NET_UP;
    const uint32_t back = d.t;
    while (d.weatherFetches == weatherBefore && d.t - back < 20 * 60000UL) d.tick();
    CHECK(d.weatherFetches > weatherBefore && d.t - back <= 15 * 60000UL + 30000);
    CHECK(d.p.failures() == 0);
  }
  {  // the router is there, the internet is not: the weather and the time are retried gently, nothing stays on
    NetDay d(kConnectTimeoutMs);
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
    NetDay d(kConnectTimeoutMs);
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
    NetDay d(kConnectTimeoutMs);
    d.spotify = true;
    d.run(3600000UL);
    d.play(true);
    d.run(16 * 60000UL);  // (the next session finds it playing)
    d.run(2 * 3600000UL);
    CHECK(d.radio && d.online);
    CHECK(d.maxStaleRun <= 12000);  // the strip is never more than a poll behind
    d.play(false);
    const uint32_t t0 = d.t;
    while (d.radio && d.t - t0 < 30000) d.tick();
    CHECK(!d.radio && d.t - t0 <= 10000 + kSettleMs + 3500);  // (the next ten second poll sees that it stopped)
    const uint32_t onAfterMusic = d.p.radioOnMs(d.t);
    d.run(4 * 3600000UL);
    CHECK(d.p.radioOnMs(d.t) - onAfterMusic < 0.03 * 4 * 3600000.0);
  }
}

// What the new rule is for: a clock that is somewhere its network is not.
void testAwayDays() {
  section("radio plan: a clock away from its network");
  {  // taken to work for eight hours, one network: a look every 2 minutes at first, then every 5, each a scan long;
     // the radio is on for a fraction of what the old "45 seconds, then back off" cost, and home is found within
     // 5 minutes of coming back
    NetDay d;
    d.run(2 * 3600000UL);
    d.net[0] = NET_ABSENT;
    const uint32_t onBefore = d.p.radioOnMs(d.t);
    d.run(8 * 3600000UL);
    const uint32_t onAway = d.p.radioOnMs(d.t) - onBefore;
    printf("  (eight hours away, one network: %u looks, radio on %.2f %% of the time, longest session %.1f s)\n", (unsigned)d.awayEnds,
           onAway / (8 * 36000.0), d.longestSession / 1000.0);
    CHECK(d.failedEnds == 0 && d.p.failures() == 0);
    CHECK(d.awayEnds >= 95 && d.awayEnds <= 104);  // 5 quick ones, then one every 5 minutes (and the 2.5 s each takes)
    CHECK(onAway < 0.012 * 8 * 3600000.0);         // about one percent
    CHECK(d.longestSession <= 20000);              // (the longest was a good session before it left)
    const uint32_t weatherBefore = d.weatherFetches;
    d.net[0] = NET_UP;
    const uint32_t back = d.t;
    while (d.weatherFetches == weatherBefore && d.t - back < 20 * 60000UL) d.tick();
    CHECK(d.weatherFetches > weatherBefore && d.t - back <= kAwaySlowMs + 10000);
    CHECK(d.p.awayLooks() == 0);
  }
  {  // the first look of an absence asks twice (4.9 s), the later ones once (2.5 s)
    NetDay d;
    d.run(3600000UL);
    d.net[0] = NET_ABSENT;
    uint32_t lengths[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
      while (!d.radio) d.tick();
      const uint32_t t0 = d.t;
      while (d.radio) d.tick();
      lengths[i] = d.t - t0;
    }
    CHECK(lengths[0] >= 4700 && lengths[0] <= 5200);
    CHECK(lengths[1] >= 2300 && lengths[1] <= 2800 && lengths[2] >= 2300 && lengths[2] <= 2800);
  }
  {  // with a backup network set, both are asked for, one after the other: twice the scan, still seconds
    NetDay d(kConnectTimeoutMs);
    d.setNets(NET_UP, NET_ABSENT);
    d.run(3600000UL);
    d.net[0] = NET_ABSENT;
    const uint32_t onBefore = d.p.radioOnMs(d.t);
    d.run(8 * 3600000UL);
    const uint32_t onAway = d.p.radioOnMs(d.t) - onBefore;
    printf("  (eight hours away, two networks: %u looks, radio on %.2f %% of the time)\n", (unsigned)d.awayEnds, onAway / (8 * 36000.0));
    CHECK(d.failedEnds == 0 && d.awayEnds >= 93 && d.awayEnds <= 104);
    CHECK(onAway < 0.022 * 8 * 3600000.0);
    // at work the phone's hotspot (the backup) is switched on: found at the next look, no button needed
    d.net[1] = NET_UP;
    const uint32_t on = d.t;
    while (!d.online && d.t - on < 20 * 60000UL) d.tick();
    CHECK(d.online && d.joinedNet == 1 && d.t - on <= kAwaySlowMs + 2 * d.scanMs + d.connectMs + 1000);
  }
  {  // ... and with a button, at once: KEY, or any button (the nudge), a few seconds after the hotspot is up
    for (int how = 0; how < 2; how++) {
      NetDay d(kConnectTimeoutMs);
      d.setNets(NET_ABSENT, NET_ABSENT);
      d.run(3600000UL);  // an hour away already: the looks are 5 minutes apart
      CHECK(d.awayEnds >= 10 && !d.radio);
      while (d.radio || d.p.retryAtMs() - d.t < 200000) d.tick();  // just after a look: the next is minutes off
      d.net[1] = NET_UP;
      const uint32_t on = d.t;
      if (how == 0) d.press();
      else d.p.lookNow();
      while (!d.online && d.t - on < 60000) d.tick();
      if (!d.online) printf("  not joined after a %s\n", how == 0 ? "key press" : "nudge");
      CHECK(d.online && d.joinedNet == 1 && d.t - on <= d.scanMs + d.connectMs + 1500);  // the main one is asked for first, then the hotspot
    }
  }
  {  // a key press where there is no network: one look of a few seconds, not 45 seconds of radio
    NetDay d;
    d.net[0] = NET_ABSENT;
    d.run(3600000UL);
    while (d.radio) d.tick();
    const uint32_t onBefore = d.p.radioOnMs(d.t);
    const uint32_t looksBefore = d.awayEnds;
    d.press();
    d.run(60000);
    const uint32_t cost = d.p.radioOnMs(d.t) - onBefore;
    CHECK(d.awayEnds == looksBefore + 1 || d.awayEnds == looksBefore + 2);  // (the press's look, and perhaps the clock's own, due in that minute)
    CHECK(cost <= 2 * 2800);
  }
  {  // the router restarts (a session finds it gone, and it comes back three minutes later): no failure is
     // counted, and the clock is on it again within two minutes of its return
    NetDay d;
    d.run(3600000UL);
    d.net[0] = NET_ABSENT;
    while (d.awayEnds == 0 && d.t < 3 * 3600000UL) d.tick();  // (the next session notices)
    CHECK(d.awayEnds == 1 && d.p.failures() == 0);
    d.run(3 * 60000UL);
    d.net[0] = NET_UP;
    const uint32_t back = d.t;
    while (!d.online && d.t - back < 10 * 60000UL) d.tick();
    CHECK(d.online && d.t - back <= kAwayQuickMs + d.connectMs + 500);
    CHECK(d.failedEnds == 0);
  }
  {  // gone in the middle of a session (the router loses power while the weather is fetched): the stack's own
     // reconnect hears "no such network" and the session ends as away, not after 25 seconds as a failure
    NetDay d;
    d.run(3600000UL);
    while (!d.online) d.tick();
    d.net[0] = NET_ABSENT;
    const uint32_t t0 = d.t;
    while (d.radio && d.t - t0 < 60000) d.tick();
    CHECK(!d.radio && d.t - t0 <= 2 * d.scanMs + 600 && d.awayEnds == 1 && d.failedEnds == 0);
  }
}

// spotify_live = off: the radio does not stay on for the music.
void testTrackMode() {
  section("radio plan: music with spotify_live = off");
  uint32_t onLive = 0, onTrack = 0;
  for (int mode = 0; mode < 2; mode++) {
    NetDay d(kConnectTimeoutMs);
    d.spotify = true;
    d.live = mode == 0;
    d.run(3600000UL);
    d.play(true);
    d.run(16 * 60000UL);  // (a session comes by and finds it playing)
    CHECK(d.seenPlaying);
    const uint32_t onBefore = d.p.radioOnMs(d.t);
    const uint32_t sessionsBefore = d.p.sessions();
    const uint32_t staleBefore = d.staleMs;
    d.maxStaleRun = 0;
    d.run(2 * 3600000UL);  // two hours of music, tracks of three and a half minutes
    const uint32_t on = d.p.radioOnMs(d.t) - onBefore;
    const uint32_t sessions = d.p.sessions() - sessionsBefore;
    if (mode == 0) {
      onLive = on;
      printf("  (two hours of music, live: radio on %.0f %%, the strip at most %.1f s behind)\n", on / 72000.0, d.maxStaleRun / 1000.0);
      CHECK(on > 0.99 * 7200000.0 && d.maxStaleRun <= 12000);
    } else {
      onTrack = on;
      printf("  (two hours of music, spotify_live = off: radio on %.1f %%, %u sessions, the strip at most %.1f s behind, %.1f %% of the time)\n",
             on / 72000.0, (unsigned)sessions, d.maxStaleRun / 1000.0, (d.staleMs - staleBefore) / 72000.0);
      CHECK(sessions >= 34 && sessions <= 46);  // one a track (34 of them), and the weather's when it does not fall together with one
      CHECK(on < 0.07 * 7200000.0);
      CHECK(d.maxStaleRun <= kTrackEndSlackMs + d.connectMs + 3000 + 1000);  // the next title a few seconds after the track changed
      CHECK(d.longestSession <= 25000);
      // the music stops (the phone is paused): the look at the end of the track sees it, and the looks stop
      d.play(false);
      d.run(d.trackMs + 20000);
      CHECK(!d.seenPlaying);
      const uint32_t s2 = d.p.sessions();
      d.run(3600000UL);
      CHECK(d.p.sessions() - s2 <= 6);  // back to the weather's four an hour (and the hourly time sync)
    }
  }
  CHECK(onTrack * 10 < onLive);  // what it is for

  {  // a track skipped on the phone shows late, and not later than the old one would have run
    NetDay d(kConnectTimeoutMs);
    d.spotify = true;
    d.live = false;
    d.run(3600000UL);
    d.play(true);
    d.run(16 * 60000UL);
    // 20 s into a track, the radio off, and no weather or time session in the next minutes (it would look too)
    while ((uint32_t)(d.t - d.trackStart) < 20000 || (uint32_t)(d.t - d.trackStart) > 30000 || d.radio ||
           (int32_t)(d.weatherAt - d.t) < 400000 || (int32_t)(d.ntpAt - d.t) < 400000) {
      d.tick();
    }
    d.maxStaleRun = 0;
    d.skip();
    d.run(d.trackMs + 30000);
    CHECK(d.maxStaleRun >= 150000);  // (it does show late: the rest of the track that was skipped.  That is the price.)
    CHECK(d.maxStaleRun <= d.trackMs + 10000);
    CHECK(d.seenTrackNo == d.trackNo || d.radio);
  }
  {  // ... and never later than the next session that comes by for the weather: every session looks at the player
    NetDay d(kConnectTimeoutMs);
    d.spotify = true;
    d.live = false;
    d.trackMs = 3600000;  // an hour-long episode
    d.run(3600000UL);
    d.play(true);
    d.run(16 * 60000UL);
    while (d.radio) d.tick();
    d.maxStaleRun = 0;
    d.skip();
    d.run(40 * 60000UL);
    CHECK(d.maxStaleRun <= 15 * 60000UL + 15000);
  }
  {  // KEY is live either way: a press brings the radio up and the player is looked at straight away
    NetDay d(kConnectTimeoutMs);
    d.spotify = true;
    d.live = false;
    d.run(3600000UL);
    d.play(true);
    d.run(16 * 60000UL);
    while (d.radio) d.tick();
    while ((uint32_t)(d.t - d.trackStart) < 20000 || (uint32_t)(d.t - d.trackStart) > 30000 || d.radio) d.tick();
    d.skip();
    d.press();
    const uint32_t t0 = d.t;
    while (d.seenTrackNo != d.trackNo && d.t - t0 < 30000) d.tick();
    CHECK(d.seenTrackNo == d.trackNo && d.t - t0 <= d.connectMs + 3000 + 1000);
  }
  {  // the network goes away while music plays: the track is taken off the screen at the first look that fails
    NetDay d;
    d.spotify = true;
    d.live = false;
    d.run(3600000UL);
    d.play(true);
    d.run(16 * 60000UL);
    while (d.radio) d.tick();
    CHECK(d.seenPlaying);
    d.net[0] = NET_ABSENT;
    d.run(d.trackMs + 30000);
    CHECK(!d.seenPlaying && d.awayEnds >= 1);
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
  CHECK(target(none, 240, 80) == 80 && target(none, 160, 40) == 40);
  // up at once, down only when quiet, nothing to do when it is there already
  CHECK(mayChange(20, 80, false) && mayChange(20, 80, true));
  CHECK(!mayChange(80, 20, false) && mayChange(80, 20, true));
  CHECK(!mayChange(80, 80, true) && !mayChange(20, 20, false));

  // the settings give the idle clock: never above the working one, and 20 MHz is the lowest on offer
  Settings st;
  st.cpuIdle = CPUIDLE_20;
  CHECK(target(none, st.cpuMhz(), st.cpuIdleMhz()) == 20);
  st.cpuIdle = CPUIDLE_40;
  CHECK(target(none, st.cpuMhz(), st.cpuIdleMhz()) == 40);
  st.cpuIdle = CPUIDLE_80;  // = cpu_mhz = 80: nothing to do
  CHECK(target(none, st.cpuMhz(), st.cpuIdleMhz()) == 80);
  int lowest = 1000;
  for (int v = 0; v < 8; v++) {
    Settings any;
    any.cpuSpeed = CPU_240;
    any.cpuIdle = (uint8_t)v;
    if (any.cpuIdleMhz() > 0 && any.cpuIdleMhz() < lowest) lowest = any.cpuIdleMhz();
  }
  CHECK(lowest == 20);
}

void testConsolePolicy() {
  section("console policy");
  using namespace consolepolicy;
  CHECK((int)CONSOLE_ON == (int)ON && (int)CONSOLE_AUTO == (int)AUTO && (int)CONSOLE_OFF == (int)OFF);  // the setting's numbers are the policy's
  {  // on: never, with or without a computer
    Watch w;
    for (uint32_t t = 0; t < 3600000; t += 1000) CHECK(!w.shouldStop(ON, false, (t / 60000) % 2 == 0, t));
  }
  {  // off: at once, a computer on the port or not
    Watch w;
    CHECK(w.shouldStop(OFF, false, true, 3000));
    Watch v;
    CHECK(v.shouldStop(OFF, false, false, 3000));
  }
  {  // auto, no computer: ten seconds after the first call, not before
    Watch w;
    uint32_t t = 2500;  // (the loop starts a few seconds after power-on)
    bool stop = false;
    while (!stop && t < 60000) {
      stop = w.shouldStop(AUTO, false, false, t);
      if (!stop) t += 4;
    }
    CHECK(stop && t - 2500 >= kNoHostMs && t - 2500 <= kNoHostMs + 8);
  }
  {  // auto, on a computer: it stays for as long as the computer is seen, and goes ten seconds after the cable is pulled
    Watch w;
    uint32_t t = 2500;
    for (; t < 3600000; t += 100) CHECK(!w.shouldStop(AUTO, false, true, t));
    const uint32_t pulled = t - 100;  // the last time the computer was seen
    bool stop = false;
    while (!stop && t - pulled < 60000) {
      t += 100;
      stop = w.shouldStop(AUTO, false, false, t);
    }
    CHECK(stop && t - pulled >= kNoHostMs && t - pulled <= kNoHostMs + 200);
  }
  {  // auto: a computer that shows up within the ten seconds (the port takes a moment after power-on) keeps it, and
     // short gaps (the port being reset by an upload) do not count up
    Watch w;
    uint32_t t = 1000;
    for (; t < 9000; t += 100) CHECK(!w.shouldStop(AUTO, false, false, t));
    for (; t < 20000; t += 100) CHECK(!w.shouldStop(AUTO, false, true, t));
    for (int round = 0; round < 20; round++) {
      for (int i = 0; i < 80; i++, t += 100) CHECK(!w.shouldStop(AUTO, false, false, t));  // 8 s without
      for (int i = 0; i < 10; i++, t += 100) CHECK(!w.shouldStop(AUTO, false, true, t));   // 1 s with
    }
  }
  {  // KEY held at start-up keeps the console whatever the setting, for the whole run
    for (int mode : {(int)ON, (int)AUTO, (int)OFF}) {
      Watch w;
      for (uint32_t t = 0; t < 600000; t += 500) CHECK(!w.shouldStop(mode, true, false, t));
    }
  }
  {  // across the wrap of millis()
    Watch w;
    uint32_t t = 0xFFFFF000u;
    CHECK(!w.shouldStop(AUTO, false, true, t));
    bool stop = false;
    uint32_t waited = 0;
    while (!stop && waited < 60000) {
      waited += 100;
      stop = w.shouldStop(AUTO, false, false, t + waited);
    }
    CHECK(stop && waited >= kNoHostMs && waited <= kNoHostMs + 100);
  }
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
  testSearch();
  testAway();
  testWrap();
  testSpotifyRules();
  testDay();
  testAwayDays();
  testTrackMode();
  testClockPolicy();
  testConsolePolicy();
  testPicker();
  testFramePlanClock();
  printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
