// SPDX-License-Identifier: GPL-2.0-or-later
// The host's own memory (gpu/shim/host_services.h): the pages the game's code is patched
// in, and the stubs beside it. The POSIX build maps the memory itself; the Windows build
// asks the kernel for it.
#include "host_services.h"

#ifndef _WIN32

size_t HostPageSize() {
    const long page = sysconf(_SC_PAGESIZE);
    return page > 0 ? static_cast<size_t>(page) : 4096;
}

int HostProtect(void* address, size_t size, int protection) {
    int prot = 0;
    if (protection & HostProtRead) {
        prot |= PROT_READ;
    }
    if (protection & HostProtWrite) {
        prot |= PROT_WRITE;
    }
    if (protection & HostProtExec) {
        prot |= PROT_EXEC;
    }
    return mprotect(address, size, prot);
}

void* HostMapAt(void* address, size_t size, int protection) {
    int prot = 0;
    if (protection & HostProtRead) {
        prot |= PROT_READ;
    }
    if (protection & HostProtWrite) {
        prot |= PROT_WRITE;
    }
    if (protection & HostProtExec) {
        prot |= PROT_EXEC;
    }
    void* mapped = mmap(address, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return mapped == MAP_FAILED ? nullptr : mapped;
}

void* HostMapAnywhere(size_t size, int protection) {
    int prot = 0;
    if (protection & HostProtRead) {
        prot |= PROT_READ;
    }
    if (protection & HostProtWrite) {
        prot |= PROT_WRITE;
    }
    if (protection & HostProtExec) {
        prot |= PROT_EXEC;
    }
    void* mapped = mmap(nullptr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return mapped == MAP_FAILED ? nullptr : mapped;
}

void HostUnmap(void* address, size_t size) {
    if (address) {
        munmap(address, size);
    }
}

#endif
