# bbport GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
project(bbgpu LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)

# bbport: link-time optimization (BB_LTO, default on) and profile-guided optimization
# (BB_PGO: off, generate = instrumented build writing into BB_PGO_DIR, use = build with that
# profile). Covers bbgpu and the static libraries linked into it. No -march: the same build
# runs on the Steam Deck.
option(BB_LTO "Link-time optimization of bbgpu" ON)
if (WIN32)
    # The link-time optimization's generated assembly carries the ELF directives (.hidden,
    # .previous) that Boost's visibility attributes add; the COFF assembler does not know them.
    set(BB_LTO OFF)
endif()
set(BB_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(BB_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
if (BB_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT bb_ipo_ok OUTPUT bb_ipo_error)
    if (bb_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${bb_ipo_error}")
    endif()
endif()
if (BB_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${BB_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${BB_PGO_DIR})
    add_compile_definitions(BB_PGO_GENERATE)
elseif (BB_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, bbgpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${BB_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

find_package(Vulkan REQUIRED)
if (WIN32)
    # The root of the MinGW-w64 environment (the toolchain file sets it).
    if (NOT MSYS2_ROOT)
        if (DEFINED ENV{MSYS2_ROOT})
            set(MSYS2_ROOT "$ENV{MSYS2_ROOT}")
        else()
            set(MSYS2_ROOT "C:/devkitPro/msys2")
        endif()
    endif()
    # fmt and Boost are used header-only (the code takes their headers, not their libraries).
    # The Windows build takes them from the MinGW-w64 environment directly: the CMake modules
    # for them expect an installation layout the package manager's headers do not have.
    # fmt itself is not header-only (the formatting machinery is in the library), so the
    # Windows build links the toolchain's copy of it (BB_FMT, below).
    add_library(bb_fmt INTERFACE)
    target_include_directories(bb_fmt INTERFACE "${MSYS2_ROOT}/mingw64/include")
    add_library(bb_boost INTERFACE)
    # The vendored Boost headers (the package manager's are incomplete). The compiler's own
    # include path has the incomplete ones too, so they are taken out of the way.
    target_include_directories(bb_boost BEFORE INTERFACE third_party/boost/include)
else()
    find_package(fmt REQUIRED)
    find_package(Boost REQUIRED)
endif()
# The header-only libraries: magic_enum, tsl-robin-map, miniz and Zydis. They are vendored
# (gpu/third_party) so that the Windows build does not need a package manager for them.
add_library(bb_magic_enum INTERFACE)
target_include_directories(bb_magic_enum INTERFACE third_party/magic_enum/include)
add_library(bb_tsl_robin_map INTERFACE)
target_include_directories(bb_tsl_robin_map INTERFACE third_party/tsl-robin-map/include)
add_library(bb_miniz INTERFACE)
target_include_directories(bb_miniz INTERFACE third_party/miniz)
# xbyak: the x86 assembler the shader recompiler uses (header-only, vendored).
add_library(bb_xbyak INTERFACE)
target_include_directories(bb_xbyak INTERFACE third_party/xbyak)
# Zydis is not header-only (the decoder's tables are in the library). The Linux build takes
# the one the system has installed; the Windows build compiles the vendored sources.
if (WIN32)
    set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/Zydis EXCLUDE_FROM_ALL)
    # Zydis::Zydis is an alias of Zydis already; the name bb_zydis is the one the rest of
    # the file uses, so it points at the same target.
    add_library(bb_zydis INTERFACE)
    target_link_libraries(bb_zydis INTERFACE Zydis::Zydis)
else()
    add_library(bb_zydis INTERFACE)
    target_include_directories(bb_zydis INTERFACE third_party/Zydis/include third_party/Zydis/dependencies/zycore/include)
endif()
add_library(bb_vma INTERFACE)
target_include_directories(bb_vma INTERFACE third_party/VulkanMemoryAllocator/include)
if (NOT WIN32)
    find_package(VulkanMemoryAllocator CONFIG REQUIRED)
endif()
if (WIN32)
    # The Windows build finds the libraries of the MinGW-w64 environment directly: its
    # pkg-config is a POSIX program whose output (the MSYS paths) the Windows linker
    # does not understand. The root comes from the toolchain file.
    if (NOT MSYS2_ROOT)
        if (DEFINED ENV{MSYS2_ROOT})
            set(MSYS2_ROOT "$ENV{MSYS2_ROOT}")
        else()
            set(MSYS2_ROOT "C:/devkitPro/msys2")
        endif()
    endif()
    function(bb_import_library target)
        find_library(${target}_LIB NAMES ${ARGN} REQUIRED
                     PATHS "${MSYS2_ROOT}/mingw64/lib" NO_DEFAULT_PATH)
        add_library(${target} UNKNOWN IMPORTED)
        set_target_properties(${target} PROPERTIES IMPORTED_LOCATION "${${target}_LIB}")
        target_include_directories(${target} INTERFACE "${MSYS2_ROOT}/mingw64/include")
    endfunction()
    # The static library: the shared one would drag in the codecs' own DLLs (a hundred of
    # them, for the video the game hardly plays). The file is named directly, because
    # find_library would pick the import library (the ".dll.a") instead.
    function(bb_static_library target)
        set(location "")
        foreach(name ${ARGN})
            if (location)
                continue()
            endif()
            if (EXISTS "${MSYS2_ROOT}/mingw64/lib/lib${name}.a")
                set(location "${MSYS2_ROOT}/mingw64/lib/lib${name}.a")
            endif()
        endforeach()
        if (NOT location)
            message(FATAL_ERROR "The static library ${ARGN} was not found in ${MSYS2_ROOT}/mingw64/lib")
        endif()
        add_library(${target} STATIC IMPORTED)
        set_target_properties(${target} PROPERTIES IMPORTED_LOCATION "${location}")
        target_include_directories(${target} INTERFACE "${MSYS2_ROOT}/mingw64/include")
    endfunction()
    bb_import_library(BB_XXHASH xxhash)
    bb_import_library(BB_SDL3 SDL3)
    # The ffmpeg: the shared libraries by default. The static ones would need every codec's
    # own library (a hundred of them, and the list keeps growing), while the shared ones
    # are collected for the package by tools/collect_dlls.py.
    # BB_FFMPEG_STATIC=1 links the static libraries instead, for a smaller package.
    option(BB_FFMPEG_STATIC "Link the ffmpeg statically (no ffmpeg DLLs to ship)" OFF)
    if (BB_FFMPEG_STATIC)
        bb_static_library(BB_AVFORMAT avformat)
        bb_static_library(BB_AVCODEC avcodec)
        bb_static_library(BB_AVUTIL avutil)
        bb_static_library(BB_SWSCALE swscale)
        bb_static_library(BB_SWRESAMPLE swresample)
        # The libraries the static ffmpeg needs. The list is the "Libs.private" of the
        # ffmpeg's own pkg-config files: every library they mention, in one place.
        set(BB_FFMPEG_DEPENDENCIES
            m atomic xml2 bz2 gme modplug bluray z gnutls rtmp gmp hogweed nettle srt ws2_32 ssh
            vpx webpmux iconv lzma dav1d opencore-amrwb rsvg-2 gdk_pixbuf-2.0 gio-2.0 cairo
            gobject-2.0 glib-2.0 intl zvbi pthread png aom gsm jxl jxl_threads lc3 mp3lame
            opencore-amrnb openjp2 opus rav1e speex SvtAv1Enc theoraenc theoradec ogg vorbis
            vorbisenc webp x264 x265 xvidcore mfuuid ole32 strmiids user32 va dl vpl gdi32 uuid
            va_win32 bcrypt soxr gomp hwy ntdll crypto ssl gcrypt gpg-error crypt32)
        foreach(dependency ${BB_FFMPEG_DEPENDENCIES})
            if (EXISTS "${MSYS2_ROOT}/mingw64/lib/lib${dependency}.a")
                bb_static_library(BB_DEP_${dependency} ${dependency})
            endif()
        endforeach()
    else()
        bb_import_library(BB_AVFORMAT avformat)
        bb_import_library(BB_AVCODEC avcodec)
        bb_import_library(BB_AVUTIL avutil)
        bb_import_library(BB_SWSCALE swscale)
        bb_import_library(BB_SWRESAMPLE swresample)
        set(BB_FFMPEG_DEPENDENCIES "")
    endif()
    # fmt: the formatting machinery is not header-only, so the library is linked.
    bb_import_library(BB_FMT fmt)
    # The sockets (Boost.Asio's Winsock initialization).
    bb_import_library(BB_WS2_32 ws2_32)
    # The ffmpeg headers and the SDL3's.
    set(FFMPEG_INCLUDE_DIRS "${MSYS2_ROOT}/mingw64/include" CACHE PATH "")
    set(SDL3_INCLUDE_DIRS "${MSYS2_ROOT}/mingw64/include" CACHE PATH "")
    set(XXHASH_INCLUDE_DIRS "${MSYS2_ROOT}/mingw64/include" CACHE PATH "")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
    pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
    if (NOT WIN32)
        # The X11 display connection the Vulkan surface is created with (Windows uses the
        # window's handle instead, gpu/shim/window.cpp).
        pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
    endif()
endif()
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# bbport: FSR 3.1 temporal upscaling / frame generation, native Vulkan (MIT, FireBurn/FSR-Vulkan).
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(fsr4_color.comp fsr4_decode_comp)
add_host_shader(fsr4_color.comp fsr4_encode_comp ENCODE=1)
add_host_shader(fsr4_reactive.comp fsr4_reactive_comp)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(occlusion_resolve.comp occlusion_resolve_comp)
add_host_shader(gpu_timestamp.comp gpu_timestamp_comp)
add_host_shader(indirect_guard.comp indirect_guard_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
if (WIN32)
    # The NT API the file handling uses (the whole file is Windows-only).
    list(APPEND COMMON shadps4/common/ntapi.cpp)
endif()
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
if (NOT WIN32)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
endif()
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
# The host services and the host memory have one implementation per platform; the one of the
# other platform would define the same symbols twice.
if (WIN32)
    list(FILTER SHIM_SOURCES EXCLUDE REGEX "host_services_posix\\.cpp$")
    list(FILTER SHIM_SOURCES EXCLUDE REGEX "host_memory_posix\\.cpp$")
else()
    list(FILTER SHIM_SOURCES EXCLUDE REGEX "host_services_win32\\.cpp$")
    list(FILTER SHIM_SOURCES EXCLUDE REGEX "host_memory_win32\\.cpp$")
endif()
# The layer's own modules (independent of the shadPS4 code): layer/.
file(GLOB LAYER_SOURCES CONFIGURE_DEPENDS layer/*.cpp)
# Game profiles (what the translator knows about one game, out of its core): games/.
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS games/*.cpp)
# The translator's own command processor (docs/TRANSLATOR_ARCHITECTURE.ru.md, step 2): cp/.
file(GLOB CP_SOURCES CONFIGURE_DEPENDS cp/*.cpp)
add_library(bbgpu SHARED ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES} ${LAYER_SOURCES} ${GAME_SOURCES} ${CP_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
    shadps4/core/libraries/videodec/video_utils.cpp)
add_dependencies(bbgpu host_shaders)
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(bbgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(bbgpu PRIVATE third_party/imgui third_party/imgui/backends)
# Dear ImGui stays private to libbbgpu.so. The library is linked into bb-probe, so its exports are
# in the global symbol scope; a Vulkan layer carrying its own Dear ImGui without -Bsymbolic
# (vkShade 0.1) had its ImGui_ImplVulkan_Init bound to this copy, which called the Vulkan loader
# with the layer's down-chain physical device handle ("Invalid physicalDevice", abort).
set_source_files_properties(third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_compile_definitions(bbgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
set_source_files_properties(shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
target_include_directories(bbgpu PRIVATE shim layer games cp shadps4 ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include)
if (WIN32)
    # The Windows build's stand-ins for the POSIX headers the code includes.
    target_include_directories(bbgpu PRIVATE third_party/win_compat)
    # The headers of Windows define the ancient "near" and "far" keywords as macros, and the
    # code uses "near" as a name; the compatibility header takes them out of the way for
    # every file of the library.
    target_compile_options(bbgpu PRIVATE -include${CMAKE_CURRENT_SOURCE_DIR}/third_party/win_compat/windows_compat.h)
    # The Boost headers (the code uses them header-only).
    target_include_directories(bbgpu PRIVATE third_party/boost/include)
    # The runtime symbols (the guest's memory, the fault recovery) live in bb-probe.exe.
    # Windows resolves a DLL's imports from another module's exports, so the library links
    # against the import library made from the runtime's module definition file.
    set(RUNTIME_EXPORTS "${CMAKE_CURRENT_SOURCE_DIR}/../src/runtime_exports.txt")
    set(RUNTIME_DEF "${CMAKE_CURRENT_BINARY_DIR}/bbgpu_runtime.def")
    find_program(DLLTOOL NAMES dlltool REQUIRED)
    set(RUNTIME_IMPORT_LIB "${CMAKE_CURRENT_BINARY_DIR}/libbbruntime.a")
    # The export list of the runtime is turned into the definition file of the import
    # library at build time: a new export of the runtime has to reach the GPU library
    # without a reconfigure of the build.
    add_custom_command(OUTPUT "${RUNTIME_DEF}"
        COMMAND "${CMAKE_COMMAND}" "-DRUNTIME_EXPORTS=${RUNTIME_EXPORTS}" "-DRUNTIME_DEF=${RUNTIME_DEF}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/make_runtime_def.cmake"
        DEPENDS "${RUNTIME_EXPORTS}" "${CMAKE_CURRENT_SOURCE_DIR}/make_runtime_def.cmake"
        COMMENT "The definition file of the runtime's exports"
        VERBATIM)
    add_custom_command(OUTPUT "${RUNTIME_IMPORT_LIB}"
        COMMAND "${DLLTOOL}" -d "${RUNTIME_DEF}" -D "bb-probe.exe" -l "${RUNTIME_IMPORT_LIB}"
        DEPENDS "${RUNTIME_DEF}"
        COMMENT "The import library of the runtime's exports (bb-probe.exe)"
        VERBATIM)
    add_custom_target(bbgpu_runtime_imports DEPENDS "${RUNTIME_IMPORT_LIB}")
    add_dependencies(bbgpu bbgpu_runtime_imports)
    target_link_libraries(bbgpu PRIVATE "${RUNTIME_IMPORT_LIB}")
    # The library is bbgpu.dll, not libbbgpu.dll: the "lib" prefix is the Unix convention.
    # The import library has the same name as the DLL, or the program that links against it
    # would look for "libbbgpu.dll" at the run time.
    set_target_properties(bbgpu PROPERTIES PREFIX "" OUTPUT_NAME "bbgpu"
        IMPORT_IMPLIB "${CMAKE_CURRENT_BINARY_DIR}/bbgpu.dll.a")
endif()
target_compile_definitions(bbgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
if (NOT MSVC)
    target_compile_options(bbgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
endif()
# Loaded with bb-probe at startup (DT_NEEDED, never dlopen): thread_local access without a
# TLS descriptor call (_dl_tlsdesc_return was ~2% of the draw recording thread).
if (NOT WIN32)
    target_compile_options(bbgpu PRIVATE -ftls-model=initial-exec)
endif()
# Runtime symbols (runtime_memory_*, runtime_process_*) resolve against bb-probe.
if (NOT WIN32)
    # The runtime symbols (runtime_memory_*, runtime_process_*) resolve against bb-probe.
    target_link_options(bbgpu PRIVATE -Wl,--allow-shlib-undefined)
endif()
target_link_libraries(bbgpu PRIVATE ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan)
if (WIN32)
    target_link_libraries(bbgpu PUBLIC Vulkan::Headers bb_fmt bb_boost bb_magic_enum
        bb_tsl_robin_map bb_vma BB_XXHASH BB_SDL3 BB_AVFORMAT BB_AVCODEC BB_AVUTIL BB_SWSCALE BB_SWRESAMPLE
        BB_FMT BB_WS2_32
        bb_miniz bb_zydis bb_xbyak sirit)
    # The static ffmpeg's own dependencies (the order matters: the linker reads them once).
    set(BB_FFMPEG_LINK "")
    foreach(dependency ${BB_FFMPEG_DEPENDENCIES})
        if (TARGET BB_DEP_${dependency})
            list(APPEND BB_FFMPEG_LINK BB_DEP_${dependency})
        endif()
    endforeach()
    if (BB_FFMPEG_LINK)
        target_link_libraries(bbgpu PUBLIC ${BB_FFMPEG_LINK})
    endif()
else()
    target_link_libraries(bbgpu PUBLIC Vulkan::Headers bb_fmt bb_boost bb_magic_enum
        bb_tsl_robin_map bb_vma PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::X11 PkgConfig::FFMPEG bb_miniz bb_zydis bb_xbyak sirit)
endif()

# Explicit test targets; neither launches the game nor needs a Vulkan device.
add_executable(motion-history-test EXCLUDE_FROM_ALL ../tests/test_motion_history.cpp)
target_compile_options(motion-history-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(motion-history-test PRIVATE ..)
add_executable(pm4-resync-test EXCLUDE_FROM_ALL ../tests/test_pm4_resync.cpp)
target_compile_options(pm4-resync-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(pm4-resync-test PRIVATE ..)
add_executable(motion-shader-test EXCLUDE_FROM_ALL ../tests/test_motion_shaders.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(motion-shader-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(motion-shader-test PRIVATE bbgpu)
add_executable(ui-composition-test EXCLUDE_FROM_ALL ../tests/test_ui_composition.cpp)
target_compile_options(ui-composition-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(ui-composition-test PRIVATE ..)
add_executable(cp-decoder-test EXCLUDE_FROM_ALL ../tests/test_cp_decoder.cpp cp/cp_decoder.cpp)
target_compile_options(cp-decoder-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(cp-decoder-test PRIVATE cp)
add_executable(settings-test EXCLUDE_FROM_ALL ../tests/test_settings.cpp shim/bbport_settings.cpp)
target_compile_options(settings-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(settings-test PRIVATE .. shim)

add_executable(scene-resolution-test EXCLUDE_FROM_ALL ../tests/test_scene_resolution.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(scene-resolution-test PRIVATE shim shadps4 third_party/gcn/include)
# The test's Vulkan-Hpp inline methods have a different set of platform extensions
# from vk_platform.cpp. Keep them local and use an explicit dispatcher in the test.
target_compile_options(scene-resolution-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_scene_resolution.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(scene-resolution-test PRIVATE bbgpu)
add_executable(taa-shader-test EXCLUDE_FROM_ALL ../tests/test_taa_shader.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(taa-shader-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(taa-shader-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_taa_shader.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(taa-shader-test PRIVATE bbgpu)
add_dependencies(taa-shader-test host_shaders)

add_executable(camera-motion-test EXCLUDE_FROM_ALL ../tests/test_camera_motion.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(camera-motion-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(camera-motion-test PRIVATE -UNDEBUG -fPIC)
target_link_libraries(camera-motion-test PRIVATE bbgpu)
add_dependencies(camera-motion-test host_shaders)

# Game profile cameras parsed from captured constant blocks (no GPU library needed).
add_executable(game-camera-test EXCLUDE_FROM_ALL ../tests/test_game_cameras.cpp games/gundam.cpp games/maxiboost.cpp)
target_include_directories(game-camera-test PRIVATE games)
target_compile_options(game-camera-test PRIVATE -UNDEBUG)

add_executable(translation-memory-test EXCLUDE_FROM_ALL ../tests/test_translation_memory.cpp
    ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(translation-memory-test PRIVATE shim shadps4 third_party/gcn/include)
target_compile_options(translation-memory-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_translation_memory.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(translation-memory-test PRIVATE bbgpu)

# bbport: FSR 4 benchmark outside the game (tools/fsr4_bench.cpp): ninja -C out/gpu fsr4-bench.
add_executable(upscaler-support-test EXCLUDE_FROM_ALL ../tests/test_upscaler_support.cpp
    shim/bbport_settings.cpp)
target_include_directories(upscaler-support-test PRIVATE shim)
target_compile_options(upscaler-support-test PRIVATE -UNDEBUG)

add_executable(fsr4-bench EXCLUDE_FROM_ALL tools/fsr4_bench.cpp
    shadps4/video_core/renderer_vulkan/fsr411/fsr411.cpp)
target_include_directories(fsr4-bench PRIVATE shadps4/video_core/renderer_vulkan/fsr411)
target_link_libraries(fsr4-bench PRIVATE ffx-vulkan::fsr4-v07-vulkan ffx-vulkan::fsr4-v07-assets Vulkan::Vulkan)

# bbport online module (gpu/bbnet, libbbnet.so): built when its libraries are there (shell.nix);
# BB_NET=OFF leaves it out. Without it the port plays offline.
option(BB_NET "Build the online module (shadNet)" ON)
if (BB_NET)
    find_package(Protobuf CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)
    find_package(OpenSSL QUIET)
    find_package(ZLIB QUIET)
    find_path(MINIUPNPC_INCLUDE miniupnpc.h PATH_SUFFIXES miniupnpc)
    find_library(MINIUPNPC_LIBRARY NAMES miniupnpc)
    if (Protobuf_FOUND AND nlohmann_json_FOUND AND OpenSSL_FOUND AND ZLIB_FOUND AND
        MINIUPNPC_INCLUDE AND MINIUPNPC_LIBRARY)
        add_subdirectory(bbnet)
        message(STATUS "Online module: on (libbbnet.so)")
    else()
        message(STATUS "Online module: off (protobuf, nlohmann_json, OpenSSL, zlib or miniupnpc missing)")
    endif()
endif()
