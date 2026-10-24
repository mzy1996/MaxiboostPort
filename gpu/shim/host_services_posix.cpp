// SPDX-License-Identifier: GPL-2.0-or-later
// The host services that differ between the hosts (gpu/shim/host_services.h): the thread's
// id and the host's own allocation for a table. The rest is in the header.
#include "host_services.h"

#ifndef _WIN32

uint64_t ThreadId() {
    // The thread's id: the kernel's own, so that the numbers of the two hosts do not mix.
    return static_cast<uint64_t>(::syscall(SYS_gettid));
}

void* HostAllocate(size_t size) {
    // The host's own table: the memory of the process, not the guest's.
    void* mem = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return mem == MAP_FAILED ? nullptr : mem;
}

#endif
