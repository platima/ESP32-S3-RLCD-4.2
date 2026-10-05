// Host-side tests for the portable parts of the firmware: calendar maths,
// sensor calculations, the time zone table, the Open-Meteo / Spotify parsers,
// URL helpers and the button gesture detector.
//
//   ./run_tests.sh        (Linux / WSL)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

#include "../../buttons.h"
#include "../../calc.h"
#include "../../charge.h"
#include "../../frame_plan.h"
#include "../../link_page.h"
#include "../../spotify_parse.h"
#include "../../timeutil.h"
#include "../../util.h"
#include "../../weather.h"
#include "../../weather_codes.h"

// Pull the table in directly so the test can see inside it.
#include "../../tz_table.cpp"

#include "check.h"

// ---------------------------------------------------------------------------
static void testCalendar() {
  section("calendar");
  CHECK(timeutil::weekday(2026, 10, 4) == 0);   // Sunday
  CHECK(timeutil::weekday(2026, 10, 5) == 1);   // Monday
  CHECK(timeutil::weekday(2000, 1, 1) == 6);    // Saturday
  CHECK(timeutil::weekday(2024, 2, 29) == 4);   // Thursday

  auto week = [](int y, int m, int d) {
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;
    timegm(&t);  // normalises tm_yday / tm_wday
    return timeutil::isoWeek(y, t.tm_yday, t.tm_wday);
  };
  CHECK(week(2026, 10, 4) == 40);
  CHECK(week(2026, 1, 1) == 1);
  CHECK(week(2027, 1, 3) == 53);   // 2026 has 53 ISO weeks
  CHECK(week(2021, 1, 1) == 53);   // Friday belonging to 2020's last week
  CHECK(week(2020, 12, 31) == 53);
  CHECK(week(2024, 12, 30) == 1);  // Monday already in 2025's week 1
  CHECK(week(2018, 12, 31) == 1);
  CHECK(week(2024, 2, 29) == 9);

  char buf[16];
  timeutil::formatUtcOffset(480, buf, sizeof buf);
  CHECK_STR(buf, "+08:00");
  timeutil::formatUtcOffset(-210, buf, sizeof buf);
  CHECK_STR(buf, "-03:30");
  timeutil::formatUtcOffset(0, buf, sizeof buf);
  CHECK_STR(buf, "+00:00");
  timeutil::formatUtcOffset(345, buf, sizeof buf);
  CHECK_STR(buf, "+05:45");

  timeutil::formatDuration(83000, buf, sizeof buf);
  CHECK_STR(buf, "1:23");
  timeutil::formatDuration(200040, buf, sizeof buf);
  CHECK_STR(buf, "3:20");
  timeutil::formatDuration(3661000, buf, sizeof buf);
  CHECK_STR(buf, "1:01:01");
  timeutil::formatDuration(0, buf, sizeof buf);
  CHECK_STR(buf, "0:00");

  CHECK_STR(timeutil::weekdayName(0), "Sunday");
  CHECK_STR(timeutil::weekdayShort(6), "Sat");
  CHECK_STR(timeutil::monthShort(9), "Oct");

  // epoch conversion against the C library
  CHECK(calc::epochFromUtc(2026, 10, 4, 0, 21, 7) == 1791073267);
  CHECK(calc::epochFromUtc(1970, 1, 1, 0, 0, 0) == 0);
  CHECK(calc::epochFromUtc(2000, 2, 29, 23, 59, 59) == 951868799);
  for (int y = 2024; y <= 2040; y++) {
    for (int m = 1; m <= 12; m++) {
      struct tm t = {};
      t.tm_year = y - 1900;
      t.tm_mon = m - 1;
      t.tm_mday = 28;
      t.tm_hour = 13;
      t.tm_min = 7;
      t.tm_sec = 9;
      CHECK(calc::epochFromUtc(y, m, 28, 13, 7, 9) == (int64_t)timegm(&t));
    }
  }
}

static void testTimeZones() {
  section("time zones");
  auto setTz = [](const char *tz) {
    setenv("TZ", tz, 1);
    tzset();
  };
  time_t jan = (time_t)calc::epochFromUtc(2026, 1, 15, 0, 0, 0);
  time_t jul = (time_t)calc::epochFromUtc(2026, 7, 15, 0, 0, 0);

  setTz("AWST-8");
  CHECK(timeutil::utcOffsetMinutes(jan) == 480);
  CHECK(timeutil::utcOffsetMinutes(jul) == 480);

  setTz("AEST-10AEDT,M10.1.0,M4.1.0/3");  // Sydney: DST in the southern summer
  CHECK(timeutil::utcOffsetMinutes(jan) == 660);
  CHECK(timeutil::utcOffsetMinutes(jul) == 600);

  setTz("EST5EDT,M3.2.0,M11.1.0");  // New York
  CHECK(timeutil::utcOffsetMinutes(jan) == -300);
  CHECK(timeutil::utcOffsetMinutes(jul) == -240);

  setTz("IST-1GMT0,M10.5.0,M3.5.0/1");  // Dublin uses "negative DST" in tzdata
  CHECK(timeutil::utcOffsetMinutes(jan) == 0);
  CHECK(timeutil::utcOffsetMinutes(jul) == 60);

  // offset spanning the date line / year boundary: local date differs from UTC date
  setTz("<+13>-13");
  time_t nye = (time_t)calc::epochFromUtc(2026, 12, 31, 20, 0, 0);  // already 2027 locally
  CHECK(timeutil::utcOffsetMinutes(nye) == 780);
  setTz("<-11>11");
  time_t ny = (time_t)calc::epochFromUtc(2027, 1, 1, 5, 0, 0);  // still 2026 locally
  CHECK(timeutil::utcOffsetMinutes(ny) == -660);

  // table lookups
  CHECK_STR(tzPosixForIana("Australia/Perth"), "AWST-8");
  CHECK_STR(tzPosixForIana("America/New_York"), "EST5EDT,M3.2.0,M11.1.0");
  CHECK_STR(tzPosixForIana("Europe/Kyiv"), "EET-2EEST,M3.5.0/3,M10.5.0/4");
  CHECK_STR(tzPosixForIana("Asia/Kolkata"), "IST-5:30");
  CHECK_STR(tzPosixForIana("Pacific/Auckland"), "NZST-12NZDT,M9.5.0,M4.1.0/3");
  CHECK(tzPosixForIana("Mars/Olympus_Mons") == nullptr);
  CHECK(tzPosixForIana("") == nullptr);
  CHECK(tzPosixForIana(nullptr) == nullptr);
  CHECK(tzPosixForIana("australia/perth") == nullptr);  // names are case sensitive

  // binary search depends on sorted order; check it and round-trip every entry
  size_t n = sizeof(kZones) / sizeof(kZones[0]);
  CHECK(n > 500);
  bool sorted = true;
  for (size_t i = 1; i < n; i++) sorted &= strcmp(kZones[i - 1].name, kZones[i].name) < 0;
  CHECK(sorted);
  size_t bad = 0;
  for (size_t i = 0; i < n; i++) {
    const char *p = tzPosixForIana(kZones[i].name);
    if (!p || strcmp(p, kZones[i].posix) != 0) bad++;
  }
  CHECK(bad == 0);

  // every rule in the table must be accepted by the C library's TZ parser and
  // yield a plausible offset (guards against a typo in the generator)
  size_t implausible = 0;
  for (size_t i = 0; i < n; i++) {
    setTz(kZones[i].posix);
    int a = timeutil::utcOffsetMinutes(jan), b = timeutil::utcOffsetMinutes(jul);
    if (a < -12 * 60 || a > 14 * 60 || b < -12 * 60 || b > 14 * 60) implausible++;
  }
  CHECK(implausible == 0);

  // synthesised fixed-offset rules
  char buf[40];
  calc::posixFromUtcOffset(28800, buf, sizeof buf);
  CHECK_STR(buf, "<+08>-8");
  calc::posixFromUtcOffset(19800, buf, sizeof buf);
  CHECK_STR(buf, "<+0530>-5:30");
  calc::posixFromUtcOffset(-12600, buf, sizeof buf);
  CHECK_STR(buf, "<-0330>3:30");
  calc::posixFromUtcOffset(0, buf, sizeof buf);
  CHECK_STR(buf, "<+00>0");
  calc::posixFromUtcOffset(-18000, buf, sizeof buf);
  CHECK_STR(buf, "<-05>5");
  int offsets[] = {28800, 19800, -12600, 0, -18000, 45900, 50400, -39600};
  for (int off : offsets) {
    calc::posixFromUtcOffset(off, buf, sizeof buf);
    setTz(buf);
    CHECK(timeutil::utcOffsetMinutes(jan) == off / 60);
  }
  setTz("UTC0");
}

