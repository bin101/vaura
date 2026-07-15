// OLED screens and the single-button state machine (browse warning types /
// send / view incoming / settings: nickname + beep frequency). See README.md
// "Bedienung" for the gesture reference.
#pragma once

#include <Arduino.h>

#include "protocol.h"

namespace Ui {

void begin();

// True while the boot-time channel selection is still open. main.cpp holds
// back heartbeats until the rider confirmed (or the timeout auto-confirmed)
// the channel, so the device never announces itself to the wrong group.
bool bootChannelPending();

// Main.cpp calls this once per loop (before tick()) with the latest reading
// from the power module, so the idle screen can show it. `available` is false
// when no INA219 answered at boot (then no percentage is drawn at all instead
// of a misleading "0%"); `low` going true wakes the display once and beeps.
void setBatteryStatusForDisplay(uint8_t percent, bool available, bool low);

// Call every loop() iteration. Handles timeouts, the delayed 2nd copy of a
// just-sent warning, the OLED sleep/wake window, and throttled redraws.
void tick();

// Wire these to a OneButton instance in main.cpp.
void onButtonClick();
void onButtonLongPress();
// Emergency gesture: immediately broadcasts a generic ACHTUNG warning, from
// any state -- deliberately also while the display sleeps (a double click is
// unmistakably intentional, unlike the single press the wake-swallow protects
// against). Discards any menu/edit in progress and returns to idle.
void onButtonDoubleClick();

// Called from main's radio-dispatch when a WARNING packet arrives. Always
// takes over the display immediately, even out of a menu -- an incoming
// warning is more urgent than whatever the rider was doing with the button.
// Dismissed with a short click only -- there is no acknowledgement to send.
void onIncomingWarning(const char *senderNickname, Protocol::WarningType type);

} // namespace Ui
