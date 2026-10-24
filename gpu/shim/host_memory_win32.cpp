// SPDX-License-Identifier: GPL-2.0-or-later
// The host's own memory on Windows (gpu/shim/host_services.h): the pages the game's code is
// patched in, and the stubs beside it.
#include "host_services.h"

#ifdef _WIN32

namespace {

// The protection of a range, the Windows way.
DWORD host_protect(int protection) {
    if (protection & HostProtWrite) {
        return (protection & HostProtExec) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    }
    if (protection & HostProtExec) {
        return PAGE_EXECUTE_READ;
    }
    if (protection & HostProtRead) {
        return PAGE_READONLY;
    }
    return PAGE_NOACCESS;
}

} // namespace

size_t HostPageSize() {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return static_cast<size_t>(info.dwPageSize);
}

int HostProtect(void* address, size_t size, int protection) {
    if (!address || !size) {
        return 0;
    }
    // The pages are protected one by one: the range may not be aligned.
    const size_t page_size = HostPageSize();
    const uintptr_t start = reinterpret_cast<uintptr_t>(address) & ~(page_size - 1);
    const uintptr_t end = (reinterpret_cast<uintptr_t>(address) + size + page_size - 1) & ~(page_size - 1);
    for (uintptr_t at = start; at < end; at += page_size) {
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(at), page_size, host_protect(protection), &old)) {
            return -1;
        }
    }
    return 0;
}

void* HostMapAt(void* address, size_t size, int protection) {
    if (!address) {
        return nullptr;
    }
    // The memory must be free: the kernel's own allocator is asked, at the given address.
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const uintptr_t start = reinterpret_cast<uintptr_t>(address) & ~(info.dwAllocationGranularity - 1);
    void* mapped = VirtualAlloc(reinterpret_cast<void*>(start), size, MEM_RESERVE | MEM_COMMIT,
                                host_protect(protection));
    return mapped == reinterpret_cast<void*>(start) ? mapped : nullptr;
}

void* HostMapAnywhere(size_t size, int protection) {
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, host_protect(protection));
}

void HostUnmap(void* address, size_t size) {
    if (address) {
        VirtualFree(address, size, MEM_RELEASE);
    }
}

#endif
