#include "ui.h"

#include <U8g2lib.h>
#include <Wire.h>

#include "battery_curve.h"
#include "config.h"
#include "radio.h"
#include "roster.h"
#include "stats.h"

namespace Ui {

namespace {
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, /*reset=*/U8X8_PIN_NONE);

// SettingsMenu: entered from Idle via long-press. Short press cycles through
// Name/Ton/Zurueck (like the warning-send Menu below); long press commits to
// whichever is currently shown.
enum class State { Idle, Menu, IncomingWarning, Rename, SettingsMenu, ToneMenu, DisplayMenu, SensitivityMenu, ChannelMenu, StatsScreen, RangeTest, DismissPrompt };
State state = State::Idle;
uint32_t stateEnteredMs = 0;

// Ordered by expected mid-ride frequency: muting for a cafe stop first, the
// tour statistics glance, then the tuning knobs (tone, display timeout,
// sensitivity), the rarely-changed group channel and name, the field-test
// tool, and back. The "Stumm" label is rendered dynamically with the current
// state (see renderSettingsMenu()).
// "Empfindlich", not "Empfindlichkeit": the 9x15 menu font fits 14 glyphs per
// line, the full word is 15.
const char *kSettingsItemLabels[] = {"Stumm",       "Statistik", "Ton",  "Anzeige",
                                     "Empfindlich", "Kanal",     "Name", "Test",
                                     "Zurueck"};
const size_t kSettingsItemCount = sizeof(kSettingsItemLabels) / sizeof(kSettingsItemLabels[0]);
constexpr size_t kSettingsItemMute = 0;
constexpr size_t kSettingsItemStats = 1;
constexpr size_t kSettingsItemTone = 2;
constexpr size_t kSettingsItemDisplay = 3;
constexpr size_t kSettingsItemSensitivity = 4;
constexpr size_t kSettingsItemChannel = 5;
constexpr size_t kSettingsItemName = 6;
constexpr size_t kSettingsItemTest = 7;
constexpr size_t kSettingsItemBack = 8;
size_t settingsIndex = 0;

// RangeTest ("Test" in settings): live RSSI readout for one chosen peer, the
// field tool for verifying real-world range and tuning the RSSI_FALLING_BACK_*
// thresholds (see README "Verifikation"). Index into the roster's slot order.
int rangeTestIndex = 0;

// Mute switch for all alert beeps. Deliberately NOT persisted: after a restart
// the device must never come up silently muted without the rider knowing --
// forgetting to unmute after a break is the exact failure this avoids. The
// idle screen shows a STUMM badge while active. Tone-menu test beeps bypass
// this on purpose (adjusting the frequency is pointless without hearing it).
bool muted = false;

// ToneMenu: the beep frequency being tentatively adjusted, only persisted on
// long-press confirm (see onButtonLongPress()). Initialized from the current
// persisted value when the menu is entered.
uint16_t toneMenuFreq = BEEP_FREQUENCY_HZ;

// DisplayMenu: index into OLED_TIMEOUT_STEPS_MS being tentatively adjusted,
// same tentative-until-confirmed pattern as the ToneMenu above.
size_t displayMenuIndex = 0;

// SensitivityMenu: SCHWACH sensitivity step being tentatively adjusted,
// same tentative-until-confirmed pattern.
uint8_t sensMenuLevel = FALLING_BACK_SENSITIVITY_DEFAULT;

// ChannelMenu: radio group channel being tentatively adjusted, same pattern.
uint8_t channelMenuValue = 0;

// The four outgoing warning types, in send-menu order (matches
// Protocol::WarningType's declared order, which is deliberately the same).
const Protocol::WarningType kMenuItems[] = {
    Protocol::WarningType::CarBehind,
    Protocol::WarningType::HazardAhead,
    Protocol::WarningType::Stopping,
    Protocol::WarningType::Regroup,
};
const size_t kMenuItemCount = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
size_t menuIndex = 0;

// Incoming-warning state. +1 spare so the buffer also fits the "#XXXX"
// fallback from Roster::nicknameFor() when the sender was never heard.
char incomingSenderNickname[Protocol::kNicknameFieldLen + 1] = {0};
Protocol::WarningType incomingType = Protocol::WarningType::CarBehind;

// Rename state: one character position at a time, cycled with a short click
// and confirmed with a long press -- the only text entry a single button can
// realistically do. Short click only ever cycles A-Z (letters only, no
// digits); a long press is how a space gets typed at all (it commits the
// current position and blanks the next one). Two blank positions in a row
// means "done" -- see renameConfirmChar().
const char kRenameAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
const size_t kRenameAlphabetLen = sizeof(kRenameAlphabet) - 1;
char renameBuf[Protocol::kNicknameFieldLen + 1] = {0};
uint8_t renamePos = 0;

// A short local toast ("-> AUTO HINTEN gesendet") shown on the idle screen's
// bottom line, competing with Roster's own join/drop/back events for whichever
// is more recent.
char toastBuf[24] = {0};
uint32_t toastMs = 0;

// One deferred re-transmission of the last sent warning, for loss resilience
// on a lossy RF link -- see config.h WARNING_REPEAT_DELAY_MS. If the *first*
// copy failed (duty-cycle budget / dead radio), the same mechanism doubles as
// a retry: pendingRepeatFirstFailed remembers that so a succeeding repeat can
// upgrade the "! NICHT gesendet" toast to the normal success one.
uint8_t pendingRepeatBuf[Protocol::kMaxPacketLen];
size_t pendingRepeatLen = 0;
uint32_t pendingRepeatDueMs = 0;
bool pendingRepeatArmed = false;
bool pendingRepeatFirstFailed = false;
Protocol::WarningType pendingRepeatType = Protocol::WarningType::CarBehind;

// ABRISS reminder: while any (non-dismissed) rider stays dropped off, nudge
// again every DROPPED_OFF_REMINDER_INTERVAL_MS -- the wind can swallow the
// single long ABRISS tone. The first reminder is just a short beep; from the
// second one on the idle screen turns into a prompt offering to remove the
// rider from the group (State::DismissPrompt).
bool dropReminderArmed = false;
uint32_t nextDropReminderMs = 0;
uint8_t dropReminderCycles = 0;
Roster::PeerInfo dismissCandidate; // the rider shown in the prompt

uint32_t lastRenderMs = 0;
const uint32_t kRenderThrottleMs = 200;

// OLED sleep: the display is off whenever nothing has happened recently, and
// wakes for a fixed window on button activity, an incoming warning, or a
// SCHWACH/ABRISS roster event. The window length is user-adjustable (0 =
// never sleep) -- see DeviceConfig::displayTimeoutMs().
bool displayIsOn = true;
uint32_t displayWakeUntilMs = 0;
// Roster::lastAlertGeneration() we've already reacted to, so tick() only
// wakes/beeps once per new SCHWACH/ABRISS event. A counter, not the alert's
// timestamp: two alerts logged in the same millisecond (e.g. two peers
// dropping in one roster tick) would be indistinguishable by timestamp and
// the second one would silently swallow its wake+beep.
uint32_t lastSeenAlertGeneration = 0;

// Set by main.cpp right before each Ui::tick() so the idle screen can show
// the battery percentage without ui.cpp depending on the power module
// directly -- data flows one way (main.cpp polls Power, pushes here) instead
// of adding a cross-module dependency.
uint8_t batteryPercentForDisplay = 0;
bool batteryAvailableForDisplay = false;
bool batteryLowForDisplay = false;

void showToast(const char *text) {
  strncpy(toastBuf, text, sizeof(toastBuf) - 1);
  toastBuf[sizeof(toastBuf) - 1] = '\0';
  toastMs = millis();
}

// Non-blocking: tone() just queues a message for the core's own FreeRTOS tone
// task and returns immediately, so queuing a whole beep-gap-beep-gap-beep
// sequence up front is safe to call from the tight main loop() -- the task
// plays it back on its own time, one queued step at a time. A frequency of 0
// is how the gaps go silent (same mechanism noTone() uses internally). Each
// event gets its own count/duration so the three are tellable apart by ear:
// 2 short = incoming warning, 3 short = SCHWACH, 1 long = ABRISS.
void beepPattern(uint8_t count, uint32_t toneDurationMs) {
  if (muted) {
    return;
  }
  uint16_t freq = DeviceConfig::beepFrequencyHz();
  for (uint8_t i = 0; i < count; i++) {
    tone(PIN_PIEZO, freq, toneDurationMs);
    if (i + 1 < count) {
      tone(PIN_PIEZO, 0, BEEP_GAP_MS);
    }
  }
}

// Extends (or (re)starts) the display's on-window. Call this on every
// interaction/event worth showing -- not just when it was actually asleep --
// so an active rider never sees it blank out mid-interaction.
// displayWakeUntilMs is meaningless while the timeout is "Nie" (0) -- tick()
// short-circuits before ever comparing against it in that case.
void wakeDisplay() {
  displayWakeUntilMs = millis() + DeviceConfig::displayTimeoutMs();
  if (!displayIsOn) {
    display.setPowerSave(0);
    displayIsOn = true;
  }
}

void sleepDisplay() {
  display.setPowerSave(1);
  displayIsOn = false;
}

void sendWarning(Protocol::WarningType type) {
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeWarning(buf, DeviceConfig::nodeId(), DeviceConfig::nextSeq(), type);
  bool sent = Radio::send(buf, len);

  // Arm one repeat copy a little later, in case the first one collided or was
  // out of range for a marginal peer. Armed even when the first copy never
  // left the antenna -- then it is simply the retry.
  memcpy(pendingRepeatBuf, buf, len);
  pendingRepeatLen = len;
  pendingRepeatDueMs = millis() + WARNING_REPEAT_DELAY_MS;
  pendingRepeatArmed = true;
  pendingRepeatFirstFailed = !sent;
  pendingRepeatType = type;

  if (sent) {
    Stats::countWarningSent();
    char toast[24];
    snprintf(toast, sizeof(toast), "> %s", Protocol::warningLabel(type));
    showToast(toast);
    // No beep on success, deliberately: the sender already sees the toast/display
    // right now, so an audible confirmation of their own action would be redundant.
    // beepPattern() is reserved for things worth an audible nudge precisely because
    // you might not be looking -- an incoming warning, or a SCHWACH/ABRISS event.
  } else {
    // The rider must never believe a warning went out when it didn't -- this is
    // the one own-action case that *does* beep, since eyes may be on the road.
    showToast("! NICHT gesendet");
    beepPattern(1, BEEP_DURATION_MS);
  }
}

void enterIdle() {
  state = State::Idle;
  stateEnteredMs = millis();
}

void enterMenu() {
  state = State::Menu;
  menuIndex = 0;
  stateEnteredMs = millis();
}

void enterRangeTest() {
  state = State::RangeTest;
  rangeTestIndex = 0;
  stateEnteredMs = millis();
}

void enterSettingsMenu() {
  state = State::SettingsMenu;
  settingsIndex = 0;
  stateEnteredMs = millis();
}

void enterToneMenu() {
  state = State::ToneMenu;
  stateEnteredMs = millis();
  toneMenuFreq = DeviceConfig::beepFrequencyHz();
  // Snap onto the 100 Hz grid (belt to the NVS validation's braces): the menu
  // shows the frequency as a 0..10 step, so an off-grid value must not survive
  // into the display math.
  toneMenuFreq = BEEP_FREQUENCY_MIN_HZ +
                 ((toneMenuFreq - BEEP_FREQUENCY_MIN_HZ) / BEEP_FREQUENCY_STEP_HZ) * BEEP_FREQUENCY_STEP_HZ;
  tone(PIN_PIEZO, toneMenuFreq, BEEP_TEST_DURATION_MS); // preview the starting value
}

void enterSensitivityMenu() {
  state = State::SensitivityMenu;
  stateEnteredMs = millis();
  sensMenuLevel = DeviceConfig::fallingBackSensitivity();
  // No preview here, unlike the tone menu -- there is nothing to hear.
}

void enterChannelMenu() {
  state = State::ChannelMenu;
  stateEnteredMs = millis();
  channelMenuValue = DeviceConfig::channel();
}

void enterStatsScreen() {
  state = State::StatsScreen;
  stateEnteredMs = millis();
}

void enterDismissPrompt() {
  if (!Roster::longestDropped(dismissCandidate)) {
    return; // everyone came back since the reminder fired
  }
  state = State::DismissPrompt;
  stateEnteredMs = millis();
}

void enterDisplayMenu() {
  state = State::DisplayMenu;
  stateEnteredMs = millis();
  displayMenuIndex = 0;
  for (size_t i = 0; i < OLED_TIMEOUT_STEP_COUNT; i++) {
    if (OLED_TIMEOUT_STEPS_MS[i] == DeviceConfig::displayTimeoutMs()) {
      displayMenuIndex = i;
      break;
    }
  }
}

void enterRename() {
  state = State::Rename;
  stateEnteredMs = millis();
  renamePos = 0;
  memset(renameBuf, ' ', Protocol::kNicknameFieldLen);
  renameBuf[Protocol::kNicknameFieldLen] = '\0';
}

void renameAdvanceChar() {
  // kRenameAlphabetLen - 1 as the "not found" starting point means a blank
  // position's first click lands on the alphabet's first symbol ('A'), not
  // its second -- a space itself never appears in this rotation.
  size_t idx = kRenameAlphabetLen - 1;
  for (size_t i = 0; i < kRenameAlphabetLen; i++) {
    if (kRenameAlphabet[i] == renameBuf[renamePos]) {
      idx = i;
      break;
    }
  }
  idx = (idx + 1) % kRenameAlphabetLen;
  renameBuf[renamePos] = kRenameAlphabet[idx];
  stateEnteredMs = millis();
}

// True once a long press would save & exit instead of just confirming and
// moving on -- either two blank positions in a row, or the last position.
// Shared by renameConfirmChar() (what actually happens) and renderRename()
// (the footer hint), so the two can never drift apart.
bool renameWouldFinish() {
  bool twoBlanksInRow = renamePos > 0 && renameBuf[renamePos] == ' ' && renameBuf[renamePos - 1] == ' ';
  bool atLastPosition = renamePos + 1 >= Protocol::kNicknameFieldLen;
  return twoBlanksInRow || atLastPosition;
}

void saveRenameAndExit(size_t contentLen) {
  char trimmed[Protocol::kNicknameFieldLen + 1];
  memcpy(trimmed, renameBuf, contentLen);
  trimmed[contentLen] = '\0';
  for (int i = static_cast<int>(contentLen) - 1; i >= 0 && trimmed[i] == ' '; i--) {
    trimmed[i] = '\0';
  }
  if (trimmed[0] != '\0') {
    DeviceConfig::setNickname(trimmed);
  }
  enterIdle();
}

void renameConfirmChar() {
  // A long press always commits the current position, whatever it holds --
  // a real character if it was cycled, or a space if it wasn't touched.
  if (renameWouldFinish()) {
    saveRenameAndExit(renamePos + 1);
    return;
  }
  renamePos++;
  renameBuf[renamePos] = ' '; // the newly-entered position always starts blank
  stateEnteredMs = millis();
}

// A peer's nickname with its status baked into the string itself, so it
// stays readable even when columns touch: "!NAME!" = signal fading (the same
// fallingBack flag that drives the SCHWACH beep -- exclamation marks as the
// active warning), "(NAME)" = dropped off ("Abriss" -- parenthesized like an
// absentee), " NAME" = fine. The leading space keeps all first
// name-characters vertically aligned. Max 5 name chars + 2 decoration = 7.
void decorateNickname(char *out, size_t outLen, const Roster::PeerInfo &p) {
  const char *fmt = p.droppedOff ? "(%s)" : (p.fallingBack ? "!%s!" : " %s");
  snprintf(out, outLen, fmt, p.nickname);
}

void renderIdle() {
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 9, DeviceConfig::nickname());