static void testSensorMaths() {
  section("sensor maths");
  CHECK(calc::batteryPercent(4.25f) == 100);
  CHECK(calc::batteryPercent(4.20f) == 100);
  CHECK(calc::batteryPercent(3.84f) == 50);
  CHECK(calc::batteryPercent(3.73f) == 20);
  CHECK(calc::batteryPercent(3.27f) == 0);
  CHECK(calc::batteryPercent(2.9f) == 0);
  int mid = calc::batteryPercent(4.00f);
  CHECK(mid >= 77 && mid <= 78);
  int prev = -1;
  bool monotonic = true;
  for (int mv = 3000; mv <= 4300; mv++) {
    int p = calc::batteryPercent(mv / 1000.0f);
    monotonic &= p >= prev && p >= 0 && p <= 100;
    prev = p;
  }
  CHECK(monotonic);

  const uint8_t beef[2] = {0xBE, 0xEF};
  CHECK(calc::crc8(beef, 2) == 0x92);  // Sensirion datasheet check value

  // 25.0 C, 50 %RH
  uint8_t raw[6] = {0x66, 0x66, 0, 0x80, 0x00, 0};
  raw[2] = calc::crc8(raw, 2);
  raw[5] = calc::crc8(raw + 3, 2);
  float t = 0, rh = 0;
  CHECK(calc::shtc3Decode(raw, &t, &rh));
  CHECK_NEAR(t, 25.0, 0.01);
  CHECK_NEAR(rh, 50.0, 0.01);
  raw[2] ^= 0x01;  // corrupt the temperature CRC
  CHECK(!calc::shtc3Decode(raw, &t, &rh));
  raw[2] ^= 0x01;
  raw[5] ^= 0x80;  // corrupt the humidity CRC
  CHECK(!calc::shtc3Decode(raw, &t, &rh));

  CHECK_NEAR(calc::humidityAtTemperature(40, 22.0f, 22.0f), 40.0, 1e-4);
  CHECK_NEAR(calc::humidityAtTemperature(40, 26.4f, 22.4f), 50.8, 0.6);  // cooler air, higher RH
  CHECK_NEAR(calc::humidityAtTemperature(90, 30.0f, 20.0f), 100.0, 1e-4);  // clamped
  CHECK(calc::humidityAtTemperature(0, 25.0f, 20.0f) == 0.0f);
}

// ---------------------------------------------------------------------------
// Charge detection on simulated voltage traces.  A reading every 5 s (+-0.5 s)
// with Gaussian noise (sigma 4 mV), one reading in ten pulled down 20..70 mV as
// if a WiFi transmit burst hit the ADC, and a slow 1.5 mV wander for load changes.
// Every scenario runs over 24 random sequences so one lucky draw cannot pass.
// ---------------------------------------------------------------------------
namespace {
using CS = ChargeDetector;

struct Sim {
  CS d;
  uint32_t t = 0;  // ms since the trace began
  uint64_t rng;
  double wander;
  unsigned seen = 0;                     // bit per State seen since clearSeen()
  double firstAt[4] = {-1, -1, -1, -1};  // minute each state was first seen since clearSeen()
  int changes = 0;                       // state changes since clearSeen()
  CS::State last = CS::UNKNOWN;

  explicit Sim(uint64_t seed) : rng(seed * 0x9E3779B97F4A7C15ull + 12345) { wander = uniform() * 6.2831853; }
  double uniform() {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(rng >> 11) / 9007199254740992.0;
  }
  double gauss() {
    const double u1 = uniform() + 1e-12, u2 = uniform();
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
  }
  void clearSeen() {
    seen = 0;
    changes = 0;
    last = d.state();
    for (double &f : firstAt) f = -1;
  }
  double now() const { return t / 60000.0; }
  template <class F>
  void run(double minutes, F truth) {
    const uint32_t end = t + (uint32_t)(minutes * 60000.0 + 0.5);
    while (t < end) {
      const double m = t / 60000.0;
      double v = truth(m) + 0.004 * gauss() + 0.0015 * sin(m * 1.26 + wander);
      if (uniform() < 0.10) v -= 0.020 + 0.050 * uniform();
      d.addSample((float)v, t);
      const CS::State s = d.state();
      seen |= 1u << s;
      if (firstAt[s] < 0) firstAt[s] = m;
      if (s != last) {
        changes++;
        last = s;
      }
      t += 4500 + (uint32_t)(uniform() * 1000.0);
    }
  }
};

struct Verdict {
  bool ok = true;
  const char *why = "";
  void need(bool cond, const char *what) {
    if (ok && !cond) {
      ok = false;
      why = what;
    }
  }
};

// Runs a scenario over 24 random sequences.  maxBad is how many may fail: zero for things the
// detector must always get right, a couple for the odd coincidence that no voltage-only method
// can exclude (a level shift that happens to be followed by a drift the same way).
template <class F>
void forSeeds(const char *name, F scenario, int maxBad = 0) {
  int bad = 0;
  uint64_t firstSeed = 0;
  const char *firstWhy = "";
  for (uint64_t seed = 1; seed <= 24; seed++) {
    Verdict v;
    scenario(seed, v);
    g_checks++;
    if (!v.ok && bad++ == 0) {
      firstSeed = seed;
      firstWhy = v.why;
    }
  }
  g_checks++;
  if (bad > maxBad) {
    g_failed++;
    printf("  FAIL %s: %d of 24 sequences (at most %d allowed), first seed %llu: %s\n", name, bad, maxBad,
           (unsigned long long)firstSeed, firstWhy);
  }
}

constexpr unsigned bit(CS::State s) { return 1u << s; }
const unsigned kClaimsPower = bit(CS::CHARGING) | bit(CS::FULL);  // "USB is in" states
}  // namespace

