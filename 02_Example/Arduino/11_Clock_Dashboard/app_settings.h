#pragma once

// The settings in force, and where they come from: the defaults in config.h and secrets.h, then
// what the clock saved in its flash ("cfg" namespace), then the settings file on an SD card,
// which is read once at boot and saved to flash so the card can be taken out again.  The format
// and the parsing live in settings.h / settings.cpp (host-tested); this file is the part that
// touches flash and the card.
//
// g_cfg is filled in during setup(), before any task starts, and only read afterwards.

#include <Arduino.h>

#include "settings.h"

extern Settings g_cfg;

struct CfgStatus {
  enum Sd : uint8_t {
    SD_NOT_TRIED = 0,
    SD_NO_CARD,
    SD_UNREADABLE,       // a card, but its file system would not mount (exFAT?)
    SD_EXAMPLE_WRITTEN,  // a card with no settings file: an example was written to it
    SD_WRITE_FAILED,     // ... or could not be (write-protected card?)
    SD_READ_FAILED,      // the file is there but could not be read (too big, card error)
    SD_FILE_APPLIED      // the file was read; `report` says what it did
  };
  Sd sd = SD_NOT_TRIED;
  int fromFlash = 0;         // settings that came from flash at this boot
  bool savedToFlash = false;
  char file[40] = "";        // the file name that was found or written
  char card[24] = "";        // "SDHC 14.8 GB"
  ConfigReport report;
};
extern CfgStatus g_cfgStatus;

// The factory defaults: config.h and secrets.h.
Settings cfgBuildDefaults();

// g_cfg = defaults, then flash.  Quick; call it first in setup().
void cfgLoadFlash();

// Looks for a card.  With a settings file: applies it on top of g_cfg and saves what it changed
// to flash.  Without one: writes an example file to the card.  Unmounts the card again.
void cfgImportSdCard();

// One line for the Info page / a toast, and the first few problems of the file.
void cfgSummary(char *out, size_t cap);
int cfgIssueCount();
void cfgIssueText(int i, char *out, size_t cap);

// Every setting on the serial port, passwords hidden.
void cfgPrint(Print &out);
