#pragma once

// Infers "charging / discharging / full" from the battery voltage alone.
//
// The ESP32-S3-RLCD-4.2 has no software-readable charge signal: the charger's
// STAT output only drives an LED and the sole power signal wired to the MCU is
// the battery voltage on GPIO4 (see PIN_CHARGE_STATUS in config.h if you add a
// wire to it).  What the voltage does tells the story, though:
//
//   * plugging USB in makes the charger push current through the cell's internal
//     resistance, so the terminal voltage steps UP by tens of millivolts at once;
//     pulling it out steps it DOWN;
//   * while charging at constant current the voltage climbs a few mV a minute;
//     while the cell carries the load it falls (steeply near full, slowly in the
//     flat middle of the curve);
//   * once the charger finishes, the voltage sits flat near 4.2 V.
//
// Readings are reduced to one value per 15 seconds, taken from the upper end of
// the bin: a WiFi transmit burst can pull a single reading down by tens of
// millivolts but never up, so the highest readings are the clean ones.  Twelve
// minutes of those are kept.  Two things are looked at:
//   1. a step: the last 30 s against 90..150 s ago, which must stay beyond
//      +-40 mV for three evaluations running.  This reacts about a minute after
//      the cable moves;
//   2. the trend: the slope of the whole history, which settles the cases a step
//      cannot (booting on a charger, unplugging a full battery where no current
//      flowed so nothing steps).  It needs eight minutes of data.  A change in
//      load moves the level for good, and to a plain straight-line fit that looks
//      like a ramp (a 12 mV shift mid-window reads as +1.5 mV/min), so a claim of
//      charging also needs a second slope, fitted with one level shift allowed
//      anywhere (the pooled slope of the two stretches either side of the best
//      split), to agree.  A ramp survives that, a shift does not.  The trend is
//      not consulted while a step is being judged, nor for twelve minutes after
//      one (the step spoils it: a decline and a jump can cancel into "flat").
//
// The readings of the first three minutes are thrown away: the board draws more
// while it joins WiFi and fetches over TLS, the battery sags, and the recovery
// looks like a plug-in.
//
// It is a heuristic and says UNKNOWN rather than guessing when the evidence is
// thin (for instance in the flat middle of the discharge curve, or in the first
// minutes after boot).  FULL means "sitting flat at the top of the curve", which
// is what a charger holding the cell at 4.2 V looks like; it also reads as full
// for a while after unplugging a full battery.  Pure logic with no Arduino
// dependency; tools/tests simulates plausible traces.

#include <math.h>
#include <stdint.h>
#include <string.h>

class ChargeDetector {
 public:
  enum State : uint8_t { UNKNOWN = 0, DISCHARGING = 1, CHARGING = 2, FULL = 3 };

  // Tunables (volts, volts per minute).
  static constexpr uint32_t kBinMs = 15000;       // one value per 15 s
  static constexpr uint32_t kMaxGapMs = 45000;    // a longer silence breaks the time axis: start over
  static constexpr uint32_t kWarmupMs = 180000;   // readings from the first three minutes are ignored
  static constexpr int kBinSamples = 8;           // readings kept per bin
  static constexpr int kBins = 48;                // twelve minutes of history
  static constexpr int kMinBinsForStep = 10;      // 150 s before a step can be judged
  static constexpr int kMinBinsForSlope = 32;     // eight minutes before the trend is trusted
  static constexpr int kMinStretch = 8;           // the level shift may sit no closer than 2 minutes to either end
  static constexpr int kStepPersist = 3;          // evaluations (15 s apart) a step must survive
  static constexpr float kStepV = 0.040f;             // 40 mV
  static constexpr float kChargeSlope = 0.0015f;      // +1.5 mV/min or more: charging
  static constexpr float kChargeSlopeFromFull = 0.0025f;  // ...but +2.5 mV/min to leave FULL for it
  static constexpr float kDischargeSlope = -0.0003f;  // -0.3 mV/min or less: discharging
  static constexpr float kHardFallSlope = -0.0010f;   // -1.0 mV/min: a fall that cannot be noise, even up at the top
  static constexpr float kFlatSlope = 0.00025f;       // within +-0.25 mV/min: flat
  static constexpr float kFullV = 4.16f;              // flat at or above this: held full
  static constexpr float kTopV = 4.15f;               // up here only a hard fall counts as discharging
  static constexpr float kFullDropV = 0.015f;         // 15 mV below the plateau, and still falling, ends FULL