  // Right-aligned header block: "aktiv/total" rider counter, then own battery.
  int rightX = 128;
  if (batteryAvailableForDisplay) {
    // The '!' prefix is the low-battery nag -- the dedicated warning row the
    // idle screen used to have now belongs to the rider list. The wake+beep
    // on the rising edge lives on in setBatteryStatusForDisplay().
    char battStr[8];
    snprintf(battStr, sizeof(battStr), "%s%d%%", batteryLowForDisplay ? "!" : "",
             batteryPercentForDisplay);
    rightX -= 6 * static_cast<int>(strlen(battStr));
    display.drawStr(rightX, 9, battStr);
  } // no INA219 -> draw nothing rather than a misleading "0%"

  int total = Roster::totalCount();
  if (total > 0) {
    // Group-style count, this device included: two riders out together read
    // "2/2", not "1/1". The header answers "how big is the group, how many
    // are connected" -- even though the device can only ever *hear* the
    // others (it does not appear in its own roster).
    char countStr[8];
    snprintf(countStr, sizeof(countStr), "%d/%d", Roster::activeCount() + 1, total + 1);
    rightX -= 6 * (static_cast<int>(strlen(countStr)) + 1); // one blank column as gap
    display.drawStr(rightX, 9, countStr);
  }

  if (muted) {
    // Centered in whatever is free between the own name and the right block,
    // nudged right of the name when the counters get wide.
    int nameEndX = 6 * static_cast<int>(strlen(DeviceConfig::nickname()));
    int x = (nameEndX + rightX) / 2 - (6 * 5) / 2;
    if (x < nameEndX + 3) {
      x = nameEndX + 3;
    }
    display.drawStr(x, 9, "STUMM");
  }

