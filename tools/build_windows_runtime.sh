#!/usr/bin/env bash
# Compile the runtime for Windows (MinGW-w64) and report what is missing.
#
# The runtime alone (the GPU library is built by tools/build_windows_all.sh).
#
# The guest speaks the SysV ABI and uses pthreads, so the Windows build is made with
# MinGW-w64 (which provides both), not with MSVC.
#
#   BUILD_WINDOWS=1 bash tools/build_windows.sh
#
# MSYS2's tools need their runtime on PATH; without it they fail silently, which is
# confusing, so the script sets it up itself.
set -u

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
msys=${MSYS2_ROOT:-/c/devkitPro/msys2}
export PATH="$msys/mingw64/bin:$msys/usr/bin:$PATH"

if [ ! -x "$msys/mingw64/bin/gcc.exe" ]; then
    echo "MinGW-w64 not found in $msys (set MSYS2_ROOT)" >&2
    exit 1
fi

cc=$msys/mingw64/bin/gcc.exe
out=${OUT:-$root/out/win}
mkdir -p "$out"
# The compiler is a Windows program: it wants Windows paths.
out_win=$(cd "$out" && pwd -W 2>/dev/null || echo "$out")
root_win=$(cd "$root" && pwd -W 2>/dev/null || echo "$root")
msys_win=$(cd "$msys" && pwd -W 2>/dev/null || echo "$msys")

# The runtime's own sources; the POSIX platform layer is not part of the Windows build.
sources=()
for file in "$root"/src/*.c; do
    case $(basename "$file") in
        platform_posix.c) continue ;;
    esac
    sources+=("$file")
done

# The libraries the runtime links against. Vulkan, SDL3 and freetype come from the
# toolchain's package manager; the import libraries are named the way MinGW names them
# (libvulkan-1.dll.a for the Vulkan loader). The GPU library (libbbgpu.dll) is the port's own.
libs=(-lvulkan-1 -lSDL3 -lfreetype -lbcrypt -lpsapi -lws2_32)

# The GPU library: the runtime's graphics go through it (bbgpu.h).
gpu_dir=${GPU_DIR:-$root/out/gpu-win}
gpu_lib=$(find "$gpu_dir" -maxdepth 2 -name '*.dll.a' 2>/dev/null | grep -v libbbruntime | head -1)
if [ -z "$gpu_lib" ]; then
    echo "The GPU library is missing ($gpu_dir); run tools/build_windows_all.sh first" >&2
    exit 1
fi
gpu_lib_win=$(cd "$(dirname "$gpu_lib")" && pwd -W 2>/dev/null)/$(basename "$gpu_lib")
gpu_include_win=$(cd "$root/gpu" && pwd -W 2>/dev/null)
echo "GPU library: $gpu_lib_win"

# The ATRAC9 decoder (third_party/LibAtrac9), a plain C library the runtime links statically.
atrac=$root/third_party/LibAtrac9/C/src
if [ ! -d "$atrac" ]; then
    echo "LibAtrac9 is missing: git submodule update --init third_party/LibAtrac9" >&2
    exit 1
fi
mkdir -p "$out_win"
atrac_objects=()
for file in "$atrac"/*.c; do
    name=$(basename "$file" .c)
    "$cc" -std=c99 -O2 -c -I "$atrac" -I "$root_win/third_party/LibAtrac9/C/include" \
        -o "$out_win/$name.o" "$file" || exit 1
    atrac_objects+=("$out_win/$name.o")
done
ar rcs "$out_win/libatrac9.a" "${atrac_objects[@]}"

echo "Compiling for Windows: ${#sources[@]} sources -> $out/bb-probe.exe"

# The runtime exports the symbols the GPU library calls (the guest's memory, the fault
# recovery). On Linux the dynamic linker does this (the runtime is linked with -rdynamic);
# Windows needs a module definition file, and the same file gives the library the import
# library (the GPU build makes it in gpu/CMakeLists.txt).
# The GPU library imports these from the runtime by the module's name ("bb-probe.exe"), so
# the runtime has to keep that name: under any other name the loader cannot resolve the
# library's imports and it calls through an unresolved import.
exports="$root/src/runtime_exports.txt"
runtime_def="$out_win/bb-probe.def"
if [ -f "$exports" ]; then
    {
        echo "; The runtime's exports: the symbols the GPU library calls."
        echo "EXPORTS"
        grep '^runtime_' "$exports"
    } > "$runtime_def"
    echo "Runtime exports: $(grep -c '^runtime_' "$exports") symbols"
fi

"$cc" -std=c11 -O2 -g -Wall -Wextra \
    -I "$root_win/src" -I "$root_win" -I "$gpu_include_win" -I "$msys_win/mingw64/include" \
    -I "$msys_win/mingw64/include/freetype2" \
    -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS \
    -o "$out_win/bb-probe.exe" "${sources[@]}" "$out_win/libatrac9.a" \
    "$gpu_lib_win" \
    ${runtime_def:+"$runtime_def"} \
    -L "$msys_win/mingw64/lib" "${libs[@]}"
status=$?
if [ $status -eq 0 ]; then
    echo "Built: $out/bb-probe.exe"
    # The GPU library sits beside the runtime.
    gpu_dll=$(find "$gpu_dir" -name 'bbgpu.dll' | head -1)
    [ -n "$gpu_dll" ] && cp "$gpu_dll" "$out/"
    cp "$msys/mingw64/bin/libgcc_s_seh-1.dll" "$msys/mingw64/bin/libwinpthread-1.dll" \
       "$msys/mingw64/bin/libstdc++-6.dll" "$out/" 2>/dev/null || true
fi
exit $status
