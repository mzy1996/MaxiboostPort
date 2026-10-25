/* libkernel/libScePosix process services: clocks, sleeping, errno mapping,
 * pthread once/keys, signal bookkeeping and a narrow sysctl. Guest values
 * use FreeBSD numbering; host errno values never reach the guest directly. */
#define _GNU_SOURCE
#include "runtime.h"
#include "platform.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <sys/time.h>
#else
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <dlfcn.h>
#include <dirent.h>
#include <ucontext.h>
#include <signal.h>
#include <sys/uio.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <x86intrin.h>
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define PAGE 16384

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct { int64_t sec, usec; } GuestTimeval;

int32_t runtime_guest_errno(int e) {
    switch (e) {
    case 0: return 0;
    case EPERM: return 1; case ENOENT: return 2; case ESRCH: return 3; case EINTR: return 4;
    case EIO: return 5; case ENXIO: return 6; case E2BIG: return 7; case ENOEXEC: return 8;
    case EBADF: return 9; case ECHILD: return 10; case EDEADLK: return 11; case ENOMEM: return 12;
    case EACCES: return 13; case EFAULT: return 14; case EBUSY: return 16; case EEXIST: return 17;
    case EXDEV: return 18; case ENODEV: return 19; case ENOTDIR: return 20; case EISDIR: return 21;
    case EINVAL: return 22; case ENFILE: return 23; case EMFILE: return 24; case ENOTTY: return 25;
    case EFBIG: return 27; case ENOSPC: return 28; case ESPIPE: return 29; case EROFS: return 30;
    case EMLINK: return 31; case EPIPE: return 32; case ERANGE: return 34; case EAGAIN: return 35;
    case ENAMETOOLONG: return 63; case ENOTEMPTY: return 66; case ETIMEDOUT: return 60;
    case ELOOP: return 62; case ENOSYS: return 78; case EOVERFLOW: return 84; case ECANCELED: return 85;
    default: return 5; /* EIO: unmapped host error */
    }
}
static int32_t fail_posix(int e) { *runtime_errno()=runtime_guest_errno(e); return -1; }

/* ---- clocks and sleeping ---- */
static uint64_t process_start;
__attribute__((constructor)) static void remember_start(void) { process_start=platform_monotonic_ns(); }
/* The guest's clock ids: which host clock each of them is. */
enum { BB_CLOCK_REALTIME=0, BB_CLOCK_MONOTONIC=4, BB_CLOCK_SECOND=13,
       BB_CLOCK_THREAD_CPUTIME=14, BB_CLOCK_PROCESS_CPUTIME=2 };