  display.drawHLine(0, 12, 128);

  if (total == 0) {
    display.setFont(u8g2_font_9x15B_tf);
    display.drawStr(0, 34, "Allein");
  } else {
    // The rider list, weakest signal first -- the riders at risk are the ones
    // this device exists for, so they are the ones that always fit. Dropped-off
    // peers sort as weakest of all (their rssiDbm is the stale pre-drop EMA,
    // deliberately not used as the key). Insertion sort: stable, and n <= 16.
    Roster::PeerInfo infos[MAX_PEERS];
    int n = 0;
    while (n < MAX_PEERS && Roster::peerInfo(n, infos[n])) {
      n++;
    }
    auto key = [](const Roster::PeerInfo &p) {
      return p.droppedOff ? INT16_MIN : p.rssiDbm;
    };
    for (int i = 1; i < n; i++) {
      Roster::PeerInfo item = infos[i];
      int j = i - 1;
      while (j >= 0 && key(infos[j]) > key(item)) {
        infos[j + 1] = infos[j];
        j--;
      }
      infos[j + 1] = item;
    }

    // Layout adapts to the group size: few riders get a roomy one-per-row
    // view including the smoothed RSSI, mid-size groups trade the number for
    // a second name column, and past 8 the font drops a size so a third
    // column fits with real gaps. Overflow shows only the weakest.
    //
    // First baselines are chosen so the topmost glyph row lands at y >= 16:
    // the common 0.96" SSD1306 modules are two-colored panels whose rows
    // 0..15 can only render yellow (the rest blue) -- the header row owns
    // that yellow strip, the rider list must not bleed into it. 6x10 glyphs
    // reach 7 rows above the baseline, 5x7 glyphs 6.
    if (n <= 4) {
      display.setFont(u8g2_font_6x10_tf);
      for (int i = 0; i < n; i++) {
        char deco[Protocol::kNicknameFieldLen + 3];
        decorateNickname(deco, sizeof(deco), infos[i]);
        // Name, smoothed RSSI, and the peer's battery from its heartbeat
        // ("--" if no INA219 is fitted there). For a dropped-off peer the
        // RSSI is meaningless ("---") but the last known battery still tells
        // whether their device simply ran out of juice.
        char line[22];
        int len;
        if (infos[i].droppedOff) {
          len = snprintf(line, sizeof(line), "%-7s  ---", deco);
        } else {
          len = snprintf(line, sizeof(line), "%-7s %4d", deco, infos[i].rssiDbm);
        }
        if (infos[i].batteryMillivolts > 0) {
          snprintf(line + len, sizeof(line) - len, " %3u%%",
                   batteryPercentFromMillivolts(infos[i].batteryMillivolts));
        } else {
          snprintf(line + len, sizeof(line) - len, "   --");
        }
        display.drawStr(0, 23 + i * 10, line);
      }
    } else {
      const bool threeCols = n > 8;
      const int cols = threeCols ? 3 : 2;
      const int colWidthPx = threeCols ? 44 : 64;
      const int rowHeightPx = threeCols ? 8 : 10;
      const int firstBaselineY = threeCols ? 22 : 23;
      const int maxRows = threeCols ? 5 : 4;
      display.setFont(threeCols ? u8g2_font_5x7_tf : u8g2_font_6x10_tf);
      int shown = n < cols * maxRows ? n : cols * maxRows;
      for (int i = 0; i < shown; i++) {
        char deco[Protocol::kNicknameFieldLen + 3];
        decorateNickname(deco, sizeof(deco), infos[i]);
        display.drawStr((i % cols) * colWidthPx, firstBaselineY + (i / cols) * rowHeightPx, deco);
      }
    }
  }

