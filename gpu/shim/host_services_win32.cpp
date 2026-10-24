// SPDX-License-Identifier: GPL-2.0-or-later
// The Windows implementation of the host services (gpu/shim/host_services.h).
#include "host_services.h"

#include <cstdio>
#include <cstring>
#include <psapi.h>
#include <windows.h>

uint64_t ThreadId() {
    // The thread's id: the kernel's own, so that the numbers of the two hosts do not mix.
    return static_cast<uint64_t>(GetCurrentThreadId());
}

void* HostAllocate(size_t size) {
    // The host's own table: the memory of the process, not the guest's.
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

namespace Core {

int MProtect(void* address, size_t size, int protection) {
    if (!address || !size) {
        return 0;
    }
    DWORD protect = PAGE_NOACCESS;
    if (protection & ProtWrite) {
        protect = (protection & ProtExec) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    } else if (protection & ProtExec) {
        protect = PAGE_EXECUTE_READ;
    } else if (protection & ProtRead) {
        protect = PAGE_READONLY;
    }
    // The host's pages are protected one by one: the range may not be aligned.
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const size_t page_size = info.dwPageSize;
    const uintptr_t start = reinterpret_cast<uintptr_t>(address) & ~(page_size - 1);
    const uintptr_t end = (reinterpret_cast<uintptr_t>(address) + size + page_size - 1) & ~(page_size - 1);
    for (uintptr_t at = start; at < end; at += page_size) {
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(at), page_size, protect, &old)) {
            return -1;
        }
    }
    return 0;
}

bool ReadProcessMemory(void* address, void* buffer, size_t size) {
    // The pages the runtime marked read-only (the GPU's page tracking) are read through the
    // pool's second view on the POSIX build; here the same copy is made with the kernel's
    // own function, which reads the pages of this process without faulting on them.
    SIZE_T read = 0;
    if (!::ReadProcessMemory(GetCurrentProcess(), address, buffer, size, &read)) {
        return false;
    }
    return read == size;
}

bool ReadStack(void* address, void* buffer, size_t size) {
    // The stack of the interrupted thread: the same kernel function reads it (the pages the
    // runtime protected are read through the pool's second view on the POSIX build).
    return ReadProcessMemory(address, buffer, size);
}

bool SymbolOf(void* address, char* module, size_t module_size, uint64_t* offset) {
    HMODULE handle = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(address), &handle)) {
        return false;
    }
    if (module && module_size) {
        char path[MAX_PATH];
        const DWORD length = GetModuleFileNameA(handle, path, MAX_PATH);
        if (length) {
            const char* slash = strrchr(path, '\\');
            snprintf(module, module_size, "%s", slash ? slash + 1 : path);
        } else {
            snprintf(module, module_size, "?");
        }
    }
    if (offset) {
        *offset = static_cast<uint64_t>(static_cast<char*>(address) -
                                         reinterpret_cast<char*>(handle));
    }
    return true;
}

int Backtrace(void** frames, int capacity) {
    // The Windows stack walk needs the frame pointers, which the build keeps.
    return CaptureStackBackTrace(0, static_cast<DWORD>(capacity), frames, nullptr);
}

bool ThreadTimes(uint64_t* user_us, uint64_t* system_us) {
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) {
        return false;
    }
    // The FILETIME counts 100 ns intervals.
    const auto to_us = [](const FILETIME& time) -> uint64_t {
        ULARGE_INTEGER value;
        value.LowPart = time.dwLowDateTime;
        value.HighPart = time.dwHighDateTime;
        return value.QuadPart / 10;
    };
    if (user_us) {
        *user_us = to_us(user);
    }
    if (system_us) {
        *system_us = to_us(kernel);
    }
    return true;
}

bool ThreadCounters(uint64_t* voluntary, uint64_t* involuntary, uint64_t* minor_faults) {
    // The Windows kernel does not count the context switches of a thread; the statistics
    // that use them stay at zero (the times above are the ones that matter).
    if (voluntary) {
        *voluntary = 0;
    }
    if (involuntary) {
        *involuntary = 0;
    }
    if (minor_faults) {
        PROCESS_MEMORY_COUNTERS counters{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
            *minor_faults = counters.PageFaultCount;
        } else {
            *minor_faults = 0;
        }
    }
    return true;
}

} // namespace Core
