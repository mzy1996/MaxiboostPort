#!/usr/bin/env python3
"""The game files mbport runs: MOBILE SUIT GUNDAM EXTREME VS. MAXIBOOST ON (PS4, 2020) with the
1.15 update merged in.

Two releases are supported: the North American one (CUSA18758) and the Hong Kong / Chinese deluxe
soundtrack edition (CUSA15006, 豪華原聲版), whose dump comes with or without the cheat trainer
injected into the eboot.

Hooks and patches use the addresses of one executable, so the loaded image decides: its hash is
the same whatever tool dumped it (the SELF headers around it differ). The base game 1.00 is
recognised and asked for its update. BB_SKIP_GAME_CHECK=1 skips the check.

Run as a script, it checks that a game folder has the files the game opens first:
game_check.py GAME_DIR
"""
import hashlib
import os
import sys
from pathlib import Path

# The retail releases, each with the SHA-256 of its 1.15 eboot's loaded image:
# CUSA18758  North America.
# CUSA15006  Hong Kong / China, the deluxe soundtrack edition (豪華原聲版). Its dump comes in
#            several flavours: the plain 1.15 patch and the one with the cheat trainer
#            injected (a code cave in the eboot that reads a JSON cheat table at run time).
#            Both are listed, so either one is accepted.
# The executable decides in the end (the hashes below), so another region with the same eboot
# runs too.
SUPPORTED_TITLES = ('CUSA18758', 'CUSA15006')
SUPPORTED_VERSION = '01.15'
SUPPORTED_IMAGES = {
    'CUSA18758': ('1ebb96baf111a1c42f7c37c6e0ecb68e6576fa9cc0edc2ada2ff21850432b375',),
    'CUSA15006': (
        '96a1a26763442580cd9b48012fe4565d5cdfa6d0c39ffb06dd8c2b9a4d3e44a5',  # plain 1.15
        '2b03a673bfd3c71beee60ec5e31aa9888a5a25666d0ff58fa6be6e1733e8e5cb',  # with the cheat trainer
    ),
}
# Kept for the code that compares against one hash (the North American one is the reference).
SUPPORTED_IMAGE = SUPPORTED_IMAGES['CUSA18758'][0]
BASE_IMAGE = 'd5ae1b51920c77fa58d51a800be80542f7edd9806320c27b02a16d963be2f0b6'  # 1.00
# The game's own version field (sce_sys/param.sfo VERSION) says 1.15 too, but the update's
# param.sfo keeps the base game's APP_VER (01.00) and puts the update's version in VERSION.
# The titles that ship it that way.
VERSION_IN_SFO_FIELD = ('CUSA15006',)
# The game opens these first; a folder without them is not a whole extraction (the archives, the
# disc map, the raw files: movies and sound banks in numbered folders).
REQUIRED_FILES = ('archives', 'sce_discmap.plt', 'raw')


def image_sha256(game):
    """The SHA-256 of eboot.bin's loaded image (its loadable segments at their addresses)."""
    from prepare import parse_self, span  # same directory
    elf, header, ph, _segments, _missing = parse_self((Path(game) / 'eboot.bin').read_bytes())
    loads = [p for p in ph if p['type'] in (1, 0x61000010)]
    image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
    for p in loads:
        image[p['vaddr']:p['vaddr'] + p['filesz']] = span(elf, p['offset'], p['filesz'])
    return hashlib.sha256(image).hexdigest()


def missing_files(game):
    root = Path(game)
    return [name for name in REQUIRED_FILES if not (root / name).exists()]


def game_title(game):
    """The title id of the game in the folder ("CUSA18758", "CUSA15006"), or "" when it
    cannot be read."""
    from prepare import sfo
    try:
        info = sfo((Path(game) / 'sce_sys/param.sfo').read_bytes())
    except (OSError, ValueError, IndexError):
        return ""
    return info.get('TITLE_ID', '')


