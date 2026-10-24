// SPDX-License-Identifier: GPL-2.0-or-later
#include "host_services.h"
#include "bbport_guest_hooks.h"
#include "bbport_heap_sites.h"
#include "bbport_toggles.h"

#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

extern "C" void runtime_memory_note_write(uintptr_t address, uint64_t size);

namespace BbGuestHooks {
namespace {
using u64 = std::uint64_t;
using u8 = std::uint8_t;
constexpr u64 ImageBase = 0x800000000ull;

/// The return of the game's GPU memory range allocator 0x26aa070 (rdi allocator, rsi size,
/// rdx alignment; it returns a node: [node + 8] the start, [[node + 0x18] + 8] the end, the next
/// node's start): `add rsp, 0x18` in its epilogue, reached by its own branches only.
constexpr u64 AllocReturn = 0x26aa255;
constexpr unsigned char AllocReturnCode[4] = {0x48, 0x83, 0xc4, 0x18};
RangeCallback on_range_allocated = nullptr;

/// The game's other GPU memory heap (TLSF-like, headers in the memory: free at 0x20858a0, alloc at
/// 0x2085a60), reached through a pointer at 0x54b9f38 set at startup. Its only three callers
/// (wrappers that record {address, size} in a map) pass r8 = rbp - 0x50 for the size handed out and
/// follow the call with `mov rbx, rax` (rax: the address).
constexpr std::array<u64, 3> HeapAllocReturns = {0x11e6629, 0x11e6736, 0x11e6846};
constexpr unsigned char HeapAllocReturnCode[3] = {0x48, 0x89, 0xc3};

/// BB_FRAME_STATS: the range allocator's collector (0x26aa860, `push rbp`; rdi the allocator): its
/// live range count [+0xc8] and byte count [+0xd0] are noted for the frame stats.
constexpr u64 CollectorEntry = 0x26aa860;
constexpr unsigned char CollectorEntryCode[1] = {0x55};

/// BB_HEAP_SITES=1: the game's deferred object release (0xbb5f10) skips the destruction when the
/// registry check (0xbf0790) fails: `js 0xbb5fc7` there, its result counted (eax).
constexpr u64 ReleaseCheck = 0xbb5fb0;
constexpr unsigned char ReleaseCheckCode[2] = {0x78, 0x15};

/// `call memcpy` (0x261a1b0: rdi destination, rsi source, rdx size) in the resource loaders:
/// 0x216d420 and 0x2171d60 copy texture and buffer data into GPU memory (most of the write
/// faults while areas stream in).
struct CallSite {
    u64 offset;
    unsigned char code[5];
};
constexpr std::array<CallSite, 2> MemcpySites = {{
    {0x216d5ea, {0xe8, 0xc1, 0xcb, 0x4a, 0x00}},
    {0x2171fea, {0xe8, 0xc1, 0x81, 0x4a, 0x00}},
}};
std::array<std::atomic<u64>, MemcpySites.size()> hits{};

/// The game's memcpy at those sites: the GPU side is told before (its write traps lift: no fault
/// per page) and after the copy (the data now there), as for file reads.
void* LoaderCopy(void* dst, const void* src, std::size_t size) {
    runtime_memory_note_write(reinterpret_cast<u64>(dst), size);
    std::memcpy(dst, src, size);
    runtime_memory_note_write(reinterpret_cast<u64>(dst), size);
    return dst;
}

/// bbport: native detours (default; BB_GUEST_HOOK_TRAPS=1: int3 traps). Each hooked site jumps to
/// a stub next to the image that calls the handler and runs the instructions the jump covers;
/// the copy call sites call LoaderCopy through a trampoline there. An int3 trap costs a signal
/// (a few microseconds in the kernel) per call; a detour costs a call. The int3 written first
/// stays the fallback while the jump's bytes go in: OnTrap sends a thread that meets it to the stub.
struct NativeSite {
    u64 offset;
    std::atomic<u64> stub{0};
};
std::array<NativeSite, 4> native_sites{{{AllocReturn}, {HeapAllocReturns[0]}, {HeapAllocReturns[1]},
                                        {HeapAllocReturns[2]}}};
u8* stub_page = nullptr;
std::size_t stub_used = 0;
constexpr std::size_t StubPageSize = 4096;

/// A page within +-2 GiB of the image (below its base: the image grows upwards).
u8* MapStubPage() {
    for (u64 k = 1; k <= 64; ++k) {
        const u64 hint = ImageBase - k * (16ull << 20);
        void* p = HostMapAt(reinterpret_cast<void*>(hint), StubPageSize, HostProtRead | HostProtWrite | HostProtExec);
        if (p) {
            return static_cast<u8*>(p);
        }
    }
    return nullptr;
}

struct Emitter {
    std::array<u8, 256> code{};
    std::size_t size = 0;
    void Bytes(std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) {
            code[size++] = b;
        }
    }
    void U32(std::uint32_t v) {
        std::memcpy(&code[size], &v, 4);
        size += 4;
    }
    void U64(u64 v) {
        std::memcpy(&code[size], &v, 8);
        size += 8;
    }
    /// Saves the caller-saved registers (the handler may change them), aligns the stack.
    void SaveVolatile() {
        Bytes({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
        Bytes({0x41, 0x54});             // push r12 (holds the unaligned stack pointer)
        Bytes({0x49, 0x89, 0xe4});       // mov r12, rsp
        Bytes({0x48, 0x83, 0xe4, 0xf0}); // and rsp, -16
    }
    void CallAbs(const void* fn) {
        Bytes({0x48, 0xb8}); // movabs rax, fn
        U64(reinterpret_cast<u64>(fn));
        Bytes({0xff, 0xd0}); // call rax
    }
    void RestoreVolatile() {
        Bytes({0x4c, 0x89, 0xe4}); // mov rsp, r12
        Bytes({0x41, 0x5c});       // pop r12
        Bytes({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});
    }
};

/// Copies `e` into the stub page; returns its address there, or 0.
u64 PlaceStub(const Emitter& e) {
    if (!stub_page || stub_used + e.size > StubPageSize) {
        return 0;
    }
    u8* at = stub_page + stub_used;
    std::memcpy(at, e.code.data(), e.size);
    stub_used += (e.size + 15) & ~std::size_t(15);
    return reinterpret_cast<u64>(at);
}

bool Rel32To(u64 from_next, u64 to, std::uint32_t& rel) {
    const std::int64_t d = std::int64_t(to) - std::int64_t(from_next);
    if (d < INT32_MIN || d > INT32_MAX) {
        return false;
    }
    rel = std::uint32_t(std::int32_t(d));
    return true;
}

/// Replaces bytes [1, 5) at `address` (byte 0 is the trap already there), then byte 0.
void WriteJumpOverTrap(u64 address, u8 opcode, std::uint32_t rel) {
    const long page_size = HostPageSize();
    void* page = reinterpret_cast<void*>(address & ~u64(page_size - 1));
    const std::size_t span = (address + 5) - reinterpret_cast<u64>(page);
    HostProtect(page, span, HostProtRead | HostProtWrite | HostProtExec);
    auto* p = reinterpret_cast<u8*>(address);
    for (int i = 0; i < 4; ++i) {
        __atomic_store_n(p + 1 + i, u8(rel >> (8 * i)), __ATOMIC_SEQ_CST);
    }
    __atomic_store_n(p, opcode, __ATOMIC_SEQ_CST);
    HostProtect(page, span, HostProtRead | HostProtExec);
}

void AllocHook(u64 node) {
    if (!node) {
        return;
    }
    const u64 start = *reinterpret_cast<const u64*>(node + 8);
    const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
    const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
    if (end > start && end - start < (u64(1) << 32)) {
        on_range_allocated(start, end - start);
    }
}

void HeapHook(u64 address, u64 size) {
    if (address != 0 && size != 0 && size < (u64(1) << 32)) {
        on_range_allocated(address, size);
    }
}

/// The detour of a site whose int3 is in place (Patch). False: left as a trap.
bool InstallDetour(NativeSite& site) {
    const u64 address = ImageBase + site.offset;
    Emitter e;
    u64 back = 0;
    if (site.offset == AllocReturn) {
        // `add rsp, 0x18; pop rbx` (5 bytes) in the allocator's epilogue; rax: the node.
        if (reinterpret_cast<const u8*>(address)[4] != 0x5b) {
            return false;
        }
        e.SaveVolatile();
        e.Bytes({0x48, 0x89, 0xc7}); // mov rdi, rax (unchanged by the saving)
        e.CallAbs(reinterpret_cast<const void*>(&AllocHook));
        e.RestoreVolatile();
        e.Bytes({0x48, 0x83, 0xc4, 0x18}); // add rsp, 0x18
        e.Bytes({0x5b});                   // pop rbx
        back = address + 5;
    } else {
        // `mov rbx, rax; test rbx, rbx` (6 bytes) after the heap call; [rbp - 0x50]: the size.
        const auto* code = reinterpret_cast<const u8*>(address);
        if (code[3] != 0x48 || code[4] != 0x85 || code[5] != 0xdb) {
            return false;
        }
        e.Bytes({0x48, 0x89, 0xc3});                               // mov rbx, rax
        e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff}); // lea rsp, [rsp - 128]
        e.SaveVolatile();
        e.Bytes({0x48, 0x89, 0xdf});       // mov rdi, rbx
        e.Bytes({0x48, 0x8b, 0x75, 0xb0}); // mov rsi, [rbp - 0x50]
        e.CallAbs(reinterpret_cast<const void*>(&HeapHook));
        e.RestoreVolatile();
        e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); // lea rsp, [rsp + 128]
        e.Bytes({0x48, 0x85, 0xdb});                               // test rbx, rbx
        back = address + 6;
    }
    // jmp back
    const u64 jmp_at = reinterpret_cast<u64>(stub_page + stub_used) + e.size;
    std::uint32_t rel_back = 0;
    if (!Rel32To(jmp_at + 5, back, rel_back)) {
        return false;
    }
    e.Bytes({0xe9});
    e.U32(rel_back);
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    site.stub.store(stub, std::memory_order_release); // a thread at the trap now goes there
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// The copy call sites: the call's target becomes a trampoline to LoaderCopy.
bool InstallCallDetour(u64 offset) {
    Emitter e;
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(reinterpret_cast<u64>(&LoaderCopy));
    const u64 stub = PlaceStub(e);
    const u64 address = ImageBase + offset;
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe8, rel);
    return true;
}
// The trap handler: the sites the guest hooks watch. It answers with true when it took
// the trap; the host's own handler (the signal on Linux, the vectored exception handler
// on Windows) then lets the thread continue where the handler sent it.
static bool HandleTrap(void* context) {
    const u64 rip = ContextRip(context) - 1; // past the int3
    for (const auto& site : native_sites) {
        if (rip == ImageBase + site.offset) {
            if (const u64 stub = site.stub.load(std::memory_order_acquire)) {
                ContextSet(context, RegRip, stub); // the detour is going in: its stub runs the site
                return true;
            }
        }
    }
    if (rip == ImageBase + ReleaseCheck) {
        const auto result = static_cast<std::int32_t>(ContextGet(context, RegRax) & 0xffffffff);
        BbHeapSites::NoteReleaseCheck(std::uint32_t(result));
        ContextSet(context, RegRip, ImageBase + (result < 0 ? 0xbb5fc7 : ReleaseCheck + 2));
        return true;
    }
    if (rip == ImageBase + CollectorEntry) {
        const u64 allocator = ContextGet(context, RegRdi);
        for (std::size_t i = 0; i < BbStats::range_allocators.size(); ++i) {
            u64 expected = 0;
            if (BbStats::range_allocators[i].load(std::memory_order_relaxed) == allocator ||
                BbStats::range_allocators[i].compare_exchange_strong(expected, allocator)) {
                BbStats::range_live[i].store(*reinterpret_cast<const u64*>(allocator + 0xc8),
                                             std::memory_order_relaxed);
                BbStats::range_bytes[i].store(*reinterpret_cast<const u64*>(allocator + 0xd0),
                                              std::memory_order_relaxed);
                break;
            }
        }
        ContextSet(context, RegRsp, ContextGet(context, RegRsp) - 8); // push rbp
        *reinterpret_cast<u64*>(ContextGet(context, RegRsp)) = ContextGet(context, RegRbp);
        ContextSet(context, RegRip, ImageBase + CollectorEntry + 1);
        return true;
    }
    for (const u64 site : HeapAllocReturns) {
        if (rip != ImageBase + site) {
            continue;
        }
        const u64 address = ContextGet(context, RegRax);
        const u64 size = *reinterpret_cast<const u64*>(ContextGet(context, RegRbp) - 0x50);
        if (address != 0 && size != 0 && size < (u64(1) << 32)) {
            on_range_allocated(address, size);
        }
        ContextSet(context, RegRbx, ContextGet(context, RegRax));
        ContextSet(context, RegRip, ImageBase + site + sizeof(HeapAllocReturnCode));
        return true;
    }
    if (rip == ImageBase + AllocReturn) {
        if (const u64 node = ContextGet(context, RegRax)) {
            const u64 start = *reinterpret_cast<const u64*>(node + 8);
            const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
            const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
            if (end > start && end - start < (u64(1) << 32)) {
                on_range_allocated(start, end - start);
            }
        }
        ContextSet(context, RegRsp, ContextGet(context, RegRsp) + 0x18);
        ContextSet(context, RegRip, ImageBase + AllocReturn + sizeof(AllocReturnCode));
        return true;
    }
    for (std::size_t i = 0; i < MemcpySites.size(); ++i) {
        const auto& site = MemcpySites[i];
        if (rip != ImageBase + site.offset) {
            continue;
        }
        hits[i].fetch_add(1, std::memory_order_relaxed);
        // The call, to LoaderCopy instead of the game's memcpy: push the return address, jump.
        const u64 ret = ImageBase + site.offset + 5;
        ContextSet(context, RegRsp, ContextGet(context, RegRsp) - 8);
        *reinterpret_cast<u64*>(ContextGet(context, RegRsp)) = ret;
        ContextSet(context, RegRip, reinterpret_cast<u64>(&LoaderCopy));
        return true;
    }
    return false;
}

