# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: MIT
#
# bbport_xess (libbbport_xess.so / bbport_xess.dll) links the Intel XeSS SDK and is built separately
# from the port. Linux: build.sh builds it when XESS_SDK_ROOT is set. Windows (MSVC):
#   cmake -S gpu/xess_bridge -B out/xess-bridge -G "Visual Studio 17 2022" -A x64 \
#         -DXESS_SDK_ROOT=<github.com/intel/XeSS-SDK checkout> \
#         -DVULKAN_INCLUDE=<Vulkan-Headers include> \
#         -DXESS_LIBRARY=<path to xess.lib / libxess.a (optional, auto-detected)>
#   cmake --build out/xess-bridge --config Release
#
# Unlike the DLSS bridge, XeSS is not single-vendor: it runs on any Vulkan GPU (Intel XMX, or its
# DP4a / generic fallback), so the port enables it for every vendor. The port loads this bridge only
# when bbport_xess.dll / libbbport_xess.so is next to bb-probe (with Intel's xess.dll / libxess.so);
# without them it silently keeps FSR 3.1.

cmake_minimum_required(VERSION 3.20)
project(bbport_xess LANGUAGES CXX)

set(XESS_SDK_ROOT "" CACHE PATH "Intel XeSS SDK root (contains include/ and lib/)")
set(VULKAN_INCLUDE "" CACHE PATH "Vulkan-Headers include directory (Linux: the system headers)")
set(XESS_LIBRARY "" CACHE FILEPATH "Intel XeSS import library (xess.lib / libxess.a); auto-detected")

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The XeSS bridge is x86-64 only")
endif()

# The SDK ships its headers either flat (include/xess_vk.h) or under include/xess/xess_vk.h.
find_path(XESS_INCLUDE_DIR NAMES xess_vk.h xess/xess_vk.h HINTS "${XESS_SDK_ROOT}/include")
if (NOT XESS_INCLUDE_DIR)
    message(FATAL_ERROR "Set XESS_SDK_ROOT to the Intel XeSS SDK (github.com/intel/XeSS-SDK)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# xess.lib is the static-CRT (/MT) variant: no Visual C++ runtime install needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_library(bbport_xess SHARED bbport_xess_bridge.cpp bbport_xess_bridge.h)
target_include_directories(bbport_xess PRIVATE "${XESS_INCLUDE_DIR}" ${VULKAN_INCLUDE})
if (WIN32)
    if (NOT XESS_LIBRARY)
        set(XESS_LIBRARY "${XESS_SDK_ROOT}/lib/Win64/vc16/xess.lib")
    endif()
    target_link_libraries(bbport_xess PRIVATE "${XESS_LIBRARY}")
    target_compile_definitions(bbport_xess PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    if (NOT XESS_LIBRARY)
        set(XESS_LIBRARY "${XESS_SDK_ROOT}/lib/Linux/libxess.a")
    endif()
    # Only BbXessGetApi is exported.
    set_target_properties(bbport_xess PROPERTIES CXX_VISIBILITY_PRESET hidden)
    target_link_libraries(bbport_xess PRIVATE "${XESS_LIBRARY}" ${CMAKE_DL_LIBS})
    target_link_options(bbport_xess PRIVATE -Wl,--no-undefined -Wl,--exclude-libs,ALL)
endif()
