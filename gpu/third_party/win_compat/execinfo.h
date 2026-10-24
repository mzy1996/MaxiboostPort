/* The Windows build's stand-in for <execinfo.h>: the host's call chain comes from the host
   services (gpu/shim/host_services.h), which use the Windows API. */
#pragma once

#include <host_services.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The host's own call chain (the frames are the ones of the calling thread).
static inline int backtrace(void** frames, int capacity) {
    return Core::Backtrace(frames, capacity);
}

/// The symbols of the frames: the Windows build has no such function, so the frames' addresses
/// are written as they are.
static inline void backtrace_symbols_fd(void** frames, int count, int fd) {
    (void)frames;
    (void)count;
    (void)fd;
}

#ifdef __cplusplus
}
#endif
