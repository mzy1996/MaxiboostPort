/* Kernel semaphores: 32-bit IDs, counted tokens and FIFO waiters.
 * Priority-ordered semaphores are approximated by FIFO order. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include "platform.h"
#ifdef _WIN32
#include <windows.h>
#endif
/* The semaphore's own parts: the host's critical section and condition variable on
 * Windows, the C library's elsewhere. */
#ifdef _WIN32
typedef CRITICAL_SECTION bb_sema_lock_t;
typedef CONDITION_VARIABLE bb_cond_t;
static INIT_ONCE bb_lock_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION bb_lock_section;
static BOOL CALLBACK bb_lock_init(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once; (void)parameter; (void)context;
    InitializeCriticalSection(&bb_lock_section);
    return TRUE;
}
/* The section is ready before the first use (the static initializer of the C library has
 * no counterpart here). */
#define bb_lock_ready() InitOnceExecuteOnce(&bb_lock_once,bb_lock_init,NULL,NULL)
#define bb_lock(l)   do { bb_lock_ready(); EnterCriticalSection(l); } while (0)
#define bb_unlock(l) LeaveCriticalSection(l)
#define bb_cond_init(c) do { InitializeConditionVariable(c); } while (0)
#define bb_cond_destroy(c) do { (void)(c); } while (0)
#define bb_cond_signal(c) (WakeConditionVariable(c), 0)
#define bb_cond_broadcast(c) (WakeAllConditionVariable(c), 0)
static int bb_cond_wait(bb_cond_t *c, bb_sema_lock_t *l) {
    return SleepConditionVariableCS(c,l,INFINITE) ? 0 : ETIMEDOUT;
}
static int bb_cond_timedwait(bb_cond_t *c, bb_sema_lock_t *l, uint32_t milliseconds) {
    return SleepConditionVariableCS(c,l,milliseconds) ? 0 : ETIMEDOUT;
}
static uint64_t now_ns(void) { return platform_realtime_ns(); }
#else
typedef pthread_mutex_t bb_sema_lock_t;
typedef pthread_cond_t bb_cond_t;
#define bb_lock_ready() (void)0
#define bb_lock(l) pthread_mutex_lock(l)
#define bb_unlock(l) pthread_mutex_unlock(l)
#define bb_cond_init(c) pthread_cond_init(c,NULL)
#define bb_cond_destroy(c) pthread_cond_destroy(c)
#define bb_cond_signal(c) bb_cond_signal(c)
#define bb_cond_broadcast(c) bb_cond_broadcast(c)
static int bb_cond_wait(bb_cond_t *c, bb_sema_lock_t *l) { return pthread_cond_wait(c,l); }
static int bb_cond_timedwait(bb_cond_t *c, bb_sema_lock_t *l, uint32_t milliseconds) {
    struct timespec end;
    clock_gettime(CLOCK_REALTIME,&end);
    end.tv_sec+=(time_t)(milliseconds/1000);
    end.tv_nsec+=(long)(milliseconds%1000)*1000000L;
    if (end.tv_nsec>=1000000000L) { end.tv_sec++; end.tv_nsec-=1000000000L; }
    return pthread_cond_timedwait(c,l,&end);
}
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
typedef struct Waiter {
    int32_t need, result;
    int done;
    bb_cond_t event;
    struct Waiter *next;
} Waiter;
typedef struct Sema {
    uint32_t id, attr;
    int32_t count, initial, maximum;
    unsigned active;
    int deleted;
    Waiter *first;
    struct Sema *next;
} Sema;
static bb_sema_lock_t lock;
static Sema *registry;
static uint32_t next_id=1;
static size_t created, deleted, acquired, signaled, timed_out;
static Sema *find(uint32_t id) {
    for (Sema *s=registry;s;s=s->next) if (s->id==id) return s;
    return NULL;
}
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: semaphore host error %d\n",e); exit(21); }
}
static ABI int32_t sem_create(uint32_t *out,const char *name,uint32_t attr,
                              int32_t initial,int32_t maximum,const void *options) {
    if (!out || !name || attr>2 || initial<0 || maximum<=0 || initial>maximum) return ERR(22);
    if (options) { fputs("STOP: semaphore options are not implemented\n",stderr); exit(21); }
    Sema *s=calloc(1,sizeof(*s));
    if (!s) return ERR(12);
    bb_lock(&lock);
    if (next_id==UINT32_MAX) { bb_unlock(&lock); free(s); return ERR(28); }
    s->id=next_id++; s->attr=attr; s->count=s->initial=initial; s->maximum=maximum;
    s->next=registry; registry=s; *out=s->id; ++created;
    printf("Runtime: semaphore id=%u, attr=%u, initial=%d, max=%d\n",s->id,attr,initial,maximum);
    bb_unlock(&lock);
    return 0;
}
static int32_t wait_count(uint32_t id,int32_t need,uint32_t *timeout,int block) {
    bb_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (need<=0 || need>s->maximum) result=ERR(22);
    else if (s->count>=need) { s->count-=need; ++acquired; }
    else if (!block) result=ERR(16);
    else if (timeout && !*timeout) { result=ERR(60); ++timed_out; }
    else {
        /* Priority-ordered semaphores (attr 2) are served FIFO: there is no
           PS4 priority scheduler, host threads run at equal priority. */
        if (getenv("BB_TRACE_SEMA")) fprintf(stderr,"Runtime: blocking wait on semaphore %u (need %d, count %d)\n",id,need,s->count); /* deadlock diagnosis */
        Waiter w={.need=need};
        bb_cond_init(&w.event);
        Waiter **tail=&s->first;
        while (*tail) tail=&(*tail)->next;
        *tail=&w; ++s->active;
        const uint64_t wait_start=runtime_wait_clock();
        uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000000 : 0;
        while (!w.done) {
            int e;
            runtime_wait_mark("a semaphore");
            if (!timeout) e=bb_cond_wait(&w.event,&lock);
            else {
                const uint64_t left=deadline>now_ns() ? deadline-now_ns() : 0;
                e=bb_cond_timedwait(&w.event,&lock,(uint32_t)((left+999999)/1000000));
            }
            runtime_wait_mark(0);
            if (e==ETIMEDOUT && !w.done) {
                Waiter **p=&s->first;
                while (*p!=&w) p=&(*p)->next;
                *p=w.next; w.result=ERR(60); w.done=1; ++timed_out;
            } else if (e!=ETIMEDOUT) host_check(e);
        }
        runtime_wait_note(2,runtime_wait_clock()-wait_start);
        result=w.result;
        if (!result) ++acquired;
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=result ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
        bb_cond_destroy(&w.event);
        if (!--s->active && s->deleted) free(s);
    }
    bb_unlock(&lock);
    return result;
}
static ABI int32_t sem_wait(uint32_t id,int32_t need,uint32_t *timeout) { return wait_count(id,need,timeout,1); }
static ABI int32_t sem_poll(uint32_t id,int32_t need) { return wait_count(id,need,NULL,0); }
static ABI int32_t sem_signal(uint32_t id,int32_t count) {
    bb_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count<=0 || count>s->maximum-s->count) result=ERR(22);
    else {
        s->count+=count; ++signaled;
        Waiter **p=&s->first;
        while (*p) {
            Waiter *w=*p;
            if (w->need>s->count) { p=&w->next; continue; }
            s->count-=w->need; *p=w->next; w->done=1;
            host_check(bb_cond_signal(&w->event));
        }
    }
    bb_unlock(&lock);
    return result;
}
static int32_t wake_all(Sema *s,int32_t result) {
    int32_t count=0;
    while (s->first) {
        Waiter *w=s->first; s->first=w->next;
        w->result=result; w->done=1; ++count;
        host_check(bb_cond_signal(&w->event));
    }
    return count;
}
static ABI int32_t sem_cancel(uint32_t id,int32_t count,int32_t *waiters) {
    bb_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count>s->maximum) result=ERR(22);
    else {
        int32_t n=wake_all(s,ERR(85));
        if (waiters) *waiters=n;
        s->count=count<0 ? s->initial : count;
    }
    bb_unlock(&lock);
    return result;
}
static ABI int32_t sem_delete(uint32_t id) {
    bb_lock(&lock);
    Sema **p=&registry;
    while (*p && (*p)->id!=id) p=&(*p)->next;
    if (!*p) { bb_unlock(&lock); return ERR(3); }
    Sema *s=*p; *p=s->next; s->deleted=1; ++deleted;
    wake_all(s,ERR(13));
    if (!s->active) free(s);
    bb_unlock(&lock);
    return 0;
}
/* Read-only test/diagnostic snapshot, not exposed to guest code. */
unsigned runtime_sema_waiters(uint32_t id) {
    bb_lock(&lock);
    Sema *s=find(id); unsigned n=0;
    if (s) for (Waiter *w=s->first;w;w=w->next) ++n;
    bb_unlock(&lock);
    return n;
}
uintptr_t runtime_sema_resolve(const char *name) {
    if (runtime_nid_is(name,"188x57JYp0g#p#J")) return (uintptr_t)sem_create;
    if (runtime_nid_is(name,"Zxa0VhQVTsk#p#J")) return (uintptr_t)sem_wait;
    if (runtime_nid_is(name,"4czppHBiriw#p#J")) return (uintptr_t)sem_signal;
    if (runtime_nid_is(name,"12wOHk8ywb0#p#J")) return (uintptr_t)sem_poll;
    if (runtime_nid_is(name,"4DM06U2BNEY#p#J")) return (uintptr_t)sem_cancel;
    if (runtime_nid_is(name,"R1Jvn8bSCW8#p#J")) return (uintptr_t)sem_delete;
    return 0;
}
void runtime_sema_report(void) {
    bb_lock(&lock);
    printf("Runtime: semaphores created=%zu, deleted=%zu, acquired=%zu, signals=%zu, timeouts=%zu\n",
           created,deleted,acquired,signaled,timed_out);
    bb_unlock(&lock);
}