  // Bottom line: whichever of the local toast or a roster event is freshest.
  const char *rosterEvent = Roster::lastEventText();
  uint32_t rosterMs = Roster::lastEventTimestampMs();
  const char *bottomText = nullptr;
  uint32_t bottomMs = 0;
  // Signed difference, not a direct >=: both are absolute millis() stamps and
  // a direct compare inverts across the ~49-day wraparound (same idiom as the
  // deadlines in tick()).
  if (static_cast<int32_t>(toastMs - rosterMs) >= 0 && toastBuf[0] != '\0') {
    bottomText = toastBuf;
    bottomMs = toastMs;
  } else if (rosterEvent != nullptr) {
    bottomText = rosterEvent;
    bottomMs = rosterMs;
  }
  if (bottomText != nullptr) {
    display.setFont(u8g2_font_6x10_tf);
    char line[28];
    uint32_t ageS = (millis() - bottomMs) / 1000;
    snprintf(line, sizeof(line), "%s (%lus)", bottomText, static_cast<unsigned long>(ageS));
    display.drawStr(0, 61, line);
  }
}

void renderMenu() {
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 9, "Warnung senden:");
  display.drawHLine(0, 12, 128);

  display.setFont(u8g2_font_9x15B_tf);
  display.drawStr(0, 34, Protocol::warningLabel(kMenuItems[menuIndex]));

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 61, "kurz=vor lang=senden");
}