static void testCharge() {
  section("charge detection");
  using S = CS;

  forSeeds("steady discharge on battery", [](uint64_t seed, Verdict &v) {
    Sim s(seed);
    auto f = [](double m) { return 3.95 - 0.0006 * m; };
    s.run(10.5, f);
    v.need(s.d.state() == S::UNKNOWN, "claimed something before three minutes of warm-up and eight of data");
    s.run(9.5, f);
    v.need(s.d.state() == S::DISCHARGING, "not discharging by minute 20");
    s.clearSeen();
    s.run(120, f);
    v.need((s.seen & kClaimsPower) == 0, "called it charging or full");
    v.need(s.changes == 0, "flapped");
    v.need(fabs(s.d.slopeMvPerMin() + 0.6) < 0.6, "slope not near -0.6 mV/min");
  });

  forSeeds("USB plugged in while on battery (+90 mV step)", [](uint64_t seed, Verdict &v) {
    Sim s(seed);
    s.run(25, [](double m) { return 3.83 - 0.0006 * m; });
    v.need(s.d.state() == S::DISCHARGING, "not discharging before the plug-in");
    const double t0 = s.now();
    s.clearSeen();
    s.run(45, [t0](double m) { return 3.83 - 0.0006 * t0 + 0.090 + 0.0025 * (m - t0); });
    v.need(s.firstAt[S::CHARGING] >= 0 && s.firstAt[S::CHARGING] - t0 <= 2.0, "charging not shown within 2 minutes");
    v.need(s.changes == 1, "flapped after the plug-in");
    v.need(s.d.state() == S::CHARGING, "not charging at the end");
  });

  forSeeds("charged from the middle of the curve up to full", [](uint64_t seed, Verdict &v) {
    Sim s(seed);
    auto f = [](double m) {
      const double x = 4.10 + 0.003 * m;  // constant current, then the charger holds 4.2 V from minute 33
      return x > 4.2 ? 4.2 : x;
    };
    s.run(17, f);
    v.need(s.d.state() == S::CHARGING, "not charging by minute 17");
    s.clearSeen();
    s.run(37, f);
    v.need(s.d.state() == S::FULL, "not full 20 minutes after the voltage stopped climbing");
    v.need((s.seen & bit(S::DISCHARGING)) == 0, "called it discharging");
    s.run(60, f);
    v.need(s.d.state() == S::FULL, "left FULL while on the charger");
  });

  // Unplugged from a full battery: the charger had stopped so no current flowed, nothing steps,
  // and only the slow decline gives it away.
  auto unplugFromFull = [](const char *name, double rate, double withinMin) {
    forSeeds(name, [rate, withinMin](uint64_t seed, Verdict &v) {
      Sim s(seed);
      s.run(25, [](double) { return 4.19; });
      v.need(s.d.state() == S::FULL, "not full on the charger");
      const double t0 = s.now();
      s.clearSeen();
      s.run(withinMin + 5, [=](double m) { return 4.19 - rate * (m - t0); });
      v.need(s.firstAt[S::DISCHARGING] >= 0 && s.firstAt[S::DISCHARGING] - t0 <= withinMin, "discharging not shown in time");
      v.need((s.seen & bit(S::CHARGING)) == 0, "called it charging");
    });
  };
  unplugFromFull("unplugged when full, steep fall (1.7 mV/min)", 0.0017, 14);
  unplugFromFull("unplugged when full, moderate fall (0.8 mV/min)", 0.0008, 30);
  unplugFromFull("unplugged when full, gentle fall (0.5 mV/min)", 0.0005, 45);

  forSeeds("booted while charging (+2 mV/min)", [](uint64_t seed, Verdict &v) {
    Sim s(seed);
    s.run(17, [](double m) { return 3.90 + 0.002 * m; });
    v.need(s.d.state() == S::CHARGING, "not charging by minute 17");
    s.clearSeen();
    s.run(40, [](double m) { return 3.90 + 0.002 * (m + 17); });
    v.need(s.changes == 0, "flapped");
  });

  forSeeds("booted while charging gently (+1 mV/min)", [](uint64_t seed, Verdict &v) {
    Sim s(seed);
    s.run(60, [](double m) { return 3.80 + 0.001 * m; });
    v.need(s.d.state() != S::DISCHARGING, "a rising voltage was called discharging");
  });

  forSeeds("booted on battery (-1 mV/min)", [](uint64_t seed, Verdict &v) {
    Sim s(seed);
    s.run(18, [](double m) { return 3.85 - 0.001 * m; });
    v.need(s.d.state() == S::DISCHARGING, "not discharging by minute 18");
    s.clearSeen();
    s.run(60, [](double m) { return 3.85 - 0.001 * (m + 18); });
    v.need((s.seen & kClaimsPower) == 0, "called it charging or full");
  });

  forSeeds("held at 4.18 V on a charger for three hours", [](uint64_t seed, Verdict &v) {
    Sim s(seed);
    s.run(24, [](double) { return 4.18; });
    v.need(s.d.state() == S::FULL, "not full by minute 24");
    s.clearSeen();
    s.run(180, [](double) { return 4.18; });
    v.need(s.seen == bit(S::FULL), "wavered while sitting flat");
  });

  // The board draws more while it joins WiFi and fetches over TLS in the first minutes after boot, the
  // battery sags and then recovers.  The recovery must not pass for a plug-in.
  auto startupSag = [](double m, double sagMv) { return m < 1.0 ? sagMv : (m < 2.5 ? sagMv * (2.5 - m) / 1.5 : 0.0); };
  forSeeds("boot on battery with a 100 mV start-up sag", [startupSag](uint64_t seed, Verdict &v) {
    Sim s(seed);
    s.run(40, [startupSag](double m) { return 3.90 - 0.0006 * m - startupSag(m, 100) / 1000.0; });
    v.need((s.seen & kClaimsPower) == 0, "the start-up sag looked like charging");
    v.need(s.d.state() == S::DISCHARGING, "not discharging after 40 minutes");
  });
  forSeeds("boot on a charger (full) with a 100 mV start-up sag", [startupSag](uint64_t seed, Verdict &v) {
    Sim s(seed);
    s.run(40, [startupSag](double m) { return 4.19 - startupSag(m, 100) / 1000.0; });
    v.need((s.seen & bit(S::DISCHARGING)) == 0, "the start-up sag looked like discharging");
    v.need(s.d.state() == S::FULL, "not full after 40 minutes");
  });
  {  // the first three minutes are thrown away, and the Info page can say so
    Sim s(5);
    auto f = [](double m) { return 3.90 - 0.001 * m; };
    s.run(2.9, f);
    CHECK(s.d.warmingUp() && s.d.minutesOfData() == 0.0f);
    s.run(1.1, f);
    CHECK(!s.d.warmingUp() && s.d.minutesOfData() > 0.0f && s.d.minutesOfData() < 1.5f);
  }

  // A load burst (a WiFi reconnect, say) every three minutes pulling the battery down 40 mV for 20 s,
  // long enough to fill a whole 15 s bin.
  auto bursts = [](double m) { return fmod(m, 3.0) < 0.333 ? -0.040 : 0.0; };
  forSeeds("charging with regular WiFi bursts", [bursts](uint64_t seed, Verdict &v) {
    Sim s(seed);
    s.run(20, [bursts](double m) { return 3.90 + 0.002 * m + bursts(m); });
    v.need(s.d.state() == S::CHARGING, "not charging by minute 20");
    s.clearSeen();
    s.run(80, [bursts](double m) { return 3.90 + 0.002 * (m + 20) + bursts(m); });
    v.need((s.seen & bit(S::DISCHARGING)) == 0, "bursts looked like discharging");
  });
  forSeeds("discharging with regular WiFi bursts", [bursts](uint64_t seed, Verdict &v) {
    Sim s(seed);
    s.run(22, [bursts](double m) { return 3.95 - 0.0008 * m + bursts(m); });
    v.need(s.d.state() == S::DISCHARGING, "not discharging by minute 22");
    s.clearSeen();
    s.run(80, [bursts](double m) { return 3.95 - 0.0008 * (m + 22) + bursts(m); });
    v.need((s.seen & kClaimsPower) == 0, "bursts looked like charging");
  });

  forSeeds("the flat middle of the discharge curve (-0.3 mV/min)", [](uint64_t seed, Verdict &v) {
    Sim s(seed);
    s.run(240, [](double m) { return 3.80 - 0.0003 * m; });
    v.need((s.seen & kClaimsPower) == 0, "called it charging or full");
    v.need(s.changes <= 2, "flapped");
  });

  forSeeds("pure noise around 3.70 V for four hours", [](uint64_t seed, Verdict &v) {
    Sim s(seed);
    s.run(240, [](double) { return 3.70; });
    v.need((s.seen & kClaimsPower) == 0, "noise looked like charging or full");
  });

  forSeeds("unplugged while charging (-80 mV step)", [](uint64_t seed, Verdict &v) {
    Sim s(seed);
    s.run(30, [](double m) { return 3.90 + 0.0025 * m; });
    v.need(s.d.state() == S::CHARGING, "not charging before the unplug");
    const double t0 = s.now();
    s.clearSeen();
    s.run(8, [t0](double m) { return 3.90 + 0.0025 * t0 - 0.080 - 0.0006 * (m - t0); });
    v.need(s.firstAt[S::DISCHARGING] >= 0 && s.firstAt[S::DISCHARGING] - t0 <= 2.5, "discharging not shown within 2.5 minutes");
    v.need((s.seen & kClaimsPower & ~bit(S::CHARGING)) == 0, "called it full");
  });

  forSeeds("plugged in when nearly full (+60 mV, capped at 4.2 V)", [](uint64_t seed, Verdict &v) {
    Sim s(seed);
    s.run(24, [](double m) { return 4.14 - 0.001 * m; });
    v.need(s.d.state() == S::DISCHARGING, "not discharging before the plug-in");
    const double t0 = s.now();
    s.clearSeen();
    s.run(25, [t0](double m) {
      const double x = 4.12 + 0.060 + 0.004 * (m - t0);
      return x > 4.2 ? 4.2 : x;
    });
    v.need(s.firstAt[S::CHARGING] >= 0 && s.firstAt[S::CHARGING] - t0 <= 2.5, "charging not shown within 2.5 minutes");
    v.need(s.d.state() == S::FULL, "not full 25 minutes later");
    v.need(s.changes == 2, "wrong sequence of states");
  });

  forSeeds("plugged in with only a small step (+25 mV, then +2 mV/min)", [](uint64_t seed, Verdict &v) {
    Sim s(seed);
    s.run(20, [](double m) { return 3.80 - 0.0006 * m; });
    const double t0 = s.now();
    s.clearSeen();
    s.run(30, [t0](double m) { return 3.80 - 0.0006 * t0 + 0.025 + 0.002 * (m - t0); });
    v.need(s.firstAt[S::CHARGING] >= 0 && s.firstAt[S::CHARGING] - t0 <= 14.0, "charging not shown within 14 minutes");
  });

  // A change in load moves the level for good; to a straight-line fit that looks like a ramp.
  forSeeds("load drops by 15 mV while on battery", [](uint64_t seed, Verdict &v) {
    Sim s(seed);
    auto f = [](double m) { return 3.90 - 0.0006 * m + (m > 30 ? 0.015 : 0.0); };
    s.run(20, f);
    s.clearSeen();
    s.run(60, f);
    v.need((s.seen & kClaimsPower) == 0, "a level shift looked like charging");
  }, 2);
  forSeeds("load drops by 25 mV in the middle of discharging", [](uint64_t seed, Verdict &v) {
    Sim s(seed);
    auto f = [](double m) { return 3.90 - 0.0006 * m + (m > 40 ? 0.025 : 0.0); };
    s.run(25, f);
    s.clearSeen();
    s.run(60, f);
    v.need((s.seen & kClaimsPower) == 0, "a level shift looked like charging");
  }, 2);
  forSeeds("load rises by 8 mV while full on the charger", [](uint64_t seed, Verdict &v) {
    Sim s(seed);
    auto f = [](double m) { return 4.19 - (m > 30 ? 0.008 : 0.0); };
    s.run(25, f);
    v.need(s.d.state() == S::FULL, "not full before the shift");
    s.clearSeen();
    s.run(60, f);
    v.need(s.seen == bit(S::FULL), "a small level shift took it out of FULL");
  });
  // A bigger drop at the top of the curve is, to a voltage reading, a battery that has started
  // supplying the load.  It may say so for a while; it must never say charging and must settle.
  forSeeds("load rises by 15 mV while full on the charger", [](uint64_t seed, Verdict &v) {
    Sim s(seed);
    auto f = [](double m) { return 4.19 - (m > 30 ? 0.015 : 0.0); };
    s.run(25, f);
    v.need(s.d.state() == S::FULL, "not full before the shift");
    s.clearSeen();
    s.run(45, f);
    v.need((s.seen & bit(S::CHARGING)) == 0, "a drop was called charging");
    v.need(s.d.state() == S::FULL, "did not settle back to FULL");
  });

  {  // the battery being removed, or readings stopping, starts it afresh
    Sim s(7);
    s.run(30, [](double m) { return 3.90 + 0.003 * m; });
    CHECK(s.d.state() == S::CHARGING && s.d.ready());
    s.d.reset();
    CHECK(s.d.state() == S::UNKNOWN && !s.d.ready() && s.d.minutesOfData() == 0.0f);

    ChargeDetector g;
    uint32_t t = 0;
    for (; t < 20 * 60000u; t += 5000) g.addSample(3.95f + 0.0025f * (t / 60000.0f), t);
    CHECK(g.state() == S::CHARGING);
    g.addSample(3.9f, t + 60000);  // 60 s of silence breaks the time axis
    CHECK(g.state() == S::UNKNOWN && g.minutesOfData() == 0.0f);
    // and the millisecond counter wrapping around is no gap at all
    ChargeDetector w;
    uint32_t tw = 0xFFFFFFFFu - 5 * 60000u;
    for (int i = 0; i < 12 * 60 / 5 + 40; i++, tw += 5000) w.addSample(3.95f - 0.001f * (i * 5 / 60.0f), tw);
    CHECK(w.state() == S::DISCHARGING);
  }
}