  // Feed one voltage reading (volts) every few seconds.
  void addSample(float volts, uint32_t nowMs) {
    if (started_ && (uint32_t)(nowMs - lastSampleMs_) > kMaxGapMs) reset();
    lastSampleMs_ = nowMs;
    if (!started_) {
      started_ = true;
      startMs_ = nowMs;
    }
    // Right after boot the board is busy joining WiFi and fetching over TLS, the battery sags
    // under that and then recovers, which to this detector looks like a plug-in.  So the first
    // minutes of readings are ignored.
    if ((uint32_t)(nowMs - startMs_) < kWarmupMs) return;
    if (!binStarted_) {
      binStarted_ = true;
      binStartMs_ = nowMs;
    }
    if (count_ < kBinSamples) samples_[count_++] = volts;
    if ((uint32_t)(nowMs - binStartMs_) >= kBinMs) {
      finishBin(upperLevel(samples_, count_));
      count_ = 0;
      binStartMs_ = nowMs;
    }
  }

  // Forget everything (battery removed, long gap in the readings).
  void reset() { *this = ChargeDetector(); }

  State state() const { return state_; }
  // Still in the first minutes after start, whose readings are thrown away.
  bool warmingUp() const { return started_ && (uint32_t)(lastSampleMs_ - startMs_) < kWarmupMs; }
  // Minutes of history gathered so far (capped at the 12 kept), not counting the warm-up.
  float minutesOfData() const { return (float)bins_ * (kBinMs / 60000.0f); }
  // True once the trend is trusted, i.e. the detector has had eight minutes.
  bool ready() const { return bins_ >= kMinBinsForSlope; }
  // The trend in mV per minute (level shifts excluded); 0 until ready().
  float slopeMvPerMin() const { return slope_ * 1000.0f; }
  // The last step measurement (last 30 s against 90..150 s ago) in mV; 0 until 150 s of data.
  float stepMv() const { return step_ * 1000.0f; }

 private:
  // The value a bin stands for: its highest reading when there are three or fewer, the second
  // highest for four, and so on (one reading in three is thrown away from the top end).
  static float upperLevel(float *v, int n) {
    for (int i = 1; i < n; i++) {  // insertion sort, n <= kBinSamples
      const float x = v[i];
      int j = i - 1;
      while (j >= 0 && v[j] > x) {
        v[j + 1] = v[j];
        j--;
      }
      v[j + 1] = x;
    }
    return v[n - 1 - (n - 1) / 3];
  }

  void finishBin(float level) {
    if (bins_ < kBins) {
      means_[bins_++] = level;
    } else {
      memmove(means_, means_ + 1, (kBins - 1) * sizeof(float));
      means_[kBins - 1] = level;
    }
    evaluate();
  }

  void evaluate() {
    const int n = bins_;

    // 1. A plug / unplug step.
    if (n >= kMinBinsForStep) {
      const float recent = (means_[n - 1] + means_[n - 2]) * 0.5f;
      const float ref = (means_[n - 7] + means_[n - 8] + means_[n - 9] + means_[n - 10]) * 0.25f;
      step_ = recent - ref;
      if (step_ >= kStepV) {
        upStreak_++;
        downStreak_ = 0;
      } else if (step_ <= -kStepV) {
        downStreak_++;
        upStreak_ = 0;
      } else {
        upStreak_ = downStreak_ = 0;
      }
      if (upStreak_ >= kStepPersist) {
        state_ = CHARGING;
        holdoff_ = kBins;
      }
      if (downStreak_ >= kStepPersist) {
        state_ = DISCHARGING;
        holdoff_ = kBins;
      }
    }

    // 2. The trend over the history.  The step itself spoils it (a decline and a jump can cancel
    // into "flat", say), so the trend stays out of the verdict while a step is being judged and
    // until the step has left the twelve minutes it looks at.
    if (holdoff_ > 0) holdoff_--;
    if (n >= kMinBinsForSlope) {
      slope_ = plainSlope(n);
      if (holdoff_ == 0 && upStreak_ == 0 && downStreak_ == 0) applyTrend(n);
    }
  }

