// Host-side tests for the settings layer and the other pure-logic modules added in 1.3
// (moon phase, date formats, battery estimate, low-battery guard, clock drift).
// Built and run by run_tests.sh next to test_logic.cpp.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "battery_est.h"
#include "calc.h"
#include "check.h"
#include "datefmt.h"
#include "drift.h"
#include "fw_logic.h"
#include "low_battery.h"
#include "moon.h"
#include "settings.h"
#include "tz_table.h"
#include "wifi_pick.h"

// ===========================================================================
// Settings
// ===========================================================================
namespace {

struct Applied {
  Settings s;
  ConfigReport r;
};

// Applies `text` to `base`, with a pristine Settings as the defaults.
Applied apply(const std::string &text, const Settings &base = Settings()) {
  Applied a;
  a.s = base;
  const Settings defaults;
  a.r = applyConfigText(a.s, defaults, text.data(), text.size());
  return a;
}

std::string utf16(const std::string &ascii, bool bigEndian) {
  std::string out = bigEndian ? std::string("\xFE\xFF", 2) : std::string("\xFF\xFE", 2);
  for (char c : ascii) {
    if (bigEndian) {
      out += '\0';
      out += c;
    } else {
      out += c;
      out += '\0';
    }
  }
  return out;
}

std::string fmt(const Settings &s, size_t i) {
  char buf[128];
  return formatSetting(s, i, buf, sizeof buf) ? buf : "<overflow>";
}

// The table position of a setting, by the name it has in the file (the tests must not depend on the order).
size_t idx(const char *key) {
  for (size_t i = 0; i < settingCount(); i++)
    if (!strcmp(settingKey(i), key)) return i;
  return (size_t)-1;
}

// Every field of every setting as text, to compare two Settings.
std::string dump(const Settings &s) {
  std::string out;
  for (size_t i = 0; i < settingCount(); i++) {
    out += settingKey(i);
    out += '=';
    out += fmt(s, i);
    out += '\n';
  }
  return out;
}

// A file that sets every setting to something other than its default.
const char *kAllSettings =
    "wifi = off\n"
    "wifi_ssid = Cafe Net\n"
    "wifi_password = p@ss w0rd\n"
    "wifi_backup_ssid = Phone Hotspot\n"
    "wifi_backup_password = h0tsp0t pw\n"
    "hostname = my-clock\n"
    "ntp_server = time.nist.gov\n"
    "wifi_power_save = max\n"
    "units = imperial\n"
    "time_format = 12h\n"
    "date_format = mdy\n"
    "show_week = off\n"
    "latitude = 40.7128 N\n"
    "longitude = 74.0060 W\n"
    "location_label = New York\n"
    "location = New York\n"
    "timezone = America/New_York\n"
    "spotify = off\n"
    "spotify_client_id = 0123456789abcdef0123456789abcdef\n"
    "indoor_offset = -2.5\n"
    "battery = none\n"
    "battery_capacity_mah = 2500\n"
    "battery_calibration = 1.0157\n"
    "low_battery_shutdown = off\n"
    "battery_cutoff_v = 3.45\n"
    "cpu_mhz = 160\n"
    "weather_interval_min = 30\n";

void testSettingsTable() {
  section("settings table");
  const size_t n = settingCount();
  CHECK(n == 27);
  CHECK(n <= 32);  // userSet is a 32 bit mask
  CHECK_STR(settingKey(n), "");
  CHECK_STR(settingNvsKey(n), "");
  for (size_t i = 0; i < n; i++) {
    CHECK(strlen(settingKey(i)) > 0);
    CHECK(strlen(settingNvsKey(i)) > 0 && strlen(settingNvsKey(i)) <= 15);  // NVS key limit
    for (size_t j = i + 1; j < n; j++) {
      CHECK(strcmp(settingKey(i), settingKey(j)) != 0);
      CHECK(strcmp(settingNvsKey(i), settingNvsKey(j)) != 0);
    }
  }
  int secrets = 0;
  for (size_t i = 0; i < n; i++) secrets += settingIsSecret(i);
  CHECK(secrets == 3);
  CHECK(settingIsSecret(2) && !strcmp(settingKey(2), "wifi_password"));
  CHECK(!settingIsSecret(3) && !strcmp(settingKey(3), "wifi_backup_ssid"));
  CHECK(settingIsSecret(4) && !strcmp(settingKey(4), "wifi_backup_password"));
  CHECK(settingIsSecret(idx("spotify_client_id")));

  // defaults a clean build ships with
  const Settings d;
  CHECK(d.wifi && d.units == UNITS_METRIC && d.timeFormat == TIME_24H && d.dateFormat == DATE_ISO);
  CHECK(d.cpuMhz() == 80 && d.lowBatteryShutdown && d.hasBattery() && !d.hasLatLon());
  CHECK_NEAR(d.batteryCutoffV, 3.30, 1e-6);
  CHECK(d.userSet == 0);
}

void testSettingsRoundTrip() {
  section("settings flash round trip");
  const Settings d;
  for (size_t i = 0; i < settingCount(); i++) {
    Settings t;
    CHECK(applyStoredSetting(t, i, fmt(d, i).c_str()));
    CHECK(settingIsUserSet(t, i));
    CHECK(fmt(t, i) == fmt(d, i));
  }

  const Applied all = apply(kAllSettings);
  CHECK(all.r.problems() == 0);
  CHECK(all.r.changed == (int)settingCount());
  CHECK(all.s.userSet == (uint32_t)((1ull << settingCount()) - 1));
  Settings t;
  for (size_t i = 0; i < settingCount(); i++) CHECK(applyStoredSetting(t, i, fmt(all.s, i).c_str()));
  CHECK(dump(t) == dump(all.s));
  CHECK(t.userSet == all.s.userSet);

  // stored text that no longer validates is ignored, and leaves the setting alone
  Settings u;
  CHECK(!applyStoredSetting(u, 0, "maybe"));
  CHECK(!applyStoredSetting(u, idx("latitude"), "500"));  // out of range
  CHECK(!applyStoredSetting(u, idx("units"), "kelvin"));
  CHECK(!applyStoredSetting(u, idx("units"), ""));        // an empty number or enum is no value
  CHECK(!applyStoredSetting(u, 999, "x"));
  CHECK(!applyStoredSetting(u, 0, nullptr));
  CHECK(u.userSet == 0 && u.wifi && u.units == UNITS_METRIC);
  // but an empty text setting is a real choice (an open network, for one)
  Settings v;
  snprintf(v.wifiPassword, sizeof v.wifiPassword, "old");
  CHECK(applyStoredSetting(v, 2, ""));
  CHECK_STR(v.wifiPassword, "");
  CHECK(settingIsUserSet(v, 2));
  // precision survives: coordinates keep 5 decimals, about a metre
  Settings w;
  w.latitude = -31.952240;
  w.longitude = 115.861456;
  Settings x;
  CHECK(applyStoredSetting(x, idx("latitude"), fmt(w, idx("latitude")).c_str()));
  CHECK(applyStoredSetting(x, idx("longitude"), fmt(w, idx("longitude")).c_str()));
  CHECK_NEAR(x.latitude, -31.952240, 1e-6);
  CHECK_NEAR(x.longitude, 115.861456, 1e-5);
}

void testSettingsSyntax() {
  section("settings file: what people type");
  // all of these say the same thing
  const char *variants[] = {
      "wifi_ssid = My Network",
      "wifi_ssid=My Network",
      "wifi_ssid   =    My Network   ",
      "\twifi_ssid\t=\tMy Network\t",
      "wifi_ssid: My Network",
      "wifi_ssid = \"My Network\"",
      "wifi_ssid = 'My Network'",
      "wifi_ssid=\"My Network\"",
      "WIFI_SSID = My Network",
      "WiFi SSID = My Network",
      "Wi-Fi-SSID = \"My Network\"",
      "wifi-ssid = My Network",
      "wifissid = My Network",
      "ssid = My Network",
      "SSID: \"My Network\"",
      "network = My Network",
      "wifi_ssid = \xE2\x80\x9CMy Network\xE2\x80\x9D",  // curly double quotes
      "wifi_ssid = \xE2\x80\x98My Network\xE2\x80\x99",  // curly single quotes
      "wifi_ssid = \xC2\xABMy Network\xC2\xBB",          // guillemets
      "wifi_ssid = \"My Network\" # my home",
      "wifi_ssid = My Network # my home",
      "wifi_ssid = My Network   ;",  // no: ';' is no comment for bare values; checked below
  };
  for (size_t i = 0; i + 1 < sizeof variants / sizeof variants[0]; i++) {
    const Applied a = apply(variants[i]);
    if (strcmp(a.s.wifiSsid, "My Network") != 0) printf("  variant %zu: [%s] gave [%s]\n", i, variants[i], a.s.wifiSsid);
    CHECK_STR(a.s.wifiSsid, "My Network");
    CHECK(a.r.problems() == 0 && a.r.changed == 1);
    CHECK(settingIsUserSet(a.s, 1));
  }
  CHECK_STR(apply(variants[sizeof variants / sizeof variants[0] - 1]).s.wifiSsid, "My Network   ;");

  // quotes and the characters inside them
  struct Case {
    const char *line;
    const char *ssid;
  } cases[] = {
      {"wifi_ssid = \"Bob's WiFi\"", "Bob's WiFi"},
      {"wifi_ssid = Bob's WiFi", "Bob's WiFi"},
      {"wifi_ssid = 'Bob\"s WiFi'", "Bob\"s WiFi"},
      {"wifi_ssid = \"Home\" # it's the \"good\" one", "Home"},
      {"wifi_ssid = \"Home\" ; the good one", "Home"},
      {"wifi_ssid = \"My \"Network\"\"", "My \"Network\""},
      {"wifi_ssid = \"Home#1\"", "Home#1"},
      {"wifi_ssid = Home#1", "Home#1"},
      {"wifi_ssid = Home #1", "Home"},
      {"wifi_ssid = \"  padded  \"", "  padded  "},
      {"wifi_ssid = \"unterminated", "unterminated"},
      {"wifi_ssid = 'tis", "'tis"},
      {"wifi_ssid = \"Caf\xC3\xA9 Net\"", "Caf\xC3\xA9 Net"},
      {"wifi_ssid = a=b:c", "a=b:c"},
      {"wifi_ssid : a=b:c", "a=b:c"},
      {"wifi_ssid = 1234 # 5678", "1234"},
      {"wifi_ssid = \"x\"y\"", "x\"y"},
  };
  for (const Case &c : cases) {
    const Applied a = apply(c.line);
    if (strcmp(a.s.wifiSsid, c.ssid) != 0) printf("  [%s] gave [%s], wanted [%s]\n", c.line, a.s.wifiSsid, c.ssid);
    CHECK_STR(a.s.wifiSsid, c.ssid);
    CHECK(a.r.problems() == 0);
  }

  // the password: bare, spaces inside, empty on purpose, forgotten
  CHECK_STR(apply("wifi_password = p@ss w0rd!").s.wifiPassword, "p@ss w0rd!");
  CHECK_STR(apply("password = \"p@ss w0rd!\"").s.wifiPassword, "p@ss w0rd!");
  CHECK_STR(apply("wifi_password = Pa$$ #1 word").s.wifiPassword, "Pa$$");  // a bare ' #' starts a comment
  CHECK_STR(apply("wifi_password = \"Pa$$ #1 word\"").s.wifiPassword, "Pa$$ #1 word");
  Settings withPassword;
  snprintf(withPassword.wifiPassword, sizeof withPassword.wifiPassword, "old");
  withPassword.userSet |= 1u << 2;
  Applied open = apply("wifi_password = \"\"", withPassword);
  CHECK_STR(open.s.wifiPassword, "");
  CHECK(settingIsUserSet(open.s, 2) && open.r.changed == 1);
  Applied open2 = apply("wifi_password = ''", withPassword);
  CHECK_STR(open2.s.wifiPassword, "");
  Applied forgot = apply("wifi_password =", withPassword);
  CHECK_STR(forgot.s.wifiPassword, "");  // the default is no password
  CHECK(!settingIsUserSet(forgot.s, 2) && forgot.r.cleared == 1 && forgot.r.changed == 0);

  // line ends and encodings
  const std::string two = "wifi_ssid = Net\nwifi_password = pw\n";
  const char *ends[] = {"\n", "\r\n", "\r"};
  for (const char *e : ends) {
    std::string t = std::string("wifi_ssid = Net") + e + "wifi_password = pw" + e;
    Applied a = apply(t);
    CHECK_STR(a.s.wifiSsid, "Net");
    CHECK_STR(a.s.wifiPassword, "pw");
    CHECK(a.r.lines == 2 && a.r.changed == 2);
  }
  Applied noEnd = apply("wifi_ssid = Net\nwifi_password = pw");
  CHECK_STR(noEnd.s.wifiPassword, "pw");
  CHECK(noEnd.r.lines == 2);
  Applied bom = apply(std::string("\xEF\xBB\xBF") + two);
  CHECK_STR(bom.s.wifiSsid, "Net");
  CHECK(bom.r.problems() == 0);
  Applied le = apply(utf16(two, false));
  CHECK_STR(le.s.wifiSsid, "Net");
  CHECK_STR(le.s.wifiPassword, "pw");
  CHECK(le.r.problems() == 0);
  Applied be = apply(utf16(two, true));
  CHECK_STR(be.s.wifiSsid, "Net");
  CHECK_STR(be.s.wifiPassword, "pw");
  Applied le16crlf = apply(utf16("wifi_ssid = Net\r\nunits = imperial\r\n", false));
  CHECK_STR(le16crlf.s.wifiSsid, "Net");
  CHECK(le16crlf.s.imperial());
  CHECK(apply("").r.lines == 0 && apply("").r.touched() == 0);
  CHECK(apply("\n\n\n").r.lines == 3 && apply("\n\n\n").r.problems() == 0);
  CHECK(apply("# only a comment\n; and another\n// and a third\n[wifi]\n").r.problems() == 0);

  // comments and section headings are skipped
  Applied commented = apply("# wifi_ssid = Nope\n; units = imperial\n// time_format = 12h\n[Units]\nunits = imperial\n");
  CHECK_STR(commented.s.wifiSsid, "");
  CHECK(commented.s.imperial() && commented.s.time12h() == false);
  CHECK(commented.r.changed == 1 && commented.r.problems() == 0);
  // indented comments, headings and blank lines too
  const Applied indented = apply("   # wifi_ssid = Nope\n\t; units = imperial\n  [Units]  \n   \n\t\n");
  CHECK(indented.r.problems() == 0 && indented.r.touched() == 0 && indented.r.lines == 5);
  CHECK_STR(indented.s.wifiSsid, "");
}

void testSettingsValues() {
  section("settings file: values");
  // booleans
  const char *yes[] = {"on", "ON", "Yes", "true", "1", "enabled", "y", "\"on\""};
  const char *no[] = {"off", "OFF", "No", "false", "0", "disabled", "n", "'off'"};
  for (const char *v : yes) CHECK(apply(std::string("show_week = ") + v, [] { Settings s; s.showWeek = false; return s; }()).s.showWeek);
  for (const char *v : no) CHECK(!apply(std::string("wifi = ") + v).s.wifi);

  // enums and their spellings
  CHECK(apply("units = imperial").s.imperial());
  CHECK(apply("units = Fahrenheit").s.imperial());
  CHECK(apply("units = F").s.imperial());
  CHECK(apply("temperature units = US").s.imperial());
  CHECK(!apply("units = metric", apply("units = imperial").s).s.imperial());
  CHECK(!apply("units = celsius", apply("units = imperial").s).s.imperial());
  CHECK(apply("time_format = 12h").s.time12h());
  CHECK(apply("time format = 12").s.time12h());
  CHECK(apply("hour format = 12 hour").s.time12h());
  CHECK(apply("time_format = AM/PM").s.time12h());
  CHECK(!apply("time_format = 24h", apply("time_format = 12h").s).s.time12h());
  struct { const char *text; int fmt; } dates[] = {
      {"iso", DATE_ISO}, {"YYYY-MM-DD", DATE_ISO}, {"dmy", DATE_DMY}, {"DD/MM/YYYY", DATE_DMY},
      {"mdy", DATE_MDY}, {"mm/dd/yyyy", DATE_MDY}, {"dmy-dot", DATE_DMY_DOT}, {"dd.mm.yyyy", DATE_DMY_DOT},
      {"d-mon-y", DATE_D_MON_Y}, {"D Mon YYYY", DATE_D_MON_Y}, {"mon-d-y", DATE_MON_D_Y}, {"Mon D, YYYY", DATE_MON_D_Y},
      {"\"dmy\"", DATE_DMY}};
  for (const auto &d : dates) CHECK(apply(std::string("date_format = ") + d.text).s.dateFormat == d.fmt);
  CHECK(apply("battery = none").s.battery == BATTERY_NONE && !apply("battery = none").s.hasBattery());
  CHECK(apply("battery = no").s.battery == BATTERY_NONE);
  CHECK(apply("battery = USB").s.battery == BATTERY_NONE);
  CHECK(apply("battery = auto", apply("battery = none").s).s.battery == BATTERY_AUTO);
  CHECK(apply("cpu_mhz = 240").s.cpuMhz() == 240);
  CHECK(apply("cpu_mhz = 160 MHz").s.cpuMhz() == 160);
  CHECK(apply("CPU = 80").s.cpuMhz() == 80);
  CHECK(apply("wifi_power_save = normal").s.wifiPowerSave == WIFISAVE_NORMAL);
  CHECK(apply("wifi_power_save = max", apply("wifi_power_save = normal").s).s.wifiPowerSave == WIFISAVE_MAX);

  // numbers
  CHECK_NEAR(apply("indoor_offset = -3.5").s.indoorOffsetC, -3.5, 1e-6);
  CHECK_NEAR(apply("indoor offset = -3,5 C").s.indoorOffsetC, -3.5, 1e-6);
  CHECK_NEAR(apply("indoor_offset = 0").s.indoorOffsetC, 0, 1e-6);
  CHECK_NEAR(apply("indoor_offset = \"-2\"").s.indoorOffsetC, -2, 1e-6);
  CHECK_NEAR(apply("indoor_offset = -4.0 \xC2\xB0" "C").s.indoorOffsetC, -4, 1e-6);
  CHECK_NEAR(apply("battery_cutoff_v = 3.4 V").s.batteryCutoffV, 3.4, 1e-6);
  CHECK_NEAR(apply("battery_cutoff_v = 3,45").s.batteryCutoffV, 3.45, 1e-6);
  CHECK(apply("weather_interval_min = 30 min").s.weatherIntervalMin == 30);
  CHECK(apply("weather_interval_min = 30.4").s.weatherIntervalMin == 30);
  CHECK(apply("battery_capacity_mah = 2500 mAh").s.batteryCapacityMah == 2500);
  CHECK(apply("battery_capacity_mah = 0").s.batteryCapacityMah == 0);

  // coordinates
  CHECK_NEAR(apply("latitude = -31.952").s.latitude, -31.952, 1e-9);
  CHECK_NEAR(apply("latitude = 31.952 S").s.latitude, -31.952, 1e-9);
  CHECK_NEAR(apply("latitude = -31.952 S").s.latitude, -31.952, 1e-9);
  CHECK_NEAR(apply("latitude = 31.952s").s.latitude, -31.952, 1e-9);
  CHECK_NEAR(apply("latitude = 31.952 N").s.latitude, 31.952, 1e-9);
  CHECK_NEAR(apply("latitude = 31.952\xC2\xB0 S").s.latitude, -31.952, 1e-9);
  CHECK_NEAR(apply("lat = -31,952").s.latitude, -31.952, 1e-9);
  CHECK_NEAR(apply("longitude = 115.861 E").s.longitude, 115.861, 1e-9);
  CHECK_NEAR(apply("longitude = 115.861").s.longitude, 115.861, 1e-9);
  CHECK_NEAR(apply("lon = 74.006 W").s.longitude, -74.006, 1e-9);
  CHECK_NEAR(apply("Long = 74.006 w").s.longitude, -74.006, 1e-9);
  CHECK_NEAR(apply("lng = -180").s.longitude, -180, 1e-9);
  CHECK(apply("latitude = -31.952\nlongitude = 115.861").s.hasLatLon());

  // strings
  CHECK_STR(apply("hostname = my-clock").s.hostname, "my-clock");
  CHECK_STR(apply("ntp_server = time.google.com").s.ntpServer, "time.google.com");
  CHECK_STR(apply("location = \"Paris, France\"").s.location, "Paris, France");
  CHECK_STR(apply("location = Paris, France").s.location, "Paris, France");
  CHECK_STR(apply("city = Perth").s.location, "Perth");
  CHECK_STR(apply("location_label = Home sweet home").s.locationLabel, "Home sweet home");
  CHECK_STR(apply("label = \"\"").s.locationLabel, "");
  CHECK_STR(apply("spotify_client_id = 0123456789abcdef0123456789abcdef").s.spotifyClientId,
            "0123456789abcdef0123456789abcdef");
  CHECK_STR(apply("client id = \"0123456789abcdef0123456789abcdef\"").s.spotifyClientId,
            "0123456789abcdef0123456789abcdef");
}

void testSettingsRejects() {
  section("settings file: bad values change nothing");
  struct { const char *line; } bad[] = {
      {"wifi = maybe"},          {"wifi = 2"},                 {"show_week = sometimes"},
      {"units = kelvin"},        {"units = 5"},                {"time_format = 36h"},
      {"date_format = yyyy"},    {"battery = lots"},           {"cpu_mhz = 100"},
      {"cpu_mhz = fast"},        {"wifi_power_save = loud"},   {"latitude = 95"},
      {"latitude = -91"},        {"latitude = north"},         {"latitude = 33 E"},
      {"longitude = 181"},       {"longitude = 100 N"},        {"longitude = abc"},
      {"latitude = nan"},        {"latitude = inf"},           {"latitude = 3.3.0"},
      {"indoor_offset = 20"},    {"indoor_offset = -16"},      {"indoor_offset = warm"},
      {"battery_cutoff_v = 2.5"}, {"battery_cutoff_v = 3.7"},  {"battery_cutoff_v = 33"},
      {"battery_capacity_mah = -5"}, {"battery_capacity_mah = 99999"},
      {"weather_interval_min = 1"}, {"weather_interval_min = 1000"}, {"weather_interval_min = soon"},
      {"hostname = my clock"},   {"hostname = -clock"},        {"hostname = clock-"},
      {"hostname = cl_ock"},     {"hostname = clock.local"},   {"ntp_server = http://x"},
      {"ntp_server = bad host"}, {"timezone = Perth"},         {"timezone = Mars/Olympus"},
      {"timezone = 12345"},      {"spotify_client_id = short"}, {"spotify_client_id = 0123456789abcdef0123456789abcde!"},
      {"spotify_client_id = 0123456789abcdef 0123456789abcdef"},
  };
  for (const auto &b : bad) {
    const Applied a = apply(b.line);
    if (!(a.r.bad == 1 && a.r.changed == 0)) printf("  accepted [%s]\n", b.line);
    CHECK(a.r.bad == 1 && a.r.unknown == 0 && a.r.changed == 0 && a.r.unchanged == 0);
    CHECK(a.s.userSet == 0);
    CHECK(dump(a.s) == dump(Settings()));
    CHECK(a.r.issueCount == 1 && a.r.issues[0].line == 1);
  }
  // too long
  const std::string longSsid(33, 'x');
  CHECK(apply("wifi_ssid = " + longSsid).r.bad == 1);
  CHECK(apply("wifi_ssid = " + std::string(32, 'x')).r.bad == 0);
  CHECK(apply("wifi_password = " + std::string(65, 'x')).r.bad == 1);
  CHECK(apply("wifi_password = " + std::string(64, 'x')).r.bad == 0);
  CHECK(apply("location_label = " + std::string(40, 'x')).r.bad == 1);
  CHECK(apply("wifi_ssid = tab\there").r.bad == 1);  // control character
  // a rejected value leaves what was there
  Settings prev = apply("wifi_ssid = Keep\nunits = imperial\nlatitude = -31.952").s;
  Applied rej = apply("wifi_ssid = " + longSsid + "\nunits = kelvin\nlatitude = 500", prev);
  CHECK(rej.r.bad == 3 && rej.r.changed == 0);
  CHECK(dump(rej.s) == dump(prev) && rej.s.userSet == prev.userSet);
}

void testSettingsTimezone() {
  section("settings file: time zone");
  CHECK_STR(apply("timezone = Australia/Perth").s.timezone, "Australia/Perth");
  CHECK_STR(apply("timezone = australia/perth").s.timezone, "Australia/Perth");
  CHECK_STR(apply("timezone = AUSTRALIA/PERTH").s.timezone, "Australia/Perth");
  CHECK_STR(apply("timezone = \"America/New York\"").s.timezone, "America/New_York");
  CHECK_STR(apply("tz = Europe/London").s.timezone, "Europe/London");
  CHECK_STR(apply("time zone = UTC").s.timezone, "UTC");
  CHECK_STR(apply("timezone = AWST-8").s.timezone, "AWST-8");
  CHECK_STR(apply("timezone = <+0530>-5:30").s.timezone, "<+0530>-5:30");
  CHECK_STR(apply("timezone = CET-1CEST,M3.5.0,M10.5.0/3").s.timezone, "CET-1CEST,M3.5.0,M10.5.0/3");
  CHECK(apply("timezone = Australia/Perth").r.changed == 1);
  CHECK_STR(apply("timezone = \"\"", apply("timezone = AWST-8").s).s.timezone, "");  // empty on purpose = follow location
  CHECK(settingIsUserSet(apply("timezone = \"\"").s, idx("timezone")));
  CHECK_STR(apply("timezone =", apply("timezone = AWST-8").s).s.timezone, "");  // forgotten = the default
  CHECK(!settingIsUserSet(apply("timezone =", apply("timezone = AWST-8").s).s, idx("timezone")));
  // what the firmware does with it afterwards
  const Applied perth = apply("timezone = australia/perth");
  CHECK_STR(tzPosixForIana(perth.s.timezone), "AWST-8");
  char canon[48];
  CHECK_STR(tzFindIanaIgnoreCase("america/new york", canon, sizeof canon), "EST5EDT,M3.2.0,M11.1.0");
  CHECK_STR(canon, "America/New_York");
  CHECK(tzFindIanaIgnoreCase("Nowhere/Land", canon, sizeof canon) == nullptr);
  CHECK(tzFindIanaIgnoreCase("", canon, sizeof canon) == nullptr);
  CHECK(tzFindIanaIgnoreCase(nullptr, canon, sizeof canon) == nullptr);
  CHECK(tzFindIanaIgnoreCase("Australia/Perth", nullptr, 0) != nullptr);
  CHECK(tzFindIanaIgnoreCase("Australia/Per", canon, sizeof canon) == nullptr);   // prefix is not a match
  CHECK(tzFindIanaIgnoreCase("Australia/Perth2", canon, sizeof canon) == nullptr);
}

void testSettingsReport() {
  section("settings file: report");
  const char *file =
      "# comment\n"
      "wifi = maybe\n"
      "unknown_key = 1\n"
      "wifi_ssid = Home\n"
      "this line has no equals\n"
      "latitude = 95\n"
      "units = imperial\n"
      "units = imperial\n";
  const Applied a = apply(file);
  CHECK(a.r.lines == 8);
  CHECK(a.r.changed == 2 && a.r.unchanged == 1);  // the second "units" repeats the first
  CHECK(a.r.unknown == 1 && a.r.bad == 3 && a.r.problems() == 4);
  CHECK(a.r.issueCount == 4);
  CHECK(a.r.issues[0].line == 2 && strstr(a.r.issues[0].text, "line 2") && strstr(a.r.issues[0].text, "wifi"));
  CHECK(a.r.issues[1].line == 3 && strstr(a.r.issues[1].text, "unknown") && strstr(a.r.issues[1].text, "unknown_key"));
  CHECK(a.r.issues[2].line == 5 && strstr(a.r.issues[2].text, "line 5"));
  CHECK(a.r.issues[3].line == 6 && strstr(a.r.issues[3].text, "latitude"));
  CHECK_STR(a.s.wifiSsid, "Home");
  CHECK(a.s.imperial() && !a.s.hasLatLon() && a.s.wifi);
  char sum[96];
  formatConfigSummary(a.r, sum, sizeof sum);
  CHECK_STR(sum, "2 changed, 1 unchanged, 4 problems");
  formatConfigSummary(apply("units = imperial").r, sum, sizeof sum);
  CHECK_STR(sum, "1 changed");
  formatConfigSummary(apply("wifi = maybe").r, sum, sizeof sum);
  CHECK_STR(sum, "1 problem");
  formatConfigSummary(apply("# nothing").r, sum, sizeof sum);
  CHECK_STR(sum, "nothing to apply");
  formatConfigSummary(apply("reset_all = yes\nunits = imperial").r, sum, sizeof sum);
  CHECK_STR(sum, "all forgotten, 1 changed");
  char tiny[8];
  formatConfigSummary(a.r, tiny, sizeof tiny);  // must stay inside the buffer
  CHECK(strlen(tiny) < sizeof tiny);

  // only the first few problems are kept, but all are counted
  std::string many;
  for (int i = 0; i < 20; i++) many += "wifi = maybe\n";
  const Applied m = apply(many);
  CHECK(m.r.bad == 20 && m.r.issueCount == kConfigMaxIssues && m.r.lines == 20);
  CHECK(m.r.issues[kConfigMaxIssues - 1].line == kConfigMaxIssues);

  // a line that is far too long is reported, not parsed
  const Applied l = apply("wifi_ssid = " + std::string(500, 'x') + "\nunits = imperial\n");
  CHECK(l.r.bad == 1 && l.r.changed == 1 && l.s.imperial() && l.s.wifiSsid[0] == 0);
  CHECK(strstr(l.r.issues[0].text, "too long"));
  // text that is no settings file at all
  static const char junkText[] = "\x01\x02\xFF\xFE garbage \x80\x81\n\0\0\0=\n";
  const Applied junk = apply(std::string(junkText, sizeof junkText - 1));
  CHECK(junk.r.touched() == 0 && junk.r.problems() >= 1);
  static const char nulText[] = "wifi_ssid = a\0b\n";  // a NUL byte inside a value is replaced, not a terminator
  CHECK_STR(apply(std::string(nulText, sizeof nulText - 1)).s.wifiSsid, "a?b");
}

void testSettingsForget() {
  section("settings file: forgetting and reset_all");
  Settings defaults;
  snprintf(defaults.hostname, sizeof defaults.hostname, "factory-name");
  defaults.latitude = -31.952;
  defaults.longitude = 115.861;
  // a clock with a few saved settings
  Settings s = defaults;
  const std::string saved = "hostname = mine\nunits = imperial\nlatitude = 10\nwifi_ssid = Home\n";
  ConfigReport r0 = applyConfigText(s, defaults, saved.data(), saved.size());
  CHECK(r0.changed == 4 && s.userSet != 0);
  CHECK_STR(s.hostname, "mine");

  // an empty value forgets one setting: the build's default is back
  const std::string forget = "hostname =\nlatitude = \n";
  Settings s1 = s;
  const ConfigReport r1 = applyConfigText(s1, defaults, forget.data(), forget.size());
  CHECK(r1.cleared == 2 && r1.changed == 0 && r1.problems() == 0);
  CHECK_STR(s1.hostname, "factory-name");
  CHECK_NEAR(s1.latitude, -31.952, 1e-9);
  CHECK(!settingIsUserSet(s1, idx("hostname")) && !settingIsUserSet(s1, idx("latitude")));
  CHECK(settingIsUserSet(s1, idx("units")) && settingIsUserSet(s1, idx("wifi_ssid")));  // the others stay
  CHECK(s1.imperial() && !strcmp(s1.wifiSsid, "Home"));

  // forgetting something that was never saved is not an error
  Settings s2 = defaults;
  const ConfigReport r2 = applyConfigText(s2, defaults, forget.data(), forget.size());
  CHECK(r2.cleared == 0 && r2.unchanged == 2 && r2.problems() == 0 && r2.touched() == 0);

  // reset_all forgets everything first, even when it comes last in the file
  const std::string reset = "reset_all = yes\n";
  Settings s3 = s;
  const ConfigReport r3 = applyConfigText(s3, defaults, reset.data(), reset.size());
  CHECK(r3.resetAll && r3.touched() == 1 && r3.problems() == 0);
  CHECK(s3.userSet == 0 && dump(s3) == dump(defaults));
  const std::string reset2 = "units = metric\nreset_all = yes\nspotify = off\n";
  Settings s4 = s;
  const ConfigReport r4 = applyConfigText(s4, defaults, reset2.data(), reset2.size());
  CHECK(r4.resetAll && r4.problems() == 0);
  CHECK(!s4.imperial() && !s4.spotify);                              // lines apply on top of the reset
  CHECK_STR(s4.hostname, "factory-name");                            // saved values are gone
  CHECK(settingIsUserSet(s4, idx("units")) && settingIsUserSet(s4, idx("spotify")));  // units and spotify are saved again
  CHECK(!settingIsUserSet(s4, idx("hostname")) && !settingIsUserSet(s4, idx("wifi_ssid")));
  CHECK(r4.changed == 1 && r4.unchanged == 1);  // metric is the default again after the reset; spotify=off is new
  for (const char *spelling : {"reset_all = on", "Reset All = TRUE", "reset-all: 1", "reset_all = \"yes\""}) {
    Settings t = s;
    const ConfigReport rr = applyConfigText(t, defaults, spelling, strlen(spelling));
    CHECK(rr.resetAll && t.userSet == 0);
  }
  for (const char *no : {"reset_all = no", "reset_all = off", "# reset_all = yes"}) {
    Settings t = s;
    const ConfigReport rr = applyConfigText(t, defaults, no, strlen(no));
    CHECK(!rr.resetAll && t.userSet == s.userSet && rr.problems() == 0);
  }
  Settings t5 = s;
  const std::string badReset = "reset_all = please\n";
  const ConfigReport r5 = applyConfigText(t5, defaults, badReset.data(), badReset.size());
  CHECK(!r5.resetAll && r5.bad == 1 && t5.userSet == s.userSet);

  // single settings, the API a web page or a button handler could use
  Settings t6 = defaults;
  char err[96];
  CHECK(applySetting(t6, defaults, "units", "Imperial", err, sizeof err) == ApplyResult::Changed);
  CHECK(applySetting(t6, defaults, "units", "imperial", err, sizeof err) == ApplyResult::Unchanged);
  CHECK(applySetting(t6, defaults, "units", "kelvin", err, sizeof err) == ApplyResult::BadValue && strstr(err, "units"));
  CHECK(applySetting(t6, defaults, "colour", "red", err, sizeof err) == ApplyResult::UnknownKey && strstr(err, "colour"));
  CHECK(applySetting(t6, defaults, "units", "", err, sizeof err) == ApplyResult::Cleared);
  CHECK(!t6.imperial() && t6.userSet == 0);
  CHECK(applySetting(t6, defaults, "wifi_ssid", "\"Quoted Net\"") == ApplyResult::Changed && !strcmp(t6.wifiSsid, "Quoted Net"));
  CHECK(applySetting(t6, defaults, "wifi_ssid", nullptr) == ApplyResult::Cleared);
}

// Lines of an example file that look like "# key = value" for a known key.
std::vector<std::string> splitLines(const std::string &text) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t e = text.find('\n', pos);
    if (e == std::string::npos) e = text.size();
    lines.push_back(text.substr(pos, e - pos));
    pos = e + 1;
  }
  return lines;
}

