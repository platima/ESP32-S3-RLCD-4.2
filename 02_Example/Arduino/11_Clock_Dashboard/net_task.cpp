#include "net_task.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <sys/time.h>

#include <atomic>

#include "app_settings.h"
#include "app_state.h"
#include "calc.h"
#include "config.h"
#include "drift.h"
#include "log.h"
#include "power.h"
#include "radio_plan.h"
#include "spotify.h"
#include "tls.h"
#include "tz_table.h"
#include "util.h"
#include "weather.h"
#include "wifi_pick.h"

namespace {

const char *const TAG = "net";
const int kReturnMinRssi = -80;  // dBm: the main network must be at least this strong for the clock to go back to it
const time_t kMinPlausibleEpoch = 1704067200;  // 2024-01-01: anything earlier means "clock not set"

Preferences s_prefs;  // namespace "clock"
volatile uint32_t s_lastSyncUtc = 0;  // seconds; 32 bits so the lwIP task and this one never see a torn value
volatile bool s_rtcWriteRequest = false;
volatile bool s_weatherNow = false;
volatile uint32_t s_demandUntilMs = 0;  // wifi_mode = sync: a key press holds the radio on until then (netWake)
volatile bool s_radioNeedsFast = false;  // the radio is on or about to be: the CPU clock must stay at 80 MHz or more
bool s_tzFromOverride = false;

// The timer and the system clock read together at the end of each NTP sync, for the drift
// measurement (drift.h).  Written by the SNTP task, read by the network task: a sequence counter
// (odd while being written) makes the pair consistent without a lock.
struct SyncStamp {
  volatile int64_t mono = 0;
  volatile int64_t sys = 0;
  std::atomic<uint32_t> seq{0};
};
SyncStamp s_stamp;
drift::Tracker s_drift;  // network task only

// Called from the lwIP/SNTP task: only record the facts here.  The stamp goes first: the network
// task acts on a new s_lastSyncUtc, and must then find this sync's stamp, not the last one's.
void onTimeSync(struct timeval *tv) {
  struct timeval now;
  gettimeofday(&now, nullptr);
  const int64_t mono = esp_timer_get_time();
  s_stamp.seq.fetch_add(1, std::memory_order_acq_rel);  // odd: being written
  s_stamp.mono = mono;
  s_stamp.sys = (int64_t)now.tv_sec * 1000000 + now.tv_usec;
  s_stamp.seq.fetch_add(1, std::memory_order_release);  // even: complete
  s_rtcWriteRequest = true;
  s_lastSyncUtc = (uint32_t)tv->tv_sec;
}

bool takeSyncStamp(int64_t *mono, int64_t *sys) {
  for (int i = 0; i < 5; i++) {
    const uint32_t a = s_stamp.seq.load(std::memory_order_acquire);
    if (a & 1) continue;
    *mono = s_stamp.mono;
    *sys = s_stamp.sys;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (s_stamp.seq.load(std::memory_order_relaxed) == a) return a != 0;
  }
  return false;
}

// One more sync for the drift figure; publishes it, and keeps it in flash for the example file
// the clock writes to an SD card (once an hour at most, when it rests on at least six hours).
void noteSyncForDrift() {
  static uint32_t lastSaveMs = 0;
  int64_t mono, sys;
  if (!takeSyncStamp(&mono, &sys)) return;
  s_drift.addSync(mono, sys);
  const drift::Result r = s_drift.result();
  if (!r.valid) return;
  {
    StateLock lock;
    g_state.driftKnown = true;
    g_state.driftLive = true;
    g_state.driftPpm = r.ppm;
    g_state.driftErrPpm = r.errorPpm;
    g_state.driftHours = r.spanHours;
  }
  LOGF(TAG, "clock drift %+.2f ppm (+-%.2f) = %+.2f s/day over %.1f h", r.ppm, r.errorPpm, r.secPerDay, r.spanHours);
  const uint32_t now = millis();
  if (r.spanHours >= 6.0f && (lastSaveMs == 0 || now - lastSaveMs > 3600UL * 1000UL)) {
    lastSaveMs = now ? now : 1;
    waitForQuietPhase();
    s_prefs.putFloat("dppm", r.ppm);
    s_prefs.putFloat("dhours", r.spanHours);
  }
}

void applyTimezone(const char *posix, const char *iana, bool persist) {
  setenv("TZ", posix, 1);
  tzset();
  {
    StateLock lock;
    copyStr(g_state.tzPosix, sizeof g_state.tzPosix, posix);
    copyStr(g_state.tzName, sizeof g_state.tzName, iana);
  }
  if (persist) {
    waitForQuietPhase();
    s_prefs.putString("tz", posix);
    s_prefs.putString("tzn", iana);
  }
}

bool clockTrusted() {
  StateLock lock;
  return g_state.timeTrusted;
}

// ---------------------------------------------------------------------------
// HTTPS GET with certificate verification.  Returns the HTTP status (or <= 0).
// ---------------------------------------------------------------------------
int httpsGet(const String &url, String *body) {
  NetworkClientSecure client;
  useCaBundle(client);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  http.setUserAgent(APP_NAME "/" APP_VERSION);
  if (!http.begin(client, url)) return -1;
  int code = http.GET();
  if (code > 0 && httpStatusHasBody(code)) *body = http.getString();
  http.end();
  return code;
}

// ---------------------------------------------------------------------------
// Location
// ---------------------------------------------------------------------------
bool resolveLocation() {
  if (g_cfg.hasLatLon()) {
    StateLock lock;
    g_state.lat = g_cfg.latitude;
    g_state.lon = g_cfg.longitude;
    copyStr(g_state.place, sizeof g_state.place, g_cfg.locationLabel);
    g_state.locationKnown = true;
    return true;
  }

  const char *query = g_cfg.location;
  if (!query[0]) {
    stateSetStatus("Set a location (SD card or secrets.h)");
    LOGF(TAG, "no location configured (latitude/longitude or location)");
    return false;
  }

  // reuse the answer from an earlier boot for the same query
  if (s_prefs.getString("q", "") == query && s_prefs.isKey("lat") && s_prefs.isKey("lon")) {
    StateLock lock;
    g_state.lat = s_prefs.getDouble("lat", 0);
    g_state.lon = s_prefs.getDouble("lon", 0);
    copyStr(g_state.place, sizeof g_state.place, g_cfg.locationLabel[0] ? g_cfg.locationLabel : s_prefs.getString("place", "").c_str());
    g_state.locationKnown = true;
    return true;
  }

  char enc[3 * sizeof g_cfg.location + 1];  // every byte may become %XX
  if (!urlEncode(query, enc, sizeof enc)) {
    stateSetStatus("Location name too long");
    return false;
  }
  String url = String("https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&format=json&name=") + enc;
  String body;
  int code = httpsGet(url, &body);
  GeoResult g;
  if (code != 200 || !parseOpenMeteoGeocode(body.c_str(), body.length(), &g)) {
    LOGF(TAG, "geocoding \"%s\" failed (http %d)", query, code);
    stateSetStatus(code == 200 ? "Location not found" : "Location lookup failed");
    return false;
  }
  waitForQuietPhase();
  s_prefs.putString("q", query);
  s_prefs.putDouble("lat", g.lat);
  s_prefs.putDouble("lon", g.lon);
  s_prefs.putString("place", g.name);
  LOGF(TAG, "\"%s\" -> %s, %s (%.4f, %.4f)", query, g.name, g.admin1, g.lat, g.lon);

  StateLock lock;
  g_state.lat = g.lat;
  g_state.lon = g.lon;
  copyStr(g_state.place, sizeof g_state.place, g_cfg.locationLabel[0] ? g_cfg.locationLabel : g.name);
  g_state.locationKnown = true;
  return true;
}

// ---------------------------------------------------------------------------
// Weather + time zone
// ---------------------------------------------------------------------------
void updateTimezone(const ZoneInfo &zone) {
  if (s_tzFromOverride || !zone.iana[0]) return;
  const char *posix = tzPosixForIana(zone.iana);
  char fixed[40];
  if (!posix) {  // zone name newer than the table: fall back to the current offset
    calc::posixFromUtcOffset(zone.utcOffsetSec, fixed, sizeof fixed);
    posix = fixed;
    LOGF(TAG, "zone %s not in table, using fixed offset %s", zone.iana, fixed);
  }
  char current[sizeof g_state.tzPosix];
  char currentName[sizeof g_state.tzName];
  {
    StateLock lock;
    copyStr(current, sizeof current, g_state.tzPosix);
    copyStr(currentName, sizeof currentName, g_state.tzName);
  }
  if (strcmp(current, posix) != 0 || strcmp(currentName, zone.iana) != 0) {
    LOGF(TAG, "time zone: %s (%s)", zone.iana, posix);
    applyTimezone(posix, zone.iana, true);
  }
}

bool fetchWeather() {
  double lat, lon;
  {
    StateLock lock;
    lat = g_state.lat;
    lon = g_state.lon;
  }
  char url[460];
  snprintf(url, sizeof url,
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,wind_speed_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,uv_index_max"
           "&timezone=auto&forecast_days=3",
           lat, lon);
  String body;
  int code = httpsGet(url, &body);
  WeatherData w;
  ZoneInfo zone;
  if (code != 200 || !parseOpenMeteoForecast(body.c_str(), body.length(), &w, &zone)) {
    LOGF(TAG, "weather fetch failed (http %d, %u bytes)", code, (unsigned)body.length());
    return false;
  }
  updateTimezone(zone);
  {
    StateLock lock;
    g_state.weather = w;
    g_state.weatherFetchedMs = millis();
  }
  LOGF(TAG, "weather: %.1f C, code %d, today %.0f/%.0f", w.temp, w.code, w.day[0].tmax, w.day[0].tmin);
  return true;
}

// ---------------------------------------------------------------------------
// The task
// ---------------------------------------------------------------------------
void publishWifi(bool connected) {
  // The WiFi calls take the driver's own locks and can wait on it for a while (a scan, a
  // reconnect), so make them first: the UI takes StateLock every frame and must never end up
  // waiting behind them.
  int rssi = -100;
  char ssid[sizeof g_state.ssid] = "", ip[sizeof g_state.ip] = "";
  bool backup = false;
  if (connected) {
    rssi = WiFi.RSSI();
    copyStr(ssid, sizeof ssid, WiFi.SSID().c_str());
    copyStr(ip, sizeof ip, WiFi.localIP().toString().c_str());
    backup = g_cfg.hasBackupWifi() &&
             wifipick::classify(ssid, g_cfg.wifiSsid, g_cfg.wifiBackupSsid) == wifipick::NET_BACKUP;
  }
  StateLock lock;
  g_state.wifiUp = connected;
  g_state.wifiBackup = backup;
  g_state.rssi = rssi;
  if (connected) {
    copyStr(g_state.ssid, sizeof g_state.ssid, ssid);
    copyStr(g_state.ip, sizeof g_state.ip, ip);
  } else {
    g_state.ip[0] = 0;
  }
}

// Is the main network in range?  A scan takes a few seconds.  A hidden network does not show up in
// one: then the clock simply stays on the backup until that drops.
bool mainNetworkInRange() {
  const int n = WiFi.scanNetworks(false /*wait for it*/, false /*no hidden*/, false /*active*/, 120 /*ms per channel*/);
  bool seen = false;
  for (int i = 0; i < n && !seen; i++) seen = WiFi.RSSI(i) >= kReturnMinRssi && WiFi.SSID(i) == g_cfg.wifiSsid;
  WiFi.scanDelete();
  return seen;
}

// Drops a progress message ("Connecting to WiFi...") that the radio going off would leave behind; a failure
// message ("Weather update failed") stays until the next success.
void clearProgressStatus() {
  static const char *const progress[] = {"Connecting to WiFi...", "Syncing time...", "Finding location...", "Updating weather..."};
  StateLock lock;
  for (const char *p : progress) {
    if (!strcmp(g_state.status, p)) {
      g_state.status[0] = 0;
      return;
    }
  }
}

// What the Info page says about the radio (wifi_mode = sync).  `wakeInSec` is -1 when not known.
void publishRadio(bool sync, bool asleep, int32_t wakeInSec, uint32_t onMs, uint32_t sinceMs, uint32_t sessions) {
  StateLock lock;
  g_state.radioSync = sync;
  g_state.radioAsleep = asleep;
  g_state.radioWakeInSec = wakeInSec;
  g_state.radioOnPermille = sinceMs ? (uint16_t)((uint64_t)onMs * 1000 / sinceMs) : 0;
  g_state.radioSessions = sessions;
}

void netTask(void *) {
  const bool haveBackup = g_cfg.hasBackupWifi();
  const bool wifiConfigured = g_cfg.hasMainWifi() || haveBackup;
  const wifi_ps_type_t saveMode = g_cfg.wifiPowerSave == WIFISAVE_MAX ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM;
  const bool syncMode = g_cfg.syncMode();  // wifi_mode = sync: the radio is on only for a session (radio_plan.h)
  bool lowLatency = false;

  spotifyBegin();

  bool sntpStarted = false;
  bool locationOk = false;
  uint32_t lastBeginMs = 0, nextLocationMs = 0, nextWeatherMs = 0, lastRssiMs = 0, syncWaitStartMs = 0;
  bool wasConnected = false;
  int lastYday = -1;
  uint32_t seenSyncUtc = 0;
  wifipick::Picker pick;  // main network first, the backup when that cannot be joined (wifi_pick.h)
  pick.configure(g_cfg.hasMainWifi(), haveBackup);
  bool leavingBackup = false;  // we dropped the backup on purpose to go back to the main network

  // wifi_mode = sync.  The planner says when the radio is on; these are the timers it is told about.  (net_task
  // in always mode keeps none of this: SNTP runs by itself every hour and the radio never goes off.)
  radioplan::Planner plan(syncMode, haveBackup ? radioplan::kConnectTimeoutMs : 25000UL);
  bool radioOn = false;
  bool ntpEver = false;  // the network time was set at least once in this run
  uint32_t ntpAtMs = 0, ntpRetryAtMs = 0, lastRadioPublishMs = 0, bootMs = millis();
  int weatherFails = 0, locationFails = 0;

  auto startRadio = [&]() {
    s_radioNeedsFast = true;  // WiFi works from 80 MHz up: the UI loop raises the clock if it is lower
    for (int i = 0; i < 300 && getCpuFrequencyMhz() < 80; i++) vTaskDelay(pdMS_TO_TICKS(10));
    WiFi.persistent(false);
    WiFi.setHostname(g_cfg.hostname);  // before mode(): turning the station on is what copies the name to the network interface
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(saveMode);
    lowLatency = false;
    wasConnected = false;
    lastBeginMs = 0;
    radioOn = true;
    spotifyWindowBegin();
    if (syncMode) LOGF(TAG, "radio on (session %u)", (unsigned)plan.sessions() + 1);
  };
  auto stopRadio = [&]() {
    spotifyRadioDown();
    if (sntpStarted) {
      esp_sntp_stop();
      sntpStarted = false;
    }
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    pick.radioOff();
    publishWifi(false);
    clearProgressStatus();
    wasConnected = false;
    radioOn = false;
    s_radioNeedsFast = false;  // the UI loop may slow the clock down now
    LOGF(TAG, "radio off (on for %u s so far, %u sessions)", (unsigned)(plan.radioOnMs(millis()) / 1000), (unsigned)plan.sessions());
  };

  if (!syncMode && wifiConfigured) startRadio();  // always on, from the start (with no network name there is nothing to join)

  for (;;) {
    const uint32_t now = millis();

    if (g_powerDown) {  // on the way to deep sleep: no new WiFi calls while the UI task switches the radio off
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (!wifiConfigured) {
      s_radioNeedsFast = false;  // no radio, ever
      stateSetStatus("No WiFi name set (see README)");
      publishWifi(false);
      spotifyTick();  // answers key presses with "no network" instead of queueing them
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    {  // the forecast days shift at local midnight: refresh then instead of up to 15 min later
      time_t t = time(nullptr);
      struct tm lt;
      localtime_r(&t, &lt);
      if (lastYday >= 0 && lt.tm_yday != lastYday) s_weatherNow = true;
      lastYday = lt.tm_yday;
    }

    if (syncMode) {
      // What needs the radio?  (the timers are the ones of the always mode, plus the hourly network time)
      radioplan::Inputs in;
      in.online = radioOn && WiFi.status() == WL_CONNECTED;
      const bool ntpWake = (!ntpEver || radioplan::due(now, ntpAtMs + radioplan::kNtpGraceMs)) && radioplan::due(now, ntpRetryAtMs);
      const bool placeWake = !locationOk && radioplan::due(now, nextLocationMs);
      const bool weatherWake = locationOk && (s_weatherNow || radioplan::due(now, nextWeatherMs));
      in.workDue = ntpWake || placeWake || weatherWake || spotifyPeekPending();
      in.holdSpotify = spotifyWantsRadio();
      in.demand = radioplan::demandActive(now, s_demandUntilMs);
      const bool wantOn = plan.update(now, in);
      if (wantOn && !radioOn) startRadio();
      if (!wantOn && radioOn) stopRadio();
      if (plan.takeGaveUp()) {
        LOGF(TAG, "could not join WiFi: next try in %u s", (unsigned)((plan.retryAtMs() - now) / 1000));
        spotifyFlushQueue("Spotify: no network");
        stateSetStatus("No WiFi, trying again later");
      }
      if (now - lastRadioPublishMs >= 1000) {
        lastRadioPublishMs = now ? now : 1;
        int32_t wake = -1;
        if (!radioOn) {  // the earliest of what is due, and not before the back-off after a failed session ends
          uint32_t earliest = now + 3600000UL;
          bool any = false;
          auto consider = [&](uint32_t at) {
            if (!any || (int32_t)(at - earliest) < 0) earliest = at;
            any = true;
          };
          consider(ntpEver ? ntpAtMs + radioplan::kNtpGraceMs : now);
          if (locationOk) consider(s_weatherNow ? now : nextWeatherMs);
          else consider(nextLocationMs);
          uint32_t at = earliest;
          if ((int32_t)(plan.retryAtMs() - at) > 0 && plan.retryAtMs() != 0) at = plan.retryAtMs();
          wake = (int32_t)(at - now) > 0 ? (int32_t)((at - now) / 1000) : 0;
        }
        publishRadio(true, !radioOn, wake, plan.radioOnMs(now), now - bootMs, plan.sessions());
      }
      if (!radioOn) {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
    }

    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != wasConnected || (connected && now - lastRssiMs > 3000)) {
      publishWifi(connected);
      lastRssiMs = now;
      if (connected != wasConnected) {
        if (connected) {
          LOGF(TAG, "WiFi connected: %s, %s, %d dBm", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
               WiFi.RSSI());
          pick.joined(wifipick::classify(WiFi.SSID().c_str(), g_cfg.wifiSsid, g_cfg.wifiBackupSsid), now);
        } else {
          LOGF(TAG, "WiFi disconnected");
          pick.lost();
          // a drop the stack did not cause gets its own auto-reconnect a 20 s head start; going back to the main
          // network on purpose starts the attempt at once
          lastBeginMs = leavingBackup ? 0 : (now ? now : 1);
          leavingBackup = false;
        }
      }
      wasConnected = connected;
    }

    if (!connected) {
      stateSetStatus("Connecting to WiFi...");
      if (lastBeginMs == 0 || now - lastBeginMs > 20000) {  // (re)start the association
        const bool backup = pick.nextAttempt() == wifipick::NET_BACKUP;
        const char *ssid = backup ? g_cfg.wifiBackupSsid : g_cfg.wifiSsid;
        const char *pass = backup ? g_cfg.wifiBackupPassword : g_cfg.wifiPassword;
        LOGF(TAG, "WiFi: trying %s network \"%s\"", backup ? "the backup" : "the main", ssid);
        WiFi.disconnect();
        WiFi.begin(ssid, pass[0] ? pass : nullptr);  // no password: an open network
        lastBeginMs = now ? now : 1;
      }
      if (!syncMode) spotifyTick();  // answers key presses with "no network" instead of queueing them (in sync mode they wait for the radio)
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    {  // associated again: drop the "Connecting" message
      StateLock lock;
      if (!strcmp(g_state.status, "Connecting to WiFi...")) g_state.status[0] = 0;
    }

    // The radio sleeps between beacons to save power; while the Spotify linking page is being
    // served it stays awake so the page answers at once.
    const bool wantLowLatency = spotifyLowLatencyWanted();
    if (wantLowLatency != lowLatency) {
      lowLatency = wantLowLatency;
      WiFi.setSleep(lowLatency ? WIFI_PS_MIN_MODEM : saveMode);
    }

    // On the backup: now and then look for the main network, and go back to it when it is there (not while
    // the Spotify setup page is being served: a scan would stall it).
    if (!lowLatency && pick.timeToLookForMain(now)) {
      const bool seen = mainNetworkInRange();
      pick.lookedForMain(now, seen);
      if (seen) {
        LOGF(TAG, "the main WiFi is back in range: leaving the backup");
        leavingBackup = true;
        WiFi.disconnect();
        vTaskDelay(pdMS_TO_TICKS(250));
        continue;  // the next pass sees the drop and starts on the main network at once
      }
    }

    // The network time.  Always mode: SNTP runs from the first connection on and syncs by itself every hour.
    // Sync mode: it is started in a session that is near the hour (or has none yet), and stopped again once
    // it has answered, or after twenty seconds without an answer (tried again in five minutes).
    if (!sntpStarted && (!syncMode || ((!ntpEver || radioplan::due(now, ntpAtMs - radioplan::kNtpEarlyMs)) && radioplan::due(now, ntpRetryAtMs)))) {
      char tz[sizeof g_state.tzPosix];
      {
        StateLock lock;
        copyStr(tz, sizeof tz, g_state.tzPosix);
      }
      sntp_set_time_sync_notification_cb(onTimeSync);
      if (g_cfg.ntpServer[0]) {  // the chosen server first, two of the usual ones behind it
        configTzTime(tz[0] ? tz : "UTC0", g_cfg.ntpServer, NTP_SERVER_1, NTP_SERVER_2);
      } else {
        configTzTime(tz[0] ? tz : "UTC0", NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
      }
      sntp_set_sync_interval(3600UL * 1000UL);  // the SDK default is longer; an hour keeps the RTC tight
      sntpStarted = true;
      syncWaitStartMs = now;
    }

    const uint32_t syncUtc = s_lastSyncUtc;
    if (syncUtc != seenSyncUtc) {  // a (new) NTP sync completed
      seenSyncUtc = syncUtc;
      {
        StateLock lock;
        g_state.ntpSynced = true;
        g_state.timeTrusted = true;
        g_state.lastNtpSyncUtc = (time_t)syncUtc;
      }
      LOGF(TAG, "NTP synced");  // never log while holding the lock: a stalled serial port would freeze the UI
      noteSyncForDrift();
      ntpEver = true;
      ntpAtMs = now + 3600UL * 1000UL;
      if (syncMode && sntpStarted) {  // done for this session
        esp_sntp_stop();
        sntpStarted = false;
      }
    } else if (syncMode && sntpStarted && now - syncWaitStartMs > 20000) {
      LOGF(TAG, "NTP: no reply in 20 s, trying again in 5 minutes");
      esp_sntp_stop();
      sntpStarted = false;
      ntpRetryAtMs = now + 300000UL;
    }

    // TLS needs a believable clock: wait for NTP (or a valid RTC reading).
    if (!clockTrusted()) {
      stateSetStatus(now - syncWaitStartMs > 20000 ? "No NTP reply (UDP 123 blocked?)" : "Syncing time...");
      spotifyTick();
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    if (!locationOk && (int32_t)(now - nextLocationMs) >= 0) {
      stateSetStatus("Finding location...");
      locationOk = resolveLocation();
      if (!locationOk) nextLocationMs = now + (syncMode ? radioplan::retryAfterMs(++locationFails) : 30000UL);
    }

    if (locationOk && (s_weatherNow || (int32_t)(now - nextWeatherMs) >= 0)) {
      stateSetStatus("Updating weather...");
      s_weatherNow = false;
      bool ok = fetchWeather();
      if (ok) weatherFails = 0;
      nextWeatherMs = millis() + (ok ? (uint32_t)g_cfg.weatherIntervalMin * 60UL * 1000UL
                                     : (syncMode ? radioplan::retryAfterMs(++weatherFails) : WEATHER_RETRY_MS));
      stateSetStatus(ok ? "" : "Weather update failed");
    } else if (locationOk) {
      // keep a stale failure message only until the next success
      StateLock lock;
      if (!strcmp(g_state.status, "Syncing time...") || !strcmp(g_state.status, "Finding location...")) g_state.status[0] = 0;
    }

    spotifyTick();
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void netLoadSettings() {
  s_prefs.begin("clock", false);

  // what the status bar can say before the network is up: the label from the settings, or the
  // place found by an earlier lookup of the same query
  {
    char place[sizeof g_state.place] = "";
    if (g_cfg.locationLabel[0]) {
      copyStr(place, sizeof place, g_cfg.locationLabel);
    } else if (!g_cfg.hasLatLon() && g_cfg.location[0] && s_prefs.getString("q", "") == g_cfg.location) {
      copyStr(place, sizeof place, s_prefs.getString("place", "").c_str());
    }
    // the drift measured on an earlier run, for the Info page
    const float ppm = s_prefs.getFloat("dppm", NAN);
    StateLock lock;
    copyStr(g_state.place, sizeof g_state.place, place);
    if (!isnan(ppm)) {
      g_state.driftKnown = true;
      g_state.driftPpm = ppm;
      g_state.driftHours = s_prefs.getFloat("dhours", 0);
    }
  }

  if (g_cfg.timezone[0]) {  // an IANA name or a POSIX rule from the settings
    s_tzFromOverride = true;
    char canon[48];
    const char *posix = tzFindIanaIgnoreCase(g_cfg.timezone, canon, sizeof canon);
    applyTimezone(posix ? posix : g_cfg.timezone, posix ? canon : "(set in settings)", false);
    return;
  }
  String tz = s_prefs.getString("tz", "");
  if (tz.length()) {
    applyTimezone(tz.c_str(), s_prefs.getString("tzn", "").c_str(), false);
    LOGF(TAG, "saved time zone: %s", tz.c_str());
  } else {
    applyTimezone("UTC0", "UTC", false);
  }
}

void netBegin() {
  if (!g_cfg.wifi) {  // WiFi off in the settings: no radio, no network time, weather or Spotify
    stateSetStatus("WiFi off");
    LOGF(TAG, "WiFi is switched off in the settings");
    return;
  }
  s_radioNeedsFast = true;  // until the network task knows better (the first session, or no radio at all)
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);
}

bool netTakeRtcWriteRequest() {
  bool p = s_rtcWriteRequest;
  s_rtcWriteRequest = false;
  return p;
}

void netRequestWeatherRefresh() { s_weatherNow = true; }

void netWake() { s_demandUntilMs = radioplan::demandUntil(millis()) | 1u; }

bool netRadioNeedsFastClock() { return s_radioNeedsFast; }