void renderIncoming() {
  display.setFont(u8g2_font_6x10_tf);
  char header[24];
  snprintf(header, sizeof(header), "WARNUNG von %s", incomingSenderNickname);
  display.drawStr(0, 9, header);
  display.drawHLine(0, 12, 128);

  display.setFont(u8g2_font_9x15B_tf);
  display.drawStr(0, 34, Protocol::warningLabel(incomingType));

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 61, "kurz = wegklicken");
}

void renderRename() {
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 9, "Name aendern:");
  display.drawHLine(0, 12, 128);

  display.setFont(u8g2_font_9x15B_tf);
  display.drawStr(0, 34, renameBuf);

  // Highlight the character currently being cycled: a solid box, then that
  // one glyph re-drawn in the inverse color on top of it.
  const int kCellW = 9; // u8g2_font_9x15B_tf is a fixed 9px-wide font
  const int cursorX = renamePos * kCellW;
  display.setDrawColor(1);
  display.drawBox(cursorX, 20, kCellW, 16);
  display.setDrawColor(0);
  char oneChar[2] = {renameBuf[renamePos], '\0'};
  display.drawStr(cursorX, 34, oneChar);
  display.setDrawColor(1);

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 61, renameWouldFinish() ? "lang=fertig" : "kurz=Zeichen lang=OK");
}

void renderSettingsMenu() {
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 9, "Einstellungen:");
  display.drawHLine(0, 12, 128);

  display.setFont(u8g2_font_9x15B_tf);
  if (settingsIndex == kSettingsItemMute) {
    char label[16];
    snprintf(label, sizeof(label), "Stumm: %s", muted ? "AN" : "AUS");
    display.drawStr(0, 34, label);
  } else {
    display.drawStr(0, 34, kSettingsItemLabels[settingsIndex]);
  }

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 61, "kurz=vor lang=waehlen");
}

void renderRangeTest() {
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 9, "Reichweiten-Test:");
  display.drawHLine(0, 12, 128);

  Roster::PeerInfo p;
  if (!Roster::peerInfo(rangeTestIndex, p)) {
    rangeTestIndex = 0;
    if (!Roster::peerInfo(0, p)) {
      display.drawStr(0, 34, "(niemand gehoert)");
      display.drawStr(0, 61, "lang=Ende");
      return;
    }
  }

  display.setFont(u8g2_font_9x15B_tf);
  char big[24];
  snprintf(big, sizeof(big), "%s %d", p.nickname, p.rssiDbm);
  display.drawStr(0, 34, big);

  display.setFont(u8g2_font_6x10_tf);
  char detail[28];
  if (p.droppedOff) {
    uint32_t ageS = (millis() - p.lastSeenMs) / 1000;
    snprintf(detail, sizeof(detail), "ABRISS  vor %lus", static_cast<unsigned long>(ageS));
  } else {
    snprintf(detail, sizeof(detail), "roh %d dBm  vor %lus", p.lastRawRssiDbm,
             static_cast<unsigned long>((millis() - p.lastSeenMs) / 1000));
  }
  display.drawStr(0, 46, detail);

  display.drawStr(0, 61, "kurz=Fahrer lang=Ende");
}

// Ruler-style scale shared by the tone and sensitivity menus: a baseline with
// one tick per step, labels 0..10 underneath, and a filled triangle pointing
// at the active step. Geometry (all glyph rows >= 16, below the two-color
// panel's yellow strip; the standard footer at baseline 61 stays clear):
// value line baseline 26, cursor rows 31-36, ruler line y=38, ticks 39-42,
// labels (4x6 font) baseline 50. Leaves the display in the 4x6 font --
// callers draw their remaining text with an explicit setFont, as everywhere.
void drawLevelRuler(uint8_t activeLevel) {
  // The geometry (12 px tick spacing spanning x = 4..124 of the 128 px panel)
  // is laid out for exactly the 11 steps 0..10 -- a different step count
  // needs a new layout, not just a changed loop bound.
  static_assert(FALLING_BACK_SENSITIVITY_MAX == 10, "ruler layout assumes steps 0..10");
  const int kFirstTickX = 4;
  const int kTickSpacingPx = 12;
  display.drawHLine(kFirstTickX, 38, kTickSpacingPx * FALLING_BACK_SENSITIVITY_MAX + 1);
  display.setFont(u8g2_font_4x6_tf);
  for (uint8_t i = 0; i <= FALLING_BACK_SENSITIVITY_MAX; i++) {
    int x = kFirstTickX + kTickSpacingPx * i;
    display.drawVLine(x, 39, 4);
    char label[4];
    snprintf(label, sizeof(label), "%u", i);
    // Roughly centered under the tick: 3 px glyph -> 1 px left, "10" -> 3 px
    // (its 7 px still end left of x = 128).
    display.drawStr(x - (i < 10 ? 1 : 3), 50, label);
  }
  int cursorX = kFirstTickX + kTickSpacingPx * activeLevel;
  display.drawTriangle(cursorX - 3, 31, cursorX + 3, 31, cursorX, 36);
}

void renderToneMenu() {
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 9, "Ton einstellen:");
  display.drawHLine(0, 12, 128);

  // Same 0..10 legend as the sensitivity menu (deliberately no Hz here --
  // the ear judges the test beep, the number is just a position to remember).
  uint8_t level = static_cast<uint8_t>((toneMenuFreq - BEEP_FREQUENCY_MIN_HZ) / BEEP_FREQUENCY_STEP_HZ);
  char valueStr[12];
  snprintf(valueStr, sizeof(valueStr), "Stufe %u", level);
  display.drawStr(0, 26, valueStr);
  drawLevelRuler(level);

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 61, "kurz=aendern lang=OK");
}

void renderSensitivityMenu() {
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 9, "Empfindlichkeit:");
  display.drawHLine(0, 12, 128);

  char valueStr[12];
  snprintf(valueStr, sizeof(valueStr), "Stufe %u", sensMenuLevel);
  display.drawStr(0, 26, valueStr);
  drawLevelRuler(sensMenuLevel);

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 61, "kurz=aendern lang=OK");
}

