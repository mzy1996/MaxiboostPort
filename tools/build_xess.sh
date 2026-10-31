#!/usr/bin/env bash
# XeSS (Intel Xe Super Sampling) for any Vulkan GPU: fetches the Intel XeSS SDK (the release
# zip, ~200 MB, once into ~/.cache/bbport), builds the bridge (gpu/xess_bridge, the only code
# that uses the SDK) and installs it with Intel's xess library and license next to bb-probe
# (out/), where the port looks for them.
#
# Cross-vendor: XeSS runs on Intel XMX and on any other Vulkan GPU through its DP4a / generic
# fallback, so the port enables it for every vendor. Without these files the port keeps its
# other upscalers (FSR 3.1 by default). Needs the Intel xess.dll / libxess.so at run time.
#
# Intel XeSS SDK: https://github.com/intel/xess. The release assets (XeSS_SDK_<ver>.zip) carry
# the prebuilt libraries and headers: include/xess/*.h, lib/Win64/vc16/xess.lib,
# bin/Win64/xess.dll (+ libxell.dll), lib/Linux/libxess.{a,so}. Pin a tag with XESS_VERSION
# (v2.1.1: xess_vk.h with the xessVKCreateContext / xessVKInit / xessVKExecute API the bridge
# calls; v1.3.x has no usable Vulkan header).
# To skip the fetch, point XESS_SDK_ROOT at an already-unpacked SDK.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
version=${XESS_VERSION:-v2.1.1}
cache=${XDG_CACHE_HOME:-$HOME/.cache}/bbport
sdk=${XESS_SDK_ROOT:-$cache/xess-sdk-$version}

if [[ ! -d $sdk ]]; then
    url=https://github.com/intel/xess/releases/download/$version/XeSS_SDK_${version#v}.zip
    mkdir -p "$cache" "$sdk"
    echo "Fetching $url"
    curl -fL --retry 3 -o "$cache/xess-$version.zip" "$url"
    # bsdtar reads zip archives. Prefer Windows' own tar (C:\Windows\System32); msys2's GNU tar
    # cannot read zip, and unzip may not be installed.
    sdk_win=$(cygpath -w "$sdk" 2>/dev/null || echo "$sdk")
    if [[ -x /c/Windows/System32/tar.exe ]]; then
        /c/Windows/System32/tar.exe -xf "$(cygpath -w "$cache/xess-$version.zip" 2>/dev/null || echo "$cache/xess-$version.zip")" -C "$sdk_win"
    elif command -v unzip > /dev/null; then
        unzip -q "$cache/xess-$version.zip" -d "$sdk"
    else
        tar -xf "$cache/xess-$version.zip" -C "$sdk"
    fi
    rm -f "$cache/xess-$version.zip"
fi

# Normalize the two layouts the SDK comes in: the release zip (and git repo) has inc/xess,
# lib/libxess.lib and bin/libxess.dll; the SDK's own documentation uses include/xess,
# lib/Win64/vc16/xess.lib and bin/Win64/xess.dll. The bridge's CMakeLists expects the latter.
if [[ ! -d $sdk/include/xess && -d $sdk/inc/xess ]]; then
    mkdir -p "$sdk/include" "$sdk/lib/Win64/vc16" "$sdk/bin/Win64"
    cp -r "$sdk/inc/xess" "$sdk/include/xess"
    cp "$sdk/lib/libxess.lib" "$sdk/lib/Win64/vc16/xess.lib"
    cp "$sdk/bin/libxess.dll" "$sdk/bin/Win64/xess.dll"
    cp "$sdk/bin/libxell.dll" "$sdk/bin/Win64/libxell.dll" 2> /dev/null || true
fi
[[ -d $sdk/include/xess ]] || { echo "No include/xess in $sdk; not an unpacked XeSS SDK" >&2; exit 1; }

if [[ "${OS:-}" == Windows_NT || "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    msys=${MSYS2_ROOT:-/c/devkitPro/msys2}
    vulkan_include=$(cygpath -m "$msys/mingw64/include" 2>/dev/null || echo "$msys/mingw64/include")
    cmake -S gpu/xess_bridge -B out/xess-bridge -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DXESS_SDK_ROOT="$sdk" -DVULKAN_INCLUDE="$vulkan_include" > /dev/null
    ninja -C out/xess-bridge > /dev/null
    rm -f out/win/bbport_xess.dll out/win/libxess.dll out/win/libxell.dll out/win/xess.dll
    install -m755 out/xess-bridge/bbport_xess.dll out/win/
    # Intel ships the XeSS shared library as libxess.dll (the bridge links it implicitly). The
    # renderer's XessLibName matches this; the older Win64/xess.dll is the same file under another
    # name and is not used.
    install -m755 "$sdk/bin/libxess.dll" out/win/
    # libxell.dll is XeLL (frame generation); libxess.dll 2.x loads it when frame generation is on.
    if [[ -s $sdk/bin/libxell.dll ]]; then install -m755 "$sdk/bin/libxell.dll" out/win/; fi
    install -m644 "$sdk/LICENSE.txt" out/win/INTEL-XESS-LICENSE.txt
    echo "XeSS ready: out/win/bbport_xess.dll, out/win/libxess.dll"
else
    cmake -S gpu/xess_bridge -B out/xess-bridge -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DXESS_SDK_ROOT="$sdk" > /dev/null
    ninja -C out/xess-bridge > /dev/null
    rm -f out/libbbport_xess.so out/libxess.so out/gpu/libbbport_xess.so out/gpu/libxess.so
    install -m755 out/xess-bridge/libbbport_xess.so out/
    # The SDK ships the shared library under bin/Linux (2.x) or lib/Linux (1.x).
    for lib in "$sdk/bin/Linux/libxess.so" "$sdk/lib/Linux/libxess.so"; do
        [[ -s $lib ]] && { install -m755 "$lib" out/; break; }
    done
    install -m644 "$sdk/LICENSE.txt" out/INTEL-XESS-LICENSE.txt
    echo "XeSS ready: out/libbbport_xess.so, out/libxess.so"
fi
