"""JSON cheat tables (the trainer format of the CodeFriends / shadPS4 family).

A cheat file is a JSON document that lists the cheats of one game version and, for each, the
bytes to write into the game's executable to turn it on and the bytes that turn it off:

    {
      "name": "...", "id": "CUSA15006", "version": "01.15", "process": "eboot.bin",
      "mods": [
        {"name": "...", "type": "checkbox",
         "memory": [{"offset": "F06CA6", "on": "75", "off": "EB"}]}
      ]
    }

The offsets are offsets into the executable's loaded image (the eboot's own image, whose first
byte is at image offset 0), not PS4 virtual addresses: the loader's image places the eboot's
virtual address 0 at image offset 0, and the eboot is loaded at 0x400000, so a PS4 virtual
address is the image offset plus 0x400000.

The cheats are written into the executable when it is loaded, like the XML patches
(scripts/patches.py): the "on" bytes of every cheat the player enabled are written, the "off"
bytes of the others are left as they are (the executable already holds them). Each write is
checked against the bytes the executable really has, so a cheat table made for another version
of the game is reported instead of corrupting the executable.
"""
import json
import sys
from pathlib import Path

# The eboot is loaded at this PS4 virtual address, so an image offset is a virtual address
# minus it (the XML patch format of scripts/patches.py uses virtual addresses).
EBOOT_BASE = 0x400000


class CheatError(Exception):
    """A cheat table that cannot be used (a wrong game, a wrong version, a malformed entry)."""


def load(path):
    """The cheat table in the file, or None when it is not one."""
    try:
        data = json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict) or not isinstance(data.get('mods'), list):
        return None
    return data


def matches(data, title_id=None, app_version=None):
    """Whether the table is for this game and version. The title and the version are checked
    when they are known; a table that names none of them is taken (the offsets are checked
    against the executable anyway)."""
    if title_id and data.get('id') and str(data['id']) != title_id:
        return False
    if app_version and data.get('version'):
        if not same_version(data['version'], app_version):
            return False
    return True


def same_version(value, app_version):
    """'1.15' is '01.15' (cheat files write either)."""
    try:
        return [int(part) for part in str(value).strip().split('.')] == \
               [int(part) for part in str(app_version).split('.')]
    except ValueError:
        return False


def entries(data):
    """[(cheat name, [(image offset, on bytes, off bytes)])]: the table's cheats and their
    writes. The cheats that are not a checkbox (a value to type, a slider) have no writes and
    are skipped."""
    found = []
    for mod in data.get('mods', []):
        if not isinstance(mod, dict) or mod.get('type') != 'checkbox':
            continue
        name = str(mod.get('name') or '')
        writes = []
        for memory in mod.get('memory', []):
            if not isinstance(memory, dict):
                continue
            try:
                offset = int(str(memory['offset']), 16)
                on = bytes.fromhex(str(memory['on']).replace(' ', ''))
                off = bytes.fromhex(str(memory['off']).replace(' ', ''))
            except (KeyError, TypeError, ValueError):
                raise CheatError(f'{name}: malformed memory entry')
            if not on or len(on) != len(off):
                raise CheatError(f'{name}: the on and off values differ in length')
            writes.append((offset, on, off))
        if writes:
            found.append((name, writes))
    return found


def compile_writes(data, enabled, image=None, title_id=None, app_version=None):
    """The writes of the cheats whose name is in enabled: [(image offset, bytes)].

    The bytes the executable holds at every offset are checked when an image is given, so a
    table made for another build of the game is refused instead of writing over the game's
    code."""
    if not matches(data, title_id, app_version):
        raise CheatError(f"the cheat table is for {data.get('id')} {data.get('version')}, "
                         f"not for {title_id} {app_version}")
    wanted = set(enabled)
    writes = []
    for name, items in entries(data):
        if name not in wanted:
            continue
        for offset, on, off in items:
            if image is not None:
                current = bytes(image[offset:offset + len(off)])
                if current != off:
                    raise CheatError(f'{name}: the executable does not hold the bytes this '
                                     f'cheat expects at {offset:#x} (it has {current.hex()}, '
                                     f'the cheat table expects {off.hex()}): the table is for '
                                     f'another build of the game')
            writes.append((offset, on))
    return writes


def enabled_from_config(config, names):
    """The cheats to apply: the ones the configuration enables. A configuration that names none
    of them enables none (the player chooses)."""
    if not config:
        return []
    try:
        settings = json.loads(Path(config).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return []
    enabled = settings.get('enabled', [])
    return [name for name in names if name in enabled]


def main(argv):
    """cheats.py FILE [--title T] [--version V]: the table's cheats, one per line."""
    if len(argv) < 2:
        print('Usage: cheats.py FILE [--title T] [--version V]', file=sys.stderr)
        return 2
    data = load(argv[1])
    if data is None:
        print(f'{argv[1]}: not a cheat table', file=sys.stderr)
        return 1
    title = argv[argv.index('--title') + 1] if '--title' in argv else None
    version = argv[argv.index('--version') + 1] if '--version' in argv else None
    if not matches(data, title, version):
        print(f'{argv[1]}: for {data.get("id")} {data.get("version")}, not for {title} {version}',
              file=sys.stderr)
        return 1
    for name, items in entries(data):
        print(name)
        for offset, on, off in items:
            print(f'    {offset:#x}: {off.hex()} -> {on.hex()}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