bool isSettingLine(const std::string &line, size_t *index) {
  if (line.compare(0, 2, "# ") != 0) return false;
  for (size_t i = 0; i < settingCount(); i++) {
    const std::string prefix = std::string("# ") + settingKey(i) + " = ";
    if (line.compare(0, prefix.size(), prefix) == 0) {
      *index = i;
      return true;
    }
  }
  return false;
}

void checkExample(const Settings &cur, const char *label) {
  printf("  (%s)\n", label);
  static char buf[16384];
  const size_t n = renderExampleConfig(cur, "Measured drift: +1.2 s/day over 3 days", buf, sizeof buf);
  CHECK(n > 2000 && n < 9000);
  CHECK(n == strlen(buf));
  const std::string text(buf, n);
  CHECK(text.back() == '\n');
  CHECK(text.find('\r') == std::string::npos);
  CHECK(text.find("Measured drift: +1.2 s/day over 3 days") != std::string::npos);
  CHECK(text.find("drift") != std::string::npos);
  CHECK(text.find("reset_all") != std::string::npos);
  CHECK(text.find("quotes") != std::string::npos);

  // nothing but comments and blank lines, none too wide for a small editor window
  size_t settingLines = 0;
  bool seen[32] = {};
  for (const std::string &line : splitLines(text)) {
    CHECK(line.empty() || line[0] == '#');
    CHECK(line.size() <= 96);
    size_t idx;
    if (isSettingLine(line, &idx)) {
      settingLines++;
      CHECK(!seen[idx]);
      seen[idx] = true;
    }
  }
  CHECK(settingLines == settingCount());  // every setting is listed, once

  // so an unedited example changes nothing
  Settings same = cur;
  const Settings defaults;
  const ConfigReport r = applyConfigText(same, defaults, text.data(), text.size());
  CHECK(r.problems() == 0 && r.touched() == 0 && r.changed == 0 && r.cleared == 0 && r.unchanged == 0);
  CHECK(dump(same) == dump(cur));
  CHECK(same.userSet == cur.userSet);

  // and removing the # in front of each (non-secret) setting gives the same values back
  std::string uncommented;
  for (const std::string &line : splitLines(text)) {
    size_t idx;
    if (isSettingLine(line, &idx) && !settingIsSecret(idx)) {
      uncommented += line.substr(2) + "\n";
    } else {
      uncommented += line + "\n";
    }
  }
  Settings round;
  const ConfigReport r2 = applyConfigText(round, defaults, uncommented.data(), uncommented.size());
  if (r2.problems()) {
    for (int i = 0; i < r2.issueCount; i++) printf("  issue: %s\n", r2.issues[i].text);
  }
  CHECK(r2.problems() == 0);
  for (size_t i = 0; i < settingCount(); i++) {
    if (settingIsSecret(i)) continue;
    if (fmt(round, i) != fmt(cur, i)) printf("  %s: [%s] vs [%s]\n", settingKey(i), fmt(round, i).c_str(), fmt(cur, i).c_str());
    CHECK(fmt(round, i) == fmt(cur, i));
  }

  // too small a buffer is reported, never overrun
  char small[200];
  CHECK(renderExampleConfig(cur, "", small, sizeof small) == 0);
  CHECK(renderExampleConfig(cur, "", buf, 0) == 0);
}