void renderStatsScreen() {
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 9, "Statistik:");
  // Right-aligned firmware version; long between-release strings
  // ("v0.1.0-3-g...") clip at the panel edge -- the full string is always in
  // the serial boot line and the `status` output.
  int verX = 128 - 6 * static_cast<int>(strlen(FIRMWARE_VERSION));
  if (verX < 66) {
    verX = 66; // keep clear of the "Statistik:" label, clip the tail instead
  }
  display.drawStr(verX, 9, FIRMWARE_VERSION);
  display.drawHLine(0, 12, 128);

  uint32_t upMin = millis() / 60000UL;
  char line[22];
  snprintf(line, sizeof(line), "Fahrzeit    %lu:%02luh", static_cast<unsigned long>(upMin / 60),
           static_cast<unsigned long>(upMin % 60));
  display.drawStr(0, 23, line);
  snprintf(line, sizeof(line), "Warn gesendet  %4lu",
           static_cast<unsigned long>(Stats::warningsSent()));
  display.drawStr(0, 33, line);
  snprintf(line, sizeof(line), "Warn empfangen %4lu",
           static_cast<unsigned long>(Stats::warningsReceived()));
  display.drawStr(0, 43, line);
  snprintf(line, sizeof(line), "Abrisse        %4lu", static_cast<unsigned long>(Stats::dropOffs()));
  display.drawStr(0, 53, line);

  display.drawStr(0, 61, "lang=zurueck");
}

void renderChannelMenu() {
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 9, "Funk-Kanal:");
  display.drawHLine(0, 12, 128);

  display.setFont(u8g2_font_9x15B_tf);
  char chStr[12];
  snprintf(chStr, sizeof(chStr), "Kanal %u", channelMenuValue);
  display.drawStr(0, 34, chStr);

  display.setFont(u8g2_font_6x10_tf);
  // Different channels never hear each other -- worth spelling out right in
  // the menu, a mismatch looks exactly like everyone dropping off.
  display.drawStr(0, 46, "alle Geraete gleich!");
  display.drawStr(0, 61, "kurz=aendern lang=OK");
}

void renderDismissPrompt() {
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 9, "Noch abgerissen:");
  display.drawHLine(0, 12, 128);

  char deco[Protocol::kNicknameFieldLen + 3];
  decorateNickname(deco, sizeof(deco), dismissCandidate);
  display.setFont(u8g2_font_9x15B_tf);
  display.drawStr(0, 34, deco);

  display.setFont(u8g2_font_6x10_tf);
  char detail[24];
  uint32_t ageMin = (millis() - dismissCandidate.lastSeenMs) / 60000UL;
  snprintf(detail, sizeof(detail), "weg seit %lu min", static_cast<unsigned long>(ageMin));
  display.drawStr(0, 46, detail);

  display.drawStr(0, 61, "kurz=ok lang=raus");
}

void renderDisplayMenu() {
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 9, "Anzeige aus nach:");
  display.drawHLine(0, 12, 128);

  display.setFont(u8g2_font_9x15B_tf);
  display.drawStr(0, 34, OLED_TIMEOUT_LABELS[displayMenuIndex]);

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 61, "kurz=aendern lang=OK");
}

void render() {
  display.clearBuffer();
  switch (state) {
    case State::Idle:
      renderIdle();
      break;
    case State::Menu:
      renderMenu();
      break;
    case State::IncomingWarning:
      renderIncoming();
      break;
    case State::Rename:
      renderRename();
      break;
    case State::SettingsMenu:
      renderSettingsMenu();
      break;
    case State::ToneMenu:
      renderToneMenu();
      break;
    case State::DisplayMenu:
      renderDisplayMenu();
      break;
    case State::SensitivityMenu:
      renderSensitivityMenu();
      break;
    case State::ChannelMenu:
      renderChannelMenu();
      break;
    case State::StatsScreen:
      renderStatsScreen();
      break;
    case State::RangeTest:
      renderRangeTest();
      break;
    case State::DismissPrompt:
      renderDismissPrompt();
      break;
  }
  display.sendBuffer();
  lastRenderMs = millis();
}
} // namespace

void setBatteryStatusForDisplay(uint8_t percent, bool available, bool low) {
  batteryPercentForDisplay = percent;
  batteryAvailableForDisplay = available;
  // Rising edge of "low" is worth one interruption -- wake + a single short
  // beep. The latched hysteresis in Power::isLow() guarantees this doesn't
  // retrigger while the voltage bounces around the threshold.
  if (low && !batteryLowForDisplay) {
    wakeDisplay();
    beepPattern(1, BEEP_DURATION_MS);
  }
  batteryLowForDisplay = low;
}

void begin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  display.begin();
  enterIdle();
  // The firmware version doubles as the very first "event": visible on the
  // idle screen right after boot ("which version is your device on?"), then
  // naturally displaced by real events.
  showToast("FW " FIRMWARE_VERSION);
  wakeDisplay(); // boot always shows the startup screen for the first window
  render();
}

