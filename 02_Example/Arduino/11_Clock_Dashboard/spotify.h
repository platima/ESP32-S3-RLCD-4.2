#pragma once

// Spotify now-playing and remote control, via the Spotify Web API.
//
// Everything here runs in the network task except spotifyPost(), which any
// task may call.  The linking flow (OAuth with PKCE, no client secret) is a small
// web page served by the device while no account is linked.

#include <Arduino.h>

enum SpotifyCommand : uint8_t {
  SPOTIFY_CMD_PLAY_PAUSE = 1,
  SPOTIFY_CMD_NEXT,
  SPOTIFY_CMD_PREVIOUS,
  SPOTIFY_CMD_REFRESH,  // poll again right now
  SPOTIFY_CMD_UNLINK    // forget the linked account
};

// True when there is a Client ID (secrets.h or the SD card settings), Spotify is switched on
// ("spotify = on") and WiFi is not switched off.
bool spotifyConfigured();

// True while the linking page is being served: the network task keeps WiFi out of its
// power-saving sleep then, so the page answers promptly.
bool spotifyLowLatencyWanted();

// Loads saved credentials and publishes the initial status.  Call once from
// the network task before the first spotifyTick().
void spotifyBegin();

// Services polling, queued commands and the linking page.  Call every ~40 ms
// from the network task; it only does real work when something is due.
void spotifyTick();

// Queue a command (thread safe, never blocks).  False if Spotify is not
// configured or the queue is full.
bool spotifyPost(SpotifyCommand cmd);
