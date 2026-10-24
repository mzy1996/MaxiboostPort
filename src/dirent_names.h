/* SPDX-License-Identifier: GPL-2.0-or-later
   The directory entries the runtime lists, on Windows.

   The MinGW-w64 environment has <dirent.h>, but its entries have no type (Linux's have one),
   and it has neither fstatat nor dirfd. The listing asks the file system for the type
   instead, so that the guest sees the same entries on both hosts. */
#ifndef BB_DIRENT_NAMES_H
#define BB_DIRENT_NAMES_H

#ifdef _WIN32

#include <dirent.h>
#include <sys/stat.h>

/* The types of a directory entry (the values the guest's listing uses). */
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10

/* The entry's type: the file system is asked, because the entry itself does not say. */
static inline unsigned char bb_dirent_type(const char* dir, const struct dirent* e) {
    struct stat entry;
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path)) {
        return DT_UNKNOWN;
    }
    if (stat(path, &entry)) {
        return DT_UNKNOWN;
    }
    if (S_ISDIR(entry.st_mode)) {
        return DT_DIR;
    }
    if (S_ISREG(entry.st_mode)) {
        return DT_REG;
    }
    return DT_UNKNOWN;
}

/* The entry's type on Linux: the entry itself says. */
#else

static inline unsigned char bb_dirent_type(const char* dir, const struct dirent* e) {
    (void)dir;
    return e->d_type;
}

#endif

#endif /* BB_DIRENT_NAMES_H */
