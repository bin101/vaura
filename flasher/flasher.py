#!/usr/bin/env python3
"""Vaura Flasher: GUI for flashing the Vaura cycling club warning device.

Downloads the latest firmware from the project's GitHub releases (or a
locally selected .bin file) and flashes it via esptool onto the XIAO
ESP32-S3 -- no PlatformIO, no compiling. Runs on macOS, Linux and Windows;
ready-to-run single-file apps are built by .github/workflows/release.yml
via PyInstaller.

Run from source:       pip install -r requirements.txt && python flasher.py
Self-test without GUI: python flasher.py --check
"""

import json
import queue
import re
import ssl
import sys
import tempfile
import threading
import time
import urllib.request
from pathlib import Path

import certifi

# ---------------------------------------------------------------------------
# GitHub repo (public) whose releases the flasher downloads firmware from --
# format "user/repo". Update this if you fork the project under a different
# repo; while it points to a nonexistent/placeholder repo, only "Local
# file ..." works (the download button explains that then).
# ---------------------------------------------------------------------------
GITHUB_REPO = "bin101/vaura"

# Name of the firmware asset in the release -- must match release.yml. It's
# the merged binary (bootloader + partition table + app in one file),
# flashed as a whole at offset 0x0.
FIRMWARE_ASSET_NAME = "vaura-firmware.bin"

APP_TITLE = "Vaura Flasher"
ESPRESSIF_USB_VID = 0x303A  # the ESP32-S3's native USB
FLASH_BAUD = "460800"  # only nominal anyway on the native USB-CDC

# NVS partition in the firmware's partition layout (between the partition
# table at 0x8000 and boot_app0 at 0xE000). The merged binary fills this gap
# with 0xFF padding -- writing it in one piece at 0x0 would erase the
# settings stored there (nickname, tone frequency). So the image is flashed
# in two pieces around that region (see flash_firmware).
NVS_START = 0x9000
NVS_END = 0xE000

# Older Tk versions (macOS's system Python ships 8.5.9 from 2010) no longer
# draw widgets on current macOS -- just an empty gray window.
MIN_TK_VERSION = 8.6

# certifi's bundle instead of the system trust store -- some Python installs
# (notably python.org builds on macOS, and the frozen PyInstaller app) don't
# have it wired up, which makes urlopen() fail with CERTIFICATE_VERIFY_FAILED.
SSL_CONTEXT = ssl.create_default_context(cafile=certifi.where())

# Serial console of the running firmware (read/write settings).
DEVICE_BAUD = 115200
# Keys of the `status` output (config.cpp) -- part of the interface.
SETTINGS_KEYS = ("name", "id", "version", "channel", "sensitivity", "tone", "display")


def open_device_serial(port):
    """Opens the device's console port. DTR/RTS stay low because the
    ESP32-S3's USB-Serial/JTAG treats toggling them as a reset command --
    but at least the macOS CDC driver still toggles on open/close (observed:
    rst:0x15 USB_UART_CHIP_RESET), so the device reboots anyway. Callers
    must therefore wait it out via wait_for_device_boot()."""
    import serial

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = DEVICE_BAUD
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def wait_for_device_boot(ser, max_s=4.0, settle_s=0.6):
    """Waits after opening for any boot to finish: if nothing arrives within
    1 s, the device just kept running; if boot lines arrive, settle_s of
    silence after the last line counts as "console ready"."""
    start = time.time()
    last_data = 0.0
    while time.time() - start < max_s:
        if ser.readline():
            last_data = time.time()
            continue
        if last_data == 0.0:
            if time.time() - start > 1.0:
                return
        elif time.time() - last_data > settle_s:
            return


def send_console_command(ser, command, wait_s=1.5):
    """Sends one console line and collects the response lines. Bails out
    early once the first response lines are followed by a brief silence."""
    ser.reset_input_buffer()
    ser.write((command + "\n").encode("ascii", errors="replace"))
    ser.flush()
    lines = []
    deadline = time.time() + wait_s
    while time.time() < deadline:
        raw = ser.readline()
        if raw:
            line = raw.decode(errors="replace").strip()
            if line:
                lines.append(line)
        elif lines:
            break
    return lines