static int host_clock(uint32_t id) {
    switch (id) {
    case 0: case 9: case 10: return BB_CLOCK_REALTIME;                        /* REALTIME(_PRECISE/_FAST) */
    case 4: case 11: case 12: case 5: case 7: case 8: return BB_CLOCK_MONOTONIC; /* MONOTONIC/UPTIME */
    case 13: return BB_CLOCK_SECOND;
    case 14: return BB_CLOCK_THREAD_CPUTIME;
    case 2: case 15: return BB_CLOCK_PROCESS_CPUTIME;                          /* PROF/PROCTIME */
    case 16: case 17: case 18: case 19: return BB_CLOCK_MONOTONIC;             /* PS4 network clocks */
    default: return -1;
    }
}
static int clock_read(uint32_t id,GuestTimespec *ts) {
    int host=host_clock(id);
    if (!ts || host<0) return EINVAL;
    uint64_t ns=0;
    if (host==BB_CLOCK_REALTIME) ns=platform_realtime_ns();
    else if (host==BB_CLOCK_MONOTONIC) ns=platform_monotonic_ns();
    else if (host==BB_CLOCK_SECOND) ns=platform_realtime_ns();
    else {
        uint64_t process_ns=0,thread_ns=0;
        platform_cpu_times(&process_ns,&thread_ns);
        ns=host==BB_CLOCK_THREAD_CPUTIME ? thread_ns : process_ns;
    }
    if (host==BB_CLOCK_SECOND) ns=ns/1000000000ull*1000000000ull;
    ts->sec=(int64_t)(ns/1000000000ull); ts->nsec=(int64_t)(ns%1000000000ull); return 0;
}
static ABI int32_t kernel_clock_gettime(uint32_t id,GuestTimespec *ts) {
    int e=clock_read(id,ts); return e ? ERR(runtime_guest_errno(e)) : 0;
}
static ABI int32_t posix_clock_gettime(uint32_t id,GuestTimespec *ts) {
    int e=clock_read(id,ts); return e ? fail_posix(e) : 0;
}
static ABI int32_t posix_clock_getres(uint32_t id,GuestTimespec *ts) {
    if (host_clock(id)<0) return fail_posix(EINVAL);
    if (ts) { ts->sec=0; ts->nsec=1; }
    return 0;
}
static ABI uint64_t process_time(void) {
    uint64_t ns=platform_monotonic_ns();
    return (ns-process_start)/1000;
}
static ABI uint64_t process_time_counter(void) {
    return platform_monotonic_ns()-process_start;
}
static ABI uint64_t process_time_frequency(void) { return 1000000000; }
static ABI uint64_t read_tsc(void) { return __rdtsc(); }
static uint64_t tsc_hz;
static ABI uint64_t tsc_frequency(void) {
    if (!tsc_hz) {
        uint64_t a=platform_monotonic_ns(); uint64_t t0=__rdtsc();
        platform_sleep_ns(20000000ull);
        uint64_t b=platform_monotonic_ns(); uint64_t t1=__rdtsc();
        uint64_t ns=b-a;
        tsc_hz=(t1-t0)*1000000000/(ns ? ns : 1);
    }
    return tsc_hz;
}
/* Shared with the GPU library so flip/label timestamps use the guest's clock. */
uint64_t runtime_process_time_us(void) { return process_time(); }
uint64_t runtime_process_time_counter(void) { return process_time_counter(); }
uint64_t runtime_tsc_frequency(void) { return tsc_frequency(); }
/* bbport (frame stats): how often and how long the game sleeps (it polls GPU labels that way). */
static _Atomic uint64_t sleep_calls, sleep_total_ns;
void runtime_sleep_stats(uint64_t *calls, uint64_t *ns) {
    *calls=atomic_exchange(&sleep_calls,0); *ns=atomic_exchange(&sleep_total_ns,0);
}
static int sleep_ns(uint64_t ns) {
    uint64_t a=platform_monotonic_ns();
    platform_sleep_ns(ns);
    uint64_t b=platform_monotonic_ns();
    atomic_fetch_add(&sleep_calls,1);
    atomic_fetch_add(&sleep_total_ns,b-a);
    runtime_wait_note(3,b-a);
    return 0;
}
static ABI int32_t kernel_usleep(uint32_t usec) { sleep_ns((uint64_t)usec*1000); return 0; }
static ABI int32_t posix_usleep(uint32_t usec) { sleep_ns((uint64_t)usec*1000); return 0; }
static ABI uint32_t posix_sleep(uint32_t seconds) { sleep_ns((uint64_t)seconds*1000000000); return 0; }
static ABI int32_t kernel_sleep(uint32_t seconds) { sleep_ns((uint64_t)seconds*1000000000); return 0; }
/* Locking pages in memory and syncing mappings: the game's memory is never paged out or backed
 * by files it expects to be written back, so both succeed. */
static ABI int32_t kernel_mlock(const void *address,uint64_t size) { (void)address; (void)size; return 0; }
static ABI int32_t posix_msync(void *address,uint64_t size,int32_t flags) { (void)address; (void)size; (void)flags; return 0; }
static ABI int32_t posix_nanosleep(const GuestTimespec *rq,GuestTimespec *rem) {
    if (!rq || rq->nsec<0 || rq->nsec>=1000000000 || rq->sec<0) return fail_posix(EINVAL);
    sleep_ns((uint64_t)rq->sec*1000000000+(uint64_t)rq->nsec);
    if (rem) { rem->sec=0; rem->nsec=0; }
    return 0;
}
static ABI int32_t kernel_nanosleep(const GuestTimespec *rq,GuestTimespec *rem) {
    return posix_nanosleep(rq,rem) ? ERR(*runtime_errno()) : 0;
}
typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;
static ABI int32_t kernel_gettimezone(GuestTimezone *tz) {
    if (!tz) return ERR(22);
    /* The guest's timezone offset: the host's local time minus the universal time. */
    time_t now=time(NULL);
    struct tm local;
#ifdef _WIN32
    localtime_s(&local,&now);
#else
    localtime_r(&now,&local);
#endif
    tz->minuteswest=(int32_t)(-platform_utc_offset_seconds()/60); tz->dsttime=0;
    return 0;
}
static ABI int32_t posix_gettimeofday(GuestTimeval *tv,GuestTimezone *tz) {
    uint64_t ns=platform_realtime_ns();
    if (tv) { tv->sec=(int64_t)(ns/1000000000ull); tv->usec=(int64_t)((ns%1000000000ull)/1000ull); }
    if (tz) kernel_gettimezone(tz);
    return 0;
}
static ABI int32_t kernel_gettimeofday(GuestTimeval *tv) {
    if (!tv) return ERR(22);
    return posix_gettimeofday(tv,NULL);
}
static ABI int64_t posix_time(int64_t *out) { int64_t t=(int64_t)time(NULL); if (out) *out=t; return t; }

