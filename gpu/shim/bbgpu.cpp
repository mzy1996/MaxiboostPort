#ifdef _WIN32
#include <cstdlib>
#endif

#include "bbport_write_log.h"
#include "bbport_game_menu.h"
#include "bbport_gnm_hooks.h"
// bbport: glue between the C loader and the vendored shadPS4 video core.
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "game_profile.h"
#include "bbport_copy.h"
#include <sys/resource.h>
#include "bbport_free_check.h"
#include "bbport_toggles.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <boost/asio/io_context.hpp>
#include "common/polyfill_thread.h"
#include "video_core/renderdoc.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "../bbgpu.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/rdtsc.h"
#include "common/signal_context.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
void runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);
int runtime_neo_mode(void);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
const uint64_t* runtime_memory_generation(void);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
void runtime_memory_set_note_write_hook(RuntimeGpuRange note);
void runtime_memory_set_cpu_write_hook(RuntimeGpuRange hook);
int runtime_memory_restore_access(void* context, uintptr_t address, int repeated);
}

Frontend::WindowSDL* g_window = nullptr;

namespace Libraries::GnmDriver { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::AvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Kernel { void RegisterEventQueue(Core::Loader::SymbolsResolver* sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const BbGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    // Existing guest mappings are replayed by the runtime when hooks are installed.
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) {
            auto* r = Memory::Instance()->GetRasterizer();
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        });
    // bbport: data written into GPU memory by a path the GPU side hears of (file reads, the game's
    // resource loaders): invalidated, and an asset (it may keep a VRAM copy).
    runtime_memory_set_note_write_hook([](uintptr_t address, uint64_t size) {
        Memory::Instance()->GetRasterizer()->NoteAssetWrite(address, size);
    });
    // bbport: the game's libc copies (memcpy, memset, memmove) over pages the caches watch.
    runtime_memory_set_cpu_write_hook([](uintptr_t address, uint64_t size) {
        Memory::Instance()->GetRasterizer()->OnCpuWrite(address, size);
    });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
namespace {
// bbport: the mapped region of the last lookup, per thread, valid for the mapping table generation
// it was read at. The GPU command thread clamps and copies thousands of constant ranges a frame,
// nearly all within the region of the one before: two calls into the runtime each (~5% of it).
struct RegionCache {
    u64 generation = 1; // odd: invalid
    uintptr_t start = 0, end = 0;
};
thread_local RegionCache region_cache;

bool CachedMapped(VAddr address, u64 size) {
    static const uint64_t* const generation_ptr = runtime_memory_generation();
    const u64 generation = __atomic_load_n(generation_ptr, __ATOMIC_ACQUIRE);
    if (generation & 1) {
        return false; // being changed
    }
    auto& cache = region_cache;
    if (cache.generation == generation && address >= cache.start && address + size <= cache.end) {
        return true;
    }
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(address, &start, &end, &mapped) || !mapped) {
        return false;
    }
    cache = {generation, start, end};
    return address + size <= end;
}
} // namespace

u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    if (size && CachedMapped(virtual_addr, size)) {
        return size;
    }
    return runtime_memory_clamp(virtual_addr, size);
}
static void CopySparseSerial(VAddr source, u8* dest, u64 size) {
    // BB_READBACKS=2 protects GPU-written pages against reads: the copy threads and recorders
    // read through the backing view, as a read fault on them would wait for the GPU thread,
    // which waits for them.
    static const bool precise =
        EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise;
    if (precise) {
        runtime_memory_read_backing(source, dest, size);
        return;
    }
    if (size && CachedMapped(source, size)) {
        std::memcpy(dest, reinterpret_cast<const void*>(source), size);
        return;
    }
    while (size) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(source, &start, &end, &mapped)) {
            end = source + size;
        }
        const u64 n = std::min<u64>(size, end - source);
        if (mapped) std::memcpy(dest, reinterpret_cast<const void*>(source), n);
        else std::memset(dest, 0, n);
        source += n; dest += n; size -= n;
    }
}
void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    // bbport: large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !BbCopy::Enabled()) {
        return CopySparseSerial(source, dest, size);
    }
    BbCopy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        CopySparseSerial(source + offset, dest + offset, std::min(Chunk, size - offset));
    });
}
void MemoryManager::ReadBacking(VAddr address, void* data, u64 size) {
    runtime_memory_read_backing(address, data, size);
}
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    BbWriteLog::Note(reinterpret_cast<uintptr_t>(address), data, size, BbWriteLog::Backing);
    return runtime_memory_write_backing(reinterpret_cast<uintptr_t>(address), data, size) != 0;
}
void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    BbStats::Timer timer{BbStats::t_protect};
    const u64 pages = (size + 4095) / 4096;
    BbStats::protect_calls.fetch_add(1, std::memory_order_relaxed);
    BbStats::protect_pages.fetch_add(pages, std::memory_order_relaxed);
    if (!True(perms & MemoryPermission::Write)) {
        BbStats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
        BbStats::protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
    }
    runtime_memory_gpu_protect(virtual_addr, size, True(perms & MemoryPermission::Read),
                               True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
} // namespace Core

// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return Common::FencedRDTSC(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return runtime_neo_mode(); } // BB_NEO=1: a PS4 Pro
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *ver = s32(g_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host thread.
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;
} // namespace

#ifdef BB_PGO_GENERATE
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
// Instrumented build (BB_PGO=generate): the game often ends through _exit (watchdog, guest
// exit), which skips the profile write at exit. Write every 30 s and reset: the files sum the
// intervals.
static void StartProfileWriter() {
    std::thread([] {
        Common::SetCurrentThreadName("bb:pgo");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            __gcov_dump();
            __gcov_reset();
            std::printf("PGO: profile written\n");
        }
    }).detach();
}
#endif

extern "C" int bbgpu_init(const BbGpuConfig* config) {
    BbSettings::Load();
    // What the translator knows about this game (games/), before anything asks for it.
    Game::Select(config->serial);
#ifdef BB_PGO_GENERATE
    StartProfileWriter();
#endif
    g_sdk_version = config->sdk_version;
    if (config->user_dir) HostSetEnv("BB_GPU_USER_DIR", config->user_dir, 0);
    Core::Emulator::FillElfInfo(*config);
    const std::string title = config->title ? config->title : "MAXIBOOST ON";
    const s32 width = config->width, height = config->height;
    g_window_thread = std::thread([title, width, height] {
        Common::SetCurrentThreadName("bb:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            SDL_Delay(2);
        }
        LOG_INFO(Frontend, "Window closed by user");
        std::fflush(stdout);
        std::_Exit(0);
    });
    g_window_thread.detach();
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }
    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    return 0;
}

namespace Libraries::Kernel { void StartKernelService(); }
extern "C" void bbgpu_register_kernel(void) {
    Libraries::Kernel::StartKernelService();
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
    Libraries::AvPlayer::RegisterLib(&resolver);
}

extern "C" uintptr_t bbgpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

namespace {
// bbport (from Awaken's BloodBorneButPc): the same access (instruction and address) faulting
// again within microseconds, many times in a row: the tracking claimed the fault but left the
// page protected, and the thread would retry forever. A write the game repeats each frame comes
// milliseconds apart.
struct RepeatedFault {
    u64 rip = 0, address = 0, last_tsc = 0;
    u32 count = 0;
};
thread_local RepeatedFault repeated_fault;

bool FaultRepeats(void* ucontext, void* address) {
    constexpr u64 RepeatCycles = 4'000'000; // ~1 ms
    constexpr u32 RepeatLimit = 64;
    auto& r = repeated_fault;
    const u64 rip = reinterpret_cast<u64>(Common::GetRip(ucontext));
    const u64 now = Common::FencedRDTSC();
    if (rip != r.rip || reinterpret_cast<u64>(address) != r.address ||
        now - r.last_tsc > RepeatCycles) {
        r = {rip, reinterpret_cast<u64>(address), now, 0};
        return false;
    }
    r.last_tsc = now;
    if (++r.count < RepeatLimit) {
        return false;
    }
    r.count = 0;
    return true;
}
} // namespace

