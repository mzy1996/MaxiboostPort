#!/usr/bin/env bash
# Pack the Windows build into a folder that can be zipped and unpacked on a Windows PC.
#
#   bash tools/package_windows.sh
#
# The package holds the runtime (bb-probe.exe), the GPU library (bbgpu.dll), the DLLs the
# program needs at run time, the scripts that prepare the game (the eboot is decrypted and
# linked with the game's modules), the launcher and the documentation.
#
# The game itself is not in the package: the player brings their own dump (the README says
# where it goes).
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
msys=${MSYS2_ROOT:-/c/devkitPro/msys2}
package=${1:-$root/out/mbport-windows}

if [ ! -f "$root/out/win/bb-probe.exe" ]; then
    echo "The runtime is not built (out/win/bb-probe.exe); run tools/build_windows_all.sh" >&2
    exit 1
fi

rm -rf "$package"
mkdir -p "$package"/{bin,scripts,launcher,patches,docs}

# The program and the library.
cp "$root/out/win/bb-probe.exe" "$package/bin/"
gpu_dll=$(find "$root/out/gpu-win" -name 'bbgpu.dll' | head -1)
[ -n "$gpu_dll" ] && cp "$gpu_dll" "$package/bin/"

# The debug symbols are dropped: the library is 300 MB with them and 13 MB without, and the
# player has no use for them.
strip_bin="$msys/mingw64/bin/strip.exe"
if [ -x "$strip_bin" ]; then
    for binary in "$package/bin/bb-probe.exe" "$package/bin/bbgpu.dll"; do
        [ -f "$binary" ] && "$strip_bin" --strip-unneeded "$binary" 2>/dev/null || true
    done
fi

# The DLLs the program needs at run time. The list is worked out from the program's own
# import table (tools/collect_dlls.py), so it stays right when a dependency changes; the
# ones that come with Windows are left out.
# The Python interpreter: the one of the environment, then the usual ones.
python_bin=""
for candidate in "${PYTHON:-}" python python3 py; do
    if [ -n "$candidate" ] && command -v "$candidate" >/dev/null 2>&1; then
        python_bin=$candidate
        break
    fi
done
if [ -z "$python_bin" ]; then
    echo "STOP: Python was not found: the program's DLLs cannot be collected" >&2
    exit 1
fi
# The search path: the toolchain's libraries and the program itself. The runtime is not
# a DLL (the GPU library imports its symbols), so it is not looked for.
dll_path="$msys/mingw64/bin:$package/bin"
if ! BB_DLL_PATH="$dll_path" BB_DLL_IGNORE="bb-probe.exe" \
        "$python_bin" "$root/tools/collect_dlls.py" \
        "$package/bin/bbgpu.dll" "$package/bin"; then
    echo "STOP: some DLLs the program needs were not found: the package would not run" >&2
    exit 1
fi
BB_DLL_PATH="$dll_path" BB_DLL_IGNORE="bb-probe.exe" \
    "$python_bin" "$root/tools/collect_dlls.py" \
    "$package/bin/bb-probe.exe" "$package/bin" || true

# The check: every import of every file in the package must be satisfied by the package
# itself (or by Windows). A package that would not start is caught here.
if ! "$python_bin" "$root/tools/check_package.py" "$package"; then
    echo "STOP: the package is not complete" >&2
    exit 1
fi

# The scripts that prepare the game.
cp "$root/scripts"/*.py "$package/scripts/"

# The launcher (the Windows one and the translations it uses).
cp "$root/launcher/win_launcher.py" "$package/launcher/"
cp "$root/launcher/bbport_i18n.py" "$package/launcher/"
cp "$root/launcher/bbport_assets.py" "$package/launcher/" 2>/dev/null || true
cp "$root/launcher/maxiboost.svg" "$package/launcher/" 2>/dev/null || true
# The file that starts the launcher by a double click.
cp "$root/launcher/launcher.bat" "$package/launcher/"

# The packaged launcher: the executable that needs nothing installed (no Python, no
# PySide6). It is the one the player starts; the Python sources stay in the package for
# the ones who want them.
for packaged in "$root/out/win/launcher-onefile/MAXIBOOST ON.exe" \
                "$root/out/win/launcher/MAXIBOOST ON/MAXIBOOST ON.exe"; do
    if [ -f "$packaged" ]; then
        cp "$packaged" "$package/launcher/"
        echo "The packaged launcher: $package/launcher/MAXIBOOST ON.exe"
        break
    fi
done
if [ ! -f "$package/launcher/MAXIBOOST ON.exe" ]; then
    echo "NOTE: the packaged launcher was not built (tools/build_launcher_exe.py); the"
    echo "      player needs Python for launcher\\win_launcher.py"
fi

# The built-in patch file.
cp "$root/patches/Maxiboost.xml" "$package/patches/"

# The documentation.
cp "$root/README.md" "$root/BUILD.win.md" "$root/LEGAL.md" "$package/docs/" 2>/dev/null || true

# A short note of what the package needs.
cat > "$package/README.txt" <<'EOF'
MAXIBOOST ON for Windows (mbport)

WHAT TO RUN
  launcher\MAXIBOOST ON.exe  the launcher: the window with the game, the cheats and the
                            settings. This is the program to start (double click it).
                            It needs nothing installed: the window's library is inside
                            it.

  launcher\launcher.bat     the same launcher, for the ones who have Python and want to
                            run it from the sources.

  bin\bb-probe.exe          the runtime: it runs the game's own code. The launcher starts
                            it for you; it has no window of its own.

WHAT IS HERE
  bin\                      the runtime and the GPU library, and the DLLs they need
  scripts\                  the scripts that prepare the game (the eboot is decrypted and
                            linked with the game's modules)
  launcher\                 the launcher (the window)
  patches\                  the built-in patch file (the game's fixes)

WHAT IS NEEDED
  - a dump of MOBILE SUIT GUNDAM EXTREME VS. MAXIBOOST ON with update 1.15 merged in
    (the North American release CUSA18758 or the Hong Kong release CUSA15006), in a folder
    the launcher can read;
  - a Vulkan driver (the GPU library needs it);
  - Python 3.9 or newer (the launcher and the scripts).

HOW TO START
  1. put the dump in a folder, e.g. C:\Games\MAXIBOOST ON;
  2. double click launcher\launcher.bat (the first start installs PySide6, the window's
     library, which takes a moment);
  3. choose the game's folder on the first page, press Play.

THE JSON CHEATS
  The "Cheats" page of the launcher lists the cheats of the JSON file you choose, one
  checkbox each. The file CUSA15006_01.15.json of the Hong Kong release is one. The port
  writes the entries you switch on into the executable when the game starts.

THE LEGAL NOTICE
  It is in docs\LEGAL.md: no game files are included here, and none may be.
EOF

echo "Package: $package"
echo "  the program: $package/bin/bb-probe.exe"
du -sh "$package" 2>/dev/null || true
