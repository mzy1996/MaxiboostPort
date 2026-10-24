#!/usr/bin/env bash
# Build the whole port for Windows: the GPU library (bbgpu) and the runtime (bb-probe).
#
# The guest speaks the SysV ABI and uses pthreads, so the Windows build is made with
# MinGW-w64 (which provides both), not with MSVC.
#
#   bash tools/build_windows_all.sh
#
# The toolchain and the libraries come from MSYS2's mingw64 repository:
#   pacman -S --needed mingw-w64-x86_64-{vulkan-headers,vulkan-loader,sdl3,freetype,fmt,
#                                         boost,vulkan-memory-allocator,xxhash,glslang,ffmpeg}
# The header-only libraries (magic_enum, tsl-robin-map, miniz, Zydis, xbyak) are vendored in
# gpu/third_party, so no package manager is needed for them. Boost is vendored too (the
# package manager's copy is incomplete); tools/merge_boost.py merges the per-library
# repositories into the one directory the compiler reads.
#
# The CMake build needs a native Windows CMake and ninja: the MSYS2 ones drive the compiler
# through a shell that rewrites the Windows paths. Both are looked up here; the paths can be
# given in the environment:
#   WINDOWS_CMAKE   the cmake.exe to use
#   WINDOWS_NINJA   the ninja.exe to use
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
msys=${MSYS2_ROOT:-/c/devkitPro/msys2}
export PATH="$msys/mingw64/bin:$msys/usr/bin:$PATH"

if [ ! -x "$msys/mingw64/bin/gcc.exe" ]; then
    echo "MinGW-w64 not found in $msys (set MSYS2_ROOT)" >&2
    exit 1
fi

# The submodules the GPU library and the runtime need.
git -C "$root" submodule update --init --recursive

# The native Windows CMake and ninja (the MSYS2 ones cannot drive a Windows compiler:
# they rewrite the Windows paths of the command line into POSIX ones).
find_tool() {
    local name=$1 hint=$2
    if [ -n "${hint:-}" ] && [ -x "$hint" ]; then
        echo "$hint"
        return 0
    fi
    local found
    found=$(command -v "$name" 2>/dev/null || true)
    if [ -n "$found" ] && [[ "$found" == *[Cc]:* ]]; then
        echo "$found"
        return 0
    fi
    # The CMake and ninja the Python packages install are native Windows programs.
    for candidate in "/c/Program Files/CMake/bin/cmake.exe" \
                     "/c/Users/$USERNAME/AppData/Local/Programs/Python/Python313/Scripts/cmake.exe" \
                     "/c/Users/$USERNAME/AppData/Roaming/Python/Python313/Scripts/cmake.exe"; do
        [ -x "$candidate" ] && { echo "$candidate"; return 0; }
    done
    for candidate in "/c/Users/$USERNAME/AppData/Local/Programs/Python/Python313/Scripts/ninja.exe" \
                     "/c/Users/$USERNAME/AppData/Roaming/Python/Python313/Scripts/ninja.exe"; do
        [ -x "$candidate" ] && { echo "$candidate"; return 0; }
    done
    echo "" >&2
}
# The paths the compiler (a Windows program) understands.
win_path() {
    local path=$1
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$path"
    else
        echo "$path"
    fi
}
cmake_exe=$(find_tool cmake.exe "${WINDOWS_CMAKE:-}")
ninja_exe=$(find_tool ninja.exe "${WINDOWS_NINJA:-}")
if [ -z "$cmake_exe" ] || [ -z "$ninja_exe" ]; then
    echo "A native Windows CMake and ninja are needed (set WINDOWS_CMAKE and WINDOWS_NINJA)" >&2
    exit 1
fi

export CC="C:/devkitPro/msys2/mingw64/bin/gcc.exe"
export CXX="C:/devkitPro/msys2/mingw64/bin/g++.exe"
vulkan_include="$msys/mingw64/include"
vulkan_library="$msys/mingw64/lib/libvulkan-1.dll.a"

# The GPU library. The paths are given to CMake in the Windows form, because CMake here is
# a native Windows program.
root_win=$(cygpath -w "$root" 2>/dev/null || echo "$root")
gpu_win=$(cygpath -w "$root/gpu" 2>/dev/null || echo "$root/gpu")
build_win=$(cygpath -w "$root/out/gpu-win" 2>/dev/null || echo "$root/out/gpu-win")
toolchain_win=$(cygpath -w "$root/tools/mingw-w64.cmake" 2>/dev/null || echo "$root/tools/mingw-w64.cmake")
ninja_win=$(cygpath -w "$ninja_exe" 2>/dev/null || echo "$ninja_exe")

"$cmake_exe" -S "$gpu_win" -B "$build_win" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$ninja_win" \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain_win" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DVulkan_INCLUDE_DIR="C:/devkitPro/msys2/mingw64/include" \
    -DVulkan_LIBRARY="C:/devkitPro/msys2/mingw64/lib/libvulkan-1.dll.a"
"$cmake_exe" --build "$build_win" --target bbgpu

# The runtime.
OUT="$root/out/win" bash "$root/tools/build_windows_runtime.sh"

echo
echo "Windows build:"
echo "  the GPU library: $root/out/gpu-win/bin/bbgpu.dll"
echo "  the runtime:     $root/out/win/bb-probe.exe"