// ---------------------------------------------------------------------------
// Frame scheduling on a simulated UI loop.  The clock advances by what each step
// costs (draw about 30 ms, send about 15 ms, naps of 4-5 ms) and by stalls injected
// at random moments (a flash write, the WiFi stack hogging the memory bus); every
// frame's finishing time is compared with the second it is for.  The loop the
// firmware had before the planner (draw for the coming second `cost + 8 ms` ahead)
// is simulated alongside for comparison.
// ---------------------------------------------------------------------------
namespace {
struct LoopSim {
  int64_t t;  // wall clock, microseconds
  uint64_t rng;
  double stallEverySec = 0;  // mean time between stalls; 0 = none
  int stallMinMs = 0, stallMaxMs = 0;
  double spikeProb = 0, spikeFactor = 4;  // some draws take this many times longer
  double forceEverySec = 0;               // mean time between forced redraws (a key press, a toast); 0 = none
  int64_t nextStallAt = INT64_MAX, nextForceAt = INT64_MAX, nextSlowAt = 0;
  bool forcePending = false;

  struct Shown {
    int64_t sec;
    int64_t endUs;
    bool scheduled;
  };
  std::vector<Shown> shown;

  explicit LoopSim(uint64_t seed) : t(1000000LL * 1000000LL + 123456), rng(seed * 0x9E3779B97F4A7C15ull + 99) {}

  double uniform() {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(rng >> 11) / 9007199254740992.0;
  }
  double expo(double mean) { return -mean * log(1.0 - uniform() + 1e-12); }
  void arm() {
    if (stallEverySec > 0) nextStallAt = t + (int64_t)(expo(stallEverySec) * 1e6);
    if (forceEverySec > 0) nextForceAt = t + (int64_t)(expo(forceEverySec) * 1e6);
  }

  void advance(int64_t us) {
    t += us;
    while (t >= nextStallAt) {  // the task is frozen for a while, asleep or not
      t += (int64_t)((stallMinMs + uniform() * (stallMaxMs - stallMinMs)) * 1000.0);
      nextStallAt = t + (int64_t)(expo(stallEverySec) * 1e6);
    }
    if (t >= nextForceAt) {
      forcePending = true;
      nextForceAt = t + (int64_t)(expo(forceEverySec) * 1e6);
    }
  }
  int64_t drawCost() { return (int64_t)(30000 * (0.9 + 0.2 * uniform()) * (uniform() < spikeProb ? spikeFactor : 1.0)); }
  int64_t sendCost() { return (int64_t)(15000 * (0.9 + 0.2 * uniform())); }
  void nap(int ms) { advance(ms * 1000 + (int64_t)(uniform() * 1000)); }  // timer tick granularity
  void slowWork() {                                                       // the sensors, every 5 s when allowed
    if (t >= nextSlowAt) {
      advance(25000);
      nextSlowAt = t + 5000000;
    }
  }

