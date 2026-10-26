/* SPDX-License-Identifier: GPL-2.0-or-later
   The symbols the guest's own library asks the system's library for.

   The game links a C library of its own (the one that came with the game), and that
   library asks the system's library for a few things: the time functions and the
   character set conversions. The runtime is what stands in for the system's library,
   so these have to be answered here. Without them the game's own linker finds no
   address for them, says so by raising a debug exception, and the game stops.

   The time functions answer in the guest's own memory: the guest packs pointers into
   forty bits, and a pointer of the host would not fit. */
#define _GNU_SOURCE
#include "runtime.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Two buffers: one for the universal time, one for the local one (the standard says
 * both may be there at the same time). */
static void *guest_tm_buffers(void) {
    static void *buffers;
    if (!buffers) {
        buffers = runtime_low_map(256, BB_PROT_READ | BB_PROT_WRITE);
        if (!buffers) return NULL;
        memset(buffers, 0, 256);
    }
    return buffers;
}

/* The guest's struct tm is the usual one: nine ints. */
typedef struct { int sec, min, hour, mday, mon, year, wday, yday, isdst; } GuestTm;
static void guest_tm_write(void *at, const struct tm *from) {
    GuestTm *to = (GuestTm *)at;
    to->sec = from->tm_sec;   to->min = from->tm_min;   to->hour = from->tm_hour;
    to->mday = from->tm_mday; to->mon = from->tm_mon;   to->year = from->tm_year;
    to->wday = from->tm_wday; to->yday = from->tm_yday; to->isdst = from->tm_isdst;
}
static void guest_tm_read(const void *at, struct tm *to) {
    const GuestTm *from = (const GuestTm *)at;
    to->tm_sec = from->sec;   to->tm_min = from->min;   to->tm_hour = from->hour;
    to->tm_mday = from->mday; to->tm_mon = from->mon;   to->tm_year = from->year;
    to->tm_wday = from->wday; to->tm_yday = from->yday; to->tm_isdst = from->isdst;
}

static ABI void *guest_gmtime(const void *timer) {
    if (!timer) return NULL;
    void *buffers = guest_tm_buffers();
    if (!buffers) return NULL;
    const int64_t value = *(const int64_t *)timer;
    struct tm *result = gmtime(&value);
    if (!result) return NULL;
    guest_tm_write(buffers, result);
    return buffers;
}
static ABI void *guest_localtime(const void *timer) {
    if (!timer) return NULL;
    void *buffers = guest_tm_buffers();
    if (!buffers) return NULL;
    const int64_t value = *(const int64_t *)timer;
    struct tm *result = localtime(&value);
    if (!result) return NULL;
    guest_tm_write((char *)buffers + 128, result);
    return (char *)buffers + 128;
}
static ABI int64_t guest_mktime(void *tm) {
    if (!tm) return -1;
    struct tm value;
    guest_tm_read(tm, &value);
    return (int64_t)mktime(&value);
}

/* The thread-safe universal time: the time comes first (a pointer to it), the
 * answer second (where it is written). The game's own library asks for it by
 * name; without an address for it the game's linker stops the game. */
static ABI int32_t guest_gmtime_s(const void *timer, void *result) {
    if (!timer || !result) return 22; /* EINVAL */
    const int64_t value = *(const int64_t *)timer;
    struct tm *answer = gmtime(&value);
    if (!answer) return 11; /* the time is not representable */
    guest_tm_write(result, answer);
    return 0;
}

/* The character set conversions (the Japanese profile of the console: Shift-JIS).
   The game asks for them by name; what it needs of them is not known here, so they
   are answered with "nothing to convert" - the game goes on with the text it has.
   The functions are here so that the game's linker finds an address for them: without
   one it stops the game. */
static ABI int32_t ces_not_converted(void) { return 0; }

uintptr_t runtime_ces_resolve(const char *name) {
    if (runtime_nid_is(name,"1mecP7RgI2A#libSceLibcInternal")) return (uintptr_t)guest_gmtime;
    if (runtime_nid_is(name,"efhK-YSUYYQ#libSceLibcInternal")) return (uintptr_t)guest_localtime;
    if (runtime_nid_is(name,"n7AepwR0s34#libSceLibcInternal")) return (uintptr_t)guest_mktime;
    if (runtime_nid_is(name,"WAIqT36MwJU#3#1")) return (uintptr_t)ces_not_converted;
    if (runtime_nid_is(name,"kz9bGYFvopY#3#1")) return (uintptr_t)ces_not_converted;
    if (runtime_nid_is(name,"yGKn6vdYInc#3#1")) return (uintptr_t)ces_not_converted;
    if (runtime_nid_is(name,"RZ8ra69Qkxw#3#1")) return (uintptr_t)ces_not_converted;
    if (runtime_nid_is(name,"ZiDCxUUGbec#3#1")) return (uintptr_t)ces_not_converted;
    if (runtime_nid_is(name,"q9ONK5ZpMVQ#3#1")) return (uintptr_t)ces_not_converted;
    if (runtime_nid_is(name,"OfNANruoaxs#3#1")) return (uintptr_t)ces_not_converted;
    if (runtime_nid_is(name,"r7Sr1i7KLus#3#1")) return (uintptr_t)ces_not_converted;
    if (runtime_nid_is(name,"IljzwFmlBKg#3#1")) return (uintptr_t)ces_not_converted;
    if (runtime_nid_is(name,"538bRGc6Zo8#3#1")) return (uintptr_t)ces_not_converted;
    if (runtime_nid_is(name,"5bBacGLyLOs#libSceLibcInternal")) return (uintptr_t)guest_gmtime_s;
    return 0;
}