/* ---- process ---- */
static ABI int32_t get_pagesize(void) { return PAGE; }
static ABI int32_t get_pid(void) { return 1000; }
static ABI int32_t yield(void) {
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
    return 0;
}
static ABI __attribute__((noreturn)) void hard_exit(int status) {
    printf("Runtime: guest requested _exit(%d)\n",status);
    runtime_report();
    fflush(stdout);
    _exit(status);
}
/* The addresses of the game's own frames on this thread's stack. */
static int guest_call_addresses(uint64_t out[], int max);
static ABI __attribute__((noreturn)) void raise_exception(uint32_t code,uint64_t argument) {
    fprintf(stderr,"STOP: guest raised debug exception code=0x%x argument=0x%llx\n",code,(unsigned long long)argument);
    /* The caller is where the game raised it: the frame pointer is often left out on
     * this platform, so the return address is read instead. */
    { const uintptr_t caller=(uintptr_t)__builtin_return_address(0);
      fprintf(stderr,"  raised at %p\n",(void *)caller);
      runtime_guest_where(stderr,(uint64_t)caller);
      runtime_report_maps(stderr);
      { fprintf(stderr,"Runtime: the game's frames:\n"); runtime_guest_backtrace(stderr); }
      /* The game runs on this processor, so its registers are the ones of the host here:
       * the return value of the call it made before (rax) and the rest say what it did. */
      uint64_t registers[16];
      __asm__ volatile(
          "movq %%rax,  0(%0)\n\t" "movq %%rbx,  8(%0)\n\t" "movq %%rcx, 16(%0)\n\t"
          "movq %%rdx, 24(%0)\n\t" "movq %%rsi, 32(%0)\n\t" "movq %%rdi, 40(%0)\n\t"
          "movq %%rbp, 48(%0)\n\t" "movq %%rsp, 56(%0)\n\t" "movq %%r8,  64(%0)\n\t"
          "movq %%r9,  72(%0)\n\t" "movq %%r10, 80(%0)\n\t" "movq %%r11, 88(%0)\n\t"
          "movq %%r12, 96(%0)\n\t" "movq %%r13,104(%0)\n\t" "movq %%r14,112(%0)\n\t"
          "movq %%r15,120(%0)\n\t" : : "r"(registers) : "memory");
      static const char *const names[16]={"rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp",
          "r8","r9","r10","r11","r12","r13","r14","r15"};
      for (int i=0;i<16;++i) fprintf(stderr,"  %s=%#llx\n",names[i],(unsigned long long)registers[i]);
      /* The game's own frames: a return address is the one after a call, and the call is
       * either a direct one (E8) or an indirect one (FF /2). Only those are frames; the
       * rest of the stack holds data that looks like an address. */
      { const uintptr_t *sp=(const uintptr_t *)__builtin_frame_address(0);
        /* The frame pointer chain of the guest (it keeps the frame pointer). */
        { uintptr_t frame=registers[6]; /* rbp */
          for (int depth=0; depth<24 && frame; ++depth) {
              uint64_t saved=0, ret=0;
              if (platform_read_memory((void *)frame,&saved,8) || platform_read_memory((void *)(frame+8),&ret,8)) break;
              uint64_t offset=0; const char *module=NULL;
              runtime_guest_address(ret,&offset,&module);
              fprintf(stderr,"  #%d %p %s+%#llx\n",depth,(void *)ret,module?module:"(host)",(unsigned long long)offset);
              if (saved<=frame) break;
              frame=saved;
          } }
        int shown=0;
        for (int i=0;i<400 && shown<12;++i) {
            const uintptr_t at=sp[i];
            if (!runtime_guest_address(at,NULL,NULL)) continue;
            uint8_t before[8];
            if (platform_read_memory((void *)(at-8),before,sizeof(before))) continue;
            const int direct=before[3]==0xE8;
            const int indirect=before[3]==0xFF && (before[4]&7)==2;
            if (!direct && !indirect) continue;
            uint64_t offset=0; const char *module=NULL;
            runtime_guest_address(at,&offset,&module);
            fprintf(stderr,"  a frame of the game: %s+%#llx (after a %s call)\n",
                    module?module:"its code",(unsigned long long)offset,direct?"direct":"indirect");
            ++shown;
        } }
      /* The game's own frames on the stack: where it was when it stopped. */
      uint64_t addresses[8];
      const int found=guest_call_addresses(addresses,8);
      for (int i=0;i<found;++i) {
          uint64_t offset=0; const char *module=NULL;
          runtime_guest_address(addresses[i],&offset,&module);
          fprintf(stderr,"  the game was at %s+%#llx\n",module?module:"its code",
                  (unsigned long long)offset);
      } }
    runtime_guest_backtrace(stderr);
    runtime_report();
    exit(23);
}
static ABI int32_t print_backtrace(void) {
    fputs("Runtime: guest requested a backtrace:\n",stderr);
    runtime_guest_backtrace(stderr);
    return 0;
}
typedef struct { uint32_t bits[4]; } GuestSigset;
static _Thread_local GuestSigset signal_mask;
static uintptr_t handlers[128];
static ABI uintptr_t guest_signal(int sig,uintptr_t handler) {
    if (sig<=0 || sig>=128) { *runtime_errno()=22; return (uintptr_t)-1; } /* SIG_ERR */
    uintptr_t old=handlers[sig]; handlers[sig]=handler;
    printf("Runtime: guest signal(%d) handler recorded (host signals are not forwarded)\n",sig);
    return old;
}
static ABI int32_t guest_sigprocmask(int how,const GuestSigset *set,GuestSigset *old) {
    if (old) *old=signal_mask;
    if (!set) return 0;
    for (int i=0;i<4;++i) {
        if (how==1) signal_mask.bits[i]|=set->bits[i];        /* SIG_BLOCK */
        else if (how==2) signal_mask.bits[i]&=~set->bits[i];  /* SIG_UNBLOCK */
        else if (how==3) signal_mask.bits[i]=set->bits[i];    /* SIG_SETMASK */
        else return fail_posix(EINVAL);
    }
    return 0;
}
static ABI int32_t guest_sigfillset(GuestSigset *set) { if (!set) return fail_posix(EINVAL); memset(set,0xff,sizeof(*set)); return 0; }
static ABI int32_t guest_sigemptyset(GuestSigset *set) { if (!set) return fail_posix(EINVAL); memset(set,0,sizeof(*set)); return 0; }
typedef struct { GuestTimeval utime, stime; int64_t rest[14]; } GuestRusage;
static ABI int32_t guest_getrusage(int who,GuestRusage *out) {
    if (!out || (who!=0 && who!=1)) return fail_posix(EINVAL);
    memset(out,0,sizeof(*out));
    uint64_t process_ns=0,thread_ns=0;
    platform_cpu_times(&process_ns,&thread_ns);
    uint64_t ns=who==0 ? process_ns : thread_ns;
    out->utime=(GuestTimeval){(int64_t)(ns/1000000000ull),(int64_t)((ns%1000000000ull)/1000ull)};
    out->stime=(GuestTimeval){0,0};
    return 0;
}
static ABI int32_t guest_sysctl(const int32_t *name,uint32_t namelen,void *old,uint64_t *oldlen,const void *new_value,uint64_t newlen) {
    (void)newlen;
    if (!name || namelen<2 || new_value) return fail_posix(EINVAL);
    if (name[0]==1 && name[1]==37) { /* kern.arandom */
        if (!old || !oldlen) return fail_posix(EINVAL);
        if (platform_random(old,(size_t)*oldlen)<0) return fail_posix(EIO);
        return 0;
    }
    if (name[0]==6 && (name[1]==7 || name[1]==3)) { /* hw.pagesize / hw.ncpu */
        if (!oldlen) return fail_posix(EINVAL);
        int32_t value=name[1]==7 ? PAGE : 7;
        if (old) { if (*oldlen<4) return fail_posix(ENOMEM); memcpy(old,&value,4); }
        *oldlen=4; return 0;
    }
    fprintf(stderr,"STOP: unsupported sysctl mib");
    for (uint32_t i=0;i<namelen && i<8;++i) fprintf(stderr," %d",name[i]);
    fputc('\n',stderr);
    exit(21);
}