  // The planner-driven loop
  void runNew(int seconds) {
    FramePlanner p(20000);
    arm();
    const int64_t end = t + (int64_t)seconds * 1000000;
    auto draw = [&](time_t sec, bool scheduled) {
      const int64_t c = drawCost();
      advance(c);
      p.drew(sec, (uint32_t)c, scheduled);
    };
    auto send = [&]() {
      const int64_t c = sendCost();
      advance(c);
      shown.push_back({(int64_t)p.preparedSec(), t, p.preparedScheduled()});
      p.sent((uint32_t)c);
    };
    draw((time_t)(t / 1000000), false);  // start-up
    send();
    while (t < end) {
      const bool force = forcePending;
      forcePending = false;
      const FramePlanner::Step st = p.next((time_t)(t / 1000000), (int32_t)(t % 1000000), force);
      switch (st.action) {
        case FramePlanner::DRAW_NOW:
          draw(st.sec, false);
          send();
          break;
        case FramePlanner::PREPARE:
          draw(st.sec, true);
          break;
        case FramePlanner::SEND:
          advance(st.waitUs);
          send();
          break;
        case FramePlanner::SLEEP:
          nap(st.waitUs > 12000 ? 4 : 1);
          break;
        default:
          if (p.quiet((int32_t)(t % 1000000))) slowWork();
          nap(4);
          break;
      }
    }
  }

  // The loop as it was before: one step that draws and sends, started `cost + 8 ms` ahead.
  void runOld(int seconds) {
    arm();
    const int64_t end = t + (int64_t)seconds * 1000000;
    uint32_t frameCost = 45000;
    time_t lastDrawn = 0;
    while (t < end) {
      const int32_t usToNext = 1000000 - (int32_t)(t % 1000000);
      bool draw = forcePending;
      forcePending = false;
      time_t sec = (time_t)(t / 1000000);
      if (usToNext <= (int32_t)(frameCost + 8000)) sec += 1;
      if (sec != lastDrawn) draw = true;
      if (draw) {
        const int64_t c = drawCost() + sendCost();
        advance(c);
        frameCost = (uint32_t)((frameCost * 3 + c) / 4);
        shown.push_back({(int64_t)sec, t, true});
        lastDrawn = sec;
      } else {
        if (usToNext > 300000) slowWork();
        nap(4);
      }
    }
  }

  // Lateness of each scheduled frame once it is on the glass (sent + 20 ms of panel), in ms
  // after its second began.  0 is perfect.
  std::vector<double> lateness(size_t skipFrames = 5) const {
    std::vector<double> v;
    for (size_t i = skipFrames; i < shown.size(); i++)
      if (shown[i].scheduled) v.push_back(((shown[i].endUs + 20000) - shown[i].sec * 1000000LL) / 1000.0);
    return v;
  }
  static int countAbove(const std::vector<double> &v, double ms) {
    int n = 0;
    for (double x : v) n += x > ms;
    return n;
  }
  // Every second between the 5th and the one 5 s before the end appears at least once, in order.
  bool everySecondShown(int fromSec = 5, int untilSecFromEnd = 5) const {
    if (shown.size() < 10) return false;
    const int64_t first = shown.front().sec + fromSec, last = shown.back().sec - untilSecFromEnd;
    int64_t want = first;
    int64_t prev = -1;
    for (const Shown &s : shown) {
      if (prev >= 0 && s.sec < prev) return false;  // never backwards
      prev = s.sec;
      if (s.sec == want) want++;
      else if (s.sec > want && want <= last) return false;  // a second was skipped
    }
    return want > last;
  }
};
}  // namespace

static void testFramePlan() {
  section("frame scheduling");
  using P = FramePlanner;

  {  // quiet(): slow work only with plenty of time before the tick and nothing waiting to be sent
    P p(20000);
    CHECK(p.quiet(100000));
    CHECK(!p.quiet(800000));  // 200 ms to the tick
    p.drew(100, 30000, true);
    CHECK(!p.quiet(100000));
    p.sent(15000);
    CHECK(p.quiet(100000));
  }
  {  // the decisions at a few moments, with the default costs (send lead 35 ms, draw lead 125 ms)
    P p(20000);
    p.drew(500, 30000, false);
    p.sent(15000);  // the frame for second 500 is on the glass
    P::Step s = p.next(500, 400000, false);
    CHECK(s.action == P::IDLE);
    s = p.next(500, 880000, false);  // 120 ms to the tick: time to draw the next second
    CHECK(s.action == P::PREPARE && s.sec == 501);
    p.drew(501, 30000, true);
    s = p.next(500, 890000, false);  // 110 ms left, due in 75 ms: sleep
    CHECK(s.action == P::SLEEP);
    s = p.next(500, 960000, false);  // 40 ms left, due in 5 ms: send after a short wait
    CHECK(s.action == P::SEND && s.waitUs > 0 && s.waitUs <= 5000);
    p.sent(15000);
    s = p.next(500, 990000, false);
    CHECK(s.action == P::IDLE);
    s = p.next(501, 100000, false);  // the second has begun and it was shown: nothing to do
    CHECK(s.action == P::IDLE);
  }
  {  // a forced redraw is immediate, and in the last ~110 ms before a tick it shows the second about to start
    P p(20000);
    p.drew(500, 30000, false);
    p.sent(15000);
    P::Step s = p.next(500, 300000, true);
    CHECK(s.action == P::DRAW_NOW && s.sec == 500);
    s = p.next(500, 850000, true);  // 150 ms left: still the current second
    CHECK(s.action == P::DRAW_NOW && s.sec == 500);
    s = p.next(500, 920000, true);  // 80 ms left: not enough to draw this one and still prepare the next
    CHECK(s.action == P::DRAW_NOW && s.sec == 501);
    s = p.next(500, 980000, true);
    CHECK(s.action == P::DRAW_NOW && s.sec == 501);
  }
  {  // far behind (start-up or a long stall): draw the current second at once
    P p(20000);
    CHECK(p.next(500, 100000, false).action == P::DRAW_NOW);
    p.drew(500, 30000, false);
    p.sent(15000);
    P::Step s = p.next(503, 100000, false);
    CHECK(s.action == P::DRAW_NOW && s.sec == 503);
  }
  {  // a prepared frame whose second has begun is sent without waiting; one from the future is dropped
    P p(20000);
    p.drew(500, 30000, false);
    p.sent(15000);
    p.drew(501, 30000, true);
    P::Step s = p.next(501, 20000, false);
    CHECK(s.action == P::SEND && s.waitUs == 0);
    P q(20000);
    q.drew(500, 30000, false);
    q.sent(15000);
    q.drew(501, 30000, true);
    s = q.next(450, 100000, false);  // the clock jumped back by 50 seconds
    CHECK(!q.prepared());
    CHECK(s.action == P::DRAW_NOW && s.sec == 450);
  }
  {  // one freak cost does not move the schedule much
    P p(20000);
    p.drew(500, 30000, false);
    p.sent(15000);
    p.drew(501, 5000000, true);  // a draw that "took" five seconds
    CHECK(p.drawCostUs() <= 60000);
  }

  // --- simulated loops -------------------------------------------------------------------
  {  // no stalls, no spikes: every frame lands within a few ms of plan
    LoopSim s(1);
    s.runNew(300);
    CHECK(s.everySecondShown());
    const std::vector<double> late = s.lateness();
    double lo = 1e9, hi = -1e9;
    for (double x : late) lo = fmin(lo, x), hi = fmax(hi, x);
    CHECK(late.size() > 280);
    CHECK(lo > -12.0 && hi < 12.0);  // the nap granularity is the only error
  }
  {  // one draw in ten takes four times as long (120 ms): the old loop shows those frames late, the new one never does
    int newLate = 0, oldLate = 0, frames = 0;
    for (uint64_t seed = 1; seed <= 8; seed++) {
      LoopSim a(seed), b(seed);
      a.spikeProb = b.spikeProb = 0.10;
      a.runNew(200);
      b.runOld(200);
      CHECK(a.everySecondShown() && b.everySecondShown());
      newLate += LoopSim::countAbove(a.lateness(), 50.0);
      oldLate += LoopSim::countAbove(b.lateness(), 50.0);
      frames += (int)a.lateness().size();
    }
    printf("  draw spikes: frames more than 50 ms late: new %d, old %d of %d\n", newLate, oldLate, frames);
    CHECK(newLate == 0);
    CHECK(oldLate > frames / 40);  // the comparison would prove nothing if the old loop coped
  }
  {  // stalls of 20-300 ms about every 8 s, plus the draw spikes: fewer late frames than before at every threshold
    int newL[2] = {0, 0}, oldL[2] = {0, 0}, frames = 0;
    for (uint64_t seed = 1; seed <= 12; seed++) {
      LoopSim a(seed), b(seed);
      for (LoopSim *s : {&a, &b}) {
        s->stallEverySec = 8;
        s->stallMinMs = 20;
        s->stallMaxMs = 300;
        s->spikeProb = 0.05;
      }
      a.runNew(300);
      b.runOld(300);
      CHECK(a.everySecondShown() && b.everySecondShown());
      const double thr[2] = {50.0, 150.0};
      for (int k = 0; k < 2; k++) {
        newL[k] += LoopSim::countAbove(a.lateness(), thr[k]);
        oldL[k] += LoopSim::countAbove(b.lateness(), thr[k]);
      }
      frames += (int)a.lateness().size();
    }
    printf("  stalls: frames more than 50 ms late: new %d, old %d; more than 150 ms: new %d, old %d (of %d)\n", newL[0],
           oldL[0], newL[1], oldL[1], frames);
    CHECK(newL[0] <= oldL[0]);
    CHECK(newL[1] <= oldL[1]);
    CHECK(newL[0] < frames / 12);  // and they are rare in absolute terms
  }
  {  // forced redraws (key presses, toasts) at random moments: nothing skipped or sent backwards, the schedule recovers
    for (uint64_t seed = 1; seed <= 6; seed++) {
      LoopSim s(seed);
      s.forceEverySec = 3.0;
      s.runNew(200);
      CHECK(s.everySecondShown());
      const std::vector<double> late = s.lateness(20);
      CHECK(late.size() > 120);
      CHECK(LoopSim::countAbove(late, 50.0) == 0);
    }
  }
  {  // the clock jumps (an NTP correction) forwards and backwards: no second is repeated or lost for long, nothing sticks
    LoopSim s(3);
    FramePlanner p(20000);
    int64_t jumps[][2] = {{40, 420000}, {80, -380000}, {120, 1300000}, {160, -1600000}, {200, 5000000}, {240, -3000000}};
    size_t nextJump = 0;
    const int64_t t0 = s.t;
    auto draw = [&](time_t sec, bool scheduled) {
      const int64_t c = s.drawCost();
      s.advance(c);
      p.drew(sec, (uint32_t)c, scheduled);
    };
    auto send = [&]() {
      const int64_t c = s.sendCost();
      s.advance(c);
      s.shown.push_back({(int64_t)p.preparedSec(), s.t, p.preparedScheduled()});
      p.sent((uint32_t)c);
    };
    draw((time_t)(s.t / 1000000), false);
    send();
    int64_t lastJumpAt = 0;
    int wrongAfterRecovery = 0, framesAfterRecovery = 0;
    while (s.t < t0 + 300 * 1000000LL) {
      if (nextJump < sizeof jumps / sizeof *jumps && s.t - t0 >= jumps[nextJump][0] * 1000000LL) {
        s.t += jumps[nextJump][1];
        lastJumpAt = s.t;
        nextJump++;
      }
      const FramePlanner::Step st = p.next((time_t)(s.t / 1000000), (int32_t)(s.t % 1000000), false);
      switch (st.action) {
        case FramePlanner::DRAW_NOW: draw(st.sec, false); send(); break;
        case FramePlanner::PREPARE: draw(st.sec, true); break;
        case FramePlanner::SEND: s.advance(st.waitUs); send(); break;
        case FramePlanner::SLEEP: s.nap(st.waitUs > 12000 ? 4 : 1); break;
        default: s.nap(4); break;
      }
      // once 3 s past a jump the frames must be back on schedule
      if (!s.shown.empty() && s.t - lastJumpAt > 3000000 && s.shown.back().scheduled) {
        const double late = ((s.shown.back().endUs + 20000) - s.shown.back().sec * 1000000LL) / 1000.0;
        framesAfterRecovery++;
        if (late > 15.0 || late < -15.0) wrongAfterRecovery++;
        s.shown.back().scheduled = false;  // count each frame once
      }
    }
    CHECK(framesAfterRecovery > 100);
    CHECK(wrongAfterRecovery == 0);
  }
}