def parse_status_lines(lines):
    """key=value lines from the `status` response -> dict; unrelated log
    lines (radio messages etc.) simply fall through the filter."""
    settings = {}
    for line in lines:
        if "=" in line:
            key, value = line.split("=", 1)
            key = key.strip()
            if key in SETTINGS_KEYS:
                settings[key] = value.strip()
    return settings


def read_device_settings(port):
    """Reads the identity + settings of the connected device."""
    ser = open_device_serial(port)
    try:
        wait_for_device_boot(ser)
        return parse_status_lines(send_console_command(ser, "status", wait_s=2.0))
    finally:
        ser.close()


def apply_device_settings(port, commands, log_cb):
    """Sends set commands and then reads back the new status -- the
    feedback shown in the GUI is always the device's actual current state."""
    ser = open_device_serial(port)
    try:
        wait_for_device_boot(ser)
        for command in commands:
            log_cb(f"> {command}")
            for line in send_console_command(ser, command):
                if line.startswith(("OK", "Error", "Saved")):
                    log_cb(f"  {line}")
        return parse_status_lines(send_console_command(ser, "status", wait_s=2.0))
    finally:
        ser.close()


def list_serial_ports():
    """[(device, description, is_likely_the_vaura_device), ...]"""
    from serial.tools import list_ports

    ports = []
    for p in sorted(list_ports.comports(), key=lambda p: p.device):
        likely = p.vid == ESPRESSIF_USB_VID
        ports.append((p.device, p.description or "", likely))
    return ports


def fetch_latest_release():
    """(tag, download_url) of the latest release, raises on errors."""
    if "YOUR-GITHUB-USER" in GITHUB_REPO:
        raise RuntimeError(
            "flasher.py has no GitHub repo configured yet (GITHUB_REPO).\n"
            "Use 'Local file ...' or set the constant."
        )
    url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
    req = urllib.request.Request(url, headers={"User-Agent": APP_TITLE})
    with urllib.request.urlopen(req, timeout=15, context=SSL_CONTEXT) as resp:
        release = json.load(resp)
    for asset in release.get("assets", []):
        if asset.get("name") == FIRMWARE_ASSET_NAME:
            return release.get("tag_name", "?"), asset["browser_download_url"]
    raise RuntimeError(
        f"Latest release ({release.get('tag_name', '?')}) has no asset "
        f"named {FIRMWARE_ASSET_NAME}."
    )


def download_firmware(url, progress_cb=None):
    """Downloads the firmware to a temp file, returns the path."""
    req = urllib.request.Request(url, headers={"User-Agent": APP_TITLE})
    fd, path = tempfile.mkstemp(prefix="vaura-", suffix=".bin")
    with urllib.request.urlopen(req, timeout=60, context=SSL_CONTEXT) as resp, open(fd, "wb") as out:
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
    """stdout replacement while esptool runs: lines -> GUI queue, percentages
    from esptool's "Writing at 0x... (NN %)" output -> progress bar.

    Reentrancy guard: if log_cb itself writes to stdout (e.g. a plain print
    in --check/CLI mode), that lands right back HERE -- without the guard,
    infinite recursion. Such reentrant writes pass through unchanged to the
    real (saved) stdout."""

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
        # esptool paints the write progress with \r into the same line
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
    """Runs esptool in-process (no subprocess: the PyInstaller app has no
    separate Python). Returns True on success."""
    import esptool

    old_stdout, old_stderr = sys.stdout, sys.stderr
    writer = _LogWriter(log_cb, progress_cb, fallback=old_stdout)
    sys.stdout, sys.stderr = writer, writer
    try:
        esptool.main(args)
        return True
    except SystemExit as e:  # esptool exits error paths via sys.exit()
        return e.code in (0, None)
    except Exception as e:  # noqa: BLE001 -- show everything in the log, GUI stays alive
        sys.stdout, sys.stderr = old_stdout, old_stderr
        log_cb(f"ERROR: {e}")
        return False
    finally:
        sys.stdout, sys.stderr = old_stdout, old_stderr