void tick() {
  uint32_t now = millis();

  if (pendingRepeatArmed && static_cast<int32_t>(now - pendingRepeatDueMs) >= 0) {
    bool sent = Radio::send(pendingRepeatBuf, pendingRepeatLen);
    pendingRepeatArmed = false;
    if (pendingRepeatFirstFailed && sent) {
      // The retry got the warning out after all -- replace the failure toast.
      // Counts as the (one) sent warning now; the failed first copy never did.
      Stats::countWarningSent();
      char toast[24];
      snprintf(toast, sizeof(toast), "> %s", Protocol::warningLabel(pendingRepeatType));
      showToast(toast);
    }
  }

  bool needsRender = false;

  if (state == State::Menu && now - stateEnteredMs > UI_MENU_TIMEOUT_MS) {
    enterIdle();
    needsRender = true;
  }
  if (state == State::IncomingWarning && now - stateEnteredMs > UI_INCOMING_DISPLAY_MS) {
    enterIdle();
    needsRender = true;
  }
  if (state == State::Rename && now - stateEnteredMs > UI_RENAME_TIMEOUT_MS) {
    enterIdle(); // abandons the edit -- nothing is saved until the last position is confirmed
    needsRender = true;
  }
  if (state == State::SettingsMenu && now - stateEnteredMs > UI_MENU_TIMEOUT_MS) {
    enterIdle();
    needsRender = true;
  }
  if (state == State::ToneMenu && now - stateEnteredMs > UI_RENAME_TIMEOUT_MS) {
    enterIdle(); // abandons the change -- nothing is saved until confirmed with a long press
    needsRender = true;
  }
  if (state == State::DisplayMenu && now - stateEnteredMs > UI_RENAME_TIMEOUT_MS) {
    enterIdle(); // abandons the change -- nothing is saved until confirmed with a long press
    needsRender = true;
  }
  if (state == State::SensitivityMenu && now - stateEnteredMs > UI_RENAME_TIMEOUT_MS) {
    enterIdle(); // abandons the change -- nothing is saved until confirmed with a long press
    needsRender = true;
  }
  if (state == State::ChannelMenu && now - stateEnteredMs > UI_RENAME_TIMEOUT_MS) {
    enterIdle(); // abandons the change -- nothing is saved until confirmed with a long press
    needsRender = true;
  }
  if (state == State::StatsScreen && now - stateEnteredMs > UI_MENU_TIMEOUT_MS) {
    enterIdle();
    needsRender = true;
  }
  if (state == State::DismissPrompt && now - stateEnteredMs > UI_INCOMING_DISPLAY_MS) {
    enterIdle(); // keeps the rider -- the next reminder cycle will ask again
    needsRender = true;
  }
  if (state == State::DismissPrompt) {
    // Keep the prompt honest while it is open: refresh the candidate every
    // tick so the "weg seit" age stays live, a rider who came back mid-prompt
    // is replaced by the next-longest dropped one -- and if nobody is dropped
    // anymore, the prompt closes itself instead of showing a stale name.
    if (!Roster::longestDropped(dismissCandidate)) {
      enterIdle();
      needsRender = true;
    }
  }

  // A SCHWACH/ABRISS event is worth interrupting the rider for, even if the
  // display was asleep -- wake it exactly once per new such event.
  uint32_t alertGen = Roster::lastAlertGeneration();
  if (alertGen != lastSeenAlertGeneration) {
    lastSeenAlertGeneration = alertGen;
    wakeDisplay();
    if (Roster::lastAlertType() == Roster::AlertType::DroppedOff) {
      beepPattern(1, DROPPED_OFF_BEEP_DURATION_MS);
    } else {
      beepPattern(FALLING_BACK_BEEP_COUNT, BEEP_DURATION_MS);
    }
    needsRender = true;
  }

  // Periodic ABRISS reminder while someone stays dropped off. Armed when the
  // first drop is noticed, disarmed (and the cycle counter reset) as soon as
  // everyone is back or removed. From the second cycle on, the beep comes
  // with the removal prompt -- but only over the idle screen; an open menu,
  // edit or incoming warning is never hijacked (the next cycle retries).
  if (!Roster::anyDroppedOff()) {
    dropReminderArmed = false;
    dropReminderCycles = 0;
  } else if (!dropReminderArmed) {
    dropReminderArmed = true;
    dropReminderCycles = 0;
    nextDropReminderMs = now + DROPPED_OFF_REMINDER_INTERVAL_MS;
  } else if (static_cast<int32_t>(now - nextDropReminderMs) >= 0) {
    nextDropReminderMs = now + DROPPED_OFF_REMINDER_INTERVAL_MS;
    dropReminderCycles++;
    beepPattern(1, BEEP_DURATION_MS);
    if (dropReminderCycles >= 2 && state == State::Idle) {
      wakeDisplay();
      enterDismissPrompt();
      needsRender = true;
    }
  }

  // RangeTest is exempt from display sleep (and has no timeout): a range walk
  // takes minutes of glancing at the live RSSI, and it only ever runs because
  // the rider explicitly started it -- exit is the long press. Timeout 0 =
  // "Nie": checked here (not via a far-future deadline, which would break the
  // signed-difference wraparound idiom for anything > 24.8 days out).
  if (displayIsOn && state != State::RangeTest && DeviceConfig::displayTimeoutMs() != 0 &&
      static_cast<int32_t>(now - displayWakeUntilMs) >= 0) {
    sleepDisplay();
  }

  if (displayIsOn && (needsRender || now - lastRenderMs >= kRenderThrottleMs)) {
    render();
  }
}