# The languages the game has, by release. The North American one (CUSA18758) is in English
# only; the Hong Kong / Chinese deluxe soundtrack edition (CUSA15006) has the traditional
# Chinese and the Korean. These are the PS4's language ids (sceSystemServiceParamGetInt
# with the parameter 1).
GAME_LANGUAGES = {
    'CUSA18758': (1,),
    'CUSA15006': (10, 11),
}


def problem(game, image_hash=None):
    """None for the supported game, else (kind, title, version): kind is 'missing_update' (the
    base game), 'wrong_eboot' (param.sfo says 1.15, eboot.bin is another version), 'other_title',
    'unreadable' or 'damaged_files' (the right executable, game files missing)."""
    if os.environ.get('BB_SKIP_GAME_CHECK') == '1':
        return None
    from prepare import sfo
    try:
        info = sfo((Path(game) / 'sce_sys/param.sfo').read_bytes())
    except (OSError, ValueError, IndexError):
        info = {}
    title, version = info.get('TITLE_ID', '?'), info.get('APP_VER', '?')
    try:
        image_hash = image_hash or image_sha256(game)
        # The eboot decides: its image is one of the known ones for its title (or of another
        # title that shares it, which then runs too).
        if image_hash in known_images():
            return ('damaged_files', title, version) if missing_files(game) else None
    except (OSError, ValueError, IndexError, StopIteration, KeyError):
        return 'unreadable', title, version
    if image_hash == BASE_IMAGE:
        return 'missing_update', title, version
    if title not in SUPPORTED_TITLES:
        return 'other_title', title, version
    if version != SUPPORTED_VERSION and not (title in VERSION_IN_SFO_FIELD and
                                              info.get('VERSION') == SUPPORTED_VERSION):
        return 'missing_update', title, version
    return 'wrong_eboot', title, version


def known_images():
    """Every known 1.15 eboot image, whichever title it belongs to. SUPPORTED_IMAGE is the one
    of the reference release; it is kept separate so it can be pointed at another image."""
    images = {image for images in SUPPORTED_IMAGES.values() for image in images}
    images.add(SUPPORTED_IMAGE)
    return images


def title_of_image(image_hash):
    """The title whose eboot image this is, or None."""
    for title, images in SUPPORTED_IMAGES.items():
        if image_hash in images:
            return title
    return None


def explain(kind, title, version):
    """What is wrong and what to do, for the log (English)."""
    found = f'Found {title} version {version} (sce_sys/param.sfo).'
    releases = ' or '.join(SUPPORTED_TITLES)
    return {
        'missing_update': f'{found} mbport needs the 1.15 update merged into the game folder: copy '
                          'everything from the dumped update (e.g. CUSA18758-patch) into the game '
                          'folder, replacing files (eboot.bin and sce_sys too).',
        'wrong_eboot': f'{found} param.sfo is from 1.15 but eboot.bin is not: copy eboot.bin from '
                       'the dumped 1.15 update into the game folder, replacing the old one.',
        'other_title': f'{found} This is not a retail release of MAXIBOOST ON that mbport knows: '
                       f'it runs MOBILE SUIT GUNDAM EXTREME VS. MAXIBOOST ON ({releases}) with '
                       'update 1.15.',
        'unreadable': f'{found} eboot.bin could not be read as a decrypted PS4 executable: dump '
                      'the game and the 1.15 update again.',
        'damaged_files': f'{found} The game folder is incomplete: one of '
                         f'{", ".join(REQUIRED_FILES)} is not there. Extract the game again.',
    }[kind]


def main(argv):
    """game_check.py GAME_DIR: the result of the check, in words."""
    if len(argv) != 2:
        print('Usage: game_check.py GAME_DIR', file=sys.stderr)
        return 2
    found = problem(argv[1])
    if found:
        print(explain(*found))
    else:
        title = title_of_image(image_sha256(argv[1])) or 'MAXIBOOST ON'
        print(f'MAXIBOOST ON 1.15: supported ({title})')
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