void testSettingsExample() {
  section("example settings file");
  checkExample(Settings(), "defaults");
  const Applied all = apply(kAllSettings);
  checkExample(all.s, "everything changed");

  // the password and the Client ID never reach the card
  Settings secret = all.s;
  snprintf(secret.wifiPassword, sizeof secret.wifiPassword, "hunter2-super-secret");
  static char buf[16384];
  const size_t n = renderExampleConfig(secret, "", buf, sizeof buf);
  CHECK(n > 0);
  CHECK(strstr(buf, "hunter2-super-secret") == nullptr);
  CHECK(strstr(buf, "0123456789abcdef0123456789abcdef") == nullptr);
  CHECK(strstr(buf, "p@ss") == nullptr);
  CHECK(strstr(buf, "Cafe Net") != nullptr);  // the network name is not secret, and helps the user
  CHECK(strstr(buf, "# wifi_password = \"your password\"") != nullptr);
  CHECK(strstr(buf, "# wifi_backup_password = \"your password\"") != nullptr);
  CHECK(strstr(buf, "h0tsp0t") == nullptr);  // (the backup password set by kAllSettings)
  CHECK(strstr(buf, "# spotify_client_id = \"your client id\"") != nullptr);

  // without a measurement the generic drift note is still there
  const size_t n2 = renderExampleConfig(Settings(), nullptr, buf, sizeof buf);
  CHECK(n2 > 0 && strstr(buf, "drift") != nullptr && strstr(buf, "Measured") == nullptr);
  // a name with a quote in it survives the round trip through the example text? (it is quoted as typed)
  Settings quote;
  snprintf(quote.wifiSsid, sizeof quote.wifiSsid, "Bob's WiFi");
  checkExample(quote, "apostrophe in the SSID");

  // the card gets Windows line ends: the same text with every newline preceded by CR, which reads the same
  static char crlfBuf[20000], lfBuf[20000];
  const size_t lfLen = renderExampleConfig(all.s, "Measured: +1.0 ppm", lfBuf, sizeof lfBuf, false);
  const size_t crlfLen = renderExampleConfig(all.s, "Measured: +1.0 ppm", crlfBuf, sizeof crlfBuf, true);
  CHECK(lfLen > 0 && crlfLen > lfLen);
  std::string lfText(lfBuf, lfLen), crlfText(crlfBuf, crlfLen), stripped;
  size_t newlines = 0, loneLf = 0;
  for (size_t i = 0; i < crlfText.size(); i++) {
    if (crlfText[i] == '\n') {
      newlines++;
      if (i == 0 || crlfText[i - 1] != '\r') loneLf++;
    }
    if (crlfText[i] != '\r') stripped += crlfText[i];
  }
  CHECK(stripped == lfText);
  CHECK(loneLf == 0 && crlfLen == lfLen + newlines);
  Settings same = all.s;
  const ConfigReport rr = applyConfigText(same, Settings(), crlfText.data(), crlfText.size());
  CHECK(rr.problems() == 0 && rr.touched() == 0 && rr.changed == 0);
  // a buffer one byte short is refused, with or without CRLF
  CHECK(renderExampleConfig(all.s, "", crlfBuf, 100, true) == 0);
  static char probe[20000];
  const size_t exact = renderExampleConfig(all.s, "", probe, sizeof probe, true);
  CHECK(exact > 0);
  CHECK(renderExampleConfig(all.s, "", probe, exact, true) == 0);      // no room for the terminator
  CHECK(renderExampleConfig(all.s, "", probe, exact + 1, true) == exact);
}

void testSettingsFuzz() {
  section("settings file: garbage in, no crash");
  static char example[16384];
  const size_t exampleLen = renderExampleConfig(Settings(), "", example, sizeof example);
  uint32_t rng = 12345;
  auto next = [&rng]() {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
  };
  int tested = 0;
  for (int round = 0; round < 3000; round++) {
    std::string text;
    if (round % 3 == 0) {  // random bytes
      const size_t len = next() % 1500;
      for (size_t i = 0; i < len; i++) text += (char)(next() & 0xFF);
    } else if (round % 3 == 1) {  // the example file with some bytes changed
      text.assign(example, exampleLen);
      std::string uncommented;
      for (const std::string &line : splitLines(text)) {
        size_t idx;
        uncommented += (isSettingLine(line, &idx) ? line.substr(2) : line) + "\n";
      }
      text = uncommented;
      const int flips = 1 + (int)(next() % 20);
      for (int i = 0; i < flips; i++) text[next() % text.size()] = (char)(next() & 0xFF);
    } else {  // settings-like lines with odd values
      static const char *keys[] = {"wifi", "wifi_ssid", "latitude", "timezone", "units", "date_format", "reset_all",
                                   "battery_cutoff_v", "hostname", "cpu_mhz", "", "=", ":"};
      static const char *values[] = {"", "\"", "'", "\"\"\"", "1e999", "-1e999", "nan", "0x1p3", "99999999999999999999",
                                     "\xE2\x80", "\xC2", "\xE2\x80\x9C", "a b c d e f g h i j k l m n o p q r s t u v w x y z",
                                     "# #", "  ", "S", "-", "1,2,3", ",", "."};
      const int lines = 1 + (int)(next() % 12);
      for (int i = 0; i < lines; i++) {
        text += keys[next() % (sizeof keys / sizeof keys[0])];
        text += (next() & 1) ? " = " : (next() & 1) ? ":" : "";
        text += values[next() % (sizeof values / sizeof values[0])];
        text += (next() & 1) ? "\r\n" : "\n";
      }
    }
    Settings s;
    const Settings defaults;
    const ConfigReport r = applyConfigText(s, defaults, text.data(), text.size());
    tested++;
    // whatever came in, the settings are still sane
    bool sane = r.lines >= 0 && r.issueCount <= kConfigMaxIssues && s.latitude >= -90 && s.latitude <= 90 &&
                s.longitude >= -180 && s.longitude <= 180 && s.indoorOffsetC >= -15 && s.indoorOffsetC <= 15 &&
                s.batteryCutoffV >= 3.1f && s.batteryCutoffV <= 3.6f && s.weatherIntervalMin >= 5 &&
                s.weatherIntervalMin <= 240 && s.batteryCapacityMah >= 0 && s.batteryCapacityMah <= 20000 &&
                s.units <= UNITS_IMPERIAL && s.timeFormat <= TIME_12H && s.dateFormat < DATE_FORMAT_COUNT &&
                s.battery <= BATTERY_NONE && s.wifiPowerSave <= WIFISAVE_MAX && s.cpuSpeed <= CPU_240;
    sane = sane && memchr(s.wifiSsid, 0, sizeof s.wifiSsid) && memchr(s.wifiPassword, 0, sizeof s.wifiPassword) &&
           memchr(s.hostname, 0, sizeof s.hostname) && memchr(s.ntpServer, 0, sizeof s.ntpServer) &&
           memchr(s.locationLabel, 0, sizeof s.locationLabel) && memchr(s.location, 0, sizeof s.location) &&
           memchr(s.timezone, 0, sizeof s.timezone) && memchr(s.spotifyClientId, 0, sizeof s.spotifyClientId);
    if (!sane) printf("  insane result for round %d\n", round);
    CHECK(sane);
    // and the text of every setting still goes through flash unchanged
    for (size_t i = 0; i < settingCount(); i++) {
      Settings t;
      if (!applyStoredSetting(t, i, fmt(s, i).c_str())) {
        printf("  round %d: %s=[%s] does not reload\n", round, settingKey(i), fmt(s, i).c_str());
        CHECK(false);
        break;
      }
    }
  }
  CHECK(tested == 3000);
  // also: null text, zero length
  Settings s;
  const Settings defaults;
  CHECK(applyConfigText(s, defaults, nullptr, 0).lines == 0);
  CHECK(applyConfigText(s, defaults, "x", 0).lines == 0);

  // the worst case for the UTF-16 decoder: every unit grows to three bytes (the buffer is sized for it)
  std::string euros = "\xFF\xFE";
  for (int i = 0; i < 4000; i++) {
    euros += '\xAC';
    euros += '\x20';
  }
  euros += '\n';
  euros += '\0';
  euros += '\0';  // an odd trailing byte pair of zeros
  const ConfigReport er = applyConfigText(s, defaults, euros.data(), euros.size());
  CHECK(er.lines >= 1 && er.bad >= 1);  // one very long line, reported as such
  std::string be = "\xFE\xFF";           // and a lone BOM, or a BOM and half a unit
  CHECK(applyConfigText(s, defaults, be.data(), be.size()).lines == 0);
  be += 'x';
  CHECK(applyConfigText(s, defaults, be.data(), be.size()).lines == 0);
}

