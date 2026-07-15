# Rennradclub-Warngerät (XIAO ESP32-S3 + Wio-SX1262)

Eigenständige Firmware für das Case ["Meshtastic Seeed Studio XIAO ESP32S3 & Wio SX1262 case MOD with screen and INA219 voltage reader" von *Lead*](https://www.printables.com/model/1433873-meshtastic-seeed-studio-xiao-esp32s3-wio-sx1262-ca). Nutzt das Wio-SX1262-Funkmodul im **GFSK-Modus**, nicht im namensgebenden LoRa-Modus — siehe "Funkprotokoll" weiter unten für die Begründung.
Kein Meshtastic an Bord — eigenes, schlankes Broadcast-Protokoll, zugeschnitten auf eine Ausfahrt mit 6–12 Fahrern:

- **Gefahren-Buttons**: Auto von hinten, Gefahrenstelle voraus, Bremsen/Stopp, Sammeln/Warten
- **Automatische Abriss-Erkennung** über Heartbeat + Signalstärke (kein GPS nötig)
- **Akkuanzeige** über den bereits verbauten INA219

## Hardware (wie im Case verbaut)

| Komponente | Detail |
|---|---|
| MCU | Seeed Studio XIAO ESP32-S3 |
| Funk | Wio-SX1262 (LoRa), auf XIAO gesteckt (B2B-Anschluss) |
| Display | 0,96" OLED SSD1306, 128×64, I²C (Zweifarb-Panel: obere 16 Pixelzeilen gelb, Rest blau) |
| Akku-Monitor | INA219, I²C, High-Side |
| Akku | LiPo 803040, 1000 mAh |
| Taster | 1× taktil |
| Piezo-Beeper | 1× Piezo-Element, geschaltet über NPN-Transistor (z. B. 2N2222) |

> **Region/Antenne:** Diese Firmware ist fest auf **EU868 (868,3 MHz)** eingestellt (siehe `src/config.h`). Das verlinkte Case ist mit "915mhz" (US) getaggt — **prüfe vor dem ersten Einschalten, dass eine 868-MHz-Antenne verbaut ist**, sonst leidet die Reichweite deutlich. Sendeleistung ist auf 14 dBm begrenzt (EU868-SRD-Grenzwert).

## Bauplan / Verdrahtung

Entspricht 1:1 dem Aufbau des Case-Autors — **keine neue Verdrahtung nötig**, wenn du dich an dessen Anleitung gehalten hast. Zur Nachvollziehbarkeit hier noch mal zusammengefasst:

1. **LoRa:** Wio-SX1262 auf den XIAO stecken (Board-to-Board-Anschluss), 868-MHz-Antenne anschrauben.
2. **I²C-Kette** (Kupferlack-/Magnetdraht, ca. 4 cm reichen): INA219 → OLED → XIAO, jeweils **GND / 3V3 / SDA / SCL** durchverbinden.
3. **Strompfad / Akkumessung:** Akku(+) → INA219 **Vin+** (High Side) → INA219 **Vin−** → Ein/Aus-Schalter → XIAO **BAT+**. Akku(−) direkt an XIAO **BAT−**.
4. **Taster:** ein Bein an GND (z. B. am OLED), das andere an **D3 / GPIO4**.
5. **Piezo-Beeper** (über NPN-Transistor als Low-Side-Schalter, z. B. 2N2222 — die GPIO liefert nicht genug Strom für den Piezo direkt):
   - Piezo (+) → **3V3**
   - Piezo (−) → **Kollektor** des Transistors
   - **Basis** des Transistors → 1 kΩ-Widerstand → **D1 / GPIO2** — **dieser Widerstand ist nicht optional:** ohne ihn liegt die Basis-Emitter-Strecke (eine Diode mit ~0,6–0,7 V Flussspannung) praktisch direkt am 3,3-V-GPIO, ohne Strombegrenzung. Das kann den maximal zulässigen Ausgangsstrom des GPIO-Pins überschreiten und diesen beschädigen.
   - **Basis** des Transistors → zusätzlich 10 kΩ-Widerstand → **GND** (Pull-down, verhindert ein Zufalls-Piepsen beim Boot). Anders als GPIO3 ist GPIO2 **kein** Strapping-Pin des ESP32-S3 (siehe Kasten unten) — dieser Pull-down ist hier also reine Komfortsache, kein Muss.
   - **Emitter** des Transistors → **GND**
   - Keine Freilaufdiode nötig (nur bei induktiven Lasten wie Relais/Motor relevant, ein Piezo ist kapazitiv).
   - **Warum D1/GPIO2 und nicht D2/GPIO3:** GPIO3 ist einer der ESP32-S3-Strapping-Pins (steuert die JTAG-Signalquelle beim Boot) und hat **keinen** eigenen internen Pull-Widerstand — alles, was extern an GPIO3 hängt, zählt zur Boot-Strapping-Beschaltung. GPIO2 hat keine solche Sonderrolle und ist damit die unkompliziertere Wahl für einen Piezo, der einfach nur an- und ausgeschaltet werden soll.
   - **Achtung Pinbelegung:** "2N2222" ist kein einheitliches Pinout — PN2222A (TO-92) ist meist E-B-C, P2N2222A (TO-92) dagegen C-B-E, von links nach rechts bei flacher Seite zu dir. Vor dem Einlöten den genauen Aufdruck des eigenen Bauteils nachschlagen oder mit einer hFE-Transistortesterbuchse am Multimeter verifizieren.
6. **Reihenfolge beim Bau:** zuerst alle Drähte am XIAO anlöten, **danach** Bauteile ins Gehäuse kleben (Heißkleber) — nicht umgekehrt, sonst kommt man an die Lötpads nicht mehr ran.

### Pin-Belegung (fest in `src/config.h`)

```
LoRa SPI:      SCK=GPIO7   MISO=GPIO8   MOSI=GPIO9   NSS=GPIO41
LoRa Steuerung: DIO1=GPIO39  RESET=GPIO42  BUSY=GPIO40  ANT_SW=GPIO38
I2C (OLED+INA219): SDA=GPIO5  SCL=GPIO6
Taster:        GPIO4 (D3), gegen GND, interner Pull-up
Piezo-Beeper:  GPIO2 (D1), über NPN-Transistor (1kΩ Basis, 10kΩ Pull-down)
```

Diese Pins gehören zum B2B-"Kit" (Wio-SX1262 aufgesteckt) — **nicht** zu der separat erhältlichen Wio-SX1262-Platine zum Anlöten, die andere Pins nutzt.

### Steckbrett-Version für den ersten Test (nur USB, ohne Netzteil, ohne Akku)

Bevor irgendetwas ins Gehäuse geklebt wird, lohnt sich ein loser Aufbau auf einem Steckbrett — Strom kommt dabei ausschließlich über das USB-Kabel vom Rechner, kein Netzteil und (erstmal) kein Akku nötig:

1. **XIAO + Wio-SX1262 zusammenstecken** (Board-to-Board-Anschluss) und Antenne anschrauben — genau wie im finalen Aufbau. Die Antenne auch beim Steckbrett-Test **immer** aufschrauben, bevor gesendet wird (sonst riskiert man die PA-Endstufe).
2. **XIAO ins Steckbrett stecken** (oder lose lassen und alles per Jumperkabel verbinden) und per **USB-C mit dem Rechner verbinden** — das versorgt den XIAO über dessen eigenen 3V3-Regler.
3. **Stromschienen versorgen:** XIAO **3V3** → Plus-Schiene, XIAO **GND** → Minus-Schiene des Steckbretts.
4. **OLED (SSD1306):** VCC → Plus-Schiene, GND → Minus-Schiene, SDA → GPIO5, SCL → GPIO6.
5. **INA219:** VCC → Plus-Schiene, GND → Minus-Schiene, SDA → GPIO5 (gleicher I²C-Bus, parallel zum OLED), SCL → GPIO6. **Vin+/Vin−** bleiben ohne Akku offen (oder mit einem Draht überbrückt) — die Firmware erkennt den INA219 trotzdem über I²C und zeigt einfach `0` bzw. eine sinnlose Spannung an; das ist für den ersten Test ohne Akku normal und kein Fehler.
6. **Taster:** ein Bein an die Minus-Schiene (GND), das andere an GPIO4 (D3).
7. **Piezo-Beeper:** Transistor-Basis über 1 kΩ an GPIO2 (D1), zusätzlich 10 kΩ von der Basis zur Minus-Schiene, Emitter an die Minus-Schiene, Kollektor an Piezo (−), Piezo (+) an die Plus-Schiene (siehe "Bauplan" oben für die vollständige Erklärung inkl. Pinbelegungs-Vorsicht).
8. **BAT+/BAT− am XIAO bleiben komplett unbeschaltet**, solange kein Akku getestet wird.

Damit lassen sich Display, Taster, Namensvergabe und LoRa-Senden/Empfangen bereits vollständig testen — nur die Akkuanzeige zeigt bis zum Anschluss eines echten Akkus keinen sinnvollen Wert. Für den vollständigen Test aus dem Abschnitt [Verifikation](#verifikation) (Abriss-Erkennung etc.) braucht es wie dort beschrieben mindestens zwei so aufgebaute Geräte, jedes an einem eigenen USB-Anschluss.

Sobald der Steckbrett-Test erfolgreich war, kann optional ein Akku ergänzt werden (Akku(+) → INA219 Vin+ → Vin− → Ein/Aus-Schalter → XIAO BAT+, Akku(−) → XIAO BAT−, siehe Schritt 3 oben im Bauplan) — danach funktioniert das Gerät auch ohne USB-Kabel.

### Für später reserviert (nicht bestückt)

Frei bleiben GPIO1 (D0, ADC-fähig) und GPIO43/44 (D6/D7) — Platz für zusätzliche Taster oder einen Vibrationsmotor in einer späteren Ausbaustufe, ohne die bestehende Verdrahtung anzufassen. (GPIO2/D1 ist inzwischen für den Piezo-Beeper vergeben, siehe oben. GPIO3/D2 bewusst frei/unbeschaltet gelassen — Strapping-Pin, siehe Bauplan.)

## Firmware bauen & flashen

Voraussetzung: [PlatformIO](https://platformio.org/) (CLI oder VS-Code-Extension).

```bash
pio run                      # kompilieren
pio run -t upload            # auf das per USB verbundene Gerät flashen
pio device monitor           # serielle Konsole / Debug-Log (115200 Baud)
pio test -e native           # Unit-Tests, laufen auf dem Rechner (keine Hardware nötig)
```

Alle Geräte bekommen **dieselbe** Firmware — es gibt keine Kompilierzeit-Unterschiede zwischen den Geräten.

**Versionierung:** Das Projekt nutzt [Semantic Versioning](https://semver.org/lang/de/) über Git-Tags (`v0.1.0`, `v0.2.0`, …) und [Conventional Commits](https://www.conventionalcommits.org/) (`feat:`, `fix:`, `docs:` …). Die Firmware bekommt ihre Version beim Bauen automatisch aus `git describe` (`FIRMWARE_VERSION`, zwischen Releases z. B. `v0.1.0-5-gabc1234`, mit `-dirty` bei lokalen Änderungen) und zeigt sie beim Boot in der Ereigniszeile, in der Statistik-Kopfzeile, in der seriellen Boot-Zeile und in der `status`-Ausgabe — so ist am Treffpunkt schnell geklärt, wer welche Version fährt.

Die Unit-Tests (in `test/`) decken das Funk-Paketformat (`src/protocol.cpp`: Encode/Decode-Roundtrips, kaputte Pakete) und die Akku-Spannungskurve (`src/battery_curve.cpp`) ab. Bei jedem Push baut GitHub Actions (`.github/workflows/ci.yml`) zusätzlich die Firmware und lässt die Tests laufen.

## Flashen ohne PlatformIO: der Warngerät-Flasher

Für Nutzer, die **nicht** selbst kompilieren wollen, gibt es eine GUI-App (macOS/Linux/Windows): Gerät per USB-C anschließen, „Neueste Version herunterladen", Port auswählen (das Warngerät wird automatisch markiert und vorausgewählt), „Firmware flashen" — fertig. Alle am Gerät gespeicherten Einstellungen (Spitzname, Tonfrequenz, Anzeige-Abschaltzeit usw.) bleiben dabei erhalten: Die App schreibt das Merged-Binary in zwei Teilen und spart den NVS-Einstellungsbereich (0x9000–0xDFFF) gezielt aus.

**Download:** Die fertigen Apps hängen an jedem GitHub-Release: `Warngeraet-Flasher-macos-apple-silicon.zip`, `-macos-intel.zip`, `-linux-x86_64`, `-windows.exe` — plus `warngeraet-firmware.bin` (das Merged-Binary, das die App lädt; auch manuell flashbar: `esptool --chip esp32s3 write_flash 0x0 warngeraet-firmware.bin` — **Achtung:** anders als die App überschreibt das manuelle Voll-Flashen an 0x0 auch den NVS-Bereich, sämtliche Geräte-Einstellungen müssen danach neu gesetzt werden).

**Geräte-Einstellungen ohne Flashen:** Der Bereich **„4. Gerät"** in der App verbindet sich mit der seriellen Konsole der laufenden Firmware. „Auslesen" zeigt **Node-ID, Spitzname und Firmware-Version** — der Beleg, dass wirklich das erwartete Gerät am Kabel hängt — und füllt die Felder Name, Kanal, Empfindlichkeit, Ton-Stufe und Anzeige-Abschaltzeit. „Übernehmen" schreibt nur die geänderten Werte und liest den tatsächlichen Gerätestand zurück. Hinweis: Das Verbinden startet das Gerät neu (macOS-Treiberverhalten am USB-Port) — die Kanal-Abfrage erscheint erneut und die Tour-Statistik beginnt bei null.

Plattform-Hinweise für Nutzer:

- **macOS:** App aus dem Zip ziehen; beim ersten Start Rechtsklick → „Öffnen" (unsigniertes Programm, Gatekeeper).
- **Windows:** SmartScreen-Warnung mit „Weitere Informationen" → „Trotzdem ausführen" bestätigen.
- **Linux:** ausführbar machen (`chmod +x`) und den Benutzer in die Gruppe `dialout` (Debian/Ubuntu) bzw. `uucp` (Arch) aufnehmen, sonst fehlen die Rechte am seriellen Port.
- Wird das Gerät nicht erkannt: mit gedrückter **BOOT-Taste (B)** einstecken und erneut flashen.

### Flasher aus dem Quelltext starten / entwickeln

```bash
# Einmalig ein Python mit aktuellem Tcl/Tk besorgen. WICHTIG (macOS): NICHT
# /usr/bin/python3 verwenden -- dessen uraltes Tk 8.5 zeichnet auf aktuellem
# macOS keine Widgets mehr, die GUI bleibt ein leeres graues Fenster.
brew install python-tk                             # Homebrew-Python inkl. tkinter/Tk >= 8.6
$(brew --prefix)/bin/python3 -m venv .flasher_venv
./.flasher_venv/bin/pip install -r flasher/requirements.txt
./.flasher_venv/bin/python flasher/flasher.py      # GUI starten
./.flasher_venv/bin/python flasher/flasher.py --check   # Selbsttest ohne GUI, prueft auch die Tk-Version (auch im CI)
```

Eine eigenständige App (ohne Python-Installation lauffähig) baut PyInstaller — **immer nur für das Betriebssystem, auf dem man gerade sitzt** (PyInstaller kann nicht cross-kompilieren; deshalb baut der Release-Workflow auf vier Runnern):

```bash
./.flasher_venv/bin/pip install pyinstaller
# --collect-data esptool ist Pflicht: die Stub-Flasher-JSONs sind Paketdaten,
# ohne die write_flash zur Laufzeit mit "Flasher stub data is missing" abbricht.
./.flasher_venv/bin/pyinstaller --onefile --windowed --collect-data esptool \
    --name Warngeraet-Flasher \
    --distpath dist --workpath build --specpath build flasher/flasher.py
# Ergebnis in dist/: unter macOS Warngeraet-Flasher.app (doppelklickbar),
# unter Linux/Windows ein einzelnes Binary bzw. eine .exe
```

### Release veröffentlichen (Maintainer)

1. **Einmalig:** öffentliches GitHub-Repo anlegen und in `flasher/flasher.py` die Konstante `GITHUB_REPO` vom Platzhalter auf `user/repo` setzen — vorher kann die App nur lokale Dateien flashen.
2. Tag pushen: `git tag v1.0.0 && git push origin main --tags`.
3. `.github/workflows/release.yml` baut dann automatisch das Release: Unit-Tests → Firmware → `warngeraet-firmware.bin` (Merged-Binary) + die vier Flasher-Apps als Assets.

## Provisionierung (einmalig pro Gerät)

Die Node-ID wird automatisch aus der Chip-MAC-Adresse abgeleitet — keine Konfiguration nötig. Nur der **Spitzname** muss einmalig gesetzt werden, über die serielle Konsole (`pio device monitor`):

```
name ROB
```

Beliebiges Kürzel, **nur Buchstaben, max. 5 Zeichen** (keine Zahlen — dieselbe Regel wie beim Namen-ändern-Menü am Gerät, siehe unten) — **kein echter Name nötig**. Der Spitzname wird dauerhaft im Flash gespeichert (bleibt nach Neustart/Akku-leer erhalten).

Die Konsole kann inzwischen **alle** Geräte-Einstellungen (Groß-/Kleinschreibung egal):

| Befehl | Wirkung |
|---|---|
| `help` | Übersicht aller Befehle |
| `id` | Node-ID anzeigen |
| `status` | alle Einstellungen maschinenlesbar als `key=value` (Name, ID, Version, Kanal, Empfindlichkeit, Tonstufe, Anzeige) |
| `name <KÜRZEL>` | Spitznamen setzen |
| `kanal <0–9>` | Funk-Kanal setzen (wirkt sofort) |
| `empfindlich <0–10>` | SCHWACH-Empfindlichkeitsstufe setzen |
| `ton <0–10>` | Piezo-Tonstufe setzen (spielt Testton) |
| `anzeige <0\|15\|30\|60\|300>` | Display-Abschaltzeit in Sekunden (0 = nie) |
| `beep [Hz]` | Testton abspielen |

Wer keinen `pio device monitor` zur Hand hat: Der **Warngerät-Flasher** hat dafür den Bereich „Gerät" (siehe Flasher-Abschnitt oben) — gleiche Befehle, mit Formularfeldern.

**Piezo-Lautstärke abstimmen:** `beep <Hz>` spielt über dieselbe Konsole einen ~600-ms-Testton bei der angegebenen Frequenz (z. B. `beep 3200`), ohne dass neu geflasht werden muss. Ein Software-Lautstärkeregler existiert bei der aktuellen Ein-Transistor-Schaltung nicht (der Piezo wird nur ein-/ausgeschaltet, keine Zwischenstufen) — die Frequenz ist der einzige Hebel, weil ein Piezo an seiner mechanischen Resonanz am lautesten ist. Die tatsächlich genutzte Frequenz ist **kein** Compile-Zeit-Wert mehr, sondern zur Laufzeit einstellbar und im Flash gespeichert (Standard: 3000 Hz, einstellbarer Bereich 2500–3500 Hz) — entweder direkt am Gerät (siehe „Einstellungen" unten; dort als **Stufe 0–10** auf einer Lineal-Skala dargestellt, Stufe = (Hz − 2500) / 100, Standard 3000 Hz = Stufe 5) oder indem man mit `beep <Hz>` erst einen guten Wert findet und ihn dann am Gerät übernimmt.

Die 5-Zeichen-Grenze ist kein Rundwert, sondern gerechnet: Die Ruhe-Anzeige listet die Fahrer in bis zu **drei Spalten**, und dort steckt der Status direkt im Namen — `!NAME!` = Signal fällt, `(NAME)` = Abriss. Ein so dekorierter Name belegt 5 + 2 = 7 Zeichen, und drei 7er-Spalten füllen exakt die 21 Zeichen, die bei 128 px Displaybreite und 6 px/Zeichen (`u8g2_font_6x10_tf`) in eine Zeile passen. Ein längerer Name wird beim `name`-Befehl **abgelehnt** (nicht stillschweigend abgeschnitten), mit einer Fehlermeldung, die auf das Limit hinweist. (Ein unter älterer Firmware gespeicherter 6-Zeichen-Name wird beim ersten Boot automatisch auf 5 gekürzt.)

> **Firmware-Update-Hinweis:** Mit dem 5-Zeichen-Limit hat sich das Funk-Paketformat geändert (Protokoll-Version 2). **v2 ist funk-inkompatibel mit v1** — beide Seiten verwerfen die Pakete der jeweils anderen sauber am Versions-Byte. Alle Geräte des Clubs müssen gemeinsam aktualisiert werden.

## UI-Mockups

`docs/ui-mockups.html` (im Browser öffnen) zeigt **jeden** Bildschirmzustand aus `ui.cpp` als Bild: Kanal-Abfrage beim Boot, Ruhe-Anzeige (Allein / 1-, 2- und 3-Spalten-Fahrerliste / stumm + Akku-Hinweis), Sende-Menü, eingehende Warnung, „Noch abgerissen"-Prompt, Einstellungen, Statistik, Ton und Empfindlichkeit (beide mit Lineal-Skala), Anzeige (Display-Abschaltzeit), Kanal, Namen ändern und Reichweiten-Test (normal + Abriss).

## Bedienung (ein Taster)

**Kanal-Abfrage beim Einschalten:** Direkt nach dem Boot fragt das Gerät zuerst den Funk-Kanal ab (`Kanal waehlen:` mit sichtbarem `Start in Ns`-Countdown), damit man gleich der richtigen Gruppe beitritt. Kurzdruck blättert 0–9 (setzt den Countdown zurück), Langdruck bestätigt; ohne Eingabe bestätigt sich die angezeigte Auswahl nach 10 s selbst — ein unbeaufsichtigter Neustart (z. B. Akku-Aussetzer unterwegs) hängt also nie in der Abfrage. **Bis zur Bestätigung sendet das Gerät keine Heartbeats** und eine eingehende Warnung piept nur, übernimmt aber nicht den Bildschirm; ein Doppelklick bestätigt (statt blind auf einen womöglich falschen Kanal zu warnen). Ein Kanalwechsel leert die Fahrerliste — Fahrer vom alten Kanal wären sonst Minuten später falsche Abrisse.

**Doppelklick = Sofort-Warnung:** Aus **jedem** Zustand (außer der Kanal-Abfrage, s. o.) — auch bei schlafendem Display, auch mitten in einem Menü — sendet ein Doppelklick sofort die generische Warnung **ACHTUNG!**, ohne Menü und ohne hinzusehen. Bewusst generisch: eine Blind-Geste darf nichts Konkretes behaupten, das falsch sein könnte. (Technische Nebenwirkung: Weil die Firmware nach jedem Klick auf einen möglichen zweiten wartet, reagiert ein Einzelklick minimal verzögert — die Wartezeit ist `BUTTON_CLICK_MS` in `src/config.h`.)

| Zustand | Kurzdruck | Langdruck |
|---|---|---|
| Kanal wählen (direkt nach dem Boot) | nächster Kanal (0–9), setzt den Countdown zurück | bestätigt den Kanal, Gerät startet den Funkbetrieb |
| Ruhe-Anzeige | öffnet Sende-Menü | öffnet Einstellungen |
| Sende-Menü | nächste Warnung durchblättern (4 Warnungen) | markierte Warnung senden |
| Eingehende Warnung | wegklicken | (keine Funktion) |
| „Noch abgerissen"-Prompt | Fahrer behalten (nächste Erinnerung in 60 s) | Fahrer aus der Gruppe werfen |
| Einstellungen | nächster Punkt (Stumm/Statistik/Ton/Anzeige/Empfindlich/Kanal/Name/Test/Zurück) | markierten Punkt öffnen bzw. Stumm umschalten |
| Statistik | (hält die Anzeige nur wach) | zurück zur Ruhe-Anzeige |
| Namen ändern | Zeichen durchblättern (nur A–Z) | bestätigen + weiter (leer = Leerzeichen); speichert am Ende |
| Ton einstellen | nächste Stufe (0–10 auf der Lineal-Skala, +100 Hz mit Wrap), spielt Testton | speichert die Stufe, zurück zur Ruhe-Anzeige |
| Anzeige einstellen | nächste Abschaltzeit-Stufe (Nie/15 s/30 s/1 min/5 min) | speichert die Stufe, zurück zur Ruhe-Anzeige |
| Empfindlichkeit einstellen | nächste Stufe (0–10 auf der Lineal-Skala, mit Wrap) | speichert die Stufe, zurück zur Ruhe-Anzeige |
| Kanal einstellen | nächster Kanal (0–9, mit Wrap) | speichert + schaltet sofort um, zurück zur Ruhe-Anzeige |
| Reichweiten-Test | nächster Fahrer | beendet den Test |

- Sende-Menü ohne Aktivität → nach 6 s zurück zur Ruhe-Anzeige (verhindert Fehlsendungen).
- Eingehende Warnung wird nach 15 s automatisch ausgeblendet, falls nicht vorher weggeklickt.
- Es gibt kein Bestätigen (ACK) — eine eingehende Warnung wird nur lokal angezeigt und per Kurzdruck weggeklickt, ohne dass etwas zurückgesendet wird.
- **Senden kann fehlschlagen** (Duty-Cycle-Budget erschöpft oder Funkmodul beim Boot nicht gefunden): dann zeigt das Display `! NICHT gesendet` und es piept 1× kurz — man glaubt also nie fälschlich, gewarnt zu haben. Die ohnehin geplante Wiederholungskopie (400 ms später) wirkt dabei als automatischer Retry und korrigiert den Toast, wenn sie durchkommt.
- Der Piezo-Beeper hat pro Ereignis ein eigenes, am Klang unterscheidbares Muster (unabhängig vom Display-Sleep-Zustand hörbar):
  - **Eingehende Warnung** von jemand anderem → 2× kurz.
  - **SCHWACH** (Fahrer fällt zurück) → 3× kurz.
  - **ABRISS** (Fahrer komplett weg) → 1× lang (~1 s).
  - **ABRISS-Erinnerung**: solange ein Fahrer abgerissen bleibt, alle 60 s 1× kurz (der lange Ton geht im Fahrtwind leicht unter). Ab der **zweiten** Erinnerung wird der Ruhe-Screen zum Prompt `Noch abgerissen: (NAME)` — **Langdruck wirft den Fahrer aus der Gruppe** (zählt nicht mehr, erscheint nicht mehr, erinnert nicht mehr), Kurzdruck oder 15 s Timeout behält ihn. Ein rausgeworfener Fahrer kommt **automatisch zurück**, sobald sein Heartbeat wieder eintrifft (reguläres `ZURUECK`). Offene Menüs/Warnungen werden vom Prompt nie unterbrochen.
  - Beim **eigenen** Senden einer Warnung piept es bewusst **nicht**: da sieht/spürt man Toast und Display ohnehin schon selbst. (Einzige Ausnahme: der Fehlschlag-Beep, siehe oben.)
  - **Eigener Akku schwach** (< 3,5 V) → 1× kurz + `!`-Präfix vor der eigenen Akku-Prozentzahl in der Kopfzeile (einmal pro Low-Episode, mit 100-mV-Hysterese gegen Flattern).
- Fällt der **Akku eines Mitfahrers** unter dieselbe Schwelle, erscheint `<Name> AKKU` in der Ereigniszeile (ohne Beep/Wecken — sein Gerät geht sonst irgendwann „kommentarlos" aus und sieht für alle wie ein Abriss aus). Die Spannung kommt aus dessen Heartbeat.
- Geräte **ohne INA219** zeigen keine Akku-Prozentzahl an (statt irreführender `0%`).
- Keine Status-LED verbaut/angesteuert — im geschlossenen Case ohnehin nicht sichtbar, der Piezo-Beeper übernimmt die "auch ohne Blick aufs Display wahrnehmbar"-Rolle.

### Display-Sleep (Stromsparen)

Das OLED schaltet sich nach einer einstellbaren Zeit ohne Ereignis automatisch ab — Standard 30 s (`OLED_WAKE_MS` in `src/config.h`), am Gerät änderbar unter **Einstellungen → Anzeige** (Stufen: Nie/15 s/30 s/1 min/5 min, im Flash gespeichert; „Nie" = dauerhaft an, kostet spürbar Akku). Das Gerät läuft dabei weiter im Hintergrund (Heartbeats, Roster, Duty-Cycle-Budget — alles unabhängig vom Display). Es wacht wieder auf bei:
- jedem Tasterdruck,
- einer eingehenden Warnung,
- einem SCHWACH- oder ABRISS-Ereignis eines Fahrers.

**Wichtig:** Ist das Display gerade aus, schaltet der **erste** Tasterdruck (kurz oder lang) es nur ein, löst aber **keine Aktion** aus — man bekommt also nie blind eine Warnung raus oder öffnet blind die Einstellungen. Erst der nächste Druck bedient das Menü normal.

### Ruhe-Anzeige: wer ist da, wer fällt zurück?

Die Ruhe-Anzeige listet die Fahrer direkt auf, **schwächstes Signal zuoberst** — die Fahrer, um die es diesem Gerät geht, stehen also immer sichtbar oben. Der Status steckt im Namen selbst: `(NAME)` = Abriss — eingeklammert wie ein Abwesender, sortiert immer ganz nach oben; `!NAME!` = fällt gerade zurück (dasselbe Kriterium wie der SCHWACH-Piep, Ausrufezeichen als aktive Warnung), ` NAME` = alles gut. Rechts in der Kopfzeile stehen `aktiv/gesamt` **inklusive dir selbst** (zwei Fahrer unterwegs = `2/2`) und der eigene Akkustand. Auf dem Zweifarb-Panel leuchtet die Kopfzeile gelb (obere 16 Pixelzeilen); die Fahrerliste beginnt bewusst genau unterhalb dieser Farbgrenze.

Das Layout passt sich der Gruppengröße an:

- **bis 4 Fahrer** — eine Zeile pro Fahrer, zusätzlich mit dem geglätteten RSSI (dBm; `---` bei Abriss) und dem Akkustand aus dessen Heartbeat (`--` ohne dortigen INA219; bei Abriss bleibt der letzte bekannte Wert stehen — er verrät, ob das Gerät schlicht leergelaufen ist):
  ```
  (MAX)     ---  15%
  !LEA!    -108  62%
   ROB      -71   --
  ```
- **5–8 Fahrer** — zwei Namensspalten (ohne RSSI-/Akku-Zahlen).
- **ab 9 Fahrern** — drei Spalten in einer kleineren Schrift (5×7), bis zu 15 Namen.

Ein per „Noch abgerissen"-Prompt **aus der Gruppe geworfener** Fahrer verschwindet aus Liste und Zähler, bis sein Heartbeat wieder eintrifft.

Passen nicht alle aufs Display, werden **nur die schwächsten** gezeigt — wer stark empfangen wird, braucht keinen Platz. Der RSSI-Detailblick pro Fahrer bleibt über den Reichweiten-Test (Einstellungen → Test) verfügbar.

### Einstellungen: Stumm, Statistik, Ton, Anzeige, Empfindlich, Kanal, Name, Test (am Gerät)

Langdruck aus der Ruhe-Anzeige öffnet die Einstellungen — nützlich unterwegs, ohne PlatformIO/Kabel. Genau wie beim Sende-Menü: **Kurzdruck** blättert durch die Punkte (**Stumm → Statistik → Ton → Anzeige → Empfindlich → Kanal → Name → Test → Zurück → ...**, beliebig oft), **Langdruck** öffnet bzw. schaltet den gerade angezeigten Punkt:

- **Stumm** → schaltet alle Alarm-Beeps um (Langdruck wechselt AN/AUS, man bleibt im Menü) — für Pause/Café. Solange aktiv, zeigt die Ruhe-Anzeige mittig `STUMM` in der Kopfzeile. **Bewusst nicht dauerhaft gespeichert:** nach einem Neustart ist das Gerät immer laut, damit es nie unbemerkt stumm in die nächste Ausfahrt geht. Die Testtöne im Ton-Menü bleiben absichtlich hörbar.
- **Statistik** → Übersicht über die laufende Tour: Fahrzeit seit dem Einschalten, gesendete und empfangene Warnungen, Abrisse. Bewusst nicht gespeichert — jede Tour beginnt bei null.
- **Ton** → Frequenz des Piezo-Beepers, dargestellt als **Stufe 0–10 auf einer Lineal-Skala** (Stufe = (Hz − 2500) / 100): Kurzdruck eine Stufe weiter (spielt jeweils einen 500-ms-Testton), am oberen Ende zurück auf Stufe 0. Langdruck speichert dauerhaft im Flash.
- **Anzeige** → Abschaltzeit des Displays: Kurzdruck blättert durch **Nie/15 s/30 s/1 min/5 min**, Langdruck speichert dauerhaft im Flash (siehe „Display-Sleep" oben).
- **Empfindlich** → Empfindlichkeit der SCHWACH-Frühwarnung, gleiche **Lineal-Skala 0–10** (Standard: Stufe 5 in der Mitte). Jede Stufe verschiebt die Warnschwelle um 3 dB:

  | Stufe | Schwelle | Bedeutung |
  |---|---|---|
  | 0 | −120 dBm | unter der Empfangsgrenze → praktisch aus (Abriss-Erkennung bleibt aktiv) |
  | 5 | −105 dBm | Standard (bisheriges Festverhalten) |
  | 10 | −90 dBm | sehr früh — warnt schon bei leichtem Zurückfallen |

  Die zweite Bedingung (nachhaltiger Abfall ≥ 6 dB unter die eigene Baseline) bleibt auf allen Stufen gleich — die Stufe bestimmt nur, **ab welcher Signalstärke** gewarnt wird. Zum Kalibrieren im Feld: Reichweiten-Test (unten) zeigt live den Wert, den diese Logik beurteilt.
- **Kanal** → Funk-Kanal 0–9 der Gruppe (derselbe, den auch die Kanal-Abfrage beim Boot einstellt). **Alle Geräte einer Ausfahrt müssen denselben Kanal haben** — verschiedene Kanäle hören sich gegenseitig überhaupt nicht (ein Kanal-Versehen sieht aus, als wären alle abgerissen). Langdruck speichert, schaltet sofort um und leert die Fahrerliste (Fahrer vom alten Kanal wären sonst falsche Abrisse). Standard: Kanal 0. Nützlich, wenn zwei Gruppen gleichzeitig unterwegs sind.
- **Name** → wie im übernächsten Abschnitt beschrieben.
- **Test** → Reichweiten-Test, siehe nächster Abschnitt.
- **Zurück** → verlässt die Einstellungen sofort, ohne etwas zu ändern.
- Ohne Tastendruck für 6 s geht es automatisch zurück zur Ruhe-Anzeige (in Ton/Empfindlich/Kanal ohne zu speichern — genau wie beim Namen ändern, siehe unten).
- Alternativ über die serielle Konsole testen (`beep <Hz>`, siehe "Piezo-Lautstärke abstimmen" oben) — praktisch, um vorab einen guten Wert zu finden, bevor man ihn am Gerät fest einstellt.

### Reichweiten-Test (Feldtest-Werkzeug)

Einstellungen → **Test** zeigt für einen Fahrer live: groß den geglätteten RSSI (der Wert, den die SCHWACH-Logik beurteilt), darunter den Roh-RSSI des letzten Heartbeats und dessen Alter. Kurzdruck wechselt den beobachteten Fahrer, Langdruck beendet. **Kein Timeout, kein Display-Sleep** in diesem Modus — ein Reichweiten-Spaziergang dauert Minuten, und man hat den Modus ja ausdrücklich gestartet. Genau das Werkzeug, um die geschätzten ~400–600 m Reichweite und die `RSSI_FALLING_BACK_*`-Schwellen im Feld zu verifizieren (siehe [Verifikation](#verifikation)).

### Namen am Gerät ändern (ohne Laptop)

Sobald in den Einstellungen "Name" mit Langdruck bestätigt wurde, wird der Name zeichenweise eingegeben:

- Start: alle 5 Positionen sind leer, die Eingabe beginnt bei Position 1.
- **Kurzdruck** blättert das markierte Zeichen ausschließlich durch `A`–`Z` — **keine Zahlen, kein Leerzeichen** in dieser Rotation.
- **Langdruck** bestätigt das aktuelle Zeichen (egal ob durchgeblättert oder noch unverändert leer) und springt zur nächsten Position — die dabei **immer leer startet**. Ein Leerzeichen entsteht also gezielt dadurch, dass man an einer Position long-presst, ohne vorher zu klicken.
- Für einen **kürzeren Namen** (z. B. "ROB"): einfach nach dem letzten gewünschten Zeichen zweimal in Folge long-pressen, ohne dazwischen zu klicken — zwei leere Positionen in Folge werden als "fertig" erkannt, der Name wird sofort (rechts getrimmt) gespeichert.
- Werden alle 5 Positionen bestätigt, wird ebenfalls automatisch gespeichert.
- Immer wenn der nächste Langdruck speichern würde (zweites Leerzeichen in Folge, oder die 5. Position ist erreicht), zeigt die Fußzeile statt `kurz=Zeichen lang=OK` den Hinweis **`lang=fertig`** an.
- Ohne Tastendruck für 6 s wird die Bearbeitung **verworfen** (nichts wird gespeichert) und es geht zurück zur Ruhe-Anzeige.
- Das 5-Zeichen-Limit ist dasselbe wie beim `name`-Befehl über die serielle Konsole (siehe Provisionierung oben) — hier kann es aber gar nicht überschritten werden, weil nur 5 Positionen zum Durchblättern angezeigt werden.

## Wie die Abriss-Erkennung funktioniert

Jedes Gerät sendet alle ~2 s (leicht zufällig verschoben) einen Heartbeat mit Spitzname + Akkuspannung. Jeder Empfänger führt daraus pro Fahrer **zwei** geglättete Signalstärke-Mittel (RSSI-EMAs): ein schnelles („wo das Signal gerade ist") und ein träges als Baseline („wo es normalerweise liegt", Zeitkonstante ~40 s):

- **"Fällt zurück"**: das schnelle Mittel ist unter die **einstellbare Schwelle** gefallen (Einstellungen → Empfindlich, Stufe 0–10 in 3-dB-Schritten von −120 bis −90 dBm; Standard Stufe 5 = −105 dBm) **und** liegt ≥6 dB unter der trägen Baseline → Frühwarnung auf der Ruhe-Anzeige. Der Vergleich gegen die Baseline (statt gegen den unmittelbar vorherigen Heartbeat) erkennt auch **allmähliches** Zurückfallen — mit nur einer EMA hätte der Roh-RSSI innerhalb eines einzigen Heartbeat-Intervalls um ~17 dB einbrechen müssen.
- **"Abriss"**: ~5,8 s lang gar kein Signal mehr → klarer Alarm samt Zeitstempel. Die Schwelle (`DROPPED_OFF_TIMEOUT_MS`) budgetiert bewusst 2 volle Intervalle **inklusive Sende-Jitter** plus Marge — ein einzelner verlorener Heartbeat (z. B. weil sich zwei Halbduplex-Sender überlappt haben) löst so keinen Fehlalarm aus, erst zwei fehlende in Folge.

Beides ist automatisch, ohne Tastendruck. Schwellwerte stehen als Konstanten in `src/config.h` (`RSSI_FALLING_BACK_*`, `RSSI_EMA_ALPHA_*`, `DROPPED_OFF_MISSED_INTERVALS`/`DROPPED_OFF_TIMEOUT_MS`) und lassen sich nach ersten Feldtests nachjustieren.

## Funkprotokoll (Kurzreferenz)

EU868, 868,3 MHz, **GFSK** (nicht LoRa) bei 19,2 kb/s, ~10 kHz Frequenzhub, 46,9 kHz RX-Bandbreite, Gaussian-Shaping (BT=0,5), eigenes Syncword (kein LoRaWAN). Der **Gruppen-Kanal** (Einstellungen → Kanal, 0–9) verschiebt das zweite Syncword-Byte — Geräte auf verschiedenen Kanälen sind füreinander unsichtbar; alle senden aber weiterhin auf derselben Frequenz und teilen sich die Luft. Der Duty-Cycle wird in Software als rollierendes 1-Stunden-Budget mitgeführt (`src/radio.cpp`) — Sendungen werden zurückgehalten, falls das gesetzlich erlaubte 1 %-Budget erschöpft ist.

**Warum GFSK statt LoRa:** Für eine Ausfahrt reichen ein paar hundert Meter Reichweite — LoRas Kernvorteil (mehrere Kilometer) wird hier nicht gebraucht. GFSK hat bei 19,2 kb/s eine deutlich kürzere Time-on-Air pro Paket (~9 ms statt ~82 ms bei LoRa SF8), wodurch der Heartbeat im selben gesetzlichen 1 %-Budget viel öfter gesendet werden kann (~2 s statt ~15 s) — das ist der Hebel, der die Abriss-/Lücken-Erkennung schneller macht. Geschätzte Reichweite bei dieser Bitrate: ~400–600 m (Empfindlichkeit ~−111 dBm vs. ~−127 dBm bei LoRa SF8) — im Feld verifizieren, bevor man sich darauf verlässt. Warnungen laufen mit im selben GFSK-Modus (ein SX1262 kann nicht gleichzeitig LoRa und GFSK empfangen).

**Hinweis zur Duty-Cycle-Berechnung:** RadioLibs `getTimeOnAir()` zählt bei FSK nur die reinen Payload-Bits, nicht Preamble/Syncword/Längen-Byte/CRC. `Radio::fullFrameTimeOnAirUs()` in `src/radio.cpp` rechnet das vollständige Frame selbst nach, damit das 1 %-Budget den tatsächlichen Sendezustand widerspiegelt.

## Verifikation

Mit **mindestens 2 Geräten**:

1. `pio run -t upload` auf beide (alle Geräte zusammen — v2 ist funk-inkompatibel mit v1, s. o.), dann je einmal Spitznamen setzen (Konsole, oder bequemer: Flasher → „Gerät").
2. Beide einschalten → die **Kanal-Abfrage** erscheint mit Countdown; Langdruck (oder 10 s warten) bestätigt Kanal 0. Die Ereigniszeile zeigt danach kurz die Firmware-Version (`FW v0.1.x`), und nach wenigen Sekunden erscheint der jeweils andere Fahrer in der Liste (mit RSSI) samt `2/2` in der Kopfzeile (der Zähler schließt das eigene Gerät ein).
3. Kurzdruck → durchblättern → Langdruck auf "AUTO HINTEN" → das andere Gerät zeigt die Warnung + piept 2× kurz. Kurzdruck dort blendet sie wieder aus.
4. **Doppelklick** an einem Gerät (auch bei schlafendem Display) → das andere zeigt sofort `ACHTUNG!`.
5. Geräte ≥ 30 min nebeneinander liegen lassen → **keine** spontanen `ABRISS`/`ZURUECK`-Ereignisse in der Ereigniszeile (genau das war der Fehlalarm-Bug der 4-s-Schwelle ohne Jitter-Marge).
6. Ein Gerät ausschalten / weit weggehen → nach ~6 s erscheint beim anderen `<Name> ABRISS` in der letzten-Ereignis-Zeile (und das Display wacht dafür automatisch auf, falls es gerade schlief); bei allmählicher Entfernung vorher `<Name> SCHWACH`. In der Liste rückt der Fahrer als `(NAME)` nach ganz oben.
7. **Stumm** (Einstellungen) aktivieren → eingehende Warnung erscheint ohne Beep, Kopfzeile zeigt `STUMM`. Danach wieder ausschalten.
8. **ABRISS-Erinnerung**: ein Gerät ausgeschaltet lassen → nach ~60 s 1 kurzer Erinnerungs-Piep, nach ~120 s der Prompt `Noch abgerissen: (NAME)` → Langdruck entfernt den Fahrer (verschwindet aus Liste und Zähler, keine weiteren Erinnerungen). Gerät wieder einschalten → Fahrer kommt automatisch mit `ZURUECK` zurück.
9. **Kanal-Test**: ein Gerät auf Kanal 1 stellen (Einstellungen → Kanal) → die Geräte verlieren sich (ABRISS nach ~6 s); zurück auf 0 → `ZURUECK`.
10. **Empfindlichkeit**: auf Stufe 10 stellen und ein Gerät in den Nebenraum legen → `SCHWACH` kommt deutlich früher als bei Stufe 5; auf Stufe 0 kommt gar kein SCHWACH mehr, nur der ABRISS.
11. **Statistik** (Einstellungen) → Fahrzeit läuft, Warn-/Abriss-Zähler passen zu den vorherigen Testschritten.
12. Erst wenn die Schritte am Tisch funktionieren, im Feld (echte Ausfahrt) testen — dafür gibt es jetzt den **Reichweiten-Test** (Einstellungen → Test): die tatsächliche GFSK-Reichweite über die im Peloton relevanten Distanzen live ablesen (die ~400–600 m sind eine Schätzung aus dem Datenblatt, kein Feldmesswert) und Heartbeat-Intervall/RSSI-Schwellen bei Bedarf feinjustieren.

## Offene Punkte für später

- 868-MHz-Antenne vor dem ersten Sendetest verifizieren.
- Optional: AES-Verschlüsselung der Pakete (aktuell aus, Format in `src/protocol.h` vorbereitet für eine spätere Erweiterung).
- Ausbaustufe 2: dedizierte Taster pro Warnung (STL-Anpassung) + Vibrationsmotor (Piezo-Beeper ist bereits umgesetzt), damit Alarme auch ohne Blick aufs Display wahrnehmbar sind; ggf. GPS für echte Distanzangaben, SOS-Funktion, Sturzerkennung.
