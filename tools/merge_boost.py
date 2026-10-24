#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Merges the Boost libraries' headers into one include directory.

The upstream Boost release has one include/boost directory with every library's headers in it;
the git repositories have one per library (boostorg/<library>). The Windows build takes the
headers from the vendored copy (gpu/third_party/boost), so they are merged here.

    python3 tools/merge_boost.py [SOURCE_DIR] [TARGET_DIR]

SOURCE_DIR: where the library repositories are (each with include/boost). TARGET_DIR: the
directory with the merged include/boost. The default source is the temporary directory the
clone script uses.
"""
import shutil
import sys
from pathlib import Path


def merge(source, target):
    """Copies every library's include/boost into the target's, the first one wins."""
    merged = 0
    libraries = 0
    for library in sorted(source.iterdir()):
        include = library / "include"
        if not include.is_dir():
            continue
        libraries += 1
        for item in include.rglob("*"):
            relative = item.relative_to(include)
            destination = target / relative
            if item.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            if destination.exists():
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(item, destination)
            merged += 1
    return libraries, merged


def main(argv):
    source = Path(argv[1]) if len(argv) > 1 else Path.home() / "AppData/Local/Temp/blibs"
    target = Path(argv[2]) if len(argv) > 2 else Path(__file__).resolve().parent.parent / "gpu/third_party/boost/include"
    if not source.is_dir():
        print(f"no such directory: {source}")
        return 1
    libraries, files = merge(source, target)
    print(f"Boost: {libraries} libraries, {files} headers -> {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