// ===========================================================================
// Moon
// ===========================================================================
int64_t utc(int y, int mo, int d, int h, int mi) { return calc::epochFromUtc(y, mo, d, h, mi, 0); }

void testMoonPhase() {
  section("moon phase");
  struct Event {
    int y, mo, d, h, mi;
  };
  // new moons and full moons that came with eclipses, times from the published eclipse circumstances (UTC)
  const Event newMoons[] = {{2000, 1, 6, 18, 14}, {2017, 8, 21, 18, 30}, {2019, 7, 2, 19, 16}, {2021, 12, 4, 7, 43},
                            {2024, 4, 8, 18, 21}, {2024, 10, 2, 18, 49}, {2025, 3, 29, 10, 58}};
  const Event fullMoons[] = {{2000, 1, 21, 4, 40}, {2018, 7, 27, 20, 21}, {2019, 1, 21, 5, 16}, {2021, 5, 26, 11, 14},
                             {2022, 11, 8, 11, 2}, {2024, 9, 18, 2, 34},  {2025, 3, 14, 6, 55}};
  for (const Event &e : newMoons) {
    const moon::Phase p = moon::phaseAt(utc(e.y, e.mo, e.d, e.h, e.mi));
    if (!(p.lit < 0.003f)) printf("  new moon %d-%02d-%02d: lit %.5f\n", e.y, e.mo, e.d, p.lit);
    CHECK(p.lit < 0.003f);
    CHECK(p.age < 0.01f || p.age > 0.99f);
    // a day before: a thin waning crescent; a day after: a thin waxing one
    const moon::Phase before = moon::phaseAt(utc(e.y, e.mo, e.d, e.h, e.mi) - 86400);
    const moon::Phase after = moon::phaseAt(utc(e.y, e.mo, e.d, e.h, e.mi) + 86400);
    CHECK(!before.waxing && before.lit > 0.005f && before.lit < 0.05f);
    CHECK(after.waxing && after.lit > 0.005f && after.lit < 0.05f);
  }
  for (const Event &e : fullMoons) {
    const moon::Phase p = moon::phaseAt(utc(e.y, e.mo, e.d, e.h, e.mi));
    if (!(p.lit > 0.997f)) printf("  full moon %d-%02d-%02d: lit %.5f\n", e.y, e.mo, e.d, p.lit);
    CHECK(p.lit > 0.997f);
    CHECK(fabsf(p.age - 0.5f) < 0.01f);
    const moon::Phase before = moon::phaseAt(utc(e.y, e.mo, e.d, e.h, e.mi) - 86400);
    const moon::Phase after = moon::phaseAt(utc(e.y, e.mo, e.d, e.h, e.mi) + 86400);
    CHECK(before.waxing && before.lit < 0.995f && before.lit > 0.95f);
    CHECK(!after.waxing && after.lit < 0.995f && after.lit > 0.95f);
  }
  // quarters (UTC): half lit, growing at the first, shrinking at the last
  const Event first[] = {{2024, 4, 15, 19, 13}, {2024, 10, 10, 18, 55}};
  const Event last[] = {{2024, 5, 1, 11, 27}, {2024, 10, 24, 8, 3}};
  for (const Event &e : first) {
    const moon::Phase p = moon::phaseAt(utc(e.y, e.mo, e.d, e.h, e.mi));
    CHECK_NEAR(p.lit, 0.5, 0.02);
    CHECK_NEAR(p.age, 0.25, 0.01);
    CHECK(moon::phaseAt(utc(e.y, e.mo, e.d, e.h, e.mi) + 3 * 3600).waxing);
  }
  for (const Event &e : last) {
    const moon::Phase p = moon::phaseAt(utc(e.y, e.mo, e.d, e.h, e.mi));
    CHECK_NEAR(p.lit, 0.5, 0.02);
    CHECK_NEAR(p.age, 0.75, 0.01);
    CHECK(!moon::phaseAt(utc(e.y, e.mo, e.d, e.h, e.mi) + 3 * 3600).waxing);
  }

  // every new moon of 2024, found by scanning for the darkest minute near the published time
  const Event year2024[] = {{2024, 1, 11, 11, 57}, {2024, 2, 9, 22, 59}, {2024, 3, 10, 9, 0},   {2024, 4, 8, 18, 21},
                            {2024, 5, 8, 3, 22},   {2024, 6, 6, 12, 38}, {2024, 7, 5, 22, 57},  {2024, 8, 4, 11, 13},
                            {2024, 9, 3, 1, 55},   {2024, 10, 2, 18, 49}, {2024, 11, 1, 12, 47}, {2024, 12, 1, 6, 21}};
  for (const Event &e : year2024) {
    const int64_t expected = utc(e.y, e.mo, e.d, e.h, e.mi);
    int64_t best = expected;
    float bestLit = 2;
    for (int64_t t = expected - 12 * 3600; t <= expected + 12 * 3600; t += 60) {
      const float lit = moon::phaseAt(t).lit;
      if (lit < bestLit) {
        bestLit = lit;
        best = t;
      }
    }
    const double offMin = (double)(best - expected) / 60.0;
    if (fabs(offMin) > 45) printf("  new moon %d-%02d-%02d: found %+.0f min from the published time\n", e.y, e.mo, e.d, offMin);
    CHECK(fabs(offMin) <= 45);  // measured worst case: 29 minutes (the darkest moment is not exactly the conjunction)
  }

  // away from an eclipse the Moon passes up to five degrees from the Sun, so it is never quite dark
  for (const Event &e : {Event{2024, 1, 11, 11, 57}, Event{2024, 2, 9, 22, 59}, Event{2024, 6, 6, 12, 38}}) {
    float darkest = 2;
    for (int64_t t = utc(e.y, e.mo, e.d, e.h, e.mi) - 6 * 3600; t <= utc(e.y, e.mo, e.d, e.h, e.mi) + 6 * 3600; t += 60)
      darkest = fminf(darkest, moon::phaseAt(t).lit);
    if (!(darkest > 0.0008f && darkest < 0.0025f)) printf("  darkest at %d-%02d-%02d: %.5f\n", e.y, e.mo, e.d, darkest);
    CHECK(darkest > 0.0008f && darkest < 0.0025f);  // (1 - cos 5 degrees) / 2 is 0.0019
  }

  // over one lunation the lit fraction rises and falls once, and the age advances steadily
  const int64_t nm = utc(2024, 4, 8, 18, 21);
  float prev = -1;
  int ups = 0, downs = 0;
  for (int h = 0; h <= 29 * 24 + 12; h++) {
    const moon::Phase p = moon::phaseAt(nm + (int64_t)h * 3600);
    CHECK(p.lit >= 0 && p.lit <= 1);
    CHECK(p.age >= 0 && p.age < 1);
    if (prev >= 0) {
      if (p.lit > prev + 1e-6f) ups++;
      else if (p.lit < prev - 1e-6f) downs++;
    }
    prev = p.lit;
  }
  CHECK(ups > 330 && ups < 380);      // about 14.8 days of waxing
  CHECK(downs > 330 && downs < 380);  // about 14.8 days of waning
  // the same instant a synodic month later is nearly the same phase
  CHECK_NEAR(moon::phaseAt(nm + (int64_t)(29.530589 * 86400)).lit, moon::phaseAt(nm).lit, 0.02);
  // dates a long way from today still work
  CHECK(moon::phaseAt(0).lit >= 0 && moon::phaseAt(0).lit <= 1);                 // 1970
  CHECK(moon::phaseAt(utc(2099, 12, 31, 12, 0)).lit <= 1);                       // far future
  CHECK(moon::phaseAt(-86400LL * 365 * 30).lit >= 0);                            // before 1970
}

// How many pixels of a disc are inside / lit.
void countDisc(int d, float lit, bool onRight, int *inside, int *litPx) {
  *inside = *litPx = 0;
  for (int r = 0; r < d; r++)
    for (int c = 0; c < d; c++) {
      const moon::DiscPixel p = moon::discPixel(c, r, d, lit, onRight);
      *inside += p.inside;
      *litPx += p.lit;
    }
}

void testMoonGlyph() {
  section("moon glyph");
  for (int d = 8; d <= 14; d++) {
    int inside, litPx;
    countDisc(d, 0.0f, true, &inside, &litPx);
    CHECK(inside >= d * d / 2 && inside <= d * d);
    CHECK(litPx == 0);  // new moon: nothing lit
    countDisc(d, 1.0f, true, &inside, &litPx);
    CHECK(litPx == inside);  // full moon: all of it
    countDisc(d, 0.003f, true, &inside, &litPx);
    CHECK(litPx == 0);
    countDisc(d, 0.997f, false, &inside, &litPx);
    CHECK(litPx == inside);

    // quarter: one half (even sizes are exactly half; odd sizes keep the middle column dark)
    int right, left;
    countDisc(d, 0.5f, true, &inside, &right);
    countDisc(d, 0.5f, false, &inside, &left);
    CHECK(right == left);
    if (d % 2 == 0) CHECK(right * 2 == inside);
    CHECK(abs(right * 2 - inside) <= d);

    // the pixels lit grow with the lit fraction, and track it
    int last = -1;
    for (int k = 0; k <= 20; k++) {
      const float lit = k / 20.0f;
      int n;
      countDisc(d, lit, true, &inside, &n);
      CHECK(n >= last);
      last = n;
      if (fabs((double)n / inside - lit) > 0.12) printf("  d=%d lit=%.2f: %d of %d pixels\n", d, lit, n, inside);
      CHECK(fabs((double)n / inside - lit) <= 0.12);
    }

    // the terminator is an arc, not a straight line: on a crescent it starts nearer the middle in
    // the rows towards the top and bottom than across the middle of the disc
    if (d >= 11) {
      auto firstLit = [&](int row) {
        for (int c = 0; c < d; c++)
          if (moon::discPixel(c, row, d, 0.25f, true).lit) return c;
        return -1;
      };
      const int mid = firstLit(d / 2), nearTop = firstLit(1), nearBottom = firstLit(d - 2);
      CHECK(mid >= 0 && nearTop >= 0 && nearBottom >= 0);
      CHECK(nearTop < mid && nearBottom < mid);
      CHECK(nearTop == nearBottom);
      // and a gibbous moon bulges the other way: the dark part is a crescent on the unlit side
      auto firstDark = [&](int row) {
        for (int c = d - 1; c >= 0; c--) {
          const moon::DiscPixel p = moon::discPixel(c, row, d, 0.75f, true);
          if (p.inside && !p.lit) return c;
        }
        return -1;
      };
      CHECK(firstDark(1) > firstDark(d / 2));  // the dark crescent's inner edge sits nearer the middle at the top
    }

    // mirrored when seen from the other hemisphere, and the lit part grows from the limb
    for (int k = 1; k < 20; k++) {
      const float lit = k / 20.0f;
      for (int r = 0; r < d; r++) {
        int firstIn = -1, lastIn = -1, litCount = 0, litFirst = -1, litLast = -1;
        for (int c = 0; c < d; c++) {
          const moon::DiscPixel a = moon::discPixel(c, r, d, lit, true);
          const moon::DiscPixel b = moon::discPixel(d - 1 - c, r, d, lit, false);
          CHECK(a.inside == b.inside && a.lit == b.lit);
          if (a.inside) {
            if (firstIn < 0) firstIn = c;
            lastIn = c;
          }
          if (a.lit) {
            if (litFirst < 0) litFirst = c;
            litLast = c;
            litCount++;
          }
        }
        if (litCount) {
          CHECK(litLast == lastIn);                        // touches the lit limb
          CHECK(litLast - litFirst + 1 == litCount);       // one run, no holes
        }
        CHECK(firstIn >= 0);  // every row of the box crosses the disc
      }
    }
  }
}

// ===========================================================================
// Date and time text
// ===========================================================================
void testDateFormats() {
  section("date formats");
  struct Case {
    uint8_t format;
    const char *oct4;   // 2026-10-04
    const char *jan9;   // 2026-01-09
    const char *dec31;  // 1999-12-31
  } cases[] = {
      {DATE_ISO, "2026-10-04", "2026-01-09", "1999-12-31"},
      {DATE_DMY, "04/10/2026", "09/01/2026", "31/12/1999"},
      {DATE_MDY, "10/04/2026", "01/09/2026", "12/31/1999"},
      {DATE_DMY_DOT, "04.10.2026", "09.01.2026", "31.12.1999"},
      {DATE_D_MON_Y, "4 Oct 2026", "9 Jan 2026", "31 Dec 1999"},
      {DATE_MON_D_Y, "Oct 4, 2026", "Jan 9, 2026", "Dec 31, 1999"},
  };
  char buf[32];
  for (const Case &c : cases) {
    datefmt::formatDate(2026, 10, 4, c.format, buf, sizeof buf);
    CHECK_STR(buf, c.oct4);
    datefmt::formatDate(2026, 1, 9, c.format, buf, sizeof buf);
    CHECK_STR(buf, c.jan9);
    datefmt::formatDate(1999, 12, 31, c.format, buf, sizeof buf);
    CHECK_STR(buf, c.dec31);
  }
  CHECK(DATE_FORMAT_COUNT == 6);
  // unknown formats fall back to ISO, a short buffer is cut and terminated
  datefmt::formatDate(2026, 10, 4, 200, buf, sizeof buf);
  CHECK_STR(buf, "2026-10-04");
  char tiny[6];
  datefmt::formatDate(2026, 10, 4, DATE_ISO, tiny, sizeof tiny);
  CHECK_STR(tiny, "2026-");
  // every date of 2026 in every format is short enough for the date line, and fixed width for the numeric ones
  for (int f = 0; f < DATE_FORMAT_COUNT; f++) {
    size_t shortest = 99, longest = 0;
    for (int mo = 1; mo <= 12; mo++)
      for (int d = 1; d <= 28; d++) {
        datefmt::formatDate(2026, mo, d, (uint8_t)f, buf, sizeof buf);
        const size_t n = strlen(buf);
        shortest = n < shortest ? n : shortest;
        longest = n > longest ? n : longest;
      }
    CHECK(longest <= 12);
    if (f <= DATE_DMY_DOT) CHECK(shortest == longest && longest == 10);  // numeric formats never change width
  }

  // 12 hour clock face
  bool pm = false;
  CHECK(datefmt::hour12(0, &pm) == 12 && !pm);
  CHECK(datefmt::hour12(1, &pm) == 1 && !pm);
  CHECK(datefmt::hour12(11, &pm) == 11 && !pm);
  CHECK(datefmt::hour12(12, &pm) == 12 && pm);
  CHECK(datefmt::hour12(13, &pm) == 1 && pm);
  CHECK(datefmt::hour12(23, &pm) == 11 && pm);
  CHECK(datefmt::hour12(24, &pm) == 12 && !pm);   // wraps like 0
  CHECK(datefmt::hour12(-1, &pm) == 11 && pm);    // never crashes on odd input
  CHECK(datefmt::hour12(5, nullptr) == 5);
  CHECK_STR(datefmt::ampm(false), "AM");
  CHECK_STR(datefmt::ampm(true), "PM");
  for (int h = 0; h < 24; h++) {
    const int h12 = datefmt::hour12(h, &pm);
    CHECK(h12 >= 1 && h12 <= 12 && pm == (h >= 12));
  }
}

