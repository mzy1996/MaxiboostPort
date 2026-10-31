#!/usr/bin/env bash
# XeSS (Intel Xe Super Sampling) for any Vulkan GPU: fetches the Intel XeSS SDK files it needs
# (headers, the xess.lib import library and the xess DLL, ~30 MB, once into ~/.cache/bbport),
# builds the bridge (gpu/xess_bridge, the only code that uses the SDK) and installs it with
# Intel's xess library and license next to bb-probe (out/), where the port looks for them.
#
# Cross-vendor: XeSS runs on Intel XMX and on any other Vulkan GPU through its DP4a / generic
# fallback, so the port enables it for every vendor. Without these files the port keeps its other
# upscalers (FSR 3.1 by default). Needs the Intel XeSS DLL/SO at run time.
#
# Intel XeSS SDK: https://github.com/intel/XeSS-SDK (headers under include/xess/, prebuilt libs
# under lib/). Pin a release tag with XESS_VERSION. The lib paths below follow the SDK layout; if a
# release differs, pass XESS_SDK_ROOT to an already-unpacked SDK and skip the fetch.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
version=${XESS_VERSION:-v1.3.1}
sdk=${XESS_SDK_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/bbport/xess-sdk-$version}
base=https://raw.githubusercontent.com/intel/XeSS-SDK/$version
fetch() {
    [[ -s $sdk/$1 ]] && return
    mkdir -p "$(dirname "$sdk/$1")"
    curl -fsSL --retry 3 -o "$sdk/$1.part" "$base/$1"
    mv "$sdk/$1.part" "$sdk/$1"
}
headers=(include/xess/xess.h include/xess/xess_vk.h include/xess/xess_version.h
         include/xess/xess_assets.h include/xess/xess_constants.h)
for header in "${headers[@]}"; do fetch "$header"; done
fetch LICENSE.txt

if [[ "${OS:-}" == Windows_NT || "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    fetch lib/Win64/vc16/xess.lib
    fetch bin/Win64/xess.dll
    cmake -S gpu/xess_bridge -B out/xess-bridge -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DXESS_SDK_ROOT="$sdk" > /dev/null
    ninja -C out/xess-bridge > /dev/null
    rm -f out/bbport_xess.dll out/xess.dll out/gpu/bbport_xess.dll out/gpu/xess.dll
    install -m755 out/xess-bridge/bbport_xess.dll out/
    install -m755 "$sdk/bin/Win64/xess.dll" out/
    install -m644 "$sdk/LICENSE.txt" out/INTEL-XESS-LICENSE.txt
    echo "XeSS ready: out/bbport_xess.dll, out/xess.dll"
else
    fetch lib/Linux/libxess.so
    fetch lib/Linux/libxess.a
    cmake -S gpu/xess_bridge -B out/xess-bridge -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DXESS_SDK_ROOT="$sdk" > /dev/null
    ninja -C out/xess-bridge > /dev/null
    rm -f out/libbbport_xess.so out/libxess.so out/gpu/libbbport_xess.so out/gpu/libxess.so
    install -m755 out/xess-bridge/libbbport_xess.so out/
    install -m755 "$sdk/lib/Linux/libxess.so" out/
    install -m644 "$sdk/LICENSE.txt" out/INTEL-XESS-LICENSE.txt
    echo "XeSS ready: out/libbbport_xess.so, out/libxess.so"
fi
