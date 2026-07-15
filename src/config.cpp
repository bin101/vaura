#include "config.h"

#include <Preferences.h>

#include "protocol.h"

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
  Serial.println(F("---- Warngeraet Konsole ----"));
  Serial.println(F("name <bis 5 Buchstaben>  Spitznamen setzen, z.B.: name ROB"));
  Serial.println(F("id                       Node-ID anzeigen"));
  Serial.println(F("beep [Hz]                Testton abspielen, z.B.: beep 3200 (ohne Hz: aktuell eingestellte Frequenz)"));
  Serial.println(F("help                     Diese Hilfe anzeigen"));
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
    Serial.printf("Node-ID: %04X\n", cachedNodeId);
  } else if (lower.startsWith("name ")) {
    String name = line.substring(5);
    name.trim();
    if (name.length() == 0) {
      Serial.println(F("Fehler: kein Name angegeben."));
    } else if (name.length() > Protocol::kNicknameFieldLen) {
      // Rejected, not silently truncated: a name that got cut off without
      // being told would be confusing on a device with no keyboard to fix it.
      Serial.printf("Fehler: max. %u Zeichen (sonst laeuft die Anzeige ueber). Bitte kuerzer waehlen.\n",
                    static_cast<unsigned>(Protocol::kNicknameFieldLen));
    } else if (containsDigit(name)) {
      // Keeps this path consistent with the on-device rename menu, whose
      // cycling alphabet is letters-only -- see ui.cpp.
      Serial.println(F("Fehler: Zahlen sind im Namen nicht erlaubt. Bitte nur Buchstaben verwenden."));
    } else {
      setNickname(name.c_str());
      Serial.printf("Gespeichert. Spitzname ist jetzt: %s\n", nickname());
    }
  } else if (lower.equals("beep") || lower.startsWith("beep ")) {
    String arg = line.length() > 4 ? line.substring(4) : "";
    arg.trim();
    // Loudness has no software volume knob on this single-transistor on/off
    // drive -- the one thing worth tuning here is the frequency, since a
    // piezo is loudest right at its mechanical resonance. Play a fixed test
    // duration so repeated "beep <Hz>" calls are directly comparable by ear.
    unsigned int freq = arg.length() == 0 ? beepFrequencyHz() : static_cast<unsigned int>(arg.toInt());
    if (freq == 0) {
      Serial.println(F("Fehler: ungueltige Frequenz."));
    } else {
      Serial.printf("Piepe bei %u Hz...\n", freq);
      tone(PIN_PIEZO, freq, 600);
    }
  } else {
    Serial.println(F("Unbekannter Befehl. 'help' fuer Uebersicht."));
  }
}
} // namespace

void begin() {
  // Use the low 16 bits of the factory MAC as a compact, collision-resistant
  // node id -- unique enough for a club-sized fleet without needing to
  // provision anything by hand.
  uint64_t mac = ESP.getEfuseMac();
  cachedNodeId = static_cast<uint16_t>(mac & 0xFFFF);

  if (!prefs.begin("warndevice", /*readOnly=*/false)) {
    // Everything below still works off the defaults; the put* calls will just
    // silently do nothing -- worth one loud line so a "settings don't stick"
    // report is diagnosable from the boot log.
    Serial.println("Konfiguration: NVS nicht verfuegbar -- Einstellungen koennen nicht gespeichert werden!");
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

  Serial.printf("Node-ID: %04X  Spitzname: %s  Kanal: %u  FW %s  (Konsole: 'help')\n", cachedNodeId,
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