// ===========================================================================
// Battery curve, estimate, shutdown guard
// ===========================================================================
void testBatteryCurve() {
  section("battery curve (float and inverse)");
  // the integer gauge is exactly the rounded float one
  for (int mv = 3000; mv <= 4400; mv++) {
    const float v = mv / 1000.0f;
    CHECK(calc::batteryPercent(v) == (int)lroundf(calc::batteryPercentF(v)));
  }
  CHECK_NEAR(calc::batteryPercentF(4.30f), 100, 1e-6);
  CHECK_NEAR(calc::batteryPercentF(4.20f), 100, 1e-6);
  CHECK_NEAR(calc::batteryPercentF(3.84f), 50, 1e-4);
  CHECK_NEAR(calc::batteryPercentF(3.27f), 0, 1e-4);
  CHECK_NEAR(calc::batteryPercentF(3.0f), 0, 1e-6);
  CHECK_NEAR(calc::batteryPercentF(3.93f), 65 + 5 * (3.93 - 3.91) / (3.95 - 3.91), 0.01);  // 67.5, between 3.91 (65) and 3.95 (70)
  // monotonic, and the inverse really is one
  float last = -1;
  for (int mv = 3200; mv <= 4250; mv++) {
    const float p = calc::batteryPercentF(mv / 1000.0f);
    CHECK(p >= last);
    last = p;
  }
  for (int i = 0; i <= 200; i++) {
    const float pct = i * 0.5f;
    const float v = calc::batteryVoltsForPercent(pct);
    CHECK_NEAR(calc::batteryPercentF(v), pct, 0.01);
  }
  CHECK_NEAR(calc::batteryVoltsForPercent(150), 4.20, 1e-6);
  CHECK_NEAR(calc::batteryVoltsForPercent(-5), 3.27, 1e-6);
  // volts fall as percent falls
  for (int i = 1; i <= 100; i++) CHECK(calc::batteryVoltsForPercent((float)i) > calc::batteryVoltsForPercent((float)i - 1));
}

// A deterministic reading source for the estimator tests.
struct Cell {
  uint32_t rng = 12345;
  double noiseV = 0;     // standard deviation of the reading, volts
  double dipRate = 0;    // chance of a transmit dip per reading
  double gaussian() {
    auto u = [this]() {
      rng = rng * 1664525u + 1013904223u;
      return ((rng >> 8) & 0xFFFFFF) / 16777216.0 + 1e-9;
    };
    return sqrt(-2.0 * log(u())) * cos(6.283185307179586 * u());
  }
  double uniform() {
    rng = rng * 1664525u + 1013904223u;
    return ((rng >> 8) & 0xFFFFFF) / 16777216.0;
  }
  double read(double pct) {
    double v = calc::batteryVoltsForPercent((float)pct);
    if (noiseV > 0) v += gaussian() * noiseV;
    if (dipRate > 0 && uniform() < dipRate) v -= 0.05 + 0.07 * uniform();
    return v;
  }
};

// Feeds `hours` of discharge starting at second `t0`, one reading every 5 s; returns the end time.
uint32_t discharge(battest::Estimator &est, Cell &cell, double startPct, double pctPerHour, double hours, uint32_t t0) {
  uint32_t t = t0;
  const uint32_t end = t0 + (uint32_t)(hours * 3600);
  for (; t < end; t += 5) {
    const double pct = startPct - pctPerHour * (double)(t - t0) / 3600.0;
    est.add(t, (float)cell.read(pct));
  }
  return t;
}

void testBatteryEstimate() {
  section("battery estimate");
  char buf[32];
  battest::formatRemaining(0.0f, buf, sizeof buf);       CHECK_STR(buf, "0 min");
  battest::formatRemaining(0.5f, buf, sizeof buf);       CHECK_STR(buf, "30 min");
  battest::formatRemaining(0.999f, buf, sizeof buf);     CHECK_STR(buf, "1 h 00 min");  // rounds up to the hour, not "60 min"
  battest::formatRemaining(1.0f, buf, sizeof buf);       CHECK_STR(buf, "1 h 00 min");
  battest::formatRemaining(5.0f + 40.0f / 60, buf, sizeof buf); CHECK_STR(buf, "5 h 40 min");
  battest::formatRemaining(47.9f, buf, sizeof buf);      CHECK_STR(buf, "47 h 54 min");
  battest::formatRemaining(48.0f, buf, sizeof buf);      CHECK_STR(buf, "2 d 0 h");
  battest::formatRemaining(100.0f, buf, sizeof buf);     CHECK_STR(buf, "4 d 4 h");
  battest::formatRemaining(24.0f * 31, buf, sizeof buf); CHECK_STR(buf, ">30 d");
  battest::formatRemaining(-1.0f, buf, sizeof buf);      CHECK_STR(buf, "--");
  battest::formatRemaining(NAN, buf, sizeof buf);        CHECK_STR(buf, "--");

  const float cutoff = 3.30f;
  const double cutPct = calc::batteryPercentF(cutoff);

  // nothing yet
  {
    battest::Estimator est;
    CHECK(est.estimate(cutoff, 0).state == battest::Estimate::OFF);
    est.add(100, NAN);
    est.add(100, 0.0f);
    est.add(100, 9.0f);
    CHECK(est.estimate(cutoff, 0).state == battest::Estimate::OFF);  // junk readings are ignored
    CHECK(est.binCount() == 0);
  }

  // a steady, noiseless discharge at 3 % an hour
  {
    battest::Estimator est;
    Cell cell;
    // minute by minute: the state goes OFF -> LEARNING (counting down) -> READY
    int lastLearn = 1000;
    bool seenLearning = false, seenReady = false;
    for (uint32_t sec = 0; sec <= 3 * 3600; sec += 5) {
      const double pct = 90 - 3.0 * sec / 3600.0;
      est.add(2000 + sec, (float)cell.read(pct));
      if (sec % 60 != 0) continue;
      const battest::Estimate e = est.estimate(cutoff, 0);
      if (e.state == battest::Estimate::LEARNING) {
        seenLearning = true;
        CHECK(!seenReady);
        CHECK(e.learnMin <= lastLearn);  // the countdown only goes down
        CHECK(e.learnMin >= 1 && e.learnMin <= 51);
        lastLearn = e.learnMin;
      } else if (e.state == battest::Estimate::READY) {
        if (!seenReady) CHECK(sec >= 45 * 60 && sec <= 56 * 60);  // 15 min settling + 30 min of history (+ the bin that is still open)
        seenReady = true;
      } else {
        CHECK(sec == 0);  // OFF only before the first reading closes
      }
    }
    CHECK(seenLearning && seenReady);
    const battest::Estimate e = est.estimate(cutoff, 1000);
    CHECK(e.state == battest::Estimate::READY);
    CHECK_NEAR(e.pctPerHour, 3.0, 0.03);
    CHECK_NEAR(e.avgMa, 30.0, 0.5);  // 3 % of 1000 mAh an hour
    CHECK(e.windowMin >= 100 && e.windowMin <= 180);  // all of the history there is: just under 3 h
    // the fitted level is that of the newest bin, at most one bin old
    CHECK_NEAR(e.levelPct, 90 - 3.0 * 3.0 + 0.25, 0.4);
    CHECK_NEAR(e.hoursLeft, (e.levelPct - cutPct) / 3.0, 0.05);
    CHECK(!e.unbounded);
    // without a capacity there is no current
    CHECK(est.estimate(cutoff, 0).avgMa == 0);
  }

  // the window follows the drain: a fast one is judged over about two hours, a slow one over many
  {
    battest::Estimator fast, slow;
    Cell c1, c2;
    discharge(fast, c1, 95, 12.0, 7.0, 0);
    discharge(slow, c2, 95, 1.5, 12.0, 0);
    const battest::Estimate f = fast.estimate(cutoff, 0), s = slow.estimate(cutoff, 0);
    CHECK(f.state == battest::Estimate::READY && s.state == battest::Estimate::READY);
    CHECK(f.windowMin >= 110 && f.windowMin <= 130);
    CHECK(s.windowMin >= 440 && s.windowMin <= 480);
    CHECK_NEAR(f.pctPerHour, 12.0, 0.15);
    CHECK_NEAR(s.pctPerHour, 1.5, 0.03);
    CHECK(slow.binCount() == battest::kMaxBins);  // 12 h of 5 minute bins: the oldest were dropped
  }

  // noise, WiFi dips: still close
  {
    battest::Estimator est;
    Cell cell;
    cell.noiseV = 0.003;
    cell.dipRate = 0.01;
    discharge(est, cell, 85, 3.0, 8.0, 500);
    const battest::Estimate e = est.estimate(cutoff, 0);
    CHECK(e.state == battest::Estimate::READY);
    const double truth = (85 - 3.0 * 8 - cutPct) / 3.0;
    if (fabs(e.pctPerHour - 3.0) > 0.45) printf("  noisy drain: %.2f %%/h, %.1f h left (truth %.1f h)\n", e.pctPerHour, e.hoursLeft, truth);
    CHECK_NEAR(e.pctPerHour, 3.0, 0.45);
    CHECK(fabs(e.hoursLeft - truth) / truth < 0.2);
  }

  // no drain at all: "unbounded", not a division by zero
  {
    battest::Estimator est;
    Cell cell;
    for (uint32_t sec = 0; sec < 5 * 3600; sec += 5) est.add(sec, 3.90f);
    const battest::Estimate e = est.estimate(cutoff, 1000);
    CHECK(e.state == battest::Estimate::READY && e.unbounded && e.pctPerHour < 0.02f && e.avgMa < 0.2f);
  }

  // a lot of transmit dips: each bin's median ignores them (a mean would sag by several percent)
  {
    battest::Estimator est;
    Cell cell;
    uint32_t rng = 777;
    for (uint32_t sec = 0; sec < 4 * 3600; sec += 5) {
      rng = rng * 1664525u + 1013904223u;
      const bool dip = ((rng >> 8) & 0xFF) < 40;  // about 16 % of the readings
      est.add(sec, dip ? 3.78f : 3.90f);
    }
    const battest::Estimate e = est.estimate(cutoff, 0);
    CHECK(e.state == battest::Estimate::READY);
    CHECK_NEAR(e.levelPct, calc::batteryPercentF(3.90f), 0.3);
  }

  // below the cut-off already: no time left
  {
    battest::Estimator est;
    Cell cell;
    discharge(est, cell, 6.0, 3.0, 2.0, 0);
    const battest::Estimate e = est.estimate(3.50f, 0);  // a cut-off above where the cell is
    CHECK(e.state == battest::Estimate::READY && e.hoursLeft == 0 && !e.unbounded);
  }

  // reset forgets everything
  {
    battest::Estimator est;
    Cell cell;
    discharge(est, cell, 90, 3.0, 2.0, 0);
    CHECK(est.estimate(cutoff, 0).state == battest::Estimate::READY);
    est.reset();
    CHECK(est.estimate(cutoff, 0).state == battest::Estimate::OFF && est.binCount() == 0);
    est.add(99999, 3.9f);
    CHECK(est.estimate(cutoff, 0).state == battest::Estimate::LEARNING);
  }

  // a reset in the middle of a busy bin must not leave its time offsets behind: the first bin after it
  // holds one reading and would be stamped hours into the future.  The line fit throws that point out, so
  // the slope and the level still come out right; what it spoils is the span and the window, and with them
  // when the first figure appears.  A fresh estimator is the reference, compared on everything the estimate
  // reports, every minute of the run.
  {
    battest::Estimator used, fresh;
    for (uint32_t t = 560; t < 600; t++) used.add(t, 3.95f);  // 40 readings late in a bin
    used.reset();
    Cell c1, c2;
    const uint32_t t0 = 599;
    int compared = 0, differing = 0, ready = 0;
    for (uint32_t t = t0; t < t0 + 3 * 3600; t += 5) {
      const double pct = 90.0 - 3.0 * (double)(t - t0) / 3600.0;
      used.add(t, (float)c1.read(pct));
      fresh.add(t, (float)c2.read(pct));
      if ((t - t0) % 60 != 0) continue;
      const battest::Estimate a = used.estimate(cutoff, 2000), b = fresh.estimate(cutoff, 2000);
      compared++;
      if (b.state == battest::Estimate::READY) ready++;
      if (a.state != b.state || a.learnMin != b.learnMin || a.windowMin != b.windowMin || a.unbounded != b.unbounded ||
          fabs(a.pctPerHour - b.pctPerHour) > 1e-6 || fabs(a.levelPct - b.levelPct) > 1e-6 ||
          fabs(a.hoursLeft - b.hoursLeft) > 1e-6 || fabs(a.avgMa - b.avgMa) > 1e-6)
        differing++;
    }
    if (differing != 0) printf("  after a reset: %d of %d estimates differ from a fresh estimator\n", differing, compared);
    CHECK(compared == 180 && ready >= 100);
    CHECK(differing == 0);
    CHECK(used.binCount() == fresh.binCount());
  }

  // a charger appearing without being announced: the readings jump up, so start over
  {
    battest::Estimator est;
    Cell cell;
    uint32_t t = discharge(est, cell, 90, 3.0, 2.0, 0);
    CHECK(est.estimate(cutoff, 0).state == battest::Estimate::READY);
    for (uint32_t k = 0; k < 20 * 60; k += 5) est.add(t + k, 4.15f);  // charging: far above the discharge line
    const battest::Estimate e = est.estimate(cutoff, 0);
    CHECK(e.state == battest::Estimate::LEARNING);
    CHECK(est.binCount() <= 4);
  }

  // the time base going backwards (a reboot of the counter) restarts the estimate
  {
    battest::Estimator est;
    Cell cell;
    discharge(est, cell, 90, 3.0, 2.0, 5000);
    est.add(100, 3.9f);
    CHECK(est.estimate(cutoff, 0).state == battest::Estimate::LEARNING);
    CHECK(est.binCount() == 0);
  }

  // the first minutes after leaving the charger fall faster than the cell drains: ignored
  {
    battest::Estimator est;
    Cell cell;
    for (uint32_t sec = 0; sec < 4 * 3600; sec += 5) {
      const double pct = 90 - 3.0 * sec / 3600.0;
      double v = cell.read(pct);
      if (sec < 600) v += 0.04 * exp(-(double)sec / 200.0);  // settling after the charger
      est.add(sec, (float)v);
    }
    const battest::Estimate e = est.estimate(cutoff, 0);
    CHECK(e.state == battest::Estimate::READY);
    if (fabs(e.pctPerHour - 3.0) > 0.15) printf("  settling: %.2f %%/h\n", e.pctPerHour);
    CHECK_NEAR(e.pctPerHour, 3.0, 0.15);
  }

  // the constants in use
  const battest::Params p;
  CHECK(p.binSec == 300 && p.skipSec == 900 && p.readySec == 1800);
  CHECK(p.minWindowSec == 7200 && p.maxWindowSec == 28800);
  CHECK(p.maxWindowSec <= (uint32_t)battest::kMaxBins * p.binSec);  // the bins cover the longest window
}

