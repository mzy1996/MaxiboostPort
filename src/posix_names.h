/* SPDX-License-Identifier: GPL-2.0-or-later
   The POSIX names the runtime uses, on Windows.

   The MinGW-w64 environment has most of them already; the few that differ (the open flags
   are fewer, the directories are made without a mode) are mapped here, so that the code
   above is the same on both hosts. */
#ifndef BB_POSIX_NAMES_H
#define BB_POSIX_NAMES_H

#ifdef _WIN32

#include <direct.h>
#include <io.h>
#include <sys/stat.h>

/* The open flags the code passes that Windows has no equivalent for: they are dropped
 * (the Windows runtime opens the files the same way). */
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef O_SYNC
#define O_SYNC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

/* The directories are made without a mode (Windows has none). */
static inline int bb_mkdir(const char* path, int mode) {
    (void)mode;
    return _mkdir(path);
}

#else /* !_WIN32 */

#include <sys/stat.h>
static inline int bb_mkdir(const char* path, int mode) { return mkdir(path, (mode_t)mode); }

#endif /* _WIN32 */

#endif /* BB_POSIX_NAMES_H */