/* ---- pthread_once and thread-specific data ---- */
static ABI int32_t thread_once(int32_t *once,void (ABI *routine)(void)) {
    if (!once || !routine) return ERR(22);
    for (;;) {
        int32_t state=__atomic_load_n(once,__ATOMIC_ACQUIRE);
        if (state==1) return 0;
        if (state==0 && __atomic_compare_exchange_n(once,&state,2,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            routine();
            __atomic_store_n(once,1,__ATOMIC_RELEASE);
            return 0;
        }
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
    }
}
static ABI int32_t posix_once(int32_t *once,void (ABI *routine)(void)) { return thread_once(once,routine) ? 22 : 0; }
#define KEYS 256
typedef void (ABI *KeyDestructor)(void *);
static struct { int used; KeyDestructor destructor; } keys[KEYS];
static pthread_mutex_t key_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local void *key_values[KEYS];
static ABI int32_t key_create(uint32_t *key,KeyDestructor destructor) {
    if (!key) return ERR(22);
    pthread_mutex_lock(&key_lock);
    for (uint32_t i=1;i<KEYS;++i) if (!keys[i].used) {
        keys[i].used=1; keys[i].destructor=destructor;
        pthread_mutex_unlock(&key_lock);
        *key=i; return 0;
    }
    pthread_mutex_unlock(&key_lock);
    return ERR(35);
}
static ABI int32_t key_delete(uint32_t key) {
    if (!key || key>=KEYS) return ERR(22);
    pthread_mutex_lock(&key_lock);
    int32_t r=keys[key].used ? 0 : ERR(22);
    keys[key].used=0; keys[key].destructor=NULL;
    pthread_mutex_unlock(&key_lock);
    return r;
}
static ABI int32_t key_set(uint32_t key,void *value) {
    if (!key || key>=KEYS || !keys[key].used) return ERR(22);
    key_values[key]=value; return 0;
}
static ABI void *key_get(uint32_t key) { return key && key<KEYS ? key_values[key] : NULL; }
void runtime_thread_keys_cleanup(void) {
    for (int round=0;round<4;++round) {
        int any=0;
        for (uint32_t i=1;i<KEYS;++i) {
            void *value=key_values[i];
            if (!value || !keys[i].used || !keys[i].destructor) continue;
            key_values[i]=NULL; any=1;
            keys[i].destructor(value);
        }
        if (!any) break;
    }
}
static ABI int32_t posix_key_create(uint32_t *k,KeyDestructor d) { int32_t r=key_create(k,d); return r ? r&0xffff : 0; }
static ABI int32_t posix_key_delete(uint32_t k) { int32_t r=key_delete(k); return r ? r&0xffff : 0; }
static ABI int32_t posix_key_set(uint32_t k,void *v) { int32_t r=key_set(k,v); return r ? r&0xffff : 0; }

/* Nothing to do or report: sync (writes go straight to the host), no address sanitizer, no
 * sanitizer allocator replacement (NULL), crash dump settings. */
static ABI int32_t kernel_nothing(void) { return 0; }
static ABI void *kernel_null(void) { return NULL; }
static const RuntimeExport exports[]={
    {"sceKernelClockGettime",kernel_clock_gettime}, {"clock_gettime",posix_clock_gettime},
    {"clock_getres",posix_clock_getres},
    {"sceKernelGetProcessTime",process_time}, {"sceKernelGetProcessTimeCounter",process_time_counter},
    {"sceKernelGetProcessTimeCounterFrequency",process_time_frequency},
    {"sceKernelReadTsc",read_tsc}, {"sceKernelGetTscFrequency",tsc_frequency},
    {"sceKernelSleep",kernel_sleep}, {"sceKernelMlock",kernel_mlock}, {"msync",posix_msync},
    {"sceKernelUsleep",kernel_usleep}, {"usleep",posix_usleep}, {"sleep",posix_sleep},
    {"nanosleep",posix_nanosleep}, {"_nanosleep",posix_nanosleep}, {"sceKernelNanosleep",kernel_nanosleep},
    {"sceKernelSync",kernel_nothing}, {"sceKernelIsAddressSanitizerEnabled",kernel_nothing},
    {"sceKernelGetSanitizerMallocReplaceExternal",kernel_null},
    {"sceKernelGetSanitizerNewReplaceExternal",kernel_null},
    {"sceCoredumpSetUserDataType",kernel_nothing},
    {"sceKernelGettimezone",kernel_gettimezone}, {"gettimeofday",posix_gettimeofday},
    {"sceKernelGettimeofday",kernel_gettimeofday},
    {"time",posix_time},
    {"getpagesize",get_pagesize}, {"getpid",get_pid}, {"sched_yield",yield},
    {"_exit",hard_exit},
    {"sceKernelDebugRaiseException",raise_exception},
    {"sceKernelDebugRaiseExceptionOnReleaseMode",raise_exception},
    {"sceKernelPrintBacktraceWithModuleInfo",print_backtrace},
    {"signal",guest_signal}, {"sigprocmask",guest_sigprocmask}, {"_sigprocmask",guest_sigprocmask},
    {"sigfillset",guest_sigfillset}, {"sigemptyset",guest_sigemptyset},
    {"getrusage",guest_getrusage}, {"sysctl",guest_sysctl},
    {"scePthreadOnce",thread_once}, {"pthread_once",posix_once},
    {"scePthreadKeyCreate",key_create}, {"scePthreadKeyDelete",key_delete},
    {"scePthreadSetspecific",key_set}, {"scePthreadGetspecific",key_get},
    {"pthread_key_create",posix_key_create}, {"pthread_key_delete",posix_key_delete},
    {"pthread_setspecific",posix_key_set}, {"pthread_getspecific",key_get},
};
uintptr_t runtime_kernel_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }

static void sample_start(void);
static void sample_report(void);

/* bbport (frame stats): time guest threads spend blocked in the runtime (condition variables,
 * mutexes, semaphores, sleeps), by thread and guest call site: the first return address into the
 * game's code on the stack. Shows where the game waits for the GPU (labels, frame pacing). */
typedef struct { _Atomic uint64_t key; uint64_t site, site2, site3; int tid, kind; char name[16]; _Atomic uint64_t count, ns; } WaitSite;
static WaitSite wait_sites[512];
static _Thread_local int wait_tid;
void runtime_guest_call_sites(uint64_t out[3]);
/* The first three return addresses into the game's code on this thread's stack (guest offsets). */
static int guest_call_addresses(uint64_t out[], int max) {
    static _Thread_local uintptr_t stack_hi;
    if (!stack_hi) {
        void *base=NULL,*top=NULL;
        platform_thread_stack(&base,&top);
        stack_hi=(uintptr_t)top;
    }
    const uintptr_t *sp=(const uintptr_t *)__builtin_frame_address(0);
    int found=0;
    /* The game's own code is in the modules that were linked into the image; where the
     * image sits is not the same on every platform. */
    for (int i=0;i<400 && found<max && (uintptr_t)(sp+i+1)<=stack_hi;++i) {
        if (runtime_guest_address(sp[i],NULL,NULL)) out[found++]=sp[i];
    }
    return found;
}
static void guest_call_sites(uint64_t out[3]) {
    uint64_t addresses[8];
    const int found=guest_call_addresses(addresses,8);
    out[0]=out[1]=out[2]=0;
    for (int i=0;i<3 && i<found;++i) {
        uint64_t offset=0;
        runtime_guest_address(addresses[i],&offset,NULL);
        out[i]=offset;
    }
}
void runtime_wait_note(int kind, uint64_t ns) {
    static int enabled=-1;
    if (enabled<0) enabled=getenv("BB_FRAME_STATS")!=NULL;
    if (!enabled) return;
    if (!wait_tid) wait_tid=(int)platform_thread_id();
    uint64_t sites[3];
    guest_call_sites(sites);
    const uint64_t key=((sites[0]<<20)^(sites[1]*0x9E3779B1ull)^(sites[2]<<7)^((uint64_t)wait_tid<<4)^(uint64_t)kind)|1;
    for (uint64_t i=0,slot=(key*0x9E3779B97F4A7C15ull)>>55;i<512;++i) {
        WaitSite *w=&wait_sites[(slot+i)%512];
        uint64_t expected=0;
        if (atomic_load(&w->key)==key || atomic_compare_exchange_strong(&w->key,&expected,key)) {
            if (!w->tid) {
                w->site=sites[0]; w->site2=sites[1]; w->site3=sites[2]; w->kind=kind;
                /* The thread's name: the host's own (the guest's threads are named by the
                 * runtime, and the host keeps them). */
                platform_thread_name_get(w->name,sizeof(w->name));
                w->tid=wait_tid;
            }
            atomic_fetch_add(&w->count,1); atomic_fetch_add(&w->ns,ns);
            return;
        }
    }
}
void runtime_wait_report(double frames) {
    sample_start();
    sample_report();
    static const char *kinds[]={"cond","mutex","sema","sleep"};
    struct { WaitSite *w; uint64_t ns, count; } rows[512]; int n=0;
    for (int i=0;i<512;++i) {
        uint64_t ns=atomic_exchange(&wait_sites[i].ns,0), count=atomic_exchange(&wait_sites[i].count,0);
        if (count) { rows[n].w=&wait_sites[i]; rows[n].ns=ns; rows[n].count=count; ++n; }
    }
    if (!n || frames<=0) return;
    static FILE *dump; static int dump_checked;
    if (!dump_checked) { const char *path=getenv("BB_WAIT_LOG"); if (path && *path) dump=fopen(path,"w"); dump_checked=1; }
    if (dump) {
        for (int i=0;i<n;++i)
            fprintf(dump,"%s %d %s %#lx %#lx %#lx %.2f %.3f\n", rows[i].w->name, rows[i].w->tid, kinds[rows[i].w->kind&3],
                    (unsigned long)rows[i].w->site, (unsigned long)rows[i].w->site2, (unsigned long)rows[i].w->site3,
                    rows[i].count/frames, rows[i].ns/(frames*1e6));
        fprintf(dump,"--\n"); fflush(dump);
    }
    for (int i=1;i<n;++i) for (int j=i;j>0 && rows[j].ns>rows[j-1].ns;--j) { __typeof__(rows[0]) t=rows[j]; rows[j]=rows[j-1]; rows[j-1]=t; }
    printf("Guest waits per frame (thread kind +site):");
    for (int i=0;i<n && i<12;++i) if (rows[i].ns/rows[i].count<5000000)
        printf("%s %s %s +%#lx<+%#lx<+%#lx %.1fx %.2f ms", i ? ";" : "", rows[i].w->name, kinds[rows[i].w->kind&3],
               (unsigned long)rows[i].w->site, (unsigned long)rows[i].w->site2, (unsigned long)rows[i].w->site3,
               rows[i].count/frames, rows[i].ns/(frames*1e6));
    printf("\n");
}

