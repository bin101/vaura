#include "config.h"

#include <Preferences.h>

#include "node_id.h"
#include "power.h" // `charge` console command reads the raw INA219 charge current
#include "protocol.h"
#include "radio.h" // console channel changes apply immediately (applyChannel)

namespace DeviceConfig {

namespace {
Preferences prefs;
char nicknameBuf[Protocol::kNicknameFieldLen + 1] = {0};
uint16_t cachedNodeId = 0;
uint16_t cachedBeepFrequencyHz = BEEP_FREQUENCY_HZ;
uint32_t cachedDisplayTimeoutMs = OLED_WAKE_MS;
uint8_t cachedFallingBackSensitivity = FALLING_BACK_SENSITIVITY_DEFAULT;
uint8_t cachedChannel = 0;
String serialLineBuf;

// Letters only, no digits -- see the "name" command below. 4 letters (base-26)
// is already enough to uniquely encode a 16-bit node id (26^4 > 65536).
void deriveDefaultNickname() {
  uint32_t v = cachedNodeId;
  for (int i = 3; i >= 0; i--) {
    nicknameBuf[i] = 'A' + static_cast<char>(v % 26);
    v /= 26;
  }
  nicknameBuf[4] = '\0';
}

bool containsDigit(const String &s) {
  for (size_t i = 0; i < s.length(); i++) {
    if (isDigit(s[i])) {
      return true;
    }
  }
  return false;
}

void printHelp() {
  Serial.println(F("---- Vaura Console ----"));
  Serial.println(F("name <up to 5 letters>   Set nickname, e.g.: name ROB"));
  Serial.println(F("id                       Show node ID"));
  Serial.println(F("status                   Print all settings as key=value"));
  Serial.println(F("channel <0-9>            Set radio channel (applies immediately)"));
  Serial.println(F("sensitivity <0-10>       Set falling-back sensitivity level"));
  Serial.println(F("tone <0-10>              Set piezo tone level (plays a test beep)"));
  Serial.println(F("display <0|15|30|60|300> Display auto-off in seconds (0 = never)"));
  Serial.println(F("beep [Hz]                Play a test tone, e.g.: beep 3200 (no Hz: current frequency)"));
  Serial.println(F("charge                   Show raw charge current -- for charging-mode calibration"));
  Serial.println(F("help                     Show this help"));
}

// Strictly numeric argument after `prefixLen` chars, or -1. toInt() alone is
// not enough -- it silently returns 0 for garbage, which is a valid value for
// several of the setters below.
long parseNumericArg(const String &line, unsigned int prefixLen) {
  String arg = line.substring(prefixLen);
  arg.trim();
  if (arg.length() == 0) {
    return -1;
  }
  for (size_t i = 0; i < arg.length(); i++) {
    if (!isDigit(arg[i])) {
      return -1;
    }
  }
  return arg.toInt();
}

void handleLine(const String &lineIn) {
  String line = lineIn;
  line.trim();
  if (line.length() == 0) {
    return;
  }
  // Commands are matched case-insensitively throughout ("NAME ROB" works like
  // "name ROB"); only the argument keeps its original casing.
  String lower = line;
  lower.toLowerCase();
  if (lower.equals("help")) {
    printHelp();
  } else if (lower.equals("id")) {
    Serial.printf("Node ID: %04X\n", cachedNodeId);
  } else if (lower.startsWith("name ")) {
    String name = line.substring(5);
    name.trim();
    if (name.length() == 0) {
      Serial.println(F("Error: no name given."));
    } else if (name.length() > Protocol::kNicknameFieldLen) {
      // Rejected, not silently truncated: a name that got cut off without
      // being told would be confusing on a device with no keyboard to fix it.
      Serial.printf("Error: max. %u characters (otherwise the display overflows). Please choose a shorter name.\n",
                    static_cast<unsigned>(Protocol::kNicknameFieldLen));
    } else if (containsDigit(name)) {
      // Keeps this path consistent with the on-device rename menu, whose
      // cycling alphabet is letters-only -- see ui.cpp.
      Serial.println(F("Error: digits are not allowed in the name. Please use letters only."));
    } else {
      setNickname(name.c_str());
      Serial.printf("Saved. Nickname is now: %s\n", nickname());
    }
  } else if (lower.equals("status")) {
    // Machine-readable key=value lines -- parsed by the Vaura Flasher's
    // settings panel. Keys are part of that interface: keep them stable.
    Serial.printf("name=%s\n", nicknameBuf);
    Serial.printf("id=%04X\n", cachedNodeId);
    Serial.printf("version=%s\n", FIRMWARE_VERSION);
    Serial.printf("channel=%u\n", cachedChannel);
    Serial.printf("sensitivity=%u\n", cachedFallingBackSensitivity);
    Serial.printf("tone=%u\n",
                  (cachedBeepFrequencyHz - BEEP_FREQUENCY_MIN_HZ) / BEEP_FREQUENCY_STEP_HZ);
    Serial.printf("display=%lu\n", static_cast<unsigned long>(cachedDisplayTimeoutMs / 1000UL));
  } else if (lower.startsWith("channel ")) {
    long ch = parseNumericArg(line, 8);
    if (ch < 0 || ch > GROUP_CHANNEL_MAX) {
      Serial.printf("Error: expected channel 0-%d.\n", GROUP_CHANNEL_MAX);
    } else {
      setChannel(static_cast<uint8_t>(ch));
      Radio::applyChannel(static_cast<uint8_t>(ch));
      Serial.printf("OK channel=%ld\n", ch);
    }
  } else if (lower.startsWith("sensitivity ")) {
    long level = parseNumericArg(line, 12);
    if (level < 0 || level > FALLING_BACK_SENSITIVITY_MAX) {
      Serial.printf("Error: expected level 0-%d.\n", FALLING_BACK_SENSITIVITY_MAX);
    } else {
      setFallingBackSensitivity(static_cast<uint8_t>(level));
      Serial.printf("OK sensitivity=%ld (floor %d dBm)\n", level,
                    fallingBackFloorDbm(static_cast<uint8_t>(level)));
    }
  } else if (lower.startsWith("tone ")) {
    long level = parseNumericArg(line, 5);
    constexpr long kMaxToneLevel = (BEEP_FREQUENCY_MAX_HZ - BEEP_FREQUENCY_MIN_HZ) / BEEP_FREQUENCY_STEP_HZ;
    if (level < 0 || level > kMaxToneLevel) {
      Serial.printf("Error: expected level 0-%ld.\n", kMaxToneLevel);
    } else {
      uint16_t hz = BEEP_FREQUENCY_MIN_HZ + static_cast<uint16_t>(level) * BEEP_FREQUENCY_STEP_HZ;
      setBeepFrequencyHz(hz);
      tone(PIN_PIEZO, hz, 600);
      Serial.printf("OK tone=%ld (%u Hz)\n", level, hz);
    }
  } else if (lower.startsWith("display ")) {
    long seconds = parseNumericArg(line, 8);
    uint32_t ms = seconds < 0 ? 1 : static_cast<uint32_t>(seconds) * 1000UL;
    bool knownStep = false;
    for (uint32_t step : OLED_TIMEOUT_STEPS_MS) {
      if (ms == step) {
        knownStep = true;
        break;
      }
    }
    if (!knownStep) {
      Serial.println(F("Error: expected 0, 15, 30, 60, or 300 seconds (0 = never)."));
    } else {
      setDisplayTimeoutMs(ms);
      Serial.printf("OK display=%ld\n", seconds);
    }
  } else if (lower.equals("beep") || lower.startsWith("beep ")) {
    String arg = line.length() > 4 ? line.substring(4) : "";
    arg.trim();
    // Loudness has no software volume knob on this single-transistor on/off
    // drive -- the one thing worth tuning here is the frequency, since a
    // piezo is loudest right at its mechanical resonance. Play a fixed test
    // duration so repeated "beep <Hz>" calls are directly comparable by ear.
    // Range-checked as a `long` before any cast to an unsigned type: unlike
    // the persisted `tone` setting (which only ever comes from the fixed
    // BEEP_FREQUENCY_MIN/MAX_HZ ruler), this is free-form test input, and
    // toInt()'s raw result can be negative or huge (e.g. "beep -5" or
    // "beep 999999") -- both would otherwise slip past a plain `== 0` check
    // once cast to unsigned int.
    constexpr long kMinTestHz = 100;
    constexpr long kMaxTestHz = 10000;
    if (arg.length() == 0) {
      unsigned int freq = beepFrequencyHz();
      Serial.printf("Beeping at %u Hz...\n", freq);
      tone(PIN_PIEZO, freq, 600);
    } else {
      long freqArg = arg.toInt();
      if (freqArg < kMinTestHz || freqArg > kMaxTestHz) {
        Serial.printf("Error: expected a frequency between %ld and %ld Hz.\n", kMinTestHz, kMaxTestHz);
      } else {
        unsigned int freq = static_cast<unsigned int>(freqArg);
        Serial.printf("Beeping at %u Hz...\n", freq);
        tone(PIN_PIEZO, freq, 600);
      }
    }
  } else if (lower.equals("charge")) {
    // Not part of `status` -- this is a one-time hardware calibration aid
    // (see config.h's INA219_CURRENT_CHARGE_SIGN comment), not a persisted
    // setting the flasher's settings panel needs to know about.
    Serial.printf("chargeCurrentMa=%d  isCharging=%s  batteryMv=%u\n",
                  Power::chargeCurrentMilliamps(), Power::isCharging() ? "yes" : "no",
                  Power::batteryMillivolts());
    // INA219_CURRENT_CHARGE_SIGN is currently -1 (see config.h), so a
    // correctly-wired board reads chargeCurrentMa POSITIVE while actually
    // charging. If it reads negative instead, this board's IN+/IN- ended up
    // the other way round -- flip the sign to +1, not -1 (it's already -1).
    Serial.println(F("If chargeCurrentMa reads negative while actually charging, flip"));
    Serial.println(F("INA219_CURRENT_CHARGE_SIGN in config.h to +1 (it is currently -1)."));
  } else {
    Serial.println(F("Unknown command. 'help' for an overview."));
  }
}
} // namespace

void begin() {
  // Derive a compact, collision-resistant node id from the factory MAC --
  // unique enough for a club-sized fleet without needing to provision
  // anything by hand. See node_id.h for why this isn't a plain `mac & 0xFFFF`
  // (that grabs the vendor-constant OUI bytes, not the per-device ones).
  cachedNodeId = nodeIdFromMac(ESP.getEfuseMac());

  if (!prefs.begin("warndevice", /*readOnly=*/false)) {
    // Everything below still works off the defaults; the put* calls will just
    // silently do nothing -- worth one loud line so a "settings don't stick"
    // report is diagnosable from the boot log.
    Serial.println("Config: NVS unavailable -- settings cannot be saved!");
  }
  String stored = prefs.getString("nickname", "");
  if (stored.length() == 0) {
    deriveDefaultNickname();
  } else {
    strncpy(nicknameBuf, stored.c_str(), sizeof(nicknameBuf) - 1);
    nicknameBuf[sizeof(nicknameBuf) - 1] = '\0';
    if (stored.length() > Protocol::kNicknameFieldLen) {
      // Name was stored under an older firmware with a longer limit -- write
      // the truncated form back once so flash and RAM agree from now on.
      prefs.putString("nickname", nicknameBuf);
    }
  }

  cachedBeepFrequencyHz = prefs.getUShort("beepHz", BEEP_FREQUENCY_HZ);
  if (cachedBeepFrequencyHz < BEEP_FREQUENCY_MIN_HZ || cachedBeepFrequencyHz > BEEP_FREQUENCY_MAX_HZ ||
      (cachedBeepFrequencyHz - BEEP_FREQUENCY_MIN_HZ) % BEEP_FREQUENCY_STEP_HZ != 0) {
    // Guard against corrupt/out-of-range flash content. The grid check
    // matters for the UI: the tone menu shows the value as a 0..10 step, and
    // an off-grid frequency would display as the wrong step forever.
    cachedBeepFrequencyHz = BEEP_FREQUENCY_HZ;
  }

  cachedDisplayTimeoutMs = prefs.getULong("dispMs", OLED_WAKE_MS);
  bool knownStep = false;
  for (uint32_t step : OLED_TIMEOUT_STEPS_MS) {
    if (cachedDisplayTimeoutMs == step) {
      knownStep = true;
      break;
    }
  }
  if (!knownStep) {
    cachedDisplayTimeoutMs = OLED_WAKE_MS; // guard against corrupt flash content
  }

  cachedFallingBackSensitivity = prefs.getUChar("sensLvl", FALLING_BACK_SENSITIVITY_DEFAULT);
  if (cachedFallingBackSensitivity > FALLING_BACK_SENSITIVITY_MAX) {
    cachedFallingBackSensitivity = FALLING_BACK_SENSITIVITY_DEFAULT; // corrupt flash content
  }

  cachedChannel = prefs.getUChar("channel", 0);
  if (cachedChannel > GROUP_CHANNEL_MAX) {
    cachedChannel = 0; // corrupt flash content -> back to the common default
  }

  Serial.printf("Node ID: %04X  Nickname: %s  Channel: %u  FW %s  (console: 'help')\n", cachedNodeId,
                nicknameBuf, cachedChannel, FIRMWARE_VERSION);
}

const char *nickname() { return nicknameBuf; }

void setNickname(const char *name) {
  strncpy(nicknameBuf, name, sizeof(nicknameBuf) - 1);
  nicknameBuf[sizeof(nicknameBuf) - 1] = '\0';
  prefs.putString("nickname", nicknameBuf);
}

uint16_t nodeId() { return cachedNodeId; }

uint16_t beepFrequencyHz() { return cachedBeepFrequencyHz; }

void setBeepFrequencyHz(uint16_t hz) {
  cachedBeepFrequencyHz = hz;
  prefs.putUShort("beepHz", hz);
}

uint32_t displayTimeoutMs() { return cachedDisplayTimeoutMs; }

void setDisplayTimeoutMs(uint32_t ms) {
  cachedDisplayTimeoutMs = ms;
  prefs.putULong("dispMs", ms);
}

uint8_t fallingBackSensitivity() { return cachedFallingBackSensitivity; }

void setFallingBackSensitivity(uint8_t level) {
  cachedFallingBackSensitivity = level;
  prefs.putUChar("sensLvl", level);
}

uint8_t channel() { return cachedChannel; }

void setChannel(uint8_t ch) {
  cachedChannel = ch;
  prefs.putUChar("channel", ch);
}

uint8_t nextSeq() {
  static uint8_t seq = 0;
  return seq++;
}

void pollSerialConsole() {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (serialLineBuf.length() > 0) {
        handleLine(serialLineBuf);
        serialLineBuf = "";
      }
    } else {
      serialLineBuf += c;
      if (serialLineBuf.length() > 64) {
        serialLineBuf = ""; // guard against runaway garbage input
      }
    }
  }
}

} // namespace DeviceConfig
