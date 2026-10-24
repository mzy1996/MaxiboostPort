# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds the packaged launcher: one executable that needs nothing installed.

The player should not have to install Python, PySide6 or anything else: the executable
carries the interpreter, the window's library and the game's preparation scripts.

The scripts are carried as data files, because the launcher runs them as they are (the
same code as in a checkout). They land next to the executable in the folder PyInstaller
unpacks into, and port_prepare.py looks for them there.

    python tools/build_launcher_exe.py [--onefile] [--output DIR]

The result is put in out/win/launcher/ (the executable) and the package's launcher folder
is updated with it.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LAUNCHER = ROOT / "launcher" / "win_launcher.py"

# The scripts the launcher runs. They are imported by name, so PyInstaller has to be told
# about them: they are code, not data.
HIDDEN = [
    "bbport_i18n",
    "game_check",
    "cheats",
    "patches",
    "port_prepare",
    "prepare",
    "link_libc",
    "link_modules",
    "dlc",
    "content_profile",
    "trophies",
    "mods",
    "online",
    "discord_presence",
]

# The Qt modules the launcher does not use: they are big, and the program does not need
# them. The list keeps the executable from carrying the whole of Qt.
EXCLUDED = [
    "PySide6.QtWebEngineCore", "PySide6.QtWebEngineWidgets", "PySide6.QtWebEngineQuick",
    "PySide6.Qt3DCore", "PySide6.Qt3DRender", "PySide6.Qt3DAnimation", "PySide6.Qt3DExtras",
    "PySide6.Qt3DInput", "PySide6.Qt3DLogic", "PySide6.Qt3DRender", "PySide6.QtCharts",
    "PySide6.QtDataVisualization", "PySide6.QtMultimedia", "PySide6.QtMultimediaWidgets",
    "PySide6.QtQuick", "PySide6.QtQuick3D", "PySide6.QtQuickWidgets", "PySide6.QtQml",
    "PySide6.QtQuickControls2", "PySide6.QtQuickShapes", "PySide6.QtQuickTemplates2",
    "PySide6.QtSql", "PySide6.QtTest", "PySide6.QtBluetooth", "PySide6.QtNfc",
    "PySide6.QtPositioning", "PySide6.QtSerialPort", "PySide6.QtSensors", "PySide6.QtWebSockets",
    "PySide6.QtDesigner", "PySide6.QtHelp", "PySide6.QtUiTools", "PySide6.QtPdf",
    "PySide6.QtPdfWidgets", "PySide6.QtSpatialAudio", "PySide6.QtTextToSpeech",
    "PySide6.QtRemoteObjects", "PySide6.QtScxml", "PySide6.QtStateMachine",
    "PySide6.QtSvgWidgets", "PySide6.QtOpenGL", "PySide6.QtOpenGLWidgets",
    "PySide6.QtNetworkAuth", "PySide6.QtHttpServer", "PySide6.QtGraphs",
    "tkinter", "unittest", "pydoc_data", "test", "lib2to3", "distutils",
    "numpy", "PIL", "matplotlib", "setuptools", "pip", "sqlite3", "lxml",
]


def build(onefile=True, output=None, clean=True):
    output = Path(output) if output else ROOT / "out" / "win" / "launcher"
    output.mkdir(parents=True, exist_ok=True)
    work = ROOT / "out" / "launcher-build"
    if clean and work.exists():
        shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True, exist_ok=True)

    command = [
        sys.executable, "-m", "PyInstaller",
        "--noconfirm",
        "--clean",
        "--name", "MAXIBOOST ON",
        "--distpath", str(output),
        "--workpath", str(work),
        "--specpath", str(work),
        "--paths", str(ROOT / "launcher"),
        "--paths", str(ROOT / "scripts"),
        # The window's own icon and the translations are data, not code.
        "--add-data", f"{ROOT / 'launcher' / 'bbport_i18n.py'}{os.pathsep}.",
        "--add-data", f"{ROOT / 'launcher' / 'maxiboost.svg'}{os.pathsep}.",
        # The game's own data (the server configuration the game asks for).
        "--add-data", f"{ROOT / 'data'}{os.pathsep}data",
    ]
    command += [item for name in HIDDEN for item in ("--hidden-import", name)]
    command += [item for name in EXCLUDED for item in ("--exclude-module", name)]
    if onefile:
        command.append("--onefile")
    else:
        command.append("--onedir")
    command.append(str(LAUNCHER))

    print("Building the packaged launcher:", " ".join(command[:6]), "...")
    result = subprocess.run(command, cwd=str(ROOT))
    if result.returncode != 0:
        print("The launcher could not be built", file=sys.stderr)
        return 1
    built = output / ("MAXIBOOST ON.exe" if onefile else "MAXIBOOST ON")
    if not built.exists():
        print(f"The launcher was not found at {built}", file=sys.stderr)
        return 1
    size = built.stat().st_size if built.is_file() else sum(
        f.stat().st_size for f in built.rglob('*') if f.is_file())
    print(f"Built: {built} ({size / 1048576:.1f} MB)")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--onedir", action="store_true",
                        help="a folder instead of a single file (it starts faster)")
    parser.add_argument("--output", default=None, help="where the executable goes")
    args = parser.parse_args(argv[1:])
    return build(onefile=not args.onedir, output=args.output)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
