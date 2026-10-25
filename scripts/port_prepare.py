# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepares the game for the runtime: the steps between the dump and the program.

The eboot is decrypted and prepared (prepare.py), the game's modules are linked into it
(link_modules.py), the add-ons of the DLC folder are set up (dlc.py), the AppContent
profile and the patches are written (content_profile.py, patches.py) and the trophies are
copied to the user's directory (trophies.py).

The steps are the repository's own scripts, called as functions. That matters for the
packaged program (a single executable): the code is carried along inside it, so the same
steps run whether the player has Python installed or not.

The environment variables are the ones the launcher sets (BB_GAME_DIR, BB_USER_DIR,
BB_DLC_DIR, BB_PATCHES_DIR, BB_CHEATS, BB_FPS, BB_RENDER_RES, ...).
"""
import io
import os
import sys
from pathlib import Path

# The repository's own directory: where the scripts and the runtime live. In a packaged
# program it is the folder the executable was unpacked into.
ROOT = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent.parent))
if str(ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts"))


class PrepareError(Exception):
    """The game could not be prepared."""


def _capture(function, *args, **kwargs):
    """Calls one of the scripts' functions and returns what it printed.

    The scripts print their progress; the launcher shows it in its log, so the output is
    taken instead of going to the console."""
    buffer = io.StringIO()
    stdout, stderr = sys.stdout, sys.stderr
    sys.stdout = buffer
    sys.stderr = buffer
    try:
        function(*args, **kwargs)
    finally:
        sys.stdout, sys.stderr = stdout, stderr
    return buffer.getvalue()


def prepare_game(data, config, modules=None, game=None):
    """Prepares the game and returns what the runtime needs: the image, the profile and the
    patches, and the folder the game is played from.

    The game's folder is the one the player chose (game); the environment names it when
    there is none."""
    import cheats  # noqa: F401  (the patches read the cheat table through it)
    import content_profile
    import dlc
    import link_modules
    import patches
    import prepare
    import trophies

    out = Path(data) / "out"
    out.mkdir(parents=True, exist_ok=True)
    user_dir = Path(os.environ.get("BB_USER_DIR", Path(data) / "user"))
    user_dir.mkdir(parents=True, exist_ok=True)

    game = Path(game) if game else Path(os.environ.get("BB_GAME_DIR", ROOT / "CUSA18758-full"))
    if not (game / "eboot.bin").is_file():
        raise PrepareError(f"no eboot.bin in {game} (the game's folder is not set)")
    original_game = game

    # The eboot is decrypted and prepared.
    try:
        _capture(prepare.prepare, game, out)
    except prepare.GameCheckError as error:
        raise PrepareError(f"unsupported game files: {error}") from error
    except Exception as error:
        raise PrepareError(f"the game could not be prepared: {error}") from error

    # The game's modules are linked into the prepared image.
    names = list(modules) if modules else ["libc.prx", "libSceFios2.prx"]
    if os.environ.get("BB_ONLINE", "0") == "1" and (game / "sce_module/libSceNpToolkit2.prx").is_file():
        names.append("libSceNpToolkit2.prx")
    try:
        _capture(link_modules.link, game, out, names)
    except Exception as error:
        raise PrepareError(f"the game's modules could not be linked: {error}") from error

    # The add-ons of the DLC folder are reported as installed.
    dlc_dir = os.environ.get("BB_DLC_DIR", "")
    if dlc_dir and Path(dlc_dir).is_dir():
        try:
            _capture(dlc.link, Path(dlc_dir), game, user_dir)
        except Exception as error:
            print(f"DLC: the add-ons in {dlc_dir} were not set up: {error}", file=sys.stderr)

    # The AppContent profile (the SKU and the region).
    try:
        _capture(content_profile.prepare, game, out,
                 os.environ.get("BB_CONTENT_SKU", "full"),
                 os.environ.get("BB_REGION", ""))
    except Exception as error:
        print(f"Profile: it could not be written: {error}", file=sys.stderr)

    # The patches and the cheats (out/patches.bin).
    try:
        _write_patches(patches, out, game, Path(data), config)
    except Exception as error:
        raise PrepareError(f"the patches could not be written: {error}") from error

    # The trophies (the dump's list and icons in the user directory).
    try:
        directory = _capture(trophies.prepare, original_game, user_dir, None,
                             os.environ.get("BB_LANGUAGE", "1"))
        directory = directory.strip().splitlines()[-1] if directory.strip() else ""
        if directory:
            os.environ["BB_TROPHY_DIR"] = directory
    except Exception as error:
        print(f"Trophies: they were not copied: {error}", file=sys.stderr)

    # The server configuration the game asks its publisher's service for.
    os.environ.setdefault("BB_GAME_ENV", str(ROOT / "data/maxiboost_env.json"))
    os.environ.setdefault("BB_GAME_ENV_TSS_SLOT", "1")

    return {
        "image": out / "boot-linked.bin",
        "profile": out / "content.bin",
        "patches": out / "patches.bin",
        "game": game,
        "original_game": original_game,
        "user_dir": user_dir,
        "out": out,
    }


def _write_patches(patches, out, game, data, config):
    """The patches and the cheats are written (out/patches.bin).

    The script's own main() does it; it is called with the arguments it would get on the
    command line, so that the two ways are the same."""
    import sys as _sys
    argv = _sys.argv
    _sys.argv = [
        "patches.py",
        "--out", str(out),
        "--fps", os.environ.get("BB_FPS", "60"),
        "--extra", os.environ.get("BB_PATCHES", ""),
        "--settings", str(config),
        "--game-dir", str(game),
        "--render-res", os.environ.get("BB_RENDER_RES", ""),
        "--output-res", os.environ.get("BB_OUTPUT_RES", ""),
        "--patches-dir", str(os.environ.get("BB_PATCHES_DIR", data / "patches")),
        "--patches-config", str(os.environ.get("BB_PATCHES_CONFIG", data / "patches.json")),
        "--cheats", os.environ.get("BB_CHEATS", ""),
        "--cheats-config", str(os.environ.get("BB_CHEATS_CONFIG", data / "cheats.json")),
    ]
    try:
        patches.main()
    finally:
        _sys.argv = argv


def runtime_arguments(prepared, extra=None):
    """The arguments the runtime is started with."""
    return [
        str(prepared["image"]),
        "--content-profile", str(prepared["profile"]),
        "--patches", str(prepared["patches"]),
        "--app0", str(prepared["game"]),
        "--user", str(prepared["user_dir"]),
        "--timeout", os.environ.get("BB_TIMEOUT", "0"),
        *(extra or []),
    ]


def runtime_path():
    """The runtime's program: the one of the package, or the one of the build tree.

    In a packaged program the runtime sits beside the executable, or in the bin folder of
    the package (the launcher is in the launcher folder, the runtime in bin)."""
    base = Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else ROOT
    for candidate in (base / "bin" / "bb-probe.exe",
                      base / "bb-probe.exe",
                      base.parent / "bin" / "bb-probe.exe",
                      base.parent / "bb-probe.exe",
                      ROOT / "bin" / "bb-probe.exe",
                      ROOT / "out" / "win" / "bin" / "bb-probe.exe",
                      ROOT / "out" / "win" / "bb-probe.exe"):
        if candidate.is_file():
            return candidate
    return None