def split_around_nvs(firmware_path):
    """Splits a merged binary into (offset, temp_path) pieces that skip the
    NVS region -- this is how the nickname and tone frequency survive
    flashing. Returns None if the file is too short to even contain the
    region (then it's not a merged binary)."""
    data = Path(firmware_path).read_bytes()
    if len(data) <= NVS_END:
        return None
    parts = []
    for offset, chunk in ((0x0, data[:NVS_START]), (NVS_END, data[NVS_END:])):
        fd, path = tempfile.mkstemp(prefix="vaura-part-", suffix=".bin")
        with open(fd, "wb") as out:
            out.write(chunk)
        parts.append((offset, path))
    return parts


def restart_device(port, log_cb, attempts=8, delay_s=0.3):
    """Forces and waits out a full reboot after flashing. esptool's own
    "--after hard_reset" already resets the chip, but the native USB-CDC port
    briefly disappears and re-enumerates during that reset, so opening it
    again (retrying while the OS catches up) both guarantees a real restart
    (see open_device_serial) and lets us confirm it actually came back up."""
    import serial

    last_error = None
    for _ in range(attempts):
        try:
            ser = open_device_serial(port)
            break
        except serial.SerialException as e:
            last_error = e
            time.sleep(delay_s)
    else:
        log_cb(f"Could not reconnect after flashing ({last_error}) -- "
               "unplug and replug the device to be sure it restarted.")
        return
    try:
        wait_for_device_boot(ser)
    finally:
        ser.close()


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
        log_cb("Flashing in 2 pieces -- the device's settings area "
               "(nickname, tone frequency, etc.) stays untouched.")
    else:
        args += ["0x0", str(firmware_path)]
        log_cb("File doesn't contain the NVS region (not a merged binary?) -- "
               "flashing unchanged at 0x0.")
    log_cb(f"Starting flash on {port} ...")
    try:
        ok = run_esptool(args, log_cb, progress_cb)
    finally:
        for _, path in parts or []:
            Path(path).unlink(missing_ok=True)
    if ok:
        progress_cb(100)
        log_cb("Flash complete -- restarting the device ...")
        restart_device(port, log_cb)
        log_cb("Done! The device has restarted and is now running the new firmware.")
    else:
        log_cb(
            "Flashing failed. Tips: try a different USB cable/port; "
            "plug the device in while holding the BOOT button (B) and "
            "flash again; on Linux, check whether the user is in the "
            "'dialout' (or 'uucp') group."
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
        root.minsize(560, 640)
        self.device_settings = None  # last-read device state (dict)

        main = ttk.Frame(root, padding=14)
        main.pack(fill="both", expand=True)

        # --- Firmware source ---
        fw = ttk.LabelFrame(main, text="1. Firmware", padding=10)
        fw.pack(fill="x")
        self.btn_download = ttk.Button(fw, text="Download latest version",
                                       command=self.on_download)
        self.btn_download.pack(side="left")
        self.btn_local = ttk.Button(fw, text="Local file ...", command=self.on_pick_file)
        self.btn_local.pack(side="left", padx=(8, 0))
        self.lbl_firmware = ttk.Label(fw, text="no firmware selected yet")
        self.lbl_firmware.pack(side="left", padx=(12, 0))

        # --- Port ---
        pt = ttk.LabelFrame(main, text="2. USB port (connect the device via USB-C)", padding=10)
        pt.pack(fill="x", pady=(10, 0))
        self.port_var = tk.StringVar()
        self.cmb_port = ttk.Combobox(pt, textvariable=self.port_var, state="readonly", width=44)
        self.cmb_port.pack(side="left")
        ttk.Button(pt, text="Refresh", command=self.refresh_ports).pack(side="left", padx=(8, 0))

        # --- Flashing ---
        fl = ttk.LabelFrame(main, text="3. Flash", padding=10)
        fl.pack(fill="x", pady=(10, 0))
        self.btn_flash = ttk.Button(fl, text="Flash firmware", command=self.on_flash)
        self.btn_flash.pack(anchor="w")
        self.progress = ttk.Progressbar(fl, maximum=100)
        self.progress.pack(fill="x", pady=(8, 0))

        # --- Device settings (via the firmware's serial console) ---
        dv = ttk.LabelFrame(main, text="4. Device: settings (over USB, no flashing needed)", padding=10)
        dv.pack(fill="x", pady=(10, 0))
        top = ttk.Frame(dv)
        top.pack(fill="x")
        self.btn_read = ttk.Button(top, text="Read", command=self.on_read_device)
        self.btn_read.pack(side="left")
        # Identity line: node ID + name + firmware -- proof that the device
        # on the other end really is the expected Vaura device.
        self.lbl_device = ttk.Label(top, text="not connected")
        self.lbl_device.pack(side="left", padx=(12, 0))

        form = ttk.Frame(dv)
        form.pack(fill="x", pady=(8, 0))
        self.var_name = tk.StringVar()
        self.var_channel = tk.StringVar()
        self.var_sensitivity = tk.StringVar()
        self.var_tone = tk.StringVar()
        self.var_display = tk.StringVar()
        fields = [
            ("Name (max 5)", ttk.Entry(form, textvariable=self.var_name, width=8)),
            ("Channel", ttk.Spinbox(form, from_=0, to=9, textvariable=self.var_channel, width=4, wrap=True)),
            ("Sensitivity", ttk.Spinbox(form, from_=0, to=10, textvariable=self.var_sensitivity, width=4, wrap=True)),
            ("Tone step", ttk.Spinbox(form, from_=0, to=10, textvariable=self.var_tone, width=4, wrap=True)),
            ("Display off (s)", ttk.Combobox(form, textvariable=self.var_display, state="readonly",
                                             width=8, values=("0 (never)", "15", "30", "60", "300"))),
        ]
        self.device_widgets = []
        for column, (label, widget) in enumerate(fields):
            ttk.Label(form, text=label).grid(row=0, column=column, sticky="w", padx=(0, 10))
            widget.grid(row=1, column=column, sticky="w", padx=(0, 10))
            self.device_widgets.append(widget)
        self.btn_apply = ttk.Button(dv, text="Apply", command=self.on_apply_device)
        self.btn_apply.pack(anchor="w", pady=(10, 0))

        # --- Log ---
        lg = ttk.LabelFrame(main, text="Log", padding=10)
        lg.pack(fill="both", expand=True, pady=(10, 0))
        self.txt_log = tk.Text(lg, height=10, state="disabled", wrap="word",
                               font=("Courier", 11) if sys.platform == "darwin" else ("TkFixedFont", 9))
        self.txt_log.pack(fill="both", expand=True)

        ttk.Label(main, foreground="#666", wraplength=520, justify="left", text=(
            "Note: if the device isn't detected, plug it in while holding "
            "the BOOT button (B). All settings stored on the device "
            "survive flashing."
        )).pack(fill="x", pady=(10, 0))

        self.refresh_ports()
        self.update_buttons()
        self.pump_queue()

        if tk.TkVersion < MIN_TK_VERSION:
            # At best someone reads this in the log -- on macOS, Tk 8.5
            # typically renders the whole window empty/gray (see README,
            # "Running the flasher from source" section).
            self.log(
                f"WARNING: Tk {tk.TkVersion} is too old (need at least "
                f"{MIN_TK_VERSION}) -- the interface will likely not render "
                "correctly. Please use a Python with a current Tk, e.g. via "
                "'brew install python-tk'."
            )

    # --- Helpers ------------------------------------------------------------
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
                elif kind == "device":
                    self.device_settings = payload
                    self.lbl_device.configure(text=(
                        f"Node ID {payload.get('id', '?')}  ·  {payload.get('name', '?')}"
                        f"  ·  FW {payload.get('version', '?')}"))
                    self.var_name.set(payload.get("name", ""))
                    self.var_channel.set(payload.get("channel", "0"))
                    self.var_sensitivity.set(payload.get("sensitivity", "5"))
                    self.var_tone.set(payload.get("tone", "5"))
                    display = payload.get("display", "30")
                    self.var_display.set("0 (never)" if display == "0" else display)
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
        can_read = not self.busy and bool(self.port_var.get())
        self.btn_read.configure(state="normal" if can_read else "disabled")
        can_apply = can_read and self.device_settings is not None
        self.btn_apply.configure(state="normal" if can_apply else "disabled")

    def refresh_ports(self):
        ports = list_serial_ports()
        values, preselect = [], None
        for device, desc, likely in ports:
            label = f"{device}  ({desc})" if desc else device
            if likely:
                label += "  ← likely the Vaura device"
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

    # --- Actions -------------------------------------------------------------
    def on_pick_file(self):
        from tkinter import filedialog

        path = filedialog.askopenfilename(
            title="Select firmware file (merged binary from the release)",
            filetypes=[("Firmware", "*.bin"), ("All files", "*")])
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
                self.log(f"Latest version: {tag} -- downloading {FIRMWARE_ASSET_NAME} ...")
                path = download_firmware(url, self.set_progress)
                self.msg_queue.put(("firmware", (path, f"{FIRMWARE_ASSET_NAME} ({tag})")))
                self.log("Download complete.")
            except Exception as e:  # noqa: BLE001
                self.log(f"Download failed: {e}")
            finally:
                self.msg_queue.put(("done", None))

        threading.Thread(target=worker, daemon=True).start()

    def on_read_device(self):
        port = self.selected_port()
        if not port:
            return
        self.busy = True
        self.update_buttons()

        def worker():
            try:
                settings = read_device_settings(port)
                if "id" not in settings:
                    self.log("No response from the device. Is it running firmware with "
                             "the 'status' command (v0.1.0+) and is the right port selected?")
                else:
                    self.msg_queue.put(("device", settings))
                    self.log(f"Connected: node ID {settings.get('id', '?')}  "
                             f"name {settings.get('name', '?')}  FW {settings.get('version', '?')}")
                    self.log("Note: connecting restarts the device "
                             "(channel selection runs, tour stats start at zero).")
            except Exception as e:  # noqa: BLE001
                self.log(f"Reading failed: {e}")
            finally:
                self.msg_queue.put(("done", None))

        threading.Thread(target=worker, daemon=True).start()

    def on_apply_device(self):
        port = self.selected_port()
        if not port or self.device_settings is None:
            return
        current = self.device_settings
        commands = []
        name = self.var_name.get().strip()
        if name and name != current.get("name"):
            if len(name) > 5 or not name.isalpha():
                self.log("Error: name = max. 5 letters, no digits/special characters.")
                return
            commands.append(f"name {name}")
        for key, var in (("channel", self.var_channel), ("sensitivity", self.var_sensitivity),
                         ("tone", self.var_tone)):
            value = var.get().strip()
            if value and value != current.get(key):
                commands.append(f"{key} {value}")
        display = self.var_display.get().split(" ")[0].strip()  # "0 (never)" -> "0"
        if display and display != current.get("display"):
            commands.append(f"display {display}")
        if not commands:
            self.log("No changes.")
            return

        self.busy = True
        self.update_buttons()

        def worker():
            try:
                settings = apply_device_settings(port, commands, self.log)
                if "id" in settings:
                    self.msg_queue.put(("device", settings))
                    self.log("Settings applied (fields now show the actual device state).")
                else:
                    self.log("Device didn't respond after writing -- please read again.")
            except Exception as e:  # noqa: BLE001
                self.log(f"Applying failed: {e}")
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
    """--check: imports, Tk version + port scan without GUI (for CI/smoke test)."""
    import esptool  # noqa: F401
    import serial  # noqa: F401
    import tkinter

    patchlevel = tkinter.Tcl().call("info", "patchlevel")
    if tkinter.TkVersion < MIN_TK_VERSION:
        print(f"ERROR: Tk {patchlevel} is too old (need at least {MIN_TK_VERSION}) -- "
              "the GUI will stay an empty gray window. Use a Python with a "
              "current Tk, e.g. via 'brew install python-tk' (macOS).")
        return 1
    ports = list_serial_ports()
    print(f"OK: esptool/pyserial/tkinter importable, Tk {patchlevel}, "
          f"{len(ports)} serial port(s):")
    for device, desc, likely in ports:
        print(f"  {device}  {desc}{'  <- Espressif USB' if likely else ''}")
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
