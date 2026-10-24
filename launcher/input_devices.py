# SPDX-License-Identifier: GPL-2.0-or-later
"""The input devices and the key bindings, for the launcher's controls page.

The runtime reads the pads (src/runtime_pad.c) and takes its bindings from the
configuration file (bbport.ini): a line "key.cross=Space,E" replaces the keyboard
binding of the cross button, "pad.cross=a" the gamepad's one. A line that is not there
keeps the default.

The devices are asked of the runtime itself ("bb-probe.exe --input-devices"), because
the SDL that reads the pads is in the GPU library, and the runtime is the one that has
it loaded.
"""
import os
import subprocess
import sys
from pathlib import Path

# The inputs the game has, and the names the runtime's configuration uses. The order is
# the runtime's own (src/runtime_pad.c).
INPUTS = [
    ("cross", "Cross"),
    ("circle", "Circle"),
    ("square", "Square"),
    ("triangle", "Triangle"),
    ("l1", "L1"),
    ("r1", "R1"),
    ("l2", "L2"),
    ("r2", "R2"),
    ("l3", "L3"),
    ("r3", "R3"),
    ("options", "Options"),
    ("touchpad", "Touchpad (left half)"),
    ("touchpad_right", "Touchpad (right half)"),
    ("up", "D-pad up"),
    ("down", "D-pad down"),
    ("left", "D-pad left"),
    ("right", "D-pad right"),
    ("move_up", "Move forward"),
    ("move_down", "Move back"),
    ("move_left", "Move left"),
    ("move_right", "Move right"),
    ("look_up", "Camera up"),
    ("look_down", "Camera down"),
    ("look_left", "Camera left"),
    ("look_right", "Camera right"),
]

# The default bindings (the runtime's own, src/runtime_pad.c): the keyboard key and the
# gamepad button. The automatic configuration writes them out.
DEFAULT_KEYS = {
    "cross": "Space", "circle": "Left Shift", "square": "E", "triangle": "Q",
    "l1": "1", "r1": "3", "l2": "R", "r2": "F", "l3": "Z", "r3": "C",
    "options": "Return", "touchpad": "Tab", "touchpad_right": "Backspace",
    "up": "I", "down": "K", "left": "J", "right": "L",
    "move_up": "W", "move_down": "S", "move_left": "A", "move_right": "D",
    "look_up": "Up", "look_down": "Down", "look_left": "Left", "look_right": "Right",
}
DEFAULT_PADS = {
    "cross": "a", "circle": "b", "square": "x", "triangle": "y",
    "l1": "leftshoulder", "r1": "rightshoulder", "l2": "lefttrigger",
    "r2": "righttrigger", "l3": "leftstick", "r3": "rightstick",
    "options": "start", "touchpad": "back,touchpad", "touchpad_right": "",
    "up": "dpup", "down": "dpdown", "left": "dpleft", "right": "dpright",
}

# The keyboard keys a binding may name. The names are the ones the runtime's
# configuration understands (SDL's key names, the way they are written on the key).
KEY_NAMES = [
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "Space", "Return", "Backspace", "Tab", "Left Shift", "Right Shift",
    "Left Ctrl", "Right Ctrl", "Left Alt", "Right Alt",
    "Up", "Down", "Left", "Right",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "Mouse Left", "Mouse Right", "Mouse Middle", "Mouse X1", "Mouse X2",
    "Wheel Up", "Wheel Down",
]

# The gamepad buttons a binding may name (SDL's).
PAD_NAMES = [
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
    "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright",
    "touchpad", "misc1", "paddle1", "paddle2", "paddle3", "paddle4",
    "lefttrigger", "righttrigger",
]


class Device:
    """An input device that is connected."""

    def __init__(self, kind, guid, name, architecture):
        self.kind = kind            # "pad" or "keyboard"
        self.guid = guid
        self.name = name
        self.architecture = architecture  # "xinput", "directinput" or ""

    @property
    def is_pad(self):
        return self.kind == "pad"

    def __repr__(self):
        return f"<Device {self.kind} {self.name!r} {self.architecture}>"


def devices(runtime=None):
    """The input devices that are connected, [] when the runtime cannot be asked."""
    if not runtime:
        return []
    try:
        result = subprocess.run([str(runtime), "--input-devices"], capture_output=True,
                                text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return []
    if result.returncode != 0:
        return []
    found = []
    for line in result.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        kind = parts[0]
        guid = parts[1] if len(parts) > 1 else ""
        name = parts[2] if len(parts) > 2 else ""
        architecture = parts[3] if len(parts) > 3 else ""
        found.append(Device(kind, guid, name, architecture))
    return found


def pad_buttons(runtime, guid):
    """The buttons a pad has, as the names a binding may use."""
    if not runtime or not guid:
        return []
    try:
        result = subprocess.run([str(runtime), "--input-buttons", guid], capture_output=True,
                                text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return []
    if result.returncode != 0:
        return []
    return [name for name in result.stdout.strip().split(",") if name]


def read_bindings(config):
    """The bindings of the configuration file: {input: {"key": ..., "pad": ...}}."""
    bindings = {}
    try:
        text = Path(config).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return bindings
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("key."):
            name, _, value = line.partition("=")
            bindings.setdefault(name[4:], {})["key"] = value.strip()
        elif line.startswith("pad."):
            name, _, value = line.partition("=")
            bindings.setdefault(name[4:], {})["pad"] = value.strip()
    return bindings


def write_bindings(config, bindings):
    """Writes the bindings into the configuration file, keeping the other lines."""
    path = Path(config)
    lines = []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        pass
    written = set()
    out = []
    for line in lines:
        name = None
        for prefix in ("key.", "pad."):
            if line.strip().startswith(prefix):
                name = line.strip()[len(prefix):].split("=", 1)[0]
                break
        if name and name in bindings:
            entry = bindings[name]
            if "key" in entry:
                out.append(f"key.{name}={entry['key']}")
            if "pad" in entry:
                out.append(f"pad.{name}={entry['pad']}")
            written.add(name)
            continue
        out.append(line)
    for name, entry in bindings.items():
        if name in written:
            continue
        if "key" in entry:
            out.append(f"key.{name}={entry['key']}")
        if "pad" in entry:
            out.append(f"pad.{name}={entry['pad']}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(out) + "\n", encoding="utf-8")


def automatic_bindings(device, buttons=None):
    """The bindings for a device: the defaults, with the pad's own buttons.

    A pad that is read through XInput has the Xbox layout (the cross button is the one
    at the bottom, the "a" of the runtime), the other ones the PlayStation layout (the
    cross button is the one at the right). The keyboard keeps the defaults."""
    bindings = {}
    for name, _label in INPUTS:
        entry = {}
        if not device or not device.is_pad:
            entry["key"] = DEFAULT_KEYS.get(name, "")
        else:
            entry["pad"] = DEFAULT_PADS.get(name, "")
        bindings[name] = entry
    return bindings
