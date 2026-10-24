// SPDX-License-Identifier: GPL-2.0-or-later
// The host's own memory (gpu/shim/host_services.h): the pages the game's code is patched in,
// and the stubs beside it.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif

/// An allocation with the given alignment (the C++ standard's is C++17; the Windows one is
/// in the C library).
inline void* bb_aligned_alloc(size_t alignment, size_t size) {
#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    return std::aligned_alloc(alignment, size);
#endif
}
