#!/usr/bin/env python3
"""Regenerates the SVGs embedded in docs/ui-mockups.md.

Each SVG is a 128x64 viewBox (the real SSD1306 pixel grid), colored to match
the physical two-tone panel (rows 0..15 yellow, 16..63 blue), with a small
glow filter approximating the OLED's text-shadow look. Positions are the
exact drawStr()/drawHLine() pixel coordinates used in src/ui.cpp -- this
script is a mechanical re-target of that file's geometry into standalone
image assets, not a redesign. Re-run it (`python3 docs/mockups/generate.py`)
whenever a screen's layout or copy changes in ui.cpp, and update the
corresponding text by hand in this file if a new screen's content differs
from what's hardcoded below.
"""
import os

OUT_DIR = os.path.dirname(os.path.abspath(__file__))

YELLOW = "#ffd97a"
BLUE = "#7ecbff"
BLUE_DIM = "#55889f"
BG = "#04070a"

HEAD = ("""<svg xmlns="http://www.w3.org/2000/svg" viewBox="-3 0 134 64" width="512" height="244">
  <defs>
    <filter id="glowY" x="-50%" y="-50%" width="200%" height="200%">
      <feGaussianBlur stdDeviation="0.5" result="b"/>
      <feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
    <filter id="glowB" x="-50%" y="-50%" width="200%" height="200%">
      <feGaussianBlur stdDeviation="0.5" result="b"/>
      <feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
    <pattern id="scan" width="1" height="4" patternUnits="userSpaceOnUse">
      <rect width="1" height="1" fill="#000" opacity="0.35"/>
    </pattern>
  </defs>
  <style>
    text { font-family: ui-monospace, "SF Mono", "Cascadia Mono", "Courier New", monospace; }
    .y { fill: """ + YELLOW + """; }
    .b { fill: """ + BLUE + """; }
    .bdim { fill: """ + BLUE_DIM + """; }
  </style>
  <rect x="-3" y="0" width="134" height="64" fill=\"""" + BG + """\"/>
""")

TAIL = """  <rect x="-3" y="0" width="134" height="64" fill="url(#scan)"/>
</svg>
"""


def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def text(x, y, s, cls="b", size=8, weight="normal", anchor="start"):
    w = ' font-weight="bold"' if weight == "bold" else ""
    glow = "glowY" if cls == "y" else "glowB"
    return (f'  <text x="{x}" y="{y}" font-size="{size}"{w} text-anchor="{anchor}" '
            f'class="{cls}" filter="url(#{glow})">{esc(s)}</text>\n')


def hline_y():
    return f'  <line x1="0" y1="12" x2="128" y2="12" stroke="{YELLOW}" stroke-width="1" class="y" filter="url(#glowY)"/>\n'


def header(left, right=None, center=None):
    out = text(0, 9, left, cls="y", size=8)
    if right is not None:
        out += text(128, 9, right, cls="y", size=8, anchor="end")
    if center is not None:
        out += text(64, 9, center, cls="y", size=8, anchor="middle")
    out += hline_y()
    return out


def write(name, body):
    svg = HEAD + body + TAIL
    path = os.path.join(OUT_DIR, name + ".svg")
    with open(path, "w") as f:
        f.write(svg)
    print("wrote", path)


# --- 1. Boot: channel selection ---------------------------------------------
b = header("Select channel:")
b += text(0, 34, "Channel 0", size=13, weight="bold")
b += text(0, 46, "Starting in 7s ...", size=8)
b += text(0, 61, "short=change long=OK", size=8)
write("boot-channel-select", b)

# --- 2. Idle: alone ----------------------------------------------------------
b = header("ROB", right="87%")
b += text(0, 34, "Alone", size=13, weight="bold")
write("idle-alone", b)

# --- 3. Idle: 1 column (<=4 riders), with RSSI + battery --------------------
b = header("TOBI", right="4/5  87%")
rows = [
    "(CARL)    ---  15%",
    "!BENE!   -108  62%",
    " ROB      -72  88%",
    " LEA      -51   --",
]
for i, row in enumerate(rows):
    b += text(0, 23 + i * 10, row, size=8)
b += text(0, 61, "CARL DROPPED (40s)", size=8)
write("idle-1col", b)

# --- 4. Idle: 2 columns (5-8 riders) -----------------------------------------
b = header("TOBI", right="7/8  74%")
cols2 = [("(MAX)", "!LEA!"), (" ROB", " ANNA"), (" JO", " PIT"), (" EVA", "")]
for i, (l, r) in enumerate(cols2):
    y = 23 + i * 10
    b += text(0, y, l, size=8)
    if r:
        b += text(64, y, r, size=8)
b += text(0, 61, "MAX DROPPED (5s)", size=8)
write("idle-2col", b)

# --- 5. Idle: 3 columns (9+ riders), small font ------------------------------
b = header("TOBI", right="12/13  65%")
cols3 = [
    ("(KURT)", "!MIRA!", "!JO!"),
    (" LEA", " ROB", " ANNA"),
    (" PIT", " EVA", " BEN"),
    (" TOM", " IDA", " MAX"),
]
for i, (c1, c2, c3) in enumerate(cols3):
    y = 22 + i * 8
    b += text(0, y, c1, size=6)
    b += text(44, y, c2, size=6)
    b += text(88, y, c3, size=6)
