// SPDX-License-Identifier: GPL-2.0-or-later
// The host services the GPU library needs, in one place, so that the POSIX and the Windows
// builds share the code above it.
//
// The library reads the guest's memory (the page manager's fault handling), asks the host
// which module an address belongs to (the profiler's symbols) and reads the host's own
// memory; those are the three things that differ.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <ucontext.h>
#endif

// The registers of an interrupted thread: the fault handler and the page tracking read them
// (Linux gives a ucontext, Windows a CONTEXT).
#ifdef _WIN32
using SignalContext = CONTEXT;
#else
using SignalContext = ucontext_t;
#endif

/// The instruction pointer of the interrupted thread.
uint64_t ContextRip(const void* context);
/// The stack pointer of the interrupted thread.
uint64_t ContextRsp(const void* context);
/// The frame pointer of the interrupted thread.
uint64_t ContextRbp(const void* context);

/// The registers of the interrupted thread, as the code names them.
enum ContextRegister {
    RegRax = 0,
    RegRbx = 1,
    RegRcx = 2,
    RegRdx = 3,
    RegRsi = 4,
    RegRdi = 5,
    RegRbp = 6,
    RegRsp = 7,
    RegRip = 8,
    RegR14 = 9,
    RegR15 = 10,
    RegFlags = 11,
};
/// Reads a register of the interrupted thread.
uint64_t ContextGet(const void* context, int reg);
/// Writes a register of the interrupted thread (the handler returns to it).
void ContextSet(void* context, int reg, uint64_t value);
/// The flags of the interrupted thread (the single step's trap flag lives there).
uint64_t ContextFlags(const void* context);
void ContextSetFlags(void* context, uint64_t flags);
/// The thread's id (the profiler and the statistics name the threads by it).
uint64_t ThreadId();
/// The host's own allocation for a table (the guest's memory is not touched).
void* HostAllocate(size_t size);
/// The size of the host's pages.
size_t HostPageSize();
/// The protection of a range of the host's memory (the game's code is patched in it).
enum HostProtection {
    HostProtNone = 0,
    HostProtRead = 1,
    HostProtWrite = 2,
    HostProtExec = 4,
};
int HostProtect(void* address, size_t size, int protection);
/// A range of the host's memory at a given address (the stubs beside the game): null when the
/// address cannot be used.
void* HostMapAt(void* address, size_t size, int protection);
/// A range of the host's memory anywhere (the stubs, when no address is given).
void* HostMapAnywhere(size_t size, int protection);
/// Releases a mapping of the host's own memory.
void HostUnmap(void* address, size_t size);
/// Sets an environment variable for the process (the GPU library's own settings).
void HostSetEnv(const char* name, const char* value, int overwrite);

namespace Core {
#ifdef _WIN32
// The guest's protection values (the Linux ones, as the runtime uses them).
enum : int {
    ProtNone = 0,
    ProtRead = 1,
    ProtWrite = 2,
    ProtExec = 4,
};
// The page protection of a range: the Windows equivalent of mprotect (the pages are
// protected one by one, the host's granularity is the page).
int MProtect(void* address, size_t size, int protection);
// Reads the process's own memory: the pages the runtime marked read-only (the GPU's page
// tracking) are read through the pool's second view, so this may fail where a plain copy
// would fault.
bool ReadProcessMemory(void* address, void* buffer, size_t size);
// Reads the stack of the interrupted thread (the guest's caller is found there).
bool ReadStack(void* address, void* buffer, size_t size);
// The module a host address belongs to and the offset in it.
bool SymbolOf(void* address, char* module, size_t module_size, uint64_t* offset);
// The host's own call chain.
int Backtrace(void** frames, int capacity);
// The host's own allocation for a table (the guest's memory is not touched).
// The thread's and the process's times (the statistics read them).
bool ThreadTimes(uint64_t* user_us, uint64_t* system_us);
// The number of the context switches and of the page faults of the thread.
bool ThreadCounters(uint64_t* voluntary, uint64_t* involuntary, uint64_t* minor_faults);
#else
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <cstring>

// Reads the process's own memory.
inline bool ReadProcessMemory(void* address, void* buffer, size_t size) {
    iovec local{buffer, size};
    iovec remote{address, size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}
// Reads the stack of the interrupted thread.
inline bool ReadStack(void* address, void* buffer, size_t size) {
    return ReadProcessMemory(address, buffer, size);
}
// The module a host address belongs to and the offset in it.
inline bool SymbolOf(void* address, char* module, size_t module_size, uint64_t* offset) {
    Dl_info info{};
    if (!dladdr(address, &info)) {
        return false;
    }
    if (module && module_size) {
        const char* slash = info.dli_fname ? strrchr(info.dli_fname, '/') : nullptr;
        snprintf(module, module_size, "%s", slash ? slash + 1 : (info.dli_fname ? info.dli_fname : "?"));
    }
    if (offset) {
        *offset = static_cast<uint64_t>(static_cast<char*>(address) - static_cast<char*>(info.dli_fbase));
    }
    return true;
}
// The host's own call chain.
inline int Backtrace(void** frames, int capacity) { return backtrace(frames, capacity); }
// The page protection of a range.
inline int MProtect(void* address, size_t size, int protection) {
    return mprotect(address, size, protection);
}
// The thread's and the process's times.
inline bool ThreadTimes(uint64_t* user_us, uint64_t* system_us) {
    rusage usage{};
    if (getrusage(RUSAGE_THREAD, &usage) != 0) {
        return false;
    }
    if (user_us) {
        *user_us = static_cast<uint64_t>(usage.ru_utime.tv_sec) * 1000000 + usage.ru_utime.tv_usec;
    }
    if (system_us) {
        *system_us = static_cast<uint64_t>(usage.ru_stime.tv_sec) * 1000000 + usage.ru_stime.tv_usec;
    }
    return true;
}
// The number of the context switches and of the page faults of the thread.
inline bool ThreadCounters(uint64_t* voluntary, uint64_t* involuntary, uint64_t* minor_faults) {
    rusage usage{};
    if (getrusage(RUSAGE_THREAD, &usage) != 0) {
        return false;
    }
    if (voluntary) {
        *voluntary = usage.ru_nvcsw;
    }
    if (involuntary) {
        *involuntary = usage.ru_nivcsw;
    }
    if (minor_faults) {
        *minor_faults = usage.ru_minflt;
    }
    return true;
}
#endif
} // namespace Core
