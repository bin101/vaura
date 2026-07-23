"""Unit tests for flasher.py's pure helper functions -- no hardware, no GUI.

Run with: pytest flasher/test_flasher.py (from the repo root), or
`cd flasher && pytest`. These cover exactly the functions where past bugs
lived (port-label round-tripping, the status key=value parser, the merged-
binary NVS split), since they're plain data-in/data-out logic that doesn't
need a device or a Tk window to exercise.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import flasher  # noqa: E402


# ---------------------------------------------------------------------------
# parse_status_lines() -- the `status` response parser, and the exact
# interface contract with the firmware's config.cpp (SETTINGS_KEYS).
# ---------------------------------------------------------------------------

def test_parse_status_lines_typical_response():
    lines = [
        "name=ROB",
        "id=1A2B",
        "version=v0.4.0",
        "channel=3",
        "sensitivity=5",
        "tone=6",
        "display=30",
    ]
    assert flasher.parse_status_lines(lines) == {
        "name": "ROB",
        "id": "1A2B",
        "version": "v0.4.0",
        "channel": "3",
        "sensitivity": "5",
        "tone": "6",
        "display": "30",
    }


def test_parse_status_lines_ignores_unrelated_log_lines():
    lines = [
        "Radio: SX1262 ready (EU868, GFSK).",
        "name=ROB",
        "Node ID: 1A2B  Nickname: ROB  Channel: 0  FW v0.4.0  (console: 'help')",
        "id=1A2B",
    ]
    assert flasher.parse_status_lines(lines) == {"name": "ROB", "id": "1A2B"}


def test_parse_status_lines_ignores_keys_outside_the_contract():
    # A stray "key=value"-shaped line whose key isn't part of SETTINGS_KEYS
    # (e.g. a future firmware addition, or the `charge` command's output,
    # which is deliberately excluded from `status`) must not leak in.
    lines = ["name=ROB", "chargeCurrentMa=12"]
    assert flasher.parse_status_lines(lines) == {"name": "ROB"}


def test_parse_status_lines_empty_input():
    assert flasher.parse_status_lines([]) == {}


def test_settings_keys_has_no_duplicates():
    assert len(flasher.SETTINGS_KEYS) == len(set(flasher.SETTINGS_KEYS))


# ---------------------------------------------------------------------------
# build_port_labels() / label_to_port() -- the port-selection round trip.
# This is where the empty-description bug lived: a "likely" label for a
# device with no description has no "  (" substring to split on.
# ---------------------------------------------------------------------------

def test_build_port_labels_with_description():
    values, preselect, by_label = flasher.build_port_labels(
        [("/dev/ttyACM0", "USB Serial", False)])
    assert values == ["/dev/ttyACM0  (USB Serial)"]
    assert preselect is None
    assert by_label == {"/dev/ttyACM0  (USB Serial)": "/dev/ttyACM0"}


def test_build_port_labels_likely_device_without_description():
    # The exact regression case: list_ports.comports() returned an empty
    # description for the auto-detected Espressif device (observed on some
    # platforms for the ESP32-S3 native USB-CDC/JTOG port).
    values, preselect, by_label = flasher.build_port_labels(
        [("COM3", "", True)])
    assert values == ["COM3  ← likely the Vaura device"]
    assert preselect == "COM3  ← likely the Vaura device"
    assert by_label == {"COM3  ← likely the Vaura device": "COM3"}


def test_build_port_labels_likely_device_with_description():
    values, preselect, by_label = flasher.build_port_labels(
        [("/dev/ttyACM0", "USB JTAG/serial debug unit", True)])
    label = "/dev/ttyACM0  (USB JTAG/serial debug unit)  ← likely the Vaura device"
    assert values == [label]
    assert preselect == label
    assert by_label == {label: "/dev/ttyACM0"}


def test_build_port_labels_first_likely_wins_preselect():
    values, preselect, by_label = flasher.build_port_labels([
        ("COM3", "", True),
        ("COM4", "", True),
    ])
    assert preselect == "COM3  ← likely the Vaura device"
    assert by_label["COM3  ← likely the Vaura device"] == "COM3"
    assert by_label["COM4  ← likely the Vaura device"] == "COM4"


def test_label_to_port_round_trips_every_build_port_labels_shape():
    ports = [
        ("/dev/ttyACM0", "USB Serial", False),
        ("COM3", "", True),
        ("/dev/ttyACM1", "USB JTAG/serial debug unit", True),
    ]
    values, _preselect, by_label = flasher.build_port_labels(ports)
    expected = {label: device for label, (device, _desc, _likely) in
                zip(values, ports)}
    for label in values:
        assert flasher.label_to_port(label, by_label) == expected[label]


def test_label_to_port_falls_back_for_a_label_not_in_the_map():
    # A stale selection left over from before the last refresh_ports() call
    # (map rebuilt, old label temporarily still in the combobox's textvariable)
    # still recovers a sane port via the "  (" split fallback.
    assert flasher.label_to_port("/dev/ttyUSB0  (FTDI)", {}) == "/dev/ttyUSB0"


def test_label_to_port_empty_selection():
    assert flasher.label_to_port("", {}) == ""


# ---------------------------------------------------------------------------
# split_around_nvs() -- merged-binary splitting so flashing preserves the
# device's persisted settings (nickname, tone, etc. living in the NVS gap).
# ---------------------------------------------------------------------------

def test_split_around_nvs_too_short_is_not_a_merged_binary(tmp_path):
    firmware = tmp_path / "tiny.bin"
    firmware.write_bytes(b"\x00" * (flasher.NVS_END - 1))
    assert flasher.split_around_nvs(str(firmware)) is None


def test_split_around_nvs_splits_around_the_nvs_gap(tmp_path):
    before = b"\xAA" * flasher.NVS_START
    nvs_gap = b"\xFF" * (flasher.NVS_END - flasher.NVS_START)
    after = b"\xBB" * 4096
    firmware = tmp_path / "merged.bin"
    firmware.write_bytes(before + nvs_gap + after)

    parts = flasher.split_around_nvs(str(firmware))
    assert parts is not None
    assert [offset for offset, _path in parts] == [0x0, flasher.NVS_END]

    try:
        (_off0, path0), (_off1, path1) = parts
        assert Path(path0).read_bytes() == before
        assert Path(path1).read_bytes() == after
    finally:
        for _offset, path in parts:
            Path(path).unlink(missing_ok=True)


if __name__ == "__main__":
    import pytest

    sys.exit(pytest.main([__file__, "-v"]))