// ---------------------------------------------------------------------------
static void testWeather() {
  section("weather");
  std::string body = readFile("fixtures/open-meteo-forecast-perth.json");
  WeatherData w;
  ZoneInfo z;
  CHECK(parseOpenMeteoForecast(body.c_str(), body.size(), &w, &z));
  CHECK(w.valid);
  CHECK_NEAR(w.temp, 20.2, 1e-4);
  CHECK_NEAR(w.feels, 19.7, 1e-4);
  CHECK(w.humidity == 57);
  CHECK(w.code == 2);
  CHECK(w.isDay);
  CHECK_NEAR(w.windKmh, 12.3, 1e-4);
  CHECK(w.day[0].code == 3 && w.day[1].code == 3 && w.day[2].code == 51);
  CHECK_NEAR(w.day[0].tmax, 20.3, 1e-4);
  CHECK_NEAR(w.day[0].tmin, 12.0, 1e-4);
  CHECK_NEAR(w.day[1].tmax, 24.4, 1e-4);
  CHECK_NEAR(w.day[1].tmin, 11.6, 1e-4);
  CHECK_NEAR(w.day[2].tmax, 19.9, 1e-4);
  CHECK_NEAR(w.day[2].tmin, 15.2, 1e-4);
  CHECK(w.day[0].rainPct == 2 && w.day[1].rainPct == 2 && w.day[2].rainPct == 45);
  CHECK(w.day[0].weekday == 1 && w.day[1].weekday == 2 && w.day[2].weekday == 3);  // Mon, Tue, Wed
  CHECK_STR(w.sunrise, "05:49");
  CHECK_STR(w.sunset, "18:21");
  CHECK_NEAR(w.uvMax, 7.05, 1e-4);
  CHECK_STR(z.iana, "Australia/Perth");
  CHECK(z.utcOffsetSec == 28800);
  CHECK_STR(tzPosixForIana(z.iana), "AWST-8");

  // icon / text mapping for the codes in the fixture
  CHECK(wmoIcon(0) == WX_CLEAR && wmoIcon(2) == WX_PARTLY && wmoIcon(3) == WX_CLOUDY && wmoIcon(51) == WX_DRIZZLE);
  CHECK(wmoIcon(95) == WX_THUNDER && wmoIcon(73) == WX_SNOW && wmoIcon(65) == WX_RAIN);
  CHECK_STR(wmoText(0), "Clear");
  CHECK_STR(wmoText(1234), "Unknown");

  // malformed input must fail cleanly
  WeatherData bad;
  CHECK(!parseOpenMeteoForecast("", 0, &bad, &z));
  CHECK(!parseOpenMeteoForecast("{}", 2, &bad, &z));
  CHECK(!parseOpenMeteoForecast("{\"current\":{},\"daily\":{}}", 25, &bad, &z));
  CHECK(!parseOpenMeteoForecast(body.c_str(), body.size() / 2, &bad, &z));  // truncated
  std::string two = body;
  size_t p = two.find("\"2026-10-07\"");  // drop the third day's date
  if (p != std::string::npos) two.replace(p, 12, "null");
  CHECK(!parseOpenMeteoForecast(two.c_str(), two.size(), &bad, &z));

  // geocoder
  std::string geo = readFile("fixtures/open-meteo-geocode-perth.json");
  GeoResult g;
  CHECK(parseOpenMeteoGeocode(geo.c_str(), geo.size(), &g));
  CHECK_NEAR(g.lat, -31.95224, 1e-5);
  CHECK_NEAR(g.lon, 115.8614, 1e-5);
  CHECK_STR(g.name, "Perth");
  CHECK_STR(g.admin1, "Western Australia");
  CHECK_STR(g.country, "AU");
  CHECK_STR(g.iana, "Australia/Perth");
  CHECK(!parseOpenMeteoGeocode("{\"generationtime_ms\":0.1}", 24, &g));  // no results key
  CHECK(!parseOpenMeteoGeocode("{\"results\":[]}", 14, &g));
}

