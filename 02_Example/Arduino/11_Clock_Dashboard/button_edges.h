#pragma once

// The buttons by interrupt, for the idle clock (cpu_idle_mhz).
//
// The UI loop reads the two buttons between frames.  At the working clock a frame takes some 30 ms to draw,
// shorter than any tap.  At 20 MHz it takes 120 ms, and a quick tap can begin and end inside that, unseen.
// So with an idle clock set, every change of the two pins is also recorded by an interrupt, with its time,
// and the loop replays the record to its click detectors (ClickDetector::edge) before it looks at the pins
// itself.  Without an idle clock nothing here is used and the buttons are read as they always were.

#include <Arduino.h>

enum ButtonId : uint8_t { BUTTON_KEY = 0, BUTTON_BOOT = 1 };

void buttonEdgesBegin();  // start recording (the pins are inputs with pull-ups already)
void buttonEdgesEnd();    // stop (before deep sleep)
bool buttonEdgesOn();

// The oldest change of that button not yet taken: the level it changed to and when (millis()).
// False when there is none.  Call it from the task that called buttonEdgesBegin().
bool buttonEdgeTake(ButtonId button, bool *down, uint32_t *atMs);