void testLowBattery() {
  section("low battery guard");
  using lowbat::BootAction;
  // a clear sag, sustained: shuts down after a minute of readings
  {
    lowbat::Guard g;
    g.configure(true, 3.30f);
    CHECK_NEAR(g.cutoffV(), 3.30, 1e-6);
    for (uint32_t t = 0; t < 60; t += 5) CHECK(!g.feed(t, 3.28f, true));  // 12 readings, but not yet a minute
    CHECK(g.tripping());
    CHECK(g.feed(60, 3.28f, true));
    CHECK(g.feed(65, 3.29f, true));  // and it stays tripped
  }
  // sparse readings (the clock was busy): time alone is not enough, three readings are needed
  {
    lowbat::Guard g;
    g.configure(true, 3.30f);
    CHECK(!g.feed(0, 3.20f, true));
    CHECK(!g.feed(100, 3.20f, true));  // a minute and a half later, but only the second reading
    CHECK(g.feed(110, 3.20f, true));
  }
  // one dip is nothing
  {
    lowbat::Guard g;
    g.configure(true, 3.30f);
    uint32_t t = 0;
    for (int i = 0; i < 100; i++, t += 5) {
      const float v = (i % 20 == 7) ? 3.10f : 3.55f;
      CHECK(!g.feed(t, v, true));
    }
    CHECK(!g.tripping());
  }
  // dips every 30 s, each followed by a good reading: never trips
  {
    lowbat::Guard g;
    g.configure(true, 3.30f);
    uint32_t t = 0;
    for (int i = 0; i < 200; i++, t += 5) CHECK(!g.feed(t, (i % 6 == 0) ? 3.20f : 3.40f, true));
  }
  // hovering within the hysteresis band keeps the countdown running
  {
    lowbat::Guard g;
    g.configure(true, 3.30f);
    CHECK(!g.feed(0, 3.29f, true));
    CHECK(!g.feed(5, 3.31f, true));   // above the cut-off but not by the margin: neither trips nor cancels
    CHECK(g.tripping());
    CHECK(!g.feed(10, 3.29f, true));
    CHECK(g.feed(65, 3.29f, true));
    CHECK(!g.feed(70, 3.33f, true));  // clearly above: cancelled
    CHECK(!g.tripping());
    CHECK(!g.feed(75, 3.29f, true));  // and a fresh minute is needed
  }
  // charging or USB: the guard stands down
  {
    lowbat::Guard g;
    g.configure(true, 3.30f);
    for (uint32_t t = 0; t < 120; t += 5) g.feed(t, 3.28f, true);
    CHECK(g.tripping());
    CHECK(!g.feed(125, 3.28f, false));
    CHECK(!g.tripping());
    CHECK(!g.feed(130, 3.28f, true));  // restarts the minute
  }
  // switched off, and clamped to the limits the setting allows
  {
    lowbat::Guard g;
    g.configure(false, 3.30f);
    for (uint32_t t = 0; t < 300; t += 5) CHECK(!g.feed(t, 3.0f, true));
    g.configure(true, 2.0f);
    CHECK_NEAR(g.cutoffV(), lowbat::kMinCutoffV, 1e-6);
    g.configure(true, 5.0f);
    CHECK_NEAR(g.cutoffV(), lowbat::kMaxCutoffV, 1e-6);
    g.configure(true, 3.30f);
    CHECK(!g.feed(0, 0.0f, true));  // no reading is no verdict
    CHECK(!g.feed(5, NAN, true));
    CHECK(!g.tripping());
  }

  // what to do at power-up
  CHECK(lowbat::bootDecision(true, 3.90f, 3.30f, false) == BootAction::RUN);
  CHECK(lowbat::bootDecision(true, 3.29f, 3.30f, false) == BootAction::STAY_ASLEEP);  // flat on a cold start
  CHECK(lowbat::bootDecision(true, 3.30f, 3.30f, false) == BootAction::RUN);
  // after a shutdown the cell has to come back well above the cut-off
  CHECK(lowbat::bootDecision(true, 3.29f, 3.30f, true) == BootAction::STAY_ASLEEP);
  CHECK(lowbat::bootDecision(true, 3.45f, 3.30f, true) == BootAction::STAY_ASLEEP);  // rebounded, not charged
  CHECK(lowbat::bootDecision(true, 3.69f, 3.30f, true) == BootAction::STAY_ASLEEP);
  CHECK(lowbat::bootDecision(true, 3.70f, 3.30f, true) == BootAction::RUN);          // charging got it there
  CHECK(lowbat::bootDecision(true, 4.10f, 3.30f, true) == BootAction::RUN);
  // switched off, or no reading
  CHECK(lowbat::bootDecision(false, 3.0f, 3.30f, true) == BootAction::RUN);
  CHECK(lowbat::bootDecision(true, 0.0f, 3.30f, true) == BootAction::RUN);
  CHECK(lowbat::bootDecision(true, NAN, 3.30f, false) == BootAction::RUN);
  // the restart level sits above anything the rebound of a just-unloaded cell can reach
  CHECK(lowbat::kRestartV >= lowbat::kMaxCutoffV + 0.05f);
}

// ===========================================================================
// Clock drift
// ===========================================================================
void testDrift() {
  section("clock drift");
  const int64_t kHour = 3600LL * 1000000;
  // the clock runs `ppm` fast (positive) or slow; syncs every `everySec` seconds, each with
  // `jitterMs` of network error, from `startMono`
  auto run = [&](double ppm, double jitterMs, int hours, int everySec, uint32_t seed, drift::Tracker *tr) {
    uint32_t rng = seed;
    auto noise = [&]() {
      rng = rng * 1664525u + 1013904223u;
      return (((rng >> 8) & 0xFFFFFF) / 16777216.0 - 0.5) * 2 * jitterMs;  // uniform +-jitter
    };
    const int64_t mono0 = 12345678;
    const int64_t sys0 = 1760000000LL * 1000000;
    for (int64_t s = 0; s <= (int64_t)hours * 3600; s += everySec) {
      const int64_t mono = mono0 + s * 1000000;
      // true time = sys0 + s; the crystal-driven timer reads s*(1 - ppm*1e-6) of it... a fast clock gains: its
      // timer advances more than true time
      const double trueElapsedUs = (double)s * 1e6 / (1.0 + ppm * 1e-6);
      const int64_t sysNtp = sys0 + (int64_t)(trueElapsedUs + noise() * 1000.0);
      tr->addSync(mono, sysNtp);
    }
  };

  for (double ppm : {0.0, 2.0, -2.0, 10.0, -10.0, 25.0}) {
    drift::Tracker tr;
    run(ppm, 30, 24, 3600, 99, &tr);
    const drift::Result r = tr.result();
    CHECK(r.valid && r.points >= 20);
    if (fabs(r.ppm - ppm) > 1.0) printf("  true %+.1f ppm, measured %+.2f +- %.2f\n", ppm, r.ppm, r.errorPpm);
    CHECK_NEAR(r.ppm, ppm, 1.0);
    CHECK_NEAR(r.secPerDay, ppm * 0.0864, 0.1);
    CHECK(r.errorPpm > 0 && r.errorPpm < 1.5);
    CHECK(r.spanHours > 22 && r.spanHours < 25);
  }
  // a longer baseline is tighter
  {
    drift::Tracker a, b;
    run(5.0, 40, 6, 3600, 7, &a);
    run(5.0, 40, 40, 3600, 7, &b);
    CHECK(a.result().valid && b.result().valid);
    CHECK(b.result().errorPpm < a.result().errorPpm);
    CHECK_NEAR(b.result().ppm, 5.0, 0.6);
  }
  // not enough history: no figure
  {
    drift::Tracker tr;
    CHECK(!tr.result().valid);
    run(5.0, 20, 2, 3600, 3, &tr);  // 3 points over 2 hours
    CHECK(!tr.result().valid);
    drift::Tracker t2;
    run(5.0, 20, 3, 3600, 3, &t2);  // 4 points over 3 hours
    CHECK(t2.result().valid);
  }
  // an outlier sync (a bad network path, 400 ms off) does not move the figure much, wherever it is
  for (int badHour : {0, 1, 11, 23, 24}) {
    drift::Tracker tr;
    const int64_t mono0 = 1000000;
    const int64_t sys0 = 1760000000LL * 1000000;
    for (int h = 0; h <= 24; h++) {
      int64_t sys = sys0 + (int64_t)h * kHour;
      if (h == badHour) sys += 400000;  // 400 ms late
      tr.addSync(mono0 + (int64_t)h * kHour, sys);
    }
    const drift::Result r = tr.result();
    CHECK(r.valid);
    if (fabs(r.ppm) > 0.5) printf("  outlier at hour %d: %+.2f ppm\n", badHour, r.ppm);
    CHECK_NEAR(r.ppm, 0.0, 0.5);
  }
  // four points are not enough if they are close together
  {
    drift::Tracker tr;
    for (int i = 0; i < 4; i++) tr.addSync(1000000LL + i * 30LL * 60 * 1000000, 1760000000LL * 1000000 + i * 30LL * 60 * 1000000);
    CHECK(tr.points() == 4 && !tr.result().valid);  // an hour and a half of history
  }
  // syncs closer than half an hour are not new information
  {
    drift::Tracker tr;
    for (int i = 0; i < 100; i++) tr.addSync(1000000LL + i * 60LL * 1000000, 1760000000LL * 1000000 + i * 60LL * 1000000);
    CHECK(tr.points() == 4);  // 0, 30, 60, 90 minutes
  }
  // the timer restarting (a reboot) forgets the old points; the ring keeps the newest
  {
    drift::Tracker tr;
    run(3.0, 10, 10, 3600, 5, &tr);
    CHECK(tr.points() == 11);
    tr.addSync(500, 1760000000LL * 1000000);  // the timer is back near zero
    CHECK(tr.points() == 1 && !tr.result().valid);
    drift::Tracker many;
    run(3.0, 10, 200, 3600, 5, &many);
    CHECK(many.points() == drift::kMaxPoints);
    CHECK_NEAR(many.result().ppm, 3.0, 0.5);
  }
}

// ===========================================================================
// Backup WiFi
// ===========================================================================
void testBackupWifiSettings() {
  section("backup WiFi: settings");
  // the names people will try for the backup network and its password
  const char *ssidNames[] = {"wifi_backup_ssid", "WiFi backup SSID", "Wi-Fi-Backup-SSID", "backup_ssid", "Backup SSID",
                             "backup-wifi", "backup_network", "ssid2", "SSID 2", "wifi_ssid2", "second_ssid",
                             "fallback_ssid", "alt_ssid"};
  for (const char *k : ssidNames) {
    const Applied a = apply(std::string(k) + " = Phone Hotspot");
    if (strcmp(a.s.wifiBackupSsid, "Phone Hotspot") != 0) printf("  [%s] gave [%s]\n", k, a.s.wifiBackupSsid);
    CHECK_STR(a.s.wifiBackupSsid, "Phone Hotspot");
    CHECK_STR(a.s.wifiSsid, "");  // the main network is left alone
    CHECK(a.r.problems() == 0 && a.r.changed == 1);
    CHECK(settingIsUserSet(a.s, idx("wifi_backup_ssid")) && !settingIsUserSet(a.s, idx("wifi_ssid")));
  }
  const char *passNames[] = {"wifi_backup_password", "WiFi backup password", "backup_password", "backup_pass",
                             "backup_wifi_password", "password2", "pass2", "wifi_password2", "second_password",
                             "fallback_password", "alt_password", "backup_key"};
  for (const char *k : passNames) {
    const Applied a = apply(std::string(k) + " = \"h0tsp0t pw\"");
    if (strcmp(a.s.wifiBackupPassword, "h0tsp0t pw") != 0) printf("  [%s] gave [%s]\n", k, a.s.wifiBackupPassword);
    CHECK_STR(a.s.wifiBackupPassword, "h0tsp0t pw");
    CHECK_STR(a.s.wifiPassword, "");
    CHECK(a.r.problems() == 0 && a.r.changed == 1);
  }
  // the plain names still mean the main network
  const Applied both = apply("ssid = Home\npassword = homepw\nbackup_ssid = Phone\nbackup_password = phonepw\n");
  CHECK_STR(both.s.wifiSsid, "Home");
  CHECK_STR(both.s.wifiPassword, "homepw");
  CHECK_STR(both.s.wifiBackupSsid, "Phone");
  CHECK_STR(both.s.wifiBackupPassword, "phonepw");
  CHECK(both.s.hasMainWifi() && both.s.hasBackupWifi());

  // quoting works as for the main network
  CHECK_STR(apply("wifi_backup_ssid = \"Bob's Phone\"").s.wifiBackupSsid, "Bob's Phone");
  CHECK_STR(apply("wifi_backup_ssid = Home#1").s.wifiBackupSsid, "Home#1");
  CHECK_STR(apply("wifi_backup_ssid = Phone # my hotspot").s.wifiBackupSsid, "Phone");

  // limits: 32 characters for a name, 64 for a password
  CHECK(apply("wifi_backup_ssid = " + std::string(32, 'x')).r.bad == 0);
  CHECK(apply("wifi_backup_ssid = " + std::string(33, 'x')).r.bad == 1);
  CHECK(apply("wifi_backup_password = " + std::string(64, 'x')).r.bad == 0);
  CHECK(apply("wifi_backup_password = " + std::string(65, 'x')).r.bad == 1);

  // forgetting the backup, and an open backup network
  Settings has;
  snprintf(has.wifiBackupSsid, sizeof has.wifiBackupSsid, "Phone");
  snprintf(has.wifiBackupPassword, sizeof has.wifiBackupPassword, "pw");
  has.userSet |= (1u << idx("wifi_backup_ssid")) | (1u << idx("wifi_backup_password"));
  const Applied gone = apply("wifi_backup_ssid =\nwifi_backup_password =", has);
  CHECK_STR(gone.s.wifiBackupSsid, "");
  CHECK_STR(gone.s.wifiBackupPassword, "");
  CHECK(gone.r.cleared == 2 && !gone.s.hasBackupWifi());
  const Applied openNet = apply("wifi_backup_password = \"\"", has);
  CHECK_STR(openNet.s.wifiBackupPassword, "");
  CHECK_STR(openNet.s.wifiBackupSsid, "Phone");
  CHECK(settingIsUserSet(openNet.s, idx("wifi_backup_password")));

  // what counts as having a backup
  Settings s;
  CHECK(!s.hasMainWifi() && !s.hasBackupWifi());
  snprintf(s.wifiBackupSsid, sizeof s.wifiBackupSsid, "Phone");
  CHECK(!s.hasMainWifi() && s.hasBackupWifi());  // a backup alone is still a network to join
  snprintf(s.wifiSsid, sizeof s.wifiSsid, "Home");
  CHECK(s.hasMainWifi() && s.hasBackupWifi());
  snprintf(s.wifiBackupSsid, sizeof s.wifiBackupSsid, "Home");
  CHECK(!s.hasBackupWifi());  // the same network twice is no backup
  snprintf(s.wifiBackupSsid, sizeof s.wifiBackupSsid, "home");
  CHECK(s.hasBackupWifi());  // names are case sensitive

  // flash round trip
  Settings t;
  CHECK(applyStoredSetting(t, idx("wifi_backup_ssid"), "Phone Hotspot"));
  CHECK(applyStoredSetting(t, idx("wifi_backup_password"), "h0tsp0t pw"));
  CHECK_STR(t.wifiBackupSsid, "Phone Hotspot");
  CHECK_STR(t.wifiBackupPassword, "h0tsp0t pw");
  CHECK(fmt(t, idx("wifi_backup_ssid")) == "Phone Hotspot" && fmt(t, idx("wifi_backup_password")) == "h0tsp0t pw");

  // the password stays out of the example file and of the summary lines; the name may be shown
  Settings withBoth = both.s;
  snprintf(withBoth.wifiBackupPassword, sizeof withBoth.wifiBackupPassword, "backup-pw-123");
  static char buf[16384];
  const size_t n = renderExampleConfig(withBoth, "", buf, sizeof buf);
  CHECK(n > 0 && strstr(buf, "backup-pw-123") == nullptr && strstr(buf, "homepw") == nullptr);
  CHECK(strstr(buf, "# wifi_backup_password = \"your password\"") != nullptr);
  CHECK(strstr(buf, "# wifi_backup_ssid = \"Phone\"") != nullptr);
}

