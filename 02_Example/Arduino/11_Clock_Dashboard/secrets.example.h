#pragma once

// Copy this file to "secrets.h" and fill it in.
// secrets.h is listed in .gitignore, so your passwords stay out of git.
//
// Everything here can also be set from a settings file on an SD card (see the README), which
// is handier once the clock is built: no reflashing to change the WiFi, the units or the place.

// WiFi (2.4 GHz; the ESP32-S3 does not do 5 GHz)
#define WIFI_SSID "YourWiFiName"
#define WIFI_PASSWORD "YourWiFiPassword"

// A backup network (optional): tried when the one above cannot be joined, say a phone's hotspot.
// While the clock is on the backup it looks for the main network every 10 minutes and goes back.
// #define WIFI_SSID_BACKUP "YourBackupWiFiName"
// #define WIFI_PASSWORD_BACKUP "YourBackupWiFiPassword"

// Spotify (optional).  Paste the Client ID of your own app from
// https://developer.spotify.com/dashboard to turn on now-playing and the
// KEY button controls.  Leave it empty to run without Spotify.
// The README walks through creating the app and linking your account.
#define SPOTIFY_CLIENT_ID ""

// Where the clock is (optional, for the weather and the time zone).  Give coordinates, or a place
// to look up; see config.h for the details.  Without either the clock still tells the time.
// #define LOCATION_LATITUDE (-33.865)
// #define LOCATION_LONGITUDE 151.209
// #define LOCATION_QUERY "Sydney"
// #define LOCATION_LABEL "Home"      // what the status bar shows