#ifdef _WIN32
// The Windows build: the breakpoints arrive as the breakpoint exception. The handler sends
// the thread to the site's detour by moving its instruction pointer.
LONG CALLBACK OnVectoredException(PEXCEPTION_POINTERS exception) {
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // The interrupted thread's context: the record holds it.
    auto* context = exception->ContextRecord;
    if (!HandleTrap(context)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // The instruction pointer was moved: the thread continues where the handler sent it.
    return EXCEPTION_CONTINUE_EXECUTION;
}
#else
struct sigaction previous_action {};

void OnTrap(int sig, siginfo_t* info, void* context) {
    if (HandleTrap(context)) {
        return;
    }
    if (previous_action.sa_flags & SA_SIGINFO) {
        if (previous_action.sa_sigaction) {
            previous_action.sa_sigaction(sig, info, context);
            return;
        }
    } else if (previous_action.sa_handler != SIG_DFL && previous_action.sa_handler != SIG_IGN &&
               previous_action.sa_handler) {
        previous_action.sa_handler(sig);
        return;
    }
    signal(SIGTRAP, SIG_DFL);
    raise(SIGTRAP);
}
#endif


bool Patch(u64 address, const unsigned char* expected, std::size_t size) {
    unsigned char bytes[16]{};
    const void* local_base = bytes; const size_t local_size = size;
    struct { void* iov_base; size_t iov_len; } local{const_cast<void*>(local_base), local_size};
    struct { void* iov_base; size_t iov_len; } remote{reinterpret_cast<void*>(address), size};
    if (!Core::ReadProcessMemory(remote.iov_base, local.iov_base, local.iov_len) || local.iov_len != (size) ||
        std::memcmp(bytes, expected, size) != 0) {
        return false;
    }
    const long page_size = HostPageSize();
    void* page = reinterpret_cast<void*>(address & ~u64(page_size - 1));
    const std::size_t span = (address + size) - reinterpret_cast<u64>(page);
    if (HostProtect(page, span, HostProtRead | HostProtWrite | HostProtExec) != 0) {
        return false;
    }
    __atomic_store_n(reinterpret_cast<unsigned char*>(address), static_cast<unsigned char>(0xcc),
                     __ATOMIC_SEQ_CST);
    HostProtect(page, span, HostProtRead | HostProtExec);
    return true;
}
} // namespace

