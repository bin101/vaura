#!/usr/bin/env python3
"""Warngeraet-Flasher: GUI zum Flashen des Rennradclub-Warngeraets.

Laedt die aktuellste Firmware aus den GitHub-Releases des Projekts (oder eine
lokal ausgewaehlte .bin-Datei) und flasht sie per esptool auf den XIAO
ESP32-S3 -- ohne PlatformIO, ohne Kompilieren. Laeuft unter macOS, Linux und
Windows; fertige Ein-Datei-Apps baut .github/workflows/release.yml per
PyInstaller.

Start aus dem Quelltext:  pip install -r requirements.txt && python flasher.py
Selbsttest ohne GUI:      python flasher.py --check
"""

import json
import queue
import re
import sys
import tempfile
import threading
import urllib.request
from pathlib import Path

# ---------------------------------------------------------------------------
# !!! VOR DEM ERSTEN RELEASE ANPASSEN !!!
# GitHub-Repo (oeffentlich), aus dessen Releases der Flasher die Firmware
# laedt -- Format "user/repo". Solange der Platzhalter unveraendert ist,
# funktioniert nur "Lokale Datei ..." (der Download-Knopf erklaert das dann).
# ---------------------------------------------------------------------------
GITHUB_REPO = "DEIN-GITHUB-USER/occ_lora"

# Name des Firmware-Assets im Release -- muss zu release.yml passen. Es ist
# das Merged-Binary (Bootloader + Partitionstabelle + App in einer Datei),
# geflasht als Ganzes an Offset 0x0.
FIRMWARE_ASSET_NAME = "warngeraet-firmware.bin"

APP_TITLE = "Warngerät-Flasher"
ESPRESSIF_USB_VID = 0x303A  # native USB des ESP32-S3
FLASH_BAUD = "460800"  # auf dem nativen USB-CDC ohnehin nur nominell

# NVS-Partition im Partitionslayout der Firmware (zwischen Partitionstabelle
# bei 0x8000 und boot_app0 bei 0xE000). Das Merged-Binary fuellt diese Luecke
# mit 0xFF-Padding -- an 0x0 in einem Stueck geschrieben wuerde das die dort
# gespeicherten Einstellungen (Spitzname, Tonfrequenz) loeschen. Deshalb wird
# das Image in zwei Teilen um die Region herum geflasht (siehe flash_firmware).
NVS_START = 0x9000
NVS_END = 0xE000

# Aeltere Tk-Versionen (macOS-System-Python bringt 8.5.9 von 2010 mit) zeichnen
# auf aktuellem macOS keine Widgets mehr -- nur ein leeres graues Fenster.
MIN_TK_VERSION = 8.6


def list_serial_ports():
    """[(device, beschreibung, ist_wahrscheinlich_warngeraet), ...]"""
    from serial.tools import list_ports

    ports = []
    for p in sorted(list_ports.comports(), key=lambda p: p.device):
        likely = p.vid == ESPRESSIF_USB_VID
        ports.append((p.device, p.description or "", likely))
    return ports


def fetch_latest_release():
    """(tag, download_url) des neuesten Releases, wirft bei Fehlern."""
    if "DEIN-GITHUB-USER" in GITHUB_REPO:
        raise RuntimeError(
            "In flasher.py ist noch kein GitHub-Repo eingetragen (GITHUB_REPO).\n"
            "Bitte 'Lokale Datei ...' verwenden oder die Konstante setzen."
        )
    url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
    req = urllib.request.Request(url, headers={"User-Agent": APP_TITLE})
    with urllib.request.urlopen(req, timeout=15) as resp:
        release = json.load(resp)
    for asset in release.get("assets", []):
        if asset.get("name") == FIRMWARE_ASSET_NAME:
            return release.get("tag_name", "?"), asset["browser_download_url"]
    raise RuntimeError(
        f"Neuestes Release ({release.get('tag_name', '?')}) enthaelt kein "
        f"Asset namens {FIRMWARE_ASSET_NAME}."
    )