// ---------------------------------------------------------------------------
static void testSpotify() {
  section("spotify parsing");
  char devId[48];
  SpotifyInfo s;
  s.status = SPOTIFY_IDLE;
  copyStr(s.message, sizeof s.message, "keep me");

  std::string track = readFile("fixtures/spotify-player-track.json");
  CHECK(parsePlayerState(track.c_str(), track.size(), &s, devId, sizeof devId));
  CHECK(s.status == SPOTIFY_PLAYING);
  CHECK_STR(s.title, "Blinding Lights");
  CHECK_STR(s.artist, "The Weeknd, Daft Punk");
  CHECK_STR(s.album, "After Hours");
  CHECK_STR(s.device, "Kitchen speaker");
  CHECK(s.durationMs == 200040 && s.progressMs == 83000);
  CHECK(s.volume == 59);
  CHECK(s.shuffle);
  CHECK_STR(devId, "0d1841b0976bae2a3a310dd74c0f3df354899bc8");
  CHECK_STR(s.message, "keep me");  // caller-owned fields are preserved

  std::string ep = readFile("fixtures/spotify-player-episode.json");
  CHECK(parsePlayerState(ep.c_str(), ep.size(), &s, devId, sizeof devId));
  CHECK(s.status == SPOTIFY_PAUSED);
  CHECK_STR(s.title, "#412 - A very long episode title about many things");
  CHECK_STR(s.artist, "The Example Podcast");
  CHECK_STR(s.album, "");
  CHECK(s.volume == -1);  // null volume
  CHECK(s.progressMs == 1234567 && s.durationMs == 5400000);
  CHECK(!s.shuffle);

  std::string ad = readFile("fixtures/spotify-player-ad.json");
  CHECK(parsePlayerState(ad.c_str(), ad.size(), &s, devId, sizeof devId));
  CHECK(s.status == SPOTIFY_PLAYING);
  CHECK_STR(s.title, "Advertisement");
  CHECK_STR(s.artist, "");
  CHECK(s.durationMs == 0);

  // oversized strings are cut, never overflow
  std::string longName(400, 'x');
  std::string big = "{\"is_playing\":true,\"progress_ms\":1,\"item\":{\"name\":\"" + longName +
                    "\",\"duration_ms\":10,\"artists\":[";
  for (int i = 0; i < 40; i++) big += std::string(i ? "," : "") + "{\"name\":\"Artist Number " + std::to_string(i) + "\"}";
  big += "],\"album\":{\"name\":\"" + longName + "\"}},\"device\":{\"name\":\"" + longName + "\"}}";
  CHECK(parsePlayerState(big.c_str(), big.size(), &s, devId, sizeof devId));
  CHECK(strlen(s.title) == sizeof(s.title) - 1);
  CHECK(strlen(s.artist) < sizeof(s.artist));
  CHECK(strncmp(s.artist, "Artist Number 0, Artist Number 1", 32) == 0);
  CHECK(s.artist[strlen(s.artist) - 1] != ' ' && s.artist[strlen(s.artist) - 1] != ',');
  CHECK(strlen(s.device) == sizeof(s.device) - 1);

  CHECK(!parsePlayerState("not json", 8, &s, devId, sizeof devId));
  CHECK(!parsePlayerState("", 0, &s, devId, sizeof devId));

  // token replies
  SpotifyTokenReply tr;
  const char *tok =
      "{\"access_token\":\"BQDabc\",\"token_type\":\"Bearer\",\"expires_in\":3600,"
      "\"refresh_token\":\"AQBxyz\",\"scope\":\"user-read-playback-state\"}";
  CHECK(parseTokenReply(tok, strlen(tok), &tr));
  CHECK_STR(tr.accessToken, "BQDabc");
  CHECK_STR(tr.refreshToken, "AQBxyz");
  CHECK(tr.expiresInSec == 3600);
  const char *tok2 = "{\"access_token\":\"BQDdef\",\"token_type\":\"Bearer\",\"expires_in\":3600}";
  CHECK(parseTokenReply(tok2, strlen(tok2), &tr));
  CHECK_STR(tr.refreshToken, "");  // not rotated
  CHECK(!parseTokenReply("{\"error\":\"invalid_grant\"}", 25, &tr));
  CHECK(!parseTokenReply("{}", 2, &tr));

  // authorize URL: exact expected text, produced independently with Python's urllib.parse.quote
  char authUrl[600];
  size_t authLen = buildAuthorizeUrl("abc123", "http://127.0.0.1:8888/callback",
                                     "user-read-playback-state user-modify-playback-state",
                                     "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM", "Zx9", authUrl, sizeof authUrl);
  CHECK(authLen == strlen(authUrl) && authLen > 0);
  CHECK_STR(authUrl,
            "https://accounts.spotify.com/authorize?client_id=abc123&response_type=code"
            "&redirect_uri=http%3A%2F%2F127.0.0.1%3A8888%2Fcallback"
            "&scope=user-read-playback-state%20user-modify-playback-state"
            "&code_challenge_method=S256&code_challenge=E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM&state=Zx9");
  char tinyUrl[40];
  CHECK(buildAuthorizeUrl("abc123", "http://127.0.0.1:8888/callback", "x", "y", "z", tinyUrl, sizeof tinyUrl) == 0);

  // error bodies
  char reason[40], msg[80];
  const char *e1 = "{\"error\":\"invalid_grant\",\"error_description\":\"Refresh token revoked\"}";
  parseSpotifyError(e1, strlen(e1), reason, sizeof reason, msg, sizeof msg);
  CHECK_STR(reason, "invalid_grant");
  CHECK_STR(msg, "Refresh token revoked");
  const char *e2 =
      "{\"error\":{\"status\":404,\"message\":\"Player command failed: No active device found\","
      "\"reason\":\"NO_ACTIVE_DEVICE\"}}";
  parseSpotifyError(e2, strlen(e2), reason, sizeof reason, msg, sizeof msg);
  CHECK_STR(reason, "NO_ACTIVE_DEVICE");
  CHECK_STR(msg, "Player command failed: No active device found");
  parseSpotifyError("garbage", 7, reason, sizeof reason, msg, sizeof msg);
  CHECK_STR(reason, "");
  parseSpotifyError(nullptr, 0, reason, sizeof reason, msg, sizeof msg);
  CHECK_STR(reason, "");
}

// ---------------------------------------------------------------------------
static void testLinkPage() {
  section("setup page");
  LinkPageInfo info;
  info.appName = "RLCD Clock";
  info.redirectUri = "http://127.0.0.1:8888/callback";
  info.clockAddress = "192.168.1.50";
  info.authorizeUrl = "https://accounts.spotify.com/authorize?client_id=abc&response_type=code&state=Zx9";

  static char body[6000], doc[8000];
  size_t n = renderLinkPageBody(info, body, sizeof body);
  CHECK(n > 500 && n == strlen(body));
  std::string b(body);
  CHECK(b.find("not linked") != std::string::npos);
  CHECK(b.find("value='http://127.0.0.1:8888/callback'") != std::string::npos);  // the URI to register
  CHECK(b.find("python spotify_link.py 192.168.1.50</code>") != std::string::npos);  // the clock's own address
  CHECK(b.find("href='/spotify_link.py'") != std::string::npos);
  CHECK(b.find("action=/link") != std::string::npos);
  CHECK(b.find("client_id=abc&amp;response_type=code&amp;state=Zx9") != std::string::npos);  // & escaped in the href
  CHECK(b.find("client_id=abc&response_type") == std::string::npos);
  CHECK(b.find("class=err") == std::string::npos && b.find("class=ok") == std::string::npos);  // no message yet
  CHECK(b.find("expected: nothing there is listening") != std::string::npos);  // the dead end is explained

  info.linked = true;
  info.daysLeft = 9;
  info.message = "Linked! <b>done</b> & dusted";
  info.messageIsError = false;
  n = renderLinkPageBody(info, body, sizeof body);
  b = body;
  CHECK(b.find("linked (Spotify expires the link in 9 days") != std::string::npos);
  CHECK(b.find("<p class=ok>Linked! &lt;b&gt;done&lt;/b&gt; &amp; dusted</p>") != std::string::npos);  // escaped
  info.messageIsError = true;
  renderLinkPageBody(info, body, sizeof body);
  CHECK(std::string(body).find("<p class=err>") != std::string::npos);

  char tiny[100];
  CHECK(renderLinkPageBody(info, tiny, sizeof tiny) == 0);  // too small is reported, not truncated silently

  info.linked = false;
  info.message = "";
  renderLinkPageBody(info, body, sizeof body);
  size_t dn = renderPageDocument("RLCD Clock", body, doc, sizeof doc);
  CHECK(dn == strlen(doc) && dn > strlen(body));
  CHECK(strncmp(doc, "<!doctype html>", 15) == 0);
  CHECK(strcmp(doc + dn - 14, "</body></html>") == 0);
  CHECK(strstr(doc, "<title>RLCD Clock - Spotify</title>") != nullptr);
  CHECK(renderPageDocument("RLCD Clock", body, tiny, sizeof tiny) == 0);

  FILE *f = fopen("build/link_page.html", "wb");  // for looking at in a browser
  if (f) {
    fwrite(doc, 1, dn, f);
    fclose(f);
  }
}