extern "C" int bbgpu_handle_fault(void* ucontext, void* address) {
    // BB_LABEL_TRAP (diagnostic): its read-only label pages first. Not passed on to GPU page
    // tracking (a write fault drains the draw pipe: thousands a second would change the timing
    // under test); label pages are not expected to be GPU-tracked.
    if (BbFreeCheck::OnTrapFault(ucontext, reinterpret_cast<std::uint64_t>(address))) {
        return 1;
    }
    if (Core::Signals::Instance()->DispatchAccessViolation(ucontext, address)) {
        if (FaultRepeats(ucontext, address)) {
            runtime_memory_restore_access(ucontext, reinterpret_cast<uintptr_t>(address), 1);
        }
        return 1;
    }
    if (BbFreeCheck::OnStaleTrapFault(reinterpret_cast<std::uint64_t>(address))) {
        return 1;
    }
    // bbport: nobody claimed it. A guest page the guest may access but GPU tracking left
    // protected (a stale protection, a page it no longer knows) gets its access back; ending the
    // game for it is worse than a GPU copy that may miss a write. Real faults stay unhandled.
    return runtime_memory_restore_access(ucontext, reinterpret_cast<uintptr_t>(address), 0);
}

extern "C" void bbgpu_patch_image(unsigned char* image, uint64_t size) {
    if (const Game::Profile* profile = Game::Active(); profile && profile->patch_image) {
        profile->patch_image(image, size);
    }
}

extern "C" unsigned bbgpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

/* The architecture of a pad: an Xbox pad is read through XInput, the others through
 * DirectInput (SDL's own driver for them). The launcher shows it, so that the person
 * knows which of the two their pad is.
 *
 * SDL gives the XInput pads the GUID that starts with "03000000" (the bus number of the
 * XInput controller is 0x03, and the vendor is Microsoft's 0x045E); the DirectInput ones
 * have the HID's bus. The name is a second hint: SDL calls the pads it reads through
 * XInput "XInput ...". */
static const char* pad_architecture(const char* guid, const char* name) {
    if (guid && strncmp(guid, "03000000", 8) == 0) {
        return "xinput";
    }
    if (name && (strstr(name, "XInput") != nullptr || strstr(name, "Xbox") != nullptr)) {
        return "xinput";
    }
    return "directinput";
}

/* The input devices the launcher shows (the controls page). The SDL that reads the pads
 * is here, so the list is built here too; the launcher asks before the game starts, and
 * the runtime opens the pads afterwards. */
extern "C" int bbgpu_input_devices(char* buffer, int size) {
    if (!buffer || size <= 0) return 0;
    buffer[0] = '\0';
    /* The gamepads are read with SDL; the subsystem is started on its own (the video one
     * is the window's, and there is no window yet). */
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) {
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            /* No gamepad subsystem: the keyboard is still an input. */
            const int length = snprintf(buffer, size, "keyboard\t\tKeyboard\t\n");
            return length > 0 ? length : 0;
        }
    }
    int written = 0;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetJoysticks(&count)) {
        /* A pad that is read through two drivers (the Xbox one is seen by the XInput and
         * the DirectInput drivers) is listed once: the launcher shows the devices, not the
         * drivers. */
        std::vector<std::string> seen;
        for (int i = 0; i < count; ++i) {
            const SDL_JoystickID id = ids[i];
            char guid[64] = {0};
            SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
            const char* name = SDL_GetJoystickNameForID(id);
            bool duplicate = false;
            for (const std::string& guid_seen : seen) {
                if (guid_seen == guid) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            seen.emplace_back(guid);
            const int length = snprintf(buffer + written, size - written, "pad\t%s\t%s\t%s\n",
                                        guid, name ? name : "",
                                        pad_architecture(guid, name));
            if (length <= 0 || written + length >= size) break;
            written += length;
        }
        SDL_free(ids);
    }
    /* The keyboard is always there: a person with only a keyboard plays too. */
    const int length = snprintf(buffer + written, size - written, "keyboard\t\tKeyboard\t\n");
    if (length > 0 && written + length < size) written += length;
    return written;
}