def download_firmware(url, progress_cb=None):
    """Laedt die Firmware in eine Temp-Datei, gibt den Pfad zurueck."""
    req = urllib.request.Request(url, headers={"User-Agent": APP_TITLE})
    fd, path = tempfile.mkstemp(prefix="warngeraet-", suffix=".bin")
    with urllib.request.urlopen(req, timeout=60) as resp, open(fd, "wb") as out:
        total = int(resp.headers.get("Content-Length") or 0)
        done = 0
        while True:
            chunk = resp.read(65536)
            if not chunk:
                break
            out.write(chunk)
            done += len(chunk)
            if progress_cb and total:
                progress_cb(done * 100 // total)
    return path


class _LogWriter:
    """stdout-Ersatz waehrend esptool laeuft: Zeilen -> GUI-Queue, Prozente
    aus esptools "Writing at 0x... (NN %)"-Ausgabe -> Fortschrittsbalken.

    Reentranz-Schutz: Schreibt der log_cb seinerseits nach stdout (z. B. ein
    schlichtes print im --check/CLI-Betrieb), landet das wieder HIER -- ohne
    Schutz eine Endlosrekursion. Solche re-entranten Schreibzugriffe gehen
    unveraendert an den echten (gesicherten) stdout durch."""

    PERCENT_RE = re.compile(r"\((\d+)\s*%\)")

    def __init__(self, log_cb, progress_cb, fallback):
        self.log_cb = log_cb
        self.progress_cb = progress_cb
        self.fallback = fallback
        self._buf = ""
        self._dispatching = False

    def write(self, text):
        if self._dispatching:
            if self.fallback is not None:
                self.fallback.write(text)
            return
        self._buf += text
        # esptool malt den Schreibfortschritt mit \r in dieselbe Zeile
        while True:
            for sep in ("\n", "\r"):
                if sep in self._buf:
                    line, self._buf = self._buf.split(sep, 1)
                    break
            else:
                return
            line = line.strip()
            if not line:
                continue
            self._dispatching = True
            try:
                m = self.PERCENT_RE.search(line)
                if m:
                    self.progress_cb(int(m.group(1)))
                else:
                    self.log_cb(line)
            finally:
                self._dispatching = False

    def flush(self):
        if self.fallback is not None:
            self.fallback.flush()


def run_esptool(args, log_cb, progress_cb):
    """Fuehrt esptool in-process aus (kein Subprozess: in der PyInstaller-App
    gibt es kein separates Python). Gibt True bei Erfolg zurueck."""
    import esptool

    old_stdout, old_stderr = sys.stdout, sys.stderr
    writer = _LogWriter(log_cb, progress_cb, fallback=old_stdout)
    sys.stdout, sys.stderr = writer, writer
    try:
        esptool.main(args)
        return True
    except SystemExit as e:  # esptool beendet Fehlerpfade via sys.exit()
        return e.code in (0, None)
    except Exception as e:  # noqa: BLE001 -- alles im Log zeigen, GUI lebt weiter
        sys.stdout, sys.stderr = old_stdout, old_stderr
        log_cb(f"FEHLER: {e}")
        return False
    finally:
        sys.stdout, sys.stderr = old_stdout, old_stderr


def split_around_nvs(firmware_path):
    """Zerlegt ein Merged-Binary in (offset, temp_pfad)-Teile, die die
    NVS-Region aussparen -- so ueberleben Spitzname und Tonfrequenz das
    Flashen. Gibt None zurueck, wenn die Datei zu kurz ist, um die Region
    ueberhaupt zu enthalten (dann ist es kein Merged-Binary)."""
    data = Path(firmware_path).read_bytes()
    if len(data) <= NVS_END:
        return None
    parts = []
    for offset, chunk in ((0x0, data[:NVS_START]), (NVS_END, data[NVS_END:])):
        fd, path = tempfile.mkstemp(prefix="warngeraet-teil-", suffix=".bin")
        with open(fd, "wb") as out:
            out.write(chunk)
        parts.append((offset, path))
    return parts


def flash_firmware(port, firmware_path, log_cb, progress_cb):
    args = [
        "--chip", "esp32s3",
        "--port", port,
        "--baud", FLASH_BAUD,
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
    ]
    parts = split_around_nvs(firmware_path)
    if parts is not None:
        for offset, path in parts:
            args += [hex(offset), path]
        log_cb("Flashe in 2 Teilen -- der Einstellungsbereich des Geraets "
               "(Spitzname, Tonfrequenz usw.) bleibt unberuehrt.")
    else:
        args += ["0x0", str(firmware_path)]
        log_cb("Datei enthaelt die NVS-Region nicht (kein Merged-Binary?) -- "
               "flashe unveraendert an 0x0.")
    log_cb(f"Starte Flash-Vorgang auf {port} ...")
    try:
        ok = run_esptool(args, log_cb, progress_cb)
    finally:
        for _, path in parts or []:
            Path(path).unlink(missing_ok=True)
    if ok:
        progress_cb(100)
        log_cb("Fertig! Das Geraet startet jetzt mit der neuen Firmware.")
    else:
        log_cb(
            "Flashen fehlgeschlagen. Tipps: anderes USB-Kabel/Port probieren; "
            "das Geraet mit gedrueckter BOOT-Taste (B) einstecken und erneut "
            "flashen; unter Linux pruefen, ob der Benutzer in der Gruppe "
            "'dialout' (bzw. 'uucp') ist."
        )
    return ok


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------
class FlasherApp:
    def __init__(self, root):
        import tkinter as tk
        from tkinter import ttk

        self.tk = tk
        self.root = root
        self.firmware_path = None
        self.firmware_label_text = None
        self.busy = False
        self.msg_queue = queue.Queue()

        root.title(APP_TITLE)
        root.minsize(560, 460)

        main = ttk.Frame(root, padding=14)
        main.pack(fill="both", expand=True)

        # --- Firmware-Quelle ---
        fw = ttk.LabelFrame(main, text="1. Firmware", padding=10)
        fw.pack(fill="x")
        self.btn_download = ttk.Button(fw, text="Neueste Version herunterladen",
                                       command=self.on_download)
        self.btn_download.pack(side="left")
        self.btn_local = ttk.Button(fw, text="Lokale Datei ...", command=self.on_pick_file)
        self.btn_local.pack(side="left", padx=(8, 0))
        self.lbl_firmware = ttk.Label(fw, text="noch keine Firmware gewaehlt")
        self.lbl_firmware.pack(side="left", padx=(12, 0))

        # --- Port ---
        pt = ttk.LabelFrame(main, text="2. USB-Port (Geraet per USB-C anschliessen)", padding=10)
        pt.pack(fill="x", pady=(10, 0))
        self.port_var = tk.StringVar()
        self.cmb_port = ttk.Combobox(pt, textvariable=self.port_var, state="readonly", width=44)
        self.cmb_port.pack(side="left")
        ttk.Button(pt, text="Aktualisieren", command=self.refresh_ports).pack(side="left", padx=(8, 0))

        # --- Flashen ---
        fl = ttk.LabelFrame(main, text="3. Flashen", padding=10)
        fl.pack(fill="both", expand=True, pady=(10, 0))
        self.btn_flash = ttk.Button(fl, text="Firmware flashen", command=self.on_flash)
        self.btn_flash.pack(anchor="w")
        self.progress = ttk.Progressbar(fl, maximum=100)
        self.progress.pack(fill="x", pady=(8, 6))
        self.txt_log = tk.Text(fl, height=12, state="disabled", wrap="word",
                               font=("Courier", 11) if sys.platform == "darwin" else ("TkFixedFont", 9))
        self.txt_log.pack(fill="both", expand=True)

        ttk.Label(main, foreground="#666", wraplength=520, justify="left", text=(
            "Hinweis: Falls das Geraet nicht erkannt wird, mit gedrueckter "
            "BOOT-Taste (B) einstecken. Alle auf dem Geraet gespeicherten "
            "Einstellungen bleiben beim Flashen erhalten."
        )).pack(fill="x", pady=(10, 0))

        self.refresh_ports()
        self.update_buttons()
        self.pump_queue()

        if tk.TkVersion < MIN_TK_VERSION:
            # Bestenfalls liest das noch jemand im Log -- auf macOS ist bei
            # Tk 8.5 typischerweise das ganze Fenster leer/grau (siehe README,
            # Abschnitt "Flasher aus dem Quelltext starten").
            self.log(
                f"WARNUNG: Tk {tk.TkVersion} ist zu alt (mindestens {MIN_TK_VERSION} "
                "noetig) -- die Oberflaeche wird vermutlich nicht korrekt gezeichnet. "
                "Bitte ein Python mit aktuellem Tk verwenden, z. B. via "
                "'brew install python-tk'."
            )

    # --- Hilfen -----------------------------------------------------------
    def log(self, line):
        self.msg_queue.put(("log", line))

    def set_progress(self, percent):
        self.msg_queue.put(("progress", percent))

    def pump_queue(self):
        try:
            while True:
                kind, payload = self.msg_queue.get_nowait()
                if kind == "log":
                    self.txt_log.configure(state="normal")
                    self.txt_log.insert("end", payload + "\n")
                    self.txt_log.see("end")
                    self.txt_log.configure(state="disabled")
                elif kind == "progress":
                    self.progress["value"] = payload
                elif kind == "firmware":
                    self.firmware_path, text = payload
                    self.lbl_firmware.configure(text=text)
                elif kind == "done":
                    self.busy = False
                    self.update_buttons()
        except queue.Empty:
            pass
        self.root.after(80, self.pump_queue)

    def update_buttons(self):
        state = "disabled" if self.busy else "normal"
        self.btn_download.configure(state=state)
        self.btn_local.configure(state=state)
        can_flash = not self.busy and self.firmware_path and self.port_var.get()
        self.btn_flash.configure(state="normal" if can_flash else "disabled")

    def refresh_ports(self):
        ports = list_serial_ports()
        values, preselect = [], None
        for device, desc, likely in ports:
            label = f"{device}  ({desc})" if desc else device
            if likely:
                label += "  ← vermutlich das Warngeraet"
                preselect = preselect or label
            values.append(label)
        self.cmb_port["values"] = values
        current = self.port_var.get()
        if preselect:
            self.port_var.set(preselect)
        elif current not in values:
            self.port_var.set(values[0] if values else "")
        self.update_buttons()

    def selected_port(self):
        return self.port_var.get().split("  (")[0].strip()

    # --- Aktionen ---------------------------------------------------------
    def on_pick_file(self):
        from tkinter import filedialog

        path = filedialog.askopenfilename(
            title="Firmware-Datei waehlen (Merged-Binary aus dem Release)",
            filetypes=[("Firmware", "*.bin"), ("Alle Dateien", "*")])
        if path:
            self.firmware_path = path
            self.lbl_firmware.configure(text=Path(path).name)
            self.update_buttons()

    def on_download(self):
        self.busy = True
        self.update_buttons()
        self.progress["value"] = 0

        def worker():
            try:
                tag, url = fetch_latest_release()
                self.log(f"Neueste Version: {tag} -- lade {FIRMWARE_ASSET_NAME} ...")
                path = download_firmware(url, self.set_progress)
                self.msg_queue.put(("firmware", (path, f"{FIRMWARE_ASSET_NAME} ({tag})")))
                self.log("Download abgeschlossen.")
            except Exception as e:  # noqa: BLE001
                self.log(f"Download fehlgeschlagen: {e}")
            finally:
                self.msg_queue.put(("done", None))

        threading.Thread(target=worker, daemon=True).start()

    def on_flash(self):
        port = self.selected_port()
        firmware = self.firmware_path
        if not port or not firmware:
            return
        self.busy = True
        self.update_buttons()
        self.progress["value"] = 0

        def worker():
            try:
                flash_firmware(port, firmware, self.log, self.set_progress)
            finally:
                self.msg_queue.put(("done", None))

        threading.Thread(target=worker, daemon=True).start()


def self_check():
    """--check: Importe, Tk-Version + Portsuche ohne GUI (fuer CI/Smoke-Test)."""
    import esptool  # noqa: F401
    import serial  # noqa: F401
    import tkinter

    patchlevel = tkinter.Tcl().call("info", "patchlevel")
    if tkinter.TkVersion < MIN_TK_VERSION:
        print(f"FEHLER: Tk {patchlevel} ist zu alt (mindestens {MIN_TK_VERSION} noetig) -- "
              "die GUI bleibt damit ein leeres graues Fenster. Python mit aktuellem "
              "Tk verwenden, z. B. via 'brew install python-tk' (macOS).")
        return 1
    ports = list_serial_ports()
    print(f"OK: esptool/pyserial/tkinter importierbar, Tk {patchlevel}, "
          f"{len(ports)} serielle(r) Port(s):")
    for device, desc, likely in ports:
        print(f"  {device}  {desc}{'  <- Espressif-USB' if likely else ''}")
    return 0


def main():
    if "--check" in sys.argv:
        sys.exit(self_check())

    import tkinter as tk

    root = tk.Tk()
    FlasherApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