b += text(0, 61, "KURT DROPPED (2s)", size=8)
write("idle-3col", b)

# --- 6. Idle: muted + low battery -------------------------------------------
b = header("ROB", right="3/3  !9%", center="MUTE")
rows = ["!LEA!    -104", " MAX      -63"]
for i, row in enumerate(rows):
    b += text(0, 23 + i * 10, row, size=8)
b += text(0, 61, "LEA BATTERY (12s)", size=8)
write("idle-muted-lowbatt", b)

# --- 7. Send warning menu ----------------------------------------------------
b = header("Send warning:")
b += text(0, 34, "CAR BEHIND", size=13, weight="bold")
b += text(0, 61, "short=next long=send", size=8)
write("send-menu", b)

# --- 8. Incoming warning ------------------------------------------------------
b = header("WARNING from MAX")
b += text(0, 34, "HAZARD AHEAD", size=13, weight="bold")
b += text(0, 61, "short = dismiss", size=8)
write("incoming-warning", b)

# --- 9. Dismiss prompt --------------------------------------------------------
b = header("Still dropped off:")
b += text(0, 34, "(CARL)", size=13, weight="bold")
b += text(0, 46, "gone 2 min", size=8)
b += text(0, 61, "short=keep long=remove", size=8)
write("dismiss-prompt", b)

# --- 10. Settings menu --------------------------------------------------------
b = header("Settings:")
b += text(0, 34, "Mute: OFF", size=13, weight="bold")
b += text(0, 61, "short=next long=select", size=8)
write("settings-menu", b)

# --- 11. Stats screen ----------------------------------------------------------
b = header("Stats:", right="v0.1.0")
rows = [
    "Ride time    2:13h",
    "Sent            3",
    "Received        5",
    "Dropped         2",
]
for i, row in enumerate(rows):
    b += text(0, 23 + i * 10, row, size=8)
b += text(0, 61, "long=back", size=8)
write("stats", b)

# --- 12. Channel menu ----------------------------------------------------------
b = header("Radio channel:")
b += text(0, 34, "Channel 0", size=13, weight="bold")
b += text(0, 46, "all devices must match!", size=8)
b += text(0, 61, "short=change long=OK", size=8)
write("channel-menu", b)

# --- 13/14. Ruler menus (tone / sensitivity) ----------------------------------
def ruler(cursor_step):
    x0 = cursor_step * 12 + 4
    out = ""
    out += f'  <polygon points="{x0-3},31 {x0+3},31 {x0},36" class="b" filter="url(#glowB)"/>\n'
    out += f'  <line x1="4" y1="38.5" x2="124.5" y2="38.5" stroke="{BLUE}" stroke-width="1" class="b" filter="url(#glowB)"/>\n'
    for i in range(11):
        x = 4 + i * 12
        out += f'  <line x1="{x}" y1="39" x2="{x}" y2="43" stroke="{BLUE}" stroke-width="1" class="b" filter="url(#glowB)"/>\n'
    for i in range(11):
        x = 4 + i * 12
        label = str(i)
        out += text(x, 50, label, size=5, anchor="middle")
    return out


b = header("Set tone:")
b += text(0, 26, "Level 6", size=8)
b += ruler(6)
b += text(0, 61, "short=change long=OK", size=8)
write("tone-menu", b)

b = header("Sensitivity:")
b += text(0, 26, "Level 5", size=8)
b += ruler(5)
b += text(0, 61, "short=change long=OK", size=8)
write("sensitivity-menu", b)

# --- 15. Display timeout menu --------------------------------------------------
b = header("Display off after:")
b += text(0, 34, "30 s", size=13, weight="bold")
b += text(0, 61, "short=change long=OK", size=8)
write("display-menu", b)

# --- 16/17. Rename screens ------------------------------------------------------
def rename(cells, cursor_idx, footer):
    # Mirrors ui.cpp renderRename(): a solid box on the cursor cell, its
    # glyph re-drawn in the inverse (background) color on top.
    out = header("Change name:")
    cell_w = 9
    for i, ch in enumerate(cells):
        x = i * cell_w
        if i == cursor_idx:
            out += f'  <rect x="{x}" y="20" width="{cell_w}" height="16" class="b" filter="url(#glowB)"/>\n'
            out += f'  <text x="{x+1}" y="34" font-size="13" font-weight="bold" fill="{BG}">{esc(ch)}</text>\n'
        else:
            out += text(x + 1, 34, ch, size=13, weight="bold")
    out += text(0, 61, footer, size=8)
    return out


write("rename-active", rename(["R", "O", " ", " ", " "], 1, "short=char long=OK"))
write("rename-finish", rename(["K", "L", "A", "U", "S"], 4, "long=done"))

# --- 18/19. Range test ------------------------------------------------------------
b = header("Range test:")
b += text(0, 34, "MAX -87", size=13, weight="bold")
b += text(0, 46, "raw -85 dBm  2s ago", size=8)
b += text(0, 61, "short=rider long=exit", size=8)
write("range-test", b)

b = header("Range test:")
b += text(0, 34, "MAX -104", size=13, weight="bold")
b += text(0, 46, "DROPPED  40s ago", size=8)
b += text(0, 61, "short=rider long=exit", size=8)
write("range-test-dropped", b)

print("done, ", len(os.listdir(OUT_DIR)), "files")