static void testUtil() {
  section("util");
  char buf[128];
  CHECK(urlEncode("a b&c=d/\xC3\xA9", buf, sizeof buf) > 0);
  CHECK_STR(buf, "a%20b%26c%3Dd%2F%C3%A9");
  CHECK(urlEncode("http://127.0.0.1:8888/callback", buf, sizeof buf) > 0);
  CHECK_STR(buf, "http%3A%2F%2F127.0.0.1%3A8888%2Fcallback");
  CHECK(urlEncode("user-read-playback-state user-modify-playback-state", buf, sizeof buf) > 0);
  CHECK_STR(buf, "user-read-playback-state%20user-modify-playback-state");
  char small[8];
  CHECK(urlEncode("this is far too long", small, sizeof small) == 0);

  char out[256];
  const char *url = "http://127.0.0.1:8888/callback?code=AQD%2Bxy%2Fz_1-2&state=abc123#frag";
  CHECK(queryParam(url, "code", out, sizeof out));
  CHECK_STR(out, "AQD+xy/z_1-2");
  CHECK(queryParam(url, "state", out, sizeof out));
  CHECK_STR(out, "abc123");
  CHECK(!queryParam(url, "error", out, sizeof out));
  CHECK(!queryParam(url, "cod", out, sizeof out));  // prefix must not match
  CHECK(queryParam("error=access_denied&state=s", "error", out, sizeof out));
  CHECK_STR(out, "access_denied");
  CHECK(!queryParam("", "code", out, sizeof out));
  char tiny[4];
  CHECK(queryParam("?code=abcdefgh", "code", tiny, sizeof tiny));
  CHECK_STR(tiny, "abc");  // truncated, still terminated

  copyStr(tiny, sizeof tiny, "abcdefgh");
  CHECK_STR(tiny, "abc");
  copyStr(tiny, sizeof tiny, nullptr);
  CHECK_STR(tiny, "");

  // PKCE (RFC 7636 appendix B): verifier dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk
  const uint8_t digest[32] = {0x13, 0xd3, 0x1e, 0x96, 0x1a, 0x1a, 0xd8, 0xec, 0x2f, 0x16, 0xb1,
                              0x0c, 0x4c, 0x98, 0x2e, 0x08, 0x76, 0xa8, 0x78, 0xad, 0x6d, 0xf1,
                              0x44, 0x56, 0x6e, 0xe1, 0x89, 0x4a, 0xcb, 0x70, 0xf9, 0xc3};
  CHECK(base64UrlEncode(digest, 32, out, sizeof out) == 43);
  CHECK_STR(out, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
  const uint8_t a[1] = {0xFB}, b[2] = {0xFB, 0xFF}, c[3] = {0xFB, 0xFF, 0xBF};
  CHECK(base64UrlEncode(a, 1, out, sizeof out) == 2);
  CHECK_STR(out, "-w");
  CHECK(base64UrlEncode(b, 2, out, sizeof out) == 3);
  CHECK_STR(out, "-_8");
  CHECK(base64UrlEncode(c, 3, out, sizeof out) == 4);
  CHECK_STR(out, "-_-_");
  CHECK(base64UrlEncode(digest, 32, small, sizeof small) == 0);
}

// ---------------------------------------------------------------------------
struct Press {
  uint32_t down, up;
};

static std::vector<std::pair<uint32_t, ClickEvent>> runButton(const std::vector<Press> &presses,
                                                              uint32_t endMs,
                                                              bool bounce = false) {
  ClickDetector d(320, 800, 25);
  std::vector<std::pair<uint32_t, ClickEvent>> events;
  for (uint32_t t = 0; t <= endMs; t += 5) {
    bool down = false;
    for (auto &p : presses) down |= (t >= p.down && t < p.up);
    if (bounce) {  // contact chatter for 12 ms after each edge
      for (auto &p : presses) {
        if ((t >= p.down && t < p.down + 12) || (t >= p.up && t < p.up + 12)) down = (t / 5) % 2;
      }
    }
    ClickEvent e = d.update(down, t);
    if (e != CLICK_NONE) events.push_back({t, e});
  }
  return events;
}

static void testButtons() {
  section("buttons");
  auto one = runButton({{100, 180}}, 2000);
  CHECK(one.size() == 1 && one[0].second == CLICK_1);
  CHECK(one.size() == 1 && one[0].first >= 180 + 320 && one[0].first <= 180 + 320 + 40);  // after the gap

  auto two = runButton({{100, 160}, {260, 320}}, 2000);
  CHECK(two.size() == 1 && two[0].second == CLICK_2);

  auto three = runButton({{100, 160}, {260, 320}, {420, 480}}, 2000);
  CHECK(three.size() == 1 && three[0].second == CLICK_3);

  auto four = runButton({{100, 160}, {260, 320}, {420, 480}, {580, 640}}, 2000);
  CHECK(four.size() == 1 && four[0].second == CLICK_3);  // 3+ clicks all map to CLICK_3

  // clicks slower than the gap are separate gestures
  auto slow = runButton({{100, 160}, {900, 960}}, 3000);
  CHECK(slow.size() == 2 && slow[0].second == CLICK_1 && slow[1].second == CLICK_1);

  // long press: fires once, while held, with no click afterwards
  auto lng = runButton({{100, 1500}}, 3000);
  CHECK(lng.size() == 1 && lng[0].second == CLICK_LONG);
  CHECK(lng.size() == 1 && lng[0].first >= 100 + 800 && lng[0].first <= 100 + 800 + 40);

  // a click followed by a long press reports only the long press
  auto mixed = runButton({{100, 160}, {260, 1400}}, 3000);
  CHECK(mixed.size() == 1 && mixed[0].second == CLICK_LONG);

  // contact bounce must not create phantom clicks
  auto bouncy = runButton({{100, 200}}, 2000, true);
  CHECK(bouncy.size() == 1 && bouncy[0].second == CLICK_1);

  // a glitch shorter than the debounce time is ignored
  auto glitch = runButton({{100, 110}}, 2000);
  CHECK(glitch.empty());

  // millis() wrap-around
  ClickDetector d(320, 800, 25);
  uint32_t base = 0xFFFFFF00u;
  ClickEvent e = CLICK_NONE;
  for (uint32_t i = 0; i < 700; i += 5) {  // base + i wraps past 2^32 around i = 256
    bool down = i >= 20 && i < 100;
    ClickEvent r = d.update(down, base + i);
    if (r != CLICK_NONE) e = r;
  }
  CHECK(e == CLICK_1);
}

int main() {
  testCalendar();
  testTimeZones();
  testSensorMaths();
  testCharge();
  testFramePlan();
  testWeather();
  testSpotify();
  testUtil();
  testLinkPage();
  testButtons();
  printf("\n%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
