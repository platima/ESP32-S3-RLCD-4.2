#pragma once

// Network task: WiFi, NTP, location, weather, time zone and Spotify.
// Runs on core 0 next to the WiFi stack so the UI loop on core 1 never waits on
// the network.

#include <Arduino.h>
#include <time.h>

// Restores the saved time zone / location from NVS (or applies the time zone from the settings).
// Call once from setup(), after the settings are loaded and before netBegin().
void netLoadSettings();

// Starts the network task (not at all when WiFi is switched off in the settings).
void netBegin();

// True once per NTP sync: the UI task should then store the time in the RTC.
bool netTakeRtcWriteRequest();

// Fetch the weather again as soon as possible.
void netRequestWeatherRefresh();
