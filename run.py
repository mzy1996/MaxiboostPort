#!/usr/bin/env python3
"""Start the game on Windows (and, with the same steps, on Linux).

The Linux launcher runs run.sh; this script does the same work in Python, so that the Windows
build needs no bash. The steps are the same and in the same order:

1. the game's folder is prepared (the eboot is decrypted and linked with the game's modules),
2. the add-ons of the DLC folder are reported as installed,
3. the AppContent profile and the patches (the XML ones and the JSON cheats) are written,
4. the runtime is started with the prepared image.

The environment variables are the ones the launcher sets (BB_GAME_DIR, BB_USER_DIR, BB_DLC_DIR,
BB_PATCHES_DIR, BB_CHEATS, BB_FPS, BB_RENDER_RES, ...); see launcher/bbport_launcher.py.
"""
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
IS_WINDOWS = os.name == "nt"


def fail(message):
    print(f"STOP: {message}", file=sys.stderr)
    raise SystemExit(1)


def run_python(script, *args, **kwargs):
    """Runs one of the repository's scripts and returns its output."""
    result = subprocess.run([sys.executable, str(ROOT / "scripts" / script), *map(str, args)],
                            capture_output=True, text=True, cwd=str(ROOT), **kwargs)
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr, end="")
    return result


def lan_address():
    """The address the other PCs reach this one at (the games find each other through it)."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect(("1.1.0.1", 80))
            return probe.getsockname()[0]
    except OSError:
        try:
            return socket.gethostbyname(socket.gethostname())
        except OSError:
            return ""


def server_up(host, port, timeout=0.5):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def main():
    data = Path(os.environ.get("BB_DATA_DIR", ROOT))
    out = data / "out"
    out.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("BB_CONFIG", str(data / "bbport.ini"))
    user_dir = Path(os.environ.get("BB_USER_DIR", data / "user"))
    os.environ.setdefault("BB_GPU_USER_DIR", str(user_dir))
    user_dir.mkdir(parents=True, exist_ok=True)

    # The game's folder (eboot.bin, sce_module, ...).
    game = Path(os.environ.get("BB_GAME_DIR", ROOT / "CUSA18758-full"))
    if not (game / "eboot.bin").is_file():
        fail(f"no eboot.bin in {game} (set BB_GAME_DIR)")
    original_game = game

    # The private merged view of the mods (scripts/mods.py); it lasts for this launch.
    mods = run_python("mods.py", game, "--out", out,
                      "--mods-dir", os.environ.get("BB_MODS_DIR", data / "mods"),
                      "--config", os.environ.get("BB_MODS_CONFIG", data / "mods.json"),
                      "--enabled", os.environ.get("BB_MODS_ENABLED", "1"), "--ui-mods", "")
    if mods.returncode == 0 and mods.stdout.strip():
        game = Path(mods.stdout.strip())

    # The eboot is decrypted and prepared (scripts/prepare.py).
    if run_python("prepare.py", game, "--out", out).returncode != 0:
        fail("the game could not be prepared")

    # Two players on one PC, and the play over the local network (BB_LAN).
    lan = os.environ.get("BB_LAN", "")
    if lan == "host":
        address = lan_address()
        if not address:
            fail("this PC has no network address for the other PCs to reach")
        os.environ["BB_ONLINE"] = "1"
        os.environ.setdefault("BB_SHADNET_SERVER", f"{address}:31333")
        os.environ.setdefault("SHADPS4_P2P_PORT", "3658")
        print(f"Network play: other PCs join this one at {address}")
    elif lan == "join":
        if not os.environ.get("BB_LAN_SERVER"):
            fail("network play needs the host PC's address (BB_LAN_SERVER)")
        server = os.environ["BB_LAN_SERVER"]
        if ":" not in server:
            server += ":31333"
        os.environ["BB_ONLINE"] = "1"
        os.environ.setdefault("BB_SHADNET_SERVER", server)
        os.environ.setdefault("SHADPS4_P2P_PORT", "3658")

    # The game's modules are linked into the prepared image (scripts/link_modules.py).
    modules = ["libc.prx", "libSceFios2.prx"]
    if os.environ.get("BB_ONLINE", "0") == "1" and (game / "sce_module/libSceNpToolkit2.prx").is_file():
        modules.append("libSceNpToolkit2.prx")
    if run_python("link_modules.py", game, "--out", out, "--modules", *modules).returncode != 0:
        fail("the game's modules could not be linked")

    # The add-ons of the DLC folder are reported as installed.
    dlc_dir = os.environ.get("BB_DLC_DIR", "")
    if dlc_dir and Path(dlc_dir).is_dir():
        result = run_python("dlc.py", "link", "--dlc", dlc_dir, "--game", game, "--user", user_dir)
        if result.returncode != 0:
            print(f"DLC: the add-ons in {dlc_dir} were not set up", file=sys.stderr)

    # The AppContent profile (the SKU and the region).
    run_python("content_profile.py", game, "--out", out,
               "--sku", os.environ.get("BB_CONTENT_SKU", "full"),
               "--region", os.environ.get("BB_REGION", ""))

    # The output size the previous launch chose is recomputed after an in-game restart.
    if os.environ.get("BB_AUTO_RENDER_RES") == "1":
        for name in ("BB_RENDER_RES", "BB_OUTPUT_RES", "BB_AUTO_RENDER_RES"):
            os.environ.pop(name, None)

    # The patches and the cheats (scripts/patches.py) write out/patches.bin.
    result = run_python("patches.py", "--out", out,
                        "--fps", os.environ.get("BB_FPS", "60"),
                        "--extra", os.environ.get("BB_PATCHES", ""),
                        "--settings", os.environ["BB_CONFIG"], "--game-dir", game,
                        "--render-res", os.environ.get("BB_RENDER_RES", ""),
                        "--output-res", os.environ.get("BB_OUTPUT_RES", ""),
                        "--patches-dir", os.environ.get("BB_PATCHES_DIR", data / "patches"),
                        "--patches-config", os.environ.get("BB_PATCHES_CONFIG", data / "patches.json"),
                        "--cheats", os.environ.get("BB_CHEATS", ""),
                        "--cheats-config", os.environ.get("BB_CHEATS_CONFIG", data / "cheats.json"))
    if result.returncode != 0:
        fail("the patches could not be written")

    # The trophies (scripts/trophies.py): the dump's list and icons in the user directory.
    trophies = run_python("trophies.py", "prepare", "--game", original_game, "--user", user_dir,
                           "--language", os.environ.get("BB_LANGUAGE", "1"))
    if trophies.returncode == 0 and trophies.stdout.strip():
        os.environ["BB_TROPHY_DIR"] = trophies.stdout.strip()

    # The server configuration the game asks its publisher's service for.
    os.environ.setdefault("BB_GAME_ENV", str(ROOT / "data/maxiboost_env.json"))
    os.environ.setdefault("BB_GAME_ENV_TSS_SLOT", "1")

    # The online module's own server (BB_ONLINE=1), started for this launch when none listens.
    server = None
    if os.environ.get("BB_ONLINE", "0") == "1":
        os.environ.setdefault("BB_SHADNET_SERVER", "127.0.0.1:31333")
        os.environ.setdefault("BB_SHADNET_NPID", os.environ.get("USERNAME", "Player"))
        os.environ.setdefault("BB_SHADNET_PASSWORD", "local")
        os.environ.setdefault("BB_UPNP", "0")
        address, _, port = os.environ["BB_SHADNET_SERVER"].rpartition(":")
        os.environ.setdefault("BB_SHADNET_WEBAPI", f"http://{address}:{int(port) + 2}")
        if address in ("127.0.0.1", "localhost") and not server_up(address, int(port)):
            binary = ROOT / "bin/gvnet-server" if os.environ.get("BB_PREBUILT") else ROOT / "out/gpu/gvnet-server"
            if not binary.exists():
                binary = binary.with_suffix(".exe")
            if not binary.exists():
                fail(f"online play needs {binary} (the online module is not in this build)")
            bind = "0.0.0.0" if os.environ.get("BB_LAN") == "host" else "127.0.0.1"
            server = subprocess.Popen([str(binary), "--bind", bind, "--port", port],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
            for _ in range(50):
                if server_up(address, int(port)):
                    break
                time.sleep(0.1)
            print(f"Online: gvnet-server on {bind}:{port} for this launch")

    # The runtime itself.
    probe = os.environ.get("BB_PROBE")
    if not probe:
        probe = str(ROOT / "bin/bb-probe" if os.environ.get("BB_PREBUILT") else ROOT / "out/bb-probe")
        if not Path(probe).exists():
            probe = str(Path(probe).with_suffix(".exe"))
    if not Path(probe).exists():
        fail(f"the runtime is not built ({probe})")
    arguments = [probe, str(out / "boot-linked.bin"),
                 "--content-profile", str(out / "content.bin"),
                 "--patches", str(out / "patches.bin"),
                 "--app0", str(game),
                 "--user", str(user_dir),
                 "--timeout", os.environ.get("BB_TIMEOUT", "0"), *sys.argv[1:]]

    try:
        return subprocess.call(arguments, cwd=str(ROOT))
    finally:
        if server:
            server.terminate()
        if game != original_game and game.exists():
            # The private merged view of the mods lasts for this launch only.
            shutil.rmtree(game, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
