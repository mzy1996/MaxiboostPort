# SPDX-License-Identifier: GPL-2.0-or-later
"""Checks that the packaged program can be loaded: every DLL it needs is there.

The Windows loader looks for the DLLs in the program's own folder, then in the system's.
This walks the same import tables and reports what is missing, so that a package that
would not start is caught here and not on the player's screen.

    python tools/check_package.py PACKAGE_DIR
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from collect_dlls import SYSTEM, imports  # noqa: E402


def main(argv):
    if len(argv) < 2:
        print('Usage: check_package.py PACKAGE_DIR', file=sys.stderr)
        return 2
    package = argv[1]
    binaries = os.path.join(package, 'bin')
    if not os.path.isdir(binaries):
        print(f'check_package.py: {binaries} is not there', file=sys.stderr)
        return 1
    missing = []
    checked = 0
    for name in sorted(os.listdir(binaries)):
        if not name.lower().endswith(('.exe', '.dll')):
            continue
        path = os.path.join(binaries, name)
        for dependency in imports(path):
            if dependency.lower() in SYSTEM:
                continue
            checked += 1
            if not os.path.exists(os.path.join(binaries, dependency)):
                missing.append((name, dependency))
    if missing:
        print(f'check_package.py: {len(missing)} imports are not satisfied:', file=sys.stderr)
        for name, dependency in missing:
            print(f'  {name} needs {dependency}', file=sys.stderr)
        return 1
    print(f'check_package.py: the package is complete ({checked} imports checked)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