// ===========================================================================
// Battery capacity and voltage calibration
// ===========================================================================
void testBatteryCalibrationSetting() {
  section("battery calibration and capacity settings");
  const Settings d;
  CHECK_NEAR(d.batteryCalibration, 1.0, 1e-9);  // no correction unless asked for
  CHECK(d.batteryCapacityMah == 0);
  CHECK(!settingIsSecret(idx("battery_calibration")));
  CHECK_STR(settingNvsKey(idx("battery_calibration")), "batcal");

  // the names people will try
  const char *names[] = {"battery_calibration", "Battery Calibration", "Battery-Calibration", "battery_cal", "adc_calibration",
                         "ADC calibration", "voltage_calibration", "battery_scale", "battery_gain", "adc_scale"};
  for (const char *k : names) {
    const Applied a = apply(std::string(k) + " = 1.016");
    if (fabs(a.s.batteryCalibration - 1.016) > 1e-6) printf("  [%s] gave %g\n", k, (double)a.s.batteryCalibration);
    CHECK_NEAR(a.s.batteryCalibration, 1.016, 1e-6);
    CHECK(a.r.problems() == 0 && a.r.changed == 1);
    CHECK(settingIsUserSet(a.s, idx("battery_calibration")));
  }
  // spellings of the number: a decimal comma, four decimals
  CHECK_NEAR(apply("battery_calibration = 1,016").s.batteryCalibration, 1.016, 1e-6);
  CHECK_NEAR(apply("battery_calibration = \"1.0157\"").s.batteryCalibration, 1.0157, 1e-6);
  CHECK_NEAR(apply("battery_calibration = 0.9847").s.batteryCalibration, 0.9847, 1e-6);
  // the range is 0.80 to 1.25, inclusive; anything else is a mistake and changes nothing
  CHECK_NEAR(apply("battery_calibration = 0.8").s.batteryCalibration, 0.8, 1e-6);
  CHECK_NEAR(apply("battery_calibration = 1.25").s.batteryCalibration, 1.25, 1e-6);
  for (const char *bad : {"0.79", "1.26", "0", "-1", "2", "abc", "1.0.1"}) {
    const Applied a = apply(std::string("battery_calibration = ") + bad);
    if (a.r.bad != 1) printf("  [%s] was not refused\n", bad);
    CHECK(a.r.bad == 1 && a.r.changed == 0);
    CHECK_NEAR(a.s.batteryCalibration, 1.0, 1e-9);
  }
  // forgetting it brings the default back (a build can have its own: here the pristine one)
  Settings set;
  set.batteryCalibration = 1.03f;
  set.userSet |= 1u << idx("battery_calibration");
  const Applied gone = apply("battery_calibration =", set);
  CHECK_NEAR(gone.s.batteryCalibration, 1.0, 1e-9);
  CHECK(gone.r.cleared == 1 && !settingIsUserSet(gone.s, idx("battery_calibration")));
  Settings withDefault;
  withDefault.batteryCalibration = 1.02f;  // config.h or secrets.h said so
  Settings fromFlash = withDefault;
  const std::string text = "battery_calibration = 1.05\n";
  Settings defaults = withDefault;
  applyConfigText(fromFlash, defaults, text.data(), text.size());
  CHECK_NEAR(fromFlash.batteryCalibration, 1.05, 1e-6);
  const std::string forget = "battery_calibration =\n";
  applyConfigText(fromFlash, defaults, forget.data(), forget.size());
  CHECK_NEAR(fromFlash.batteryCalibration, 1.02, 1e-6);  // back to the build's own value, not to 1

  // the figure survives flash (it is stored as text) with its four decimals
  Settings w;
  w.batteryCalibration = 1.0157f;
  Settings x;
  CHECK(fmt(w, idx("battery_calibration")) == "1.0157");
  CHECK(applyStoredSetting(x, idx("battery_calibration"), fmt(w, idx("battery_calibration")).c_str()));
  CHECK_NEAR(x.batteryCalibration, 1.0157, 1e-6);
  CHECK(fmt(Settings(), idx("battery_calibration")) == "1");

  // the capacity: more names, and a unit
  CHECK(apply("battery_capacity = 2500").s.batteryCapacityMah == 2500);
  CHECK(apply("capacity_mah = 3400").s.batteryCapacityMah == 3400);
  CHECK(apply("Battery Capacity mAh = \"2600 mAh\"").s.batteryCapacityMah == 2600);

  // the example file explains the calibration and names the serial command
  static char buf[16384];
  const size_t n = renderExampleConfig(Settings(), "", buf, sizeof buf);
  CHECK(n > 0);
  CHECK(strstr(buf, "# battery_calibration = 1\n") != nullptr);
  CHECK(strstr(buf, "batcal 4.20") != nullptr);
  CHECK(strstr(buf, "# battery_capacity_mah = 0\n") != nullptr);
}

void testWifiPick() {
  section("backup WiFi: which network next");
  using namespace wifipick;
  const uint32_t kMin = 60UL * 1000UL;

  {  // no names, one name
    Picker none;
    none.configure(false, false);
    CHECK(none.nextAttempt() == NET_NONE && none.nextAttempt() == NET_NONE);
    Picker onlyMain;
    onlyMain.configure(true, false);
    for (int i = 0; i < 6; i++) CHECK(onlyMain.nextAttempt() == NET_MAIN);
    onlyMain.joined(NET_MAIN, 0);
    CHECK(!onlyMain.timeToLookForMain(100 * 60 * kMin) && !onlyMain.onBackup());
    Picker onlyBackup;
    onlyBackup.configure(false, true);
    for (int i = 0; i < 6; i++) CHECK(onlyBackup.nextAttempt() == NET_BACKUP);
    onlyBackup.joined(NET_BACKUP, 0);
    CHECK(onlyBackup.onBackup() && !onlyBackup.timeToLookForMain(100 * 60 * kMin));  // nothing to go back to
  }

  {  // both: main first, then they alternate
    Picker p;
    p.configure(true, true);
    const Net want[] = {NET_MAIN, NET_BACKUP, NET_MAIN, NET_BACKUP, NET_MAIN};
    for (Net w : want) CHECK(p.nextAttempt() == w);
  }

  {  // after a connection is lost the main network is tried first again, whatever was tried last
    Picker p;
    p.configure(true, true);
    CHECK(p.nextAttempt() == NET_MAIN);
    p.joined(NET_MAIN, 1000);
    p.lost();
    CHECK(p.nextAttempt() == NET_MAIN);  // not the backup, although the last attempt was the main one
    CHECK(p.nextAttempt() == NET_BACKUP);
    p.joined(NET_BACKUP, 5000);
    CHECK(p.onBackup() && p.current() == NET_BACKUP);
    p.lost();
    CHECK(p.current() == NET_NONE && !p.onBackup());
    CHECK(p.nextAttempt() == NET_MAIN);
  }

  {  // looking for the main network from the backup: not before 10 minutes, then every 10
    Picker p;
    p.configure(true, true);
    const uint32_t t0 = 123456;
    p.joined(NET_BACKUP, t0);
    CHECK(!p.timeToLookForMain(t0) && !p.timeToLookForMain(t0 + 10 * kMin - 1));
    CHECK(p.timeToLookForMain(t0 + 10 * kMin) && p.timeToLookForMain(t0 + 3 * 60 * kMin));
    p.lookedForMain(t0 + 10 * kMin, false);  // it was not there
    CHECK(!p.timeToLookForMain(t0 + 10 * kMin + 1) && !p.timeToLookForMain(t0 + 20 * kMin - 1));
    CHECK(p.timeToLookForMain(t0 + 20 * kMin));
    p.joined(NET_MAIN, t0 + 30 * kMin);
    CHECK(!p.timeToLookForMain(t0 + 300 * kMin));  // on the main network there is nothing to look for
    Picker q;
    q.configure(true, true);
    q.joined(NET_BACKUP, 0);
    q.lost();
    CHECK(!q.timeToLookForMain(100 * 60 * kMin));  // not connected: no looking either
  }

  {  // the millisecond counter wraps after 49 days: the timing must not care
    Picker p;
    p.configure(true, true);
    const uint32_t t0 = 0xFFFFFFFFu - 3 * kMin;
    p.joined(NET_BACKUP, t0);
    // the deadline wrapped past zero; the clock itself has not yet (for the first 3 minutes), and a plain
    // "now >= deadline" would call that moment late already
    CHECK(!p.timeToLookForMain(t0) && !p.timeToLookForMain(t0 + 1 * kMin) && !p.timeToLookForMain(t0 + 2 * kMin));
    CHECK(!p.timeToLookForMain(t0 + 5 * kMin));  // after the wrap, before the deadline
    CHECK(!p.timeToLookForMain(t0 + 10 * kMin - 1));
    CHECK(p.timeToLookForMain(t0 + 10 * kMin));
  }

  {  // going back fails again and again: the looks come further apart (20, 40, 60, 60 minutes)
    Picker p;
    p.configure(true, true);
    uint32_t now = 1000;
    p.joined(NET_BACKUP, now);
    CHECK(p.lookInterval() == 10 * kMin);
    const uint32_t gaps[] = {20 * kMin, 40 * kMin, 60 * kMin, 60 * kMin, 60 * kMin};
    for (uint32_t gap : gaps) {
      now += p.lookInterval();
      CHECK(p.timeToLookForMain(now));
      p.lookedForMain(now, true);  // the main network is in range: leave
      p.lost();
      CHECK(p.nextAttempt() == NET_MAIN);   // ... but it cannot be joined
      CHECK(p.nextAttempt() == NET_BACKUP);  // so back to the backup
      now += 40 * 1000;
      p.joined(NET_BACKUP, now);
      CHECK(p.lookInterval() == gap);
      CHECK(!p.timeToLookForMain(now + gap - 1) && p.timeToLookForMain(now + gap));
    }
    // the main network is joined at last: the next stay on the backup starts over at 10 minutes
    now += p.lookInterval();
    p.lookedForMain(now, true);
    p.lost();
    CHECK(p.nextAttempt() == NET_MAIN);
    p.joined(NET_MAIN, now + 5000);
    CHECK(p.lookInterval() == 10 * kMin);
    p.lost();
    p.joined(NET_BACKUP, now + 20 * kMin);
    CHECK(p.lookInterval() == 10 * kMin && p.timeToLookForMain(now + 30 * kMin));
  }

  {  // a backup that drops for its own reasons is not a failed return
    Picker p;
    p.configure(true, true);
    p.joined(NET_BACKUP, 0);
    p.lost();
    CHECK(p.nextAttempt() == NET_MAIN && p.nextAttempt() == NET_BACKUP);
    p.joined(NET_BACKUP, 1000);
    CHECK(p.lookInterval() == 10 * kMin);
    // looking and finding nothing does not count either
    p.lookedForMain(10 * kMin, false);
    p.lookedForMain(20 * kMin, false);
    CHECK(p.lookInterval() == 10 * kMin);
  }

  // which of the two a connection is, by the name the stack reports
  CHECK(classify("Home", "Home", "Phone") == NET_MAIN);
  CHECK(classify("Phone", "Home", "Phone") == NET_BACKUP);
  CHECK(classify("Elsewhere", "Home", "Phone") == NET_MAIN);   // a name we do not know: the main one
  CHECK(classify("Phone", "", "Phone") == NET_BACKUP);          // only a backup is set
  CHECK(classify("Elsewhere", "", "Phone") == NET_BACKUP);
  CHECK(classify("Home", "Home", "") == NET_MAIN);
  CHECK(classify("", "Home", "Phone") == NET_MAIN);
  CHECK(classify("home", "Home", "Phone") == NET_MAIN && classify("PHONE", "Home", "Phone") == NET_MAIN);  // case matters
}