  void applyTrend(int n) {
    const float level = (lifted(n - 1) + lifted(n - 2) + lifted(n - 3) + lifted(n - 4)) * 0.25f;  // the last minute

    // Charging is the claim most worth guarding, and a level shift fools the plain slope into
    // it, so the slope that allows for a shift has to agree (at half the strength: it is a
    // noisier estimate, because the fit gets to pick where the shift sits).  Off a full plateau
    // a smaller shift passes for a ramp, and a real restart of charging comes with a step
    // anyway, so there the climb has to be clearer.
    const float needed = (state_ == FULL) ? kChargeSlopeFromFull : kChargeSlope;
    const bool rising = slope_ >= needed && shiftTolerantSlope(n) >= 0.5f * needed;
    const bool falling = slope_ <= kDischargeSlope;
    const bool fallingHard = slope_ <= kHardFallSlope;
    const bool flat = fabsf(slope_) < kFlatSlope;

    if (rising) {
      state_ = CHARGING;
    } else if (state_ == FULL) {
      // Off the plateau for good: clearly below where it settled, and still going down.
      if (level < fullRef_ - kFullDropV && falling) state_ = DISCHARGING;
    } else if (fallingHard || (falling && level < kTopV)) {
      // Up at the top of the curve (where a charger parks the cell) noise must not read as a
      // fall, so there only a hard one counts; lower down a gentle one is enough.
      state_ = DISCHARGING;
    }
    // Flat at the top of the curve: a charger is holding the cell full.
    if (flat && level >= kFullV) {
      fullRef_ = (state_ == FULL) ? fullRef_ + 0.02f * (level - fullRef_) : level;  // follow the plateau slowly
      state_ = FULL;
    }
  }

  // A bin lifted to the highest of itself and the two before it: a load burst of up to 30 s
  // pulls one or two bins down, and this hides it from the trend without hiding a real shift
  // (which keeps every bin lower for good).
  float lifted(int i) const {
    float v = means_[i];
    if (i >= 1 && means_[i - 1] > v) v = means_[i - 1];
    if (i >= 2 && means_[i - 2] > v) v = means_[i - 2];
    return v;
  }

  // Least-squares slope of the (lifted) history in volts per minute.
  float plainSlope(int n) const {
    float mean = 0;
    for (int i = 0; i < n; i++) mean += lifted(i);
    mean /= (float)n;
    const float xMean = 0.5f * (float)(n - 1);
    float sxy = 0, sxx = 0;
    for (int i = 0; i < n; i++) {
      const float dx = (float)i - xMean;
      sxy += dx * (lifted(i) - mean);
      sxx += dx * dx;
    }
    return (sxy / sxx) * (60000.0f / (float)kBinMs);
  }

  // Slope of the (lifted) history in volts per minute with one level shift allowed: for every
  // split point the two stretches get their own level but share one slope (their pooled
  // least-squares slope), and the split that leaves the smallest residual wins.
  float shiftTolerantSlope(int n) const {
    double px[kBins + 1], py[kBins + 1], pxx[kBins + 1], pxy[kBins + 1], pyy[kBins + 1];  // prefix sums
    px[0] = py[0] = pxx[0] = pxy[0] = pyy[0] = 0;
    const double y0 = lifted(0);
    for (int i = 0; i < n; i++) {
      const double x = i, y = lifted(i) - y0;
      px[i + 1] = px[i] + x;
      py[i + 1] = py[i] + y;
      pxx[i + 1] = pxx[i] + x * x;
      pxy[i + 1] = pxy[i] + x * y;
      pyy[i + 1] = pyy[i] + y * y;
    }
    double bestSse = 1e30, bestSlope = 0;
    for (int k = kMinStretch; k <= n - kMinStretch; k++) {
      double sxx[2], sxy[2], syy[2];
      const int lo[2] = {0, k}, hi[2] = {k, n};
      for (int s = 0; s < 2; s++) {  // each stretch about its own mean
        const double m = hi[s] - lo[s];
        const double sx = px[hi[s]] - px[lo[s]], sy = py[hi[s]] - py[lo[s]];
        sxx[s] = (pxx[hi[s]] - pxx[lo[s]]) - sx * sx / m;
        sxy[s] = (pxy[hi[s]] - pxy[lo[s]]) - sx * sy / m;
        syy[s] = (pyy[hi[s]] - pyy[lo[s]]) - sy * sy / m;
      }
      const double b = (sxy[0] + sxy[1]) / (sxx[0] + sxx[1]);
      const double sse = syy[0] + syy[1] - b * (sxy[0] + sxy[1]);
      if (sse < bestSse) {
        bestSse = sse;
        bestSlope = b;
      }
    }
    return (float)(bestSlope * (60000.0 / (double)kBinMs));
  }

  State state_ = UNKNOWN;
  bool started_ = false;
  bool binStarted_ = false;
  uint32_t startMs_ = 0;
  uint32_t binStartMs_ = 0;
  uint32_t lastSampleMs_ = 0;
  float samples_[kBinSamples] = {};
  int count_ = 0;
  float means_[kBins] = {};
  int bins_ = 0;
  int upStreak_ = 0;
  int downStreak_ = 0;
  int holdoff_ = 0;    // evaluations left in which the trend may not change the state
  float step_ = 0;     // volts
  float slope_ = 0;    // volts per minute
  float fullRef_ = 0;  // volts: where the plateau sits while FULL
};