/* The buttons a gamepad has, in the names the runtime's configuration uses (SDL's). */
extern "C" int bbgpu_input_buttons(const char* guid_text, char* buffer, int size) {
    if (!buffer || size <= 0) return 0;
    buffer[0] = '\0';
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) {
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return 0;
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    if (!ids) return 0;
    int written = 0;
    for (int i = 0; i < count; ++i) {
        char guid[64] = {0};
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), guid, sizeof(guid));
        if (guid_text && *guid_text && strcmp(guid, guid_text) != 0) continue;
        /* The buttons of the gamepad, as the runtime's configuration names them. The
         * names are the ones SDL knows; the ones the pad does not have are left out (a
         * pad without a touch surface has no "touchpad"). */
        static const char* const names[] = {
            "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
            "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright",
            "touchpad", "misc1", "paddle1", "paddle2", "paddle3", "paddle4",
            "lefttrigger", "righttrigger",
        };
        SDL_Gamepad* gamepad = SDL_GetGamepadFromID(ids[i]);
        for (const char* name : names) {
            const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(name);
            if (button == SDL_GAMEPAD_BUTTON_INVALID) continue;
            /* The pad has the button when SDL says so; when the pad is not opened as a
             * gamepad (the gamepad subsystem is not started yet), the button is taken as
             * it is: a binding that names a button the pad does not have never fires. */
            if (gamepad && !SDL_GamepadHasButton(gamepad, button)) continue;
            const int length = snprintf(buffer + written, size - written, "%s%s",
                                        written ? "," : "", name);
            if (length <= 0 || written + length >= size) break;
            written += length;
        }
        /* The triggers are axes, not buttons; the runtime takes them as buttons, so they
         * are named as well. SDL does not say whether a pad has them (a pad without
         * triggers has no axis at that place), and a binding that names a button the pad
         * does not have simply never fires - the person can change it on the page. */
        {
            for (const char* name : {"lefttrigger", "righttrigger"}) {
                const int length = snprintf(buffer + written, size - written, "%s%s",
                                            written ? "," : "", name);
                if (length <= 0 || written + length >= size) break;
                written += length;
            }
        }
        break;
    }
    SDL_free(ids);
    return written;
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("bb:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
            // Taken before the run: a request made while it runs (a timer armed after run()
            // found no work) wakes the next one instead of being cleared after it.
            asio_requests.exchange(0);
        }
        if (stoken.stop_requested()) break;
        io_context.run();
        io_context.restart();
    }
}

void StartKernelService() {
    service_thread = std::jthread{KernelServiceThread};
}

int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
} // namespace Libraries::Kernel

// RenderDoc capture hooks: not used by the port.
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return false; }
void RequestScreenshot(ScreenshotRequest) {}
u32 ConsumeGameOnlyScreenshotRequests() { return 0; }
u32 ConsumeWithOverlaysScreenshotRequests() { return 0; }
ScreenshotRequests ConsumeScreenshotRequests() { return {}; }
} // namespace VideoCore

extern "C" int bbgpu_overlay_captures_input(void) {
    return BbOverlay::CapturesInput() ? 1 : 0;
}

extern "C" void bbgpu_trophy_notice(int id, char grade, const char* name_utf8, const char* icon_path) {
    std::vector<u8> icon;
    int size = 0;
    if (icon_path != nullptr) {
        if (std::FILE* file = std::fopen(icon_path, "rb")) {
            u32 header[2]{};
            if (std::fread(header, sizeof(header), 1, file) == 1 && header[0] == header[1] &&
                header[0] > 0 && header[0] <= 512) {
                icon.resize(size_t(header[0]) * header[0] * 4);
                if (std::fread(icon.data(), 1, icon.size(), file) == icon.size()) {
                    size = int(header[0]);
                } else {
                    icon.clear();
                }
            }
            std::fclose(file);
        }
    }
    BbOverlay::TrophyNotice(id, grade, name_utf8 ? name_utf8 : "", std::move(icon), size);
}

extern "C" void bbgpu_mouse_take(double* dx, double* dy, int* wheel_up, int* wheel_down) {
    double x = 0, y = 0;
    int up = 0, down = 0;
    if (g_window) {
        g_window->TakeMouse(x, y, up, down);
    }
    *dx = x;
    *dy = y;
    *wheel_up = up;
    *wheel_down = down;
}

extern "C" int bbgpu_mouse_captured(void) {
    return g_window && g_window->MouseCaptured() ? 1 : 0;
}

extern "C" void bbgpu_mouse_look_enable(int enabled) {
    Frontend::WindowSDL::EnableMouseLook(enabled != 0);
}

extern "C" int bbgpu_text_input_begin(const char* initial, const char* prompt) {
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}

extern "C" int bbgpu_text_input_poll(char* out, uint64_t size) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}