/* bbport BB_SAMPLE_THREAD=<name> (with frame stats): where that thread runs, sampled every
 * 0.5 ms; reported with the wait profile. A thread that never blocks but waits for the GPU
 * shows up spinning in its polling loop. */
static _Atomic uint64_t sample_keys[1024], sample_counts[1024];
static uint64_t sample_rips[1024], sample_callers[1024];
static _Atomic uint64_t samples_total;
#ifdef _WIN32
/* The Windows build samples with the vectored exception handler instead of a signal: the
 * handler is installed by probe.c, which dispatches the sample to the runtime. */
void runtime_sample_hit(uint64_t rip, uint64_t stack_pointer);
#else
static void sample_handler(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    const ucontext_t *uc=(const ucontext_t *)context;
    uint64_t rip=(uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    const uint64_t text_lo=0x800000000ull, text_hi=0x800000000ull+0x50d96dcull;
    /* Host code: keyed with its guest caller, the first return address into the game on the stack. */
    uint64_t caller=0;
    if (rip<text_lo || rip>=text_hi) {
        uint64_t stack[256]={0};
        struct iovec local={stack,sizeof(stack)}, remote={(void *)uc->uc_mcontext.gregs[REG_RSP],sizeof(stack)};
        const ssize_t got=process_vm_readv(getpid(),&local,1,&remote,1,0);
        for (ssize_t i=0;i<got/8;++i) if (stack[i]>=text_lo && stack[i]<text_hi) { caller=stack[i]-text_lo; break; }
    }
    const uint64_t key=(rip*0x9E3779B97F4A7C15ull)^caller^1;
    atomic_fetch_add(&samples_total,1);
    for (uint64_t i=0,slot=(key*0x9E3779B97F4A7C15ull)>>54;i<1024;++i) {
        _Atomic uint64_t *k=&sample_keys[(slot+i)%1024];
        uint64_t expected=0;
        if (atomic_load(k)==key || atomic_compare_exchange_strong(k,&expected,key)) {
            sample_rips[(slot+i)%1024]=rip; sample_callers[(slot+i)%1024]=caller;
            atomic_fetch_add(&sample_counts[(slot+i)%1024],1);
            return;
        }
    }
}
#endif
/* The sample is recorded under the address and its guest caller (the first return address
 * into the game on the stack). */
void runtime_sample_record(uint64_t rip, uint64_t caller) {
    const uint64_t key=(rip*0x9E3779B97F4A7C15ull)^caller^1;
    atomic_fetch_add(&samples_total,1);
    for (uint64_t i=0,slot=(key*0x9E3779B97F4A7C15ull)>>54;i<1024;++i) {
        _Atomic uint64_t *k=&sample_keys[(slot+i)%1024];
        uint64_t expected=0;
        if (atomic_load(k)==key || atomic_compare_exchange_strong(k,&expected,key)) {
            sample_rips[(slot+i)%1024]=rip; sample_callers[(slot+i)%1024]=caller;
            atomic_fetch_add(&sample_counts[(slot+i)%1024],1);
            return;
        }
    }
}
static void *sampler_main(void *arg) {
    /* Every thread whose name starts with one of the comma-separated names, in turn. */
    const char *names=(const char *)arg;
    uint64_t tids[64]; char comm[64][32]; int count=0;
    while (!count) {
        int found=platform_threads(tids,comm,64);
        for (int i=0;i<found && count<64;++i) {
            for (const char *at=names; at && *at;) {
                const char *end=strchr(at,',');
                const size_t n=end ? (size_t)(end-at) : strlen(at);
                if (n && !strncmp(comm[i],at,n)) { tids[count++]=tids[i]; break; }
                at=end ? end+1 : NULL;
            }
        }
        if (!count) platform_sleep_ns(1000000000ull);
    }
    printf("Runtime: sampling %d threads named %s\n",count,names);
    for (unsigned i=0;;++i) {
#ifdef _WIN32
        /* The Windows build has no signals to raise: the sample is taken by the exception
         * handler, which the runtime asks for through the thread's context. */
        platform_sleep_ns(500000ull/count+1);
#else
        if (syscall(SYS_tgkill,getpid(),(pid_t)tids[i%count],SIGPROF) && count==1) break; /* one gone: the others go on */
        platform_sleep_ns(500000ull/count+1);
#endif
    }
    return NULL;
}
static void sample_start(void) {
    static int started;
    const char *name=getenv("BB_SAMPLE_THREAD");
    if (started || !name || !*name) return;
    started=1;
#ifndef _WIN32
    struct sigaction action={0};
    action.sa_sigaction=sample_handler;
    action.sa_flags=SA_SIGINFO|SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF,&action,NULL);
#endif
    pthread_t thread;
    pthread_create(&thread,NULL,sampler_main,(void *)name);
    pthread_detach(thread);
}
static void sample_report(void) {
    static int got_dumped;
    if (!got_dumped) {
        got_dumped=1;
        const char *got=getenv("BB_DUMP_GOT"); /* a guest GOT slot (offset): which host function it calls */
        if (got && *got) {
            const uint64_t target=*(const uint64_t *)(0x800000000ull+strtoull(got,NULL,0));
            char module[256]={0}; uint64_t offset=0;
            platform_symbol_of((void *)(uintptr_t)target,module,sizeof(module),&offset);
            printf("Runtime: GOT %s -> %#lx %s+%#lx\n",got,(unsigned long)target,module,(unsigned long)offset);
        }
    }
    const uint64_t total=atomic_exchange(&samples_total,0);
    if (!total) return;
    struct { uint64_t rip, caller, count; } rows[1024]; int n=0;
    for (int i=0;i<1024;++i) { uint64_t c=atomic_exchange(&sample_counts[i],0); if (c) { rows[n].rip=sample_rips[i]; rows[n].caller=sample_callers[i]; rows[n].count=c; ++n; } }
    for (int i=1;i<n;++i) for (int j=i;j>0 && rows[j].count>rows[j-1].count;--j) { __typeof__(rows[0]) t=rows[j]; rows[j]=rows[j-1]; rows[j-1]=t; }
    printf("Thread samples (%llu):",(unsigned long long)total);
    /* BB_SAMPLE_ALL=1: every sampled address, not the first 16. */
    static int all=-1;
    if (all<0) { const char *a=getenv("BB_SAMPLE_ALL"); all=a && a[0]=='1'; }
    for (int i=0;i<n && (all || i<16);++i) {
        const uint64_t text_lo=0x800000000ull, text_hi=0x800000000ull+0x50d96dcull;
        if (rows[i].rip>=text_lo && rows[i].rip<text_hi) {
            printf("%s +%#lx %.1f%%",i?";":"",(unsigned long)(rows[i].rip-text_lo),100.0*rows[i].count/total);
            continue;
        }
        char module[256]={0}; uint64_t offset=0;
        platform_symbol_of((void *)(uintptr_t)rows[i].rip,module,sizeof(module),&offset);
        printf("%s host:%s+%#lx (from +%#lx) %.1f%%",i?";":"",module,(unsigned long)offset,
               (unsigned long)rows[i].caller,100.0*rows[i].count/total);
    }
    printf("\n");
}
/* The first three return addresses into the game's code on the calling thread's stack. */
void runtime_guest_call_sites(uint64_t out[3]) { guest_call_sites(out); }