// A day in the life of the clock: a model of the radio around the Picker, one second at a time.  An
// attempt takes 20 s to give up and 3 s to succeed, as in net_task.cpp.
void testWifiPickDay() {
  section("backup WiFi: a simulated day");
  using namespace wifipick;
  struct Sim {
    Picker pick;
    bool mainUp = true, backupUp = true;
    Net on = NET_NONE;  // connected to
    uint32_t attemptAt = 0, joinAt = 0;
    Net trying = NET_NONE;
    int scans = 0, scansOnMain = 0, switches = 0;
    uint32_t joinedMain = 0;

    void step(uint32_t nowMs) {
      if (on != NET_NONE && ((on == NET_MAIN && !mainUp) || (on == NET_BACKUP && !backupUp))) {
        on = NET_NONE;
        pick.lost();
        attemptAt = nowMs ? nowMs : 1;  // the stack's own reconnect gets 20 s, as in the sketch
        trying = NET_NONE;
      }
      if (on == NET_NONE) {
        if (trying != NET_NONE && nowMs >= joinAt) {
          on = trying;
          trying = NET_NONE;
          pick.joined(on, nowMs);
          if (on == NET_MAIN) joinedMain = nowMs;
        } else if (attemptAt == 0 || nowMs - attemptAt > 20000) {
          trying = pick.nextAttempt();
          const bool up = trying == NET_MAIN ? mainUp : backupUp;
          attemptAt = nowMs ? nowMs : 1;
          joinAt = up ? nowMs + 3000 : 0xFFFFFFFFu;
          if (!up) trying = NET_NONE;
        }
        return;
      }
      if (pick.timeToLookForMain(nowMs)) {
        scans++;
        const bool seen = mainUp;
        pick.lookedForMain(nowMs, seen);
        if (seen) {
          switches++;
          on = NET_NONE;
          pick.lost();
          attemptAt = 0;  // start on the main network at once
          trying = NET_NONE;
        }
      } else if (on == NET_MAIN) {
        // nothing: the clock sits on the main network
      }
    }
  };

  Sim sim;
  sim.pick.configure(true, true);
  sim.mainUp = false;  // away from home: only the phone is there
  uint32_t t = 1000;
  uint32_t onBackupAt = 0;
  for (; t < 2 * 3600 * 1000UL; t += 1000) {
    sim.step(t);
    if (sim.on == NET_BACKUP && !onBackupAt) onBackupAt = t;
  }
  CHECK(onBackupAt > 0 && onBackupAt <= 1000 + 20000 + 3000 + 1000);  // main fails (20 s), the backup works (3 s)
  CHECK(sim.on == NET_BACKUP);
  CHECK(sim.scans >= 11 && sim.scans <= 12);  // a look every 10 minutes, nothing found

  const uint32_t homeAt = t;  // the main network is up again
  sim.mainUp = true;
  for (; t < homeAt + 20 * 60 * 1000UL; t += 1000) sim.step(t);
  CHECK(sim.on == NET_MAIN);
  CHECK(sim.joinedMain > homeAt && sim.joinedMain <= homeAt + 10 * 60 * 1000UL + 5000);  // back within one look interval
  CHECK(sim.switches == 1);

  const int scansBefore = sim.scans;
  for (; t < homeAt + 3 * 3600 * 1000UL; t += 1000) sim.step(t);
  CHECK(sim.scans == scansBefore && sim.on == NET_MAIN);  // no scanning while on the main network

  // the router dies: the clock ends up on the backup within a minute, and comes back when the router does
  sim.mainUp = false;
  const uint32_t deadAt = t;
  uint32_t backAt = 0;
  for (; t < deadAt + 3 * 60 * 1000UL; t += 1000) {
    sim.step(t);
    if (sim.on == NET_BACKUP && !backAt) backAt = t;
  }
  CHECK(backAt > deadAt && backAt <= deadAt + 20000 + 20000 + 3000 + 2000);  // head start, main (20 s), backup
  sim.mainUp = true;
  const uint32_t routerAt = t;
  for (; t < routerAt + 15 * 60 * 1000UL; t += 1000) sim.step(t);
  CHECK(sim.on == NET_MAIN);

  // both gone, then the backup alone comes back: the clock finds it
  sim.mainUp = false;
  sim.backupUp = false;
  for (int i = 0; i < 600; i++, t += 1000) sim.step(t);
  CHECK(sim.on == NET_NONE);
  sim.backupUp = true;
  for (int i = 0; i < 90; i++, t += 1000) sim.step(t);
  CHECK(sim.on == NET_BACKUP);
}

// ===========================================================================
// Firmware update from the SD card
// ===========================================================================
void testFirmwareLogic() {
  section("firmware update: which file is accepted");
  using namespace fwlogic;

  // the real thing: the first bytes of an exported build of this sketch, and of the bootloader the IDE exports with it
  const std::string app = readFile("fixtures/esp32s3_app_head.bin");
  const std::string boot = readFile("fixtures/esp32s3_bootloader_head.bin");
  CHECK(app.size() == kHeadBytes && boot.size() == kHeadBytes);
  const uint8_t *appHead = (const uint8_t *)app.data(), *bootHead = (const uint8_t *)boot.data();
  const uint32_t kRealSize = 1467536;  // that build's .ino.bin
  const uint32_t kSlot = 0x300000;     // an app slot of the README's partition scheme
  CHECK(inspectImage(appHead, app.size(), kRealSize, kSlot) == Problem::NONE);
  CHECK(elfHash(appHead)[0] == 0xb0 && elfHash(appHead)[1] == 0x1e && elfHash(appHead)[2] == 0x2f && elfHash(appHead)[3] == 0x9c);
  CHECK(le32(appHead + 32) == kAppDescMagic);  // (the offsets above are the real ones)
  // the other files the IDE exports next to it are refused, each for its own reason
  CHECK(inspectImage(bootHead, boot.size(), 19984, kSlot) == Problem::TOO_SMALL);       // ...bootloader.bin
  CHECK(inspectImage(bootHead, boot.size(), 3072, kSlot) == Problem::TOO_SMALL);        // ...partitions.bin
  CHECK(inspectImage(bootHead, boot.size(), 16777216, kSlot) == Problem::TOO_BIG);      // ...merged.bin: 16 MB, starts like a bootloader
  CHECK(inspectImage(bootHead, boot.size(), 400 * 1024, kSlot) == Problem::NOT_AN_APP);  // a bootloader of a plausible size

  // every header field the check reads, changed in turn on a copy of the real header
  auto with = [&](size_t at, int value) {
    std::string h = app;
    h[at] = (char)value;
    return h;
  };
  auto problemIn = [&](const std::string &h, uint32_t size, uint32_t slot) {
    return inspectImage((const uint8_t *)h.data(), h.size(), size, slot);
  };
  auto problemOf = [&](const std::string &h, uint32_t size) { return problemIn(h, size, kSlot); };
  CHECK(problemOf(with(0, 0x00), kRealSize) == Problem::NOT_AN_IMAGE);
  CHECK(problemOf(with(0, 0xAA), kRealSize) == Problem::NOT_AN_IMAGE);  // a partition table
  for (int chip : {0, 2, 5, 12, 13, 18}) CHECK(problemOf(with(12, chip), kRealSize) == Problem::WRONG_CHIP);  // other ESP32s
  CHECK(problemOf(with(13, 1), kRealSize) == Problem::WRONG_CHIP);  // 0x0109 is not 9 either
  CHECK(problemOf(with(32, 0x00), kRealSize) == Problem::NOT_AN_APP);
  CHECK(problemOf(with(35, 0x00), kRealSize) == Problem::NOT_AN_APP);  // the last byte of the magic word counts too
  CHECK(problemOf(with(23, 0), kRealSize) == Problem::NO_DIGEST);
  CHECK(problemOf(with(23, 2), kRealSize) == Problem::NO_DIGEST);
  // sizes: the limits are inclusive
  CHECK(problemOf(app, kMinImageBytes - 1) == Problem::TOO_SMALL);
  CHECK(problemOf(app, kMinImageBytes) == Problem::NONE);
  CHECK(problemOf(app, 0) == Problem::TOO_SMALL);
  CHECK(problemOf(app, kSlot) == Problem::NONE);
  CHECK(problemOf(app, kSlot + 1) == Problem::TOO_BIG);
  CHECK(problemOf(app, 0xFFFFFFFFu) == Problem::TOO_BIG);
  CHECK(problemIn(app, kSlot + 1, 0) == Problem::NONE);  // slot size unknown: no upper limit here
  CHECK(inspectImage(appHead, kHeadBytes - 1, kRealSize, kSlot) == Problem::TOO_SMALL);  // a head that was not read in full
  // size problems are reported before header problems
  CHECK(problemOf(with(12, 5), 1000) == Problem::TOO_SMALL);
  CHECK(problemOf(with(12, 5), kSlot + 1) == Problem::TOO_BIG);

  section("firmware update: what the clock decides");
  Inputs in;
  memcpy(in.head, appHead, kHeadBytes);
  in.files = 1;
  in.fileSize = kRealSize;
  in.slotSize = kSlot;
  in.runningElf[0] = 0x42;  // some other build
  CHECK(decide(in).verdict == Verdict::INSTALL && decide(in).problem == Problem::NONE);

  Inputs none = in;
  none.files = 0;
  CHECK(decide(none).verdict == Verdict::NOTHING);
  Inputs two = in;
  two.files = 2;
  CHECK(decide(two).verdict == Verdict::SEVERAL);
  two.head[0] = 0;  // several wins over a bad file: nothing is looked at
  CHECK(decide(two).verdict == Verdict::SEVERAL);
  Inputs noSlot = in;
  noSlot.slotSize = 0;
  CHECK(decide(noSlot).verdict == Verdict::NO_SLOT);
  Inputs junk = in;
  junk.head[0] = 0;
  CHECK(decide(junk).verdict == Verdict::BAD_FILE && decide(junk).problem == Problem::NOT_AN_IMAGE);
  Inputs small = in;
  small.fileSize = 19984;
  CHECK(decide(small).verdict == Verdict::BAD_FILE && decide(small).problem == Problem::TOO_SMALL);

  // the same build as the one that runs: nothing to do, whatever else is true
  Inputs same = in;
  memcpy(same.runningElf, elfHash(appHead), kDigestBytes);
  CHECK(decide(same).verdict == Verdict::SAME_AS_RUNNING);
  same.hasBattery = true;
  same.batteryMv = 3000;
  same.haveLast = true;
  memcpy(same.lastElf, elfHash(appHead), kDigestBytes);
  CHECK(decide(same).verdict == Verdict::SAME_AS_RUNNING);
  // one byte of the ELF hash differs: a different build
  Inputs near = in;
  memcpy(near.runningElf, elfHash(appHead), kDigestBytes);
  near.runningElf[kDigestBytes - 1] ^= 1;
  CHECK(decide(near).verdict == Verdict::INSTALL);
  near.runningElf[kDigestBytes - 1] ^= 1;
  near.runningElf[0] ^= 0x80;
  CHECK(decide(near).verdict == Verdict::INSTALL);

  // installed from the card before, and not what runs: not again
  Inputs again = in;
  again.haveLast = true;
  memcpy(again.lastElf, elfHash(appHead), kDigestBytes);
  CHECK(decide(again).verdict == Verdict::TRIED_BEFORE);
  again.haveLast = false;  // no memory of it: the stored bytes mean nothing
  CHECK(decide(again).verdict == Verdict::INSTALL);
  again.haveLast = true;
  again.lastElf[10] ^= 4;  // a different build was the last one
  CHECK(decide(again).verdict == Verdict::INSTALL);

  // the battery: only when one is fitted and read, and only below 3.60 V
  Inputs bat = in;
  bat.hasBattery = true;
  bat.batteryMv = kMinBatteryMv - 1;
  CHECK(decide(bat).verdict == Verdict::LOW_BATTERY);
  bat.batteryMv = kMinBatteryMv;
  CHECK(decide(bat).verdict == Verdict::INSTALL);
  bat.batteryMv = 0;  // could not be read
  CHECK(decide(bat).verdict == Verdict::INSTALL);
  bat.hasBattery = false;  // "battery = none": running from USB
  bat.batteryMv = 2500;
  CHECK(decide(bat).verdict == Verdict::INSTALL);
  Inputs lowAndTried = in;  // the cheaper reasons come first
  lowAndTried.hasBattery = true;
  lowAndTried.batteryMv = 3000;
  lowAndTried.haveLast = true;
  memcpy(lowAndTried.lastElf, elfHash(appHead), kDigestBytes);
  CHECK(decide(lowAndTried).verdict == Verdict::TRIED_BEFORE);
  Inputs lowAndJunk = in;
  lowAndJunk.hasBattery = true;
  lowAndJunk.batteryMv = 3000;
  lowAndJunk.head[0] = 0;
  CHECK(decide(lowAndJunk).verdict == Verdict::BAD_FILE);

  section("firmware update: what the screen says");
  char text[96];
  for (Verdict v : {Verdict::SEVERAL, Verdict::NO_SLOT, Verdict::TRIED_BEFORE, Verdict::LOW_BATTERY}) {
    Decision d;
    d.verdict = v;
    describe(d, 3500, text, sizeof text);
    CHECK(strlen(text) > 10 && strlen(text) <= 46);
  }
  for (Problem p : {Problem::TOO_SMALL, Problem::TOO_BIG, Problem::NOT_AN_IMAGE, Problem::WRONG_CHIP, Problem::NOT_AN_APP, Problem::NO_DIGEST}) {
    Decision d;
    d.verdict = Verdict::BAD_FILE;
    d.problem = p;
    describe(d, 0, text, sizeof text);
    CHECK(strlen(text) > 10 && strlen(text) <= 46);
  }
  Decision low;
  low.verdict = Verdict::LOW_BATTERY;
  describe(low, 3504, text, sizeof text);
  CHECK(strstr(text, "3.50 V") != nullptr);
  describe(low, 3999, text, sizeof text);
  CHECK(strstr(text, "3.99 V") != nullptr);
  describe(low, 3000, text, sizeof text);
  CHECK(strstr(text, "3.00 V") != nullptr);
  Decision quiet;
  quiet.verdict = Verdict::SAME_AS_RUNNING;
  describe(quiet, 0, text, sizeof text);
  CHECK_STR(text, "");  // a leftover copy of the running build gets no message
  quiet.verdict = Verdict::NOTHING;
  describe(quiet, 0, text, sizeof text);
  CHECK_STR(text, "");
  // the merged-image hint is the one people need most
  Decision big;
  big.verdict = Verdict::BAD_FILE;
  big.problem = Problem::TOO_BIG;
  describe(big, 0, text, sizeof text);
  CHECK(strstr(text, ".merged.bin") != nullptr && strstr(text, ".ino.bin") != nullptr);
}

// The example settings file kept in docs/ for people who want to see the format before they have
// a card: it must be exactly what the clock writes to a card with none.  Regenerate it with
//   ./build/test_features --write-example
void testRepoExampleFile(bool write) {
  section("example settings file in docs/");
  static char buf[16384];
  const size_t n = renderExampleConfig(Settings(), "", buf, sizeof buf);
  const char *path = "../../docs/ESP32-S3-RLCD-Config.example.txt";
  if (write) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(buf, 1, n, f) != n) {
      printf("  cannot write %s\n", path);
      g_failed++;
    } else {
      printf("  wrote %s (%zu bytes)\n", path, n);
    }
    if (f) fclose(f);
    return;
  }
  const std::string onDisk = readFile(path);
  const bool same = onDisk == std::string(buf, n);
  if (!same) printf("  %s is out of date: run  ./build/test_features --write-example\n", path);
  CHECK(same);
}

// Every setting is described in the README, and the README says nothing about one that is gone.
void testReadmeLists(const char *path) {
  section("README lists every setting");
  const std::string readme = readFile(path);
  CHECK(!readme.empty());
  for (size_t i = 0; i < settingCount(); i++) {
    const std::string key = std::string("`") + settingKey(i) + "`";
    if (readme.find(key) == std::string::npos) printf("  README does not mention %s\n", key.c_str());
    CHECK(readme.find(key) != std::string::npos);
  }
}

}  // namespace

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--write-example")) {
    testRepoExampleFile(true);
    return g_failed ? 1 : 0;
  }
  testRepoExampleFile(false);
  testReadmeLists("../../README.md");
  testMoonPhase();
  testMoonGlyph();
  testDateFormats();
  testBatteryCurve();
  testBatteryEstimate();
  testLowBattery();
  testDrift();
  testSettingsTable();
  testSettingsRoundTrip();
  testSettingsSyntax();
  testSettingsValues();
  testSettingsRejects();
  testSettingsTimezone();
  testSettingsReport();
  testSettingsForget();
  testSettingsExample();
  testBackupWifiSettings();
  testBatteryCalibrationSetting();
  testWifiPick();
  testWifiPickDay();
  testFirmwareLogic();
  testSettingsFuzz();

  printf("\n%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
