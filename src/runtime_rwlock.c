/* Read-write locks behind PS4 pointer-to-handle ABI. The registry protects
   lifetime, static initialization and ownership; waiting happens outside it. */
#define _GNU_SOURCE
#include "runtime.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#ifdef _WIN32
#include <windows.h>
#endif
/* The lock itself: the host's read-write lock on Windows (SRWLOCK), the C library's
 * elsewhere. The release is told whether the thread holds it for reading or for writing
 * (the registry above knows). */
#ifdef _WIN32
typedef SRWLOCK bb_rwlock_t;
static int bb_rwlock_init(bb_rwlock_t *lock) { InitializeSRWLock(lock); return 0; }
static int bb_rwlock_rdlock(bb_rwlock_t *lock) { AcquireSRWLockShared(lock); return 0; }
static int bb_rwlock_wrlock(bb_rwlock_t *lock) { AcquireSRWLockExclusive(lock); return 0; }
static int bb_rwlock_tryrdlock(bb_rwlock_t *lock) { return TryAcquireSRWLockShared(lock) ? 0 : EBUSY; }
static int bb_rwlock_trywrlock(bb_rwlock_t *lock) { return TryAcquireSRWLockExclusive(lock) ? 0 : EBUSY; }
static int bb_rwlock_release(bb_rwlock_t *lock, int writer) {
    if (writer) ReleaseSRWLockExclusive(lock); else ReleaseSRWLockShared(lock);
    return 0;
}
static int bb_rwlock_destroy(bb_rwlock_t *lock) { (void)lock; return 0; }
/* The timed ones are a wait of a millisecond at a time (the host's lock has no deadline
 * of its own). */
