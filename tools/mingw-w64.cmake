# The toolchain for building the Windows binaries with MinGW-w64 from an MSYS2 (or Git Bash)
# environment: CMake runs on the host, the compiler and the libraries are MinGW-w64's.
#
#   cmake -S gpu -B out/gpu-win -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=tools/mingw-w64.cmake
#
# The paths are given in the Windows form, because the compiler is a Windows program.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Where the MinGW-w64 toolchain lives (the MSYS2 root).
if(DEFINED ENV{MSYS2_ROOT})
    set(MSYS2_ROOT "$ENV{MSYS2_ROOT}")
else()
    set(MSYS2_ROOT "C:/devkitPro/msys2")
endif()

set(CMAKE_C_COMPILER "${MSYS2_ROOT}/mingw64/bin/gcc.exe" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "${MSYS2_ROOT}/mingw64/bin/g++.exe" CACHE FILEPATH "")
set(CMAKE_RC_COMPILER "${MSYS2_ROOT}/mingw64/bin/windres.exe" CACHE FILEPATH "")
set(CMAKE_AR "${MSYS2_ROOT}/mingw64/bin/ar.exe" CACHE FILEPATH "")
set(CMAKE_RANLIB "${MSYS2_ROOT}/mingw64/bin/ranlib.exe" CACHE FILEPATH "")
set(CMAKE_STRIP "${MSYS2_ROOT}/mingw64/bin/strip.exe" CACHE FILEPATH "")
# The build program is a Windows program too (the POSIX one cannot drive it).
set(CMAKE_MAKE_PROGRAM "${MSYS2_ROOT}/usr/bin/ninja.exe" CACHE FILEPATH "")
# The MSYS2 shell rewrites the Windows paths of the command line into POSIX ones, which the
# Windows programs (the compiler) do not understand.
set(MSYS2_ARG_CONV_EXCL "*")

set(CMAKE_FIND_ROOT_PATH "${MSYS2_ROOT}/mingw64")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The Vulkan SDK's headers and the loader's import library. MSYS2's mingw64 carries them
# (the loader is named libvulkan-1 there).
if(NOT DEFINED Vulkan_INCLUDE_DIR)
    set(Vulkan_INCLUDE_DIR "${MSYS2_ROOT}/mingw64/include" CACHE PATH "")
endif()
if(NOT DEFINED Vulkan_LIBRARY)
    find_library(Vulkan_LIBRARY NAMES vulkan-1 vulkan
                 PATHS "${MSYS2_ROOT}/mingw64/lib" NO_DEFAULT_PATH)
    set(Vulkan_LIBRARY "${Vulkan_LIBRARY}" CACHE FILEPATH "")
endif()
# The headers are found through the include path, not through a package.
set(Vulkan_INCLUDE_DIRS "${Vulkan_INCLUDE_DIR}")

# Boost (the headers only) and the fmt headers: MSYS2's mingw64 carries them.
if(NOT DEFINED Boost_INCLUDE_DIR)
    set(Boost_INCLUDE_DIR "${MSYS2_ROOT}/mingw64/include" CACHE PATH "")
endif()
set(Boost_NO_SYSTEM_PATHS ON CACHE BOOL "")
set(Boost_USE_STATIC_LIBS OFF CACHE BOOL "")

# pkg-config finds the libraries of the MinGW-w64 environment. The MSYS2 one is a POSIX
# program: it is told not to rewrite the paths, so the libraries are named the way CMake
# expects them.
set(ENV{PKG_CONFIG_PATH} "${MSYS2_ROOT}/mingw64/lib/pkgconfig")
set(ENV{MSYS2_ARG_CONV_EXCL} "*")
set(PKG_CONFIG_EXECUTABLE "${MSYS2_ROOT}/usr/bin/pkg-config.exe" CACHE FILEPATH "")

# The DLLs the program needs are beside it (the build copies them there).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