void onButtonClick() {
  // First press while asleep only wakes the display -- the rider can't see
  // what they're about to trigger otherwise (menu open / warning armed).
  if (!displayIsOn) {
    wakeDisplay();
    render();
    return;
  }
  wakeDisplay();

  switch (state) {
    case State::Idle:
      enterMenu();
      break;
    case State::Menu:
      menuIndex = (menuIndex + 1) % kMenuItemCount;
      stateEnteredMs = millis(); // clicking counts as activity, resets the timeout
      break;
    case State::RangeTest: {
      int total = Roster::totalCount();
      rangeTestIndex = total > 0 ? (rangeTestIndex + 1) % total : 0;
      break;
    }
    case State::IncomingWarning:
      enterIdle(); // dismiss without acknowledging
      break;
    case State::DismissPrompt:
      enterIdle(); // keep the rider -- the next reminder cycle will ask again
      break;
    case State::Rename:
      renameAdvanceChar();
      break;
    case State::SettingsMenu:
      settingsIndex = (settingsIndex + 1) % kSettingsItemCount;
      stateEnteredMs = millis(); // clicking counts as activity, resets the timeout
      break;
    case State::ToneMenu:
      toneMenuFreq += BEEP_FREQUENCY_STEP_HZ;
      if (toneMenuFreq > BEEP_FREQUENCY_MAX_HZ) {
        toneMenuFreq = BEEP_FREQUENCY_MIN_HZ;
      }
      stateEnteredMs = millis(); // clicking counts as activity, resets the timeout
      tone(PIN_PIEZO, toneMenuFreq, BEEP_TEST_DURATION_MS);
      break;
    case State::DisplayMenu:
      displayMenuIndex = (displayMenuIndex + 1) % OLED_TIMEOUT_STEP_COUNT;
      stateEnteredMs = millis(); // clicking counts as activity, resets the timeout
      break;
    case State::SensitivityMenu:
      sensMenuLevel = (sensMenuLevel + 1) % (FALLING_BACK_SENSITIVITY_MAX + 1);
      stateEnteredMs = millis(); // clicking counts as activity, resets the timeout
      break;
    case State::ChannelMenu:
      channelMenuValue = (channelMenuValue + 1) % (GROUP_CHANNEL_MAX + 1);
      stateEnteredMs = millis(); // clicking counts as activity, resets the timeout
      break;
    case State::StatsScreen:
      stateEnteredMs = millis(); // nothing to cycle -- just keep the screen alive
      break;
  }
  render();
}

void onButtonLongPress() {
  // Same rule as onButtonClick(): a long press while asleep must not fire
  // blind (it can send a warning or open name-edit) -- swallow it as a wake.
  if (!displayIsOn) {
    wakeDisplay();
    render();
    return;
  }
  wakeDisplay();

  switch (state) {
    case State::Idle:
      enterSettingsMenu();
      break;
    case State::Menu:
      sendWarning(kMenuItems[menuIndex]);
      enterIdle();
      break;
    case State::RangeTest:
      enterIdle(); // the only way out -- this mode has no timeout, see tick()
      break;
    case State::IncomingWarning:
      break; // dismissed with a short click only, by design -- see onButtonClick()
    case State::DismissPrompt: {
      char toast[24];
      if (Roster::dismiss(dismissCandidate.nodeId)) {
        snprintf(toast, sizeof(toast), "%s entfernt", dismissCandidate.nickname);
      } else {
        // Heartbeat came back between prompt and long press -- dismiss() was
        // a no-op, and claiming "entfernt" over a list that shows the rider
        // alive would be a lie.
        snprintf(toast, sizeof(toast), "%s ist zurueck", dismissCandidate.nickname);
      }
      showToast(toast);
      enterIdle();
      break;
    }
    case State::Rename:
      renameConfirmChar();
      break;
    case State::SettingsMenu:
      switch (settingsIndex) {
        case kSettingsItemMute:
          // Toggle in place -- staying in the menu shows the new state
          // immediately and allows toggling right back.
          muted = !muted;
          stateEnteredMs = millis();
          break;
        case kSettingsItemName:
          enterRename();
          break;
        case kSettingsItemTone:
          enterToneMenu();
          break;
        case kSettingsItemDisplay:
          enterDisplayMenu();
          break;
        case kSettingsItemSensitivity:
          enterSensitivityMenu();
          break;
        case kSettingsItemChannel:
          enterChannelMenu();
          break;
        case kSettingsItemStats:
          enterStatsScreen();
          break;
        case kSettingsItemTest:
          enterRangeTest();
          break;
        default: // kSettingsItemBack
          enterIdle();
          break;
      }
      break;
    case State::ToneMenu:
      DeviceConfig::setBeepFrequencyHz(toneMenuFreq);
      enterIdle();
      break;
    case State::DisplayMenu:
      DeviceConfig::setDisplayTimeoutMs(OLED_TIMEOUT_STEPS_MS[displayMenuIndex]);
      // The wakeDisplay() at the top of this handler still used the old
      // timeout -- re-arm so the just-chosen value applies immediately.
      wakeDisplay();
      enterIdle();
      break;
    case State::SensitivityMenu:
      DeviceConfig::setFallingBackSensitivity(sensMenuLevel);
      enterIdle();
      break;
    case State::ChannelMenu:
      DeviceConfig::setChannel(channelMenuValue);
      Radio::applyChannel(channelMenuValue); // effective immediately, not just after reboot
      enterIdle();
      break;
    case State::StatsScreen:
      enterIdle();
      break;
  }
  render();
}

void onButtonDoubleClick() {
  // No display-asleep swallow here, unlike click/long-press: this is the
  // "warn NOW, eyes stay on the road" path, and sending a generic ACHTUNG
  // blind is exactly its purpose. Whatever menu/edit was open is abandoned --
  // same priority call as an incoming warning taking over the screen.
  wakeDisplay();
  sendWarning(Protocol::WarningType::Attention);
  enterIdle();
  render();
}

void onIncomingWarning(const char *senderNickname, Protocol::WarningType type) {
  wakeDisplay(); // an incoming warning must be visible even if the display was asleep
  state = State::IncomingWarning;
  stateEnteredMs = millis();
  strncpy(incomingSenderNickname, senderNickname, sizeof(incomingSenderNickname) - 1);
  incomingSenderNickname[sizeof(incomingSenderNickname) - 1] = '\0';
  incomingType = type;
  beepPattern(WARNING_BEEP_COUNT, BEEP_DURATION_MS);
  render();
}

} // namespace Ui