void Install(RangeCallback on_gpu_range_allocated) {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) {
        return;
    }
#ifdef _WIN32
    // The vectored exception handler: it sees the breakpoints before the runtime's own
    // handler (the runtime's handler is registered later, so it comes second).
    AddVectoredExceptionHandler(1, reinterpret_cast<PVECTORED_EXCEPTION_HANDLER>(OnVectoredException));
#else
    struct sigaction action {};
    action.sa_sigaction = OnTrap;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTRAP, &action, &previous_action);
#endif
    int patched = 0;
    for (const auto& site : MemcpySites) {
        patched += Patch(ImageBase + site.offset, site.code, sizeof(site.code));
    }
    std::printf("Guest hooks: %d of %zu resource copy sites tell the GPU side of their writes\n",
                patched, MemcpySites.size());
    on_range_allocated = on_gpu_range_allocated;
    const bool allocator = Patch(ImageBase + AllocReturn, AllocReturnCode, sizeof(AllocReturnCode));
    // BB_HEAP_HOOKS=0 (bisecting): the heap's ranges are not reported.
    const char* heap_env = std::getenv("BB_HEAP_HOOKS");
    const bool heap_hooks = !heap_env || heap_env[0] != '0';
    if (BbStats::enabled) {
        Patch(ImageBase + CollectorEntry, CollectorEntryCode, sizeof(CollectorEntryCode));
    }
    int heap_sites = 0;
    for (const u64 site : HeapAllocReturns) {
        if (!heap_hooks) {
            break;
        }
        heap_sites += Patch(ImageBase + site, HeapAllocReturnCode, sizeof(HeapAllocReturnCode));
    }
    std::printf("Guest hooks: GPU heap allocations seen at %d of %zu call sites\n", heap_sites,
                HeapAllocReturns.size());
    std::printf("Guest hooks: GPU memory allocator %s\n", allocator ? "tells the GPU side of new ranges" : "not found");
    // Native detours over the traps just placed (BB_GUEST_HOOK_TRAPS=1 keeps the traps).
    const char* traps_env = std::getenv("BB_GUEST_HOOK_TRAPS");
    if (!(traps_env && traps_env[0] == '1') && (stub_page = MapStubPage()) != nullptr) {
        int detours = 0;
        for (const auto& site : MemcpySites) {
            if (reinterpret_cast<const u8*>(ImageBase + site.offset)[0] == 0xcc) {
                detours += InstallCallDetour(site.offset);
            }
        }
        for (auto& site : native_sites) {
            if (reinterpret_cast<const u8*>(ImageBase + site.offset)[0] == 0xcc) {
                detours += InstallDetour(site);
            }
        }
        HostProtect(stub_page, StubPageSize, HostProtRead | HostProtExec);
        std::printf("Guest hooks: %d native detours (no traps; BB_GUEST_HOOK_TRAPS=1: int3)\n",
                    detours);
    }
    BbHeapSites::Install();
    if (const char* env = std::getenv("BB_HEAP_SITES"); env && env[0] == '1') {
        Patch(ImageBase + ReleaseCheck, ReleaseCheckCode, sizeof(ReleaseCheckCode));
    }
}
} // namespace BbGuestHooks