static int bb_rwlock_timed(bb_rwlock_t *lock, int writer, uint64_t deadline_ns) {
    for (;;) {
        const int e=writer ? bb_rwlock_trywrlock(lock) : bb_rwlock_tryrdlock(lock);
        if (!e) return 0;
        if (e!=EBUSY) return e;
        if (platform_realtime_ns()>=deadline_ns) return ETIMEDOUT;
        platform_sleep_ns(1000000ull);
    }
}
#else
typedef pthread_rwlock_t bb_rwlock_t;
static int bb_rwlock_init(bb_rwlock_t *lock) { return pthread_rwlock_init(lock,NULL); }
static int bb_rwlock_rdlock(bb_rwlock_t *lock) { return pthread_rwlock_rdlock(lock); }
static int bb_rwlock_wrlock(bb_rwlock_t *lock) { return pthread_rwlock_wrlock(lock); }
static int bb_rwlock_tryrdlock(bb_rwlock_t *lock) { return pthread_rwlock_tryrdlock(lock); }
static int bb_rwlock_trywrlock(bb_rwlock_t *lock) { return pthread_rwlock_trywrlock(lock); }
static int bb_rwlock_release(bb_rwlock_t *lock, int writer) { (void)writer; return pthread_rwlock_unlock(lock); }
static int bb_rwlock_destroy(bb_rwlock_t *lock) { return pthread_rwlock_destroy(lock); }
static int bb_rwlock_timed(bb_rwlock_t *lock, int writer, uint64_t deadline_ns) {
    struct timespec deadline;
    const uint64_t seconds=deadline_ns/1000000000ull;
    deadline.tv_sec=(time_t)seconds;
    deadline.tv_nsec=(long)(deadline_ns-seconds*1000000000ull);
    return writer ? pthread_rwlock_timedwrlock(lock,&deadline) : pthread_rwlock_timedrdlock(lock,&deadline);
}
#endif
typedef struct Holder {
    pthread_t thread;
    unsigned readers, writer, pending;
    struct Holder *next;
} Holder;
typedef struct Rwlock {
    bb_rwlock_t native;
    Holder *holders;
    unsigned inflight;
    struct Rwlock *next;
} Rwlock;
typedef struct { int64_t seconds, nanoseconds; } GuestTime;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static Rwlock *registry;
static size_t created, reads, writes, unlocks;
static int32_t error(int e) {
    if (!e) return 0;
    unsigned code;
    switch(e) {
    case EPERM: code=1; break;
    case EDEADLK: code=11; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case ETIMEDOUT: code=60; break;
    default: fprintf(stderr,"STOP: unmapped rwlock error %d\n",e); exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000)|code);
}
/* All helpers ending in _locked require registry_lock. */
static Rwlock *find_locked(Rwlock *handle) {
    for (Rwlock *r=registry; r; r=r->next) if (r==handle) return r;
    return NULL;
}
static int create_locked(Rwlock **out) {
    Rwlock *r=calloc(1,sizeof(*r));
    if (!r) return ENOMEM;
    int e=bb_rwlock_init(&r->native);
    if (e) { free(r); return e; }
    r->next=registry; registry=r; *out=r; ++created; return 0;
}
static ABI int32_t rw_init(Rwlock **out, void *const *attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return error(EINVAL);
    /* An attribute object (rw_attr_init) only names reader or writer preference: Linux's default
     * order serves both. */
    pthread_mutex_lock(&registry_lock);
    /* POSIX allows uninitialized storage, so never read *out during init. */
    int e=create_locked(out);
    pthread_mutex_unlock(&registry_lock); return error(e);
}
static void prune_locked(Rwlock *r, Holder *h) {
    if (h->readers || h->writer || h->pending) return;
    Holder **link=&r->holders;
    while (*link!=h) link=&(*link)->next;
    *link=h->next; free(h);
}
enum Operation { READ, WRITE, TRY_READ, TRY_WRITE, TIMED_READ, TIMED_WRITE };
static int32_t acquire(Rwlock **handle, enum Operation op, const GuestTime *time) {
    if (!handle) return error(EINVAL);
    int writer=op==WRITE || op==TRY_WRITE || op==TIMED_WRITE;
    int attempt=op==TRY_READ || op==TRY_WRITE;
    int timed=op==TIMED_READ || op==TIMED_WRITE;
    pthread_mutex_lock(&registry_lock);
    int e=0;
    if (!*handle) e=create_locked(handle);
    Rwlock *r=e ? NULL : find_locked(*handle);
    if (!r) { pthread_mutex_unlock(&registry_lock); return error(e ? e : EINVAL); }
    Holder *h=r->holders;
    while (h && !pthread_equal(h->thread,pthread_self())) h=h->next;
    if (h && (h->writer || (writer && h->readers))) {
        pthread_mutex_unlock(&registry_lock); return error(attempt ? EBUSY : EDEADLK);
    }
    if (h && h->readers==UINT_MAX) { pthread_mutex_unlock(&registry_lock); return error(EAGAIN); }
    if (!h) {
        h=calloc(1,sizeof(*h));
        if (!h) { pthread_mutex_unlock(&registry_lock); return error(ENOMEM); }
        h->thread=pthread_self(); h->next=r->holders; r->holders=h;
    }
    ++r->inflight; ++h->pending;
    pthread_mutex_unlock(&registry_lock);
    /* Timed operations first try the lock, matching POSIX timeout validation. */
    if (attempt) e=writer ? bb_rwlock_trywrlock(&r->native) : bb_rwlock_tryrdlock(&r->native);
    else if (timed) {
        if (!time || time->nanoseconds<0 || time->nanoseconds>=1000000000) e=EINVAL;
        else {
            const uint64_t deadline=(uint64_t)time->seconds*1000000000ull+(uint64_t)time->nanoseconds;
            e=bb_rwlock_timed(&r->native,writer,deadline);
        }
    }
    else e=writer ? bb_rwlock_wrlock(&r->native) : bb_rwlock_rdlock(&r->native);
    pthread_mutex_lock(&registry_lock);
    --r->inflight; --h->pending;
    if (!e) {
        if (writer) { h->writer=1; ++writes; }
        else { ++h->readers; ++reads; }
    }
    prune_locked(r,h);
    pthread_mutex_unlock(&registry_lock); return error(e);
}
static ABI int32_t rw_read(Rwlock **r) { return acquire(r,READ,NULL); }
static ABI int32_t rw_write(Rwlock **r) { return acquire(r,WRITE,NULL); }
static ABI int32_t rw_tryread(Rwlock **r) { return acquire(r,TRY_READ,NULL); }
static ABI int32_t rw_trywrite(Rwlock **r) { return acquire(r,TRY_WRITE,NULL); }
static ABI int32_t rw_timedread(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_READ,t); }
static ABI int32_t rw_timedwrite(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_WRITE,t); }
static ABI int32_t rw_unlock(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    pthread_mutex_lock(&registry_lock);
    Rwlock *r=find_locked(*handle);
    if (!r) { pthread_mutex_unlock(&registry_lock); return error(EINVAL); }
    Holder *h=r->holders;
    while (h && !pthread_equal(h->thread,pthread_self())) h=h->next;
    if (!h || (!h->readers && !h->writer)) { pthread_mutex_unlock(&registry_lock); return error(EPERM); }
    int e=bb_rwlock_release(&r->native,h->writer);
    if (!e) { if (h->writer) h->writer=0; else --h->readers; ++unlocks; }
    prune_locked(r,h);
    pthread_mutex_unlock(&registry_lock); return error(e);
}
static ABI int32_t rw_destroy(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    pthread_mutex_lock(&registry_lock);
    if (!*handle) { pthread_mutex_unlock(&registry_lock); return 0; }
    Rwlock *r=find_locked(*handle);
    if (!r) { pthread_mutex_unlock(&registry_lock); return error(EINVAL); }
    if (r->inflight || r->holders) { pthread_mutex_unlock(&registry_lock); return error(EBUSY); }
    int e=bb_rwlock_destroy(&r->native);
    if (!e) {
        Rwlock **link=&registry;
        while (*link!=r) link=&(*link)->next;
        *link=r->next; free(r); *handle=(Rwlock *)(uintptr_t)1;
    }
    pthread_mutex_unlock(&registry_lock); return error(e);
}
/* Attribute objects: a handle to the type (PS4: 1 prefer readers, 2 prefer writers). */
static ABI int32_t rw_attr_init(int32_t **attr) {
    if (!attr) return error(EINVAL);
    if (!(*attr=calloc(1,sizeof(**attr)))) return error(ENOMEM);
    **attr=1; return 0;
}
static ABI int32_t rw_attr_destroy(int32_t **attr) {
    if (!attr || !*attr) return error(EINVAL);
    free(*attr); *attr=NULL; return 0;
}
static ABI int32_t rw_attr_settype(int32_t **attr, int32_t type) {
    if (!attr || !*attr || type<1 || type>2) return error(EINVAL);
    **attr=type; return 0;
}
static ABI int32_t rw_attr_gettype(int32_t *const *attr, int32_t *type) {
    if (!attr || !*attr || !type) return error(EINVAL);
    *type=**attr; return 0;
}
/* POSIX names (libc.prx calls them): positive errno values instead of Orbis codes. */
static int32_t posix(int32_t r) { return r ? (int32_t)((uint32_t)r&0xffff) : 0; }
static ABI int32_t posix_read(Rwlock **r) { return posix(rw_read(r)); }
static ABI int32_t posix_write(Rwlock **r) { return posix(rw_write(r)); }
static ABI int32_t posix_unlock(Rwlock **r) { return posix(rw_unlock(r)); }
static ABI int32_t posix_tryread(Rwlock **r) { return posix(rw_tryread(r)); }
static ABI int32_t posix_trywrite(Rwlock **r) { return posix(rw_trywrite(r)); }
static ABI int32_t posix_init(Rwlock **r,void *const *attr) { return posix(rw_init(r,attr,NULL)); }
static ABI int32_t posix_destroy(Rwlock **r) { return posix(rw_destroy(r)); }
static const RuntimeExport attr_exports[]={
    {"scePthreadRwlockattrInit",rw_attr_init}, {"scePthreadRwlockattrDestroy",rw_attr_destroy},
    {"scePthreadRwlockattrSettype",rw_attr_settype}, {"scePthreadRwlockattrGettype",rw_attr_gettype},
    {"pthread_rwlock_rdlock",posix_read}, {"pthread_rwlock_wrlock",posix_write},
    {"pthread_rwlock_unlock",posix_unlock}, {"pthread_rwlock_tryrdlock",posix_tryread},
    {"pthread_rwlock_trywrlock",posix_trywrite}, {"pthread_rwlock_init",posix_init},
    {"pthread_rwlock_destroy",posix_destroy},
};
uintptr_t runtime_rwlock_resolve(const char *name) {
    const uintptr_t attr=RUNTIME_LOOKUP(attr_exports,name);
    if (attr) return attr;
    if (runtime_nid_is(name,"6ULAa0fq4jA#p#J")) return (uintptr_t)rw_init;
    if (runtime_nid_is(name,"BB+kb08Tl9A#p#J")) return (uintptr_t)rw_destroy;
    if (runtime_nid_is(name,"Ox9i0c7L5w0#p#J")) return (uintptr_t)rw_read;
    if (runtime_nid_is(name,"mqdNorrB+gI#p#J")) return (uintptr_t)rw_write;
    if (runtime_nid_is(name,"XD3mDeybCnk#p#J")) return (uintptr_t)rw_tryread;
    if (runtime_nid_is(name,"bIHoZCTomsI#p#J")) return (uintptr_t)rw_trywrite;
    if (runtime_nid_is(name,"+L98PIbGttk#p#J")) return (uintptr_t)rw_unlock;
    if (runtime_nid_is(name,"iPtZRWICjrM#p#J")) return (uintptr_t)rw_timedread;
    if (runtime_nid_is(name,"adh--6nIqTk#p#J")) return (uintptr_t)rw_timedwrite;
    return 0;
}
void runtime_rwlock_report(void) {
    pthread_mutex_lock(&registry_lock);
    printf("Runtime: rwlocks created=%zu, reads=%zu, writes=%zu, unlocks=%zu\n",created,reads,writes,unlocks);
    pthread_mutex_unlock(&registry_lock);
}
