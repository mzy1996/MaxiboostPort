/* BB_GUEST_COUNT=<offset>[,<offset>...]: how often the game's code reaches these instructions
 * (image offsets, as guest+0x... in the traces), reported every two seconds:
 *   Guest counts (2 s): +0x5a6223=118 +0x5a62ac=0
 * Each site gets an int3; the trap counts it, puts the original byte back, single-steps that one
 * instruction and arms the int3 again. A diagnostic: a thread that runs the site while another
 * steps over it is not counted, and every hit costs two signals. */
#define _GNU_SOURCE
#include "runtime.h"
#include "platform.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#define MAX_SITES 256
#define TRAP_FLAG 0x100ull
/* The registers the runtime reads, in the x86-64 numbering (platform_context_register). */
#define REG_RAX 0
#define REG_RCX 1
#define REG_RDX 2
#define REG_RBX 3
#define REG_RSP 4
#define REG_RBP 5
#define REG_RSI 6
#define REG_RDI 7
#define REG_R8 8
#define REG_R9 9
#define REG_R10 10
#define REG_R11 11
#define REG_R12 12
#define REG_R13 13
#define REG_R14 14
#define REG_R15 15

#define MAX_CALLERS 8
static struct {
    uint64_t offset; unsigned char *code; unsigned char original; _Atomic uint64_t hits;
    _Atomic uint64_t callers[MAX_CALLERS]; /* return addresses seen at the site (function entries) */
    _Atomic uint64_t rdis[MAX_CALLERS];    /* first distinct rdi values there (the object, at entries) */
} sites[MAX_SITES];
static unsigned char *image_base;
/* BB_GUEST_DELAY=<offset>:<microseconds>: the thread reaching that site sleeps first (ordering
 * experiments: does the game read GPU results too early?). The site must also be counted. */
static uint64_t delay_offset, delay_us;
/* BB_GUEST_REG=<offset>:<reg>: the distinct values a register holds at that site (reg: a REG_*
 * index name such as rdx, edx counts the low 32 bits). BB_GUEST_LIST=<offset>:<reg>: at that site
 * the register points at the effect draw list (32-byte entries: id <= 0xfff, count != 0); the
 * number of leading valid entries is counted and the first entries of the last hit are shown. */
static uint64_t reg_offset, list_offset; static int reg_index = -1, list_index = -1; static int reg_low;
static _Atomic uint64_t reg_values[16], reg_counts[16];
static _Atomic uint64_t list_hist[6]; static uint32_t list_last[24];
static int reg_by_name(const char *name, int *low) {
    static const struct { const char *n; int r; } names[] = {{"rax", REG_RAX}, {"rbx", REG_RBX}, {"rcx", REG_RCX},
        {"rdx", REG_RDX}, {"rsi", REG_RSI}, {"rdi", REG_RDI}, {"r8", REG_R8}, {"r9", REG_R9},
        {"r12", REG_R12}, {"r13", REG_R13}, {"r14", REG_R14}, {"r15", REG_R15}};
    *low = name[0] == 'e';
    char wide[8]; snprintf(wide, sizeof(wide), "%s%s", *low ? "r" : "", *low ? name + 1 : name);
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (!strcmp(names[i].n, wide)) return names[i].r;
    return -1;
}
/* BB_GUEST_TRANSITIONS=<offset>:<key reg>:<value reg>: at that site, each time the value register
 * holds something else than at the key's last hit, a line "Guest transition +<site>: <key>
 * <old> -> <new>" (the first 4096; 256 keys). With a state machine's dispatcher (key: the object,
 * value: its state function) it lists every state change in order. The site must be counted. */
static uint64_t trans_offset; static int trans_key = -1, trans_value = -1;
#define MAX_TRANS_KEYS 256
static _Atomic uint64_t trans_keys[MAX_TRANS_KEYS], trans_values[MAX_TRANS_KEYS];
static _Atomic int trans_printed;
static void note_transition(const PlatformContext *uc) {
    const uint64_t key = platform_context_register(uc,trans_key);
    const uint64_t value = platform_context_register(uc,trans_value);
    for (int i = 0; i < MAX_TRANS_KEYS; ++i) {
        const int k = (int)((key * 0x9e3779b97f4a7c15ull >> 56) + (uint64_t)i) % MAX_TRANS_KEYS;
        uint64_t seen = atomic_load(&trans_keys[k]);
        if (seen == 0 && !atomic_compare_exchange_strong(&trans_keys[k], &seen, key) && seen != key) continue;
        if (seen != 0 && seen != key) continue;
        const uint64_t old = atomic_exchange(&trans_values[k], value);
        if (old != value && atomic_fetch_add(&trans_printed, 1) < 4096) {
            const uint64_t base = (uint64_t)(uintptr_t)image_base;
            char line[160];
            snprintf(line, sizeof(line), "Guest transition +%#" PRIx64 ": %#" PRIx64 " +%#" PRIx64 " -> +%#" PRIx64 "\n",
                     trans_offset, key, old ? old - base : 0, value - base);
            platform_write_stdout(line, strlen(line));
        }
        return;
    }
}
static int site_count;
#ifndef _WIN32
static struct sigaction previous;
#endif
/* The site each thread is stepping over, by thread id: no thread-local storage in the handler
 * (the game's threads run with the guest's TLS register setup). */
#define MAX_STEPPERS 256
static struct { _Atomic int tid; int site; } steppers[MAX_STEPPERS];
static int *stepper_slot(int tid, int create) {
    for (int i = 0; i < MAX_STEPPERS; ++i) {
        const int k = (int)(((unsigned)tid * 2654435761u + (unsigned)i) % MAX_STEPPERS);
        int seen = atomic_load(&steppers[k].tid);
        if (seen == tid) return &steppers[k].site;
        if (seen == 0) {
            if (!create) return NULL;
            int expected = 0;
            if (atomic_compare_exchange_strong(&steppers[k].tid, &expected, tid)) return &steppers[k].site;
            if (expected == tid) return &steppers[k].site;
        }
    }
    return NULL;
}

/* The sites' pages stay writable (made so once at install): a hit is two signals, no mprotect. */
static void write_byte(unsigned char *code, unsigned char value) {
    *(volatile unsigned char *)code = value;
}

/* The trap that interrupted the thread: an int3 the runtime put in the game's code, or the
 * single step that followed it. Linux tells them apart by the signal's code; Windows has no
 * such code, so the thread's own state says which one it is. */
enum { TRAP_NONE=0, TRAP_BREAKPOINT, TRAP_STEP };

#ifndef _WIN32
static void on_trap_body(int sig, siginfo_t *info, void *context);
static void on_trap(int sig, siginfo_t *info, void *context) {
    const int saved_errno = errno;
    on_trap_body(sig, info, context);
    errno = saved_errno;
}
static void on_trap_body(int sig, siginfo_t *info, void *context) {
    ucontext_t *raw = (ucontext_t *)context;
    PlatformContext uc = { context };
    uint64_t rip = platform_context_rip(&uc);
    const int tid = (int)platform_thread_id();
    int *stepping = stepper_slot(tid, 0);
    /* The kernel tells a single step (TRAP_TRACE) from an int3 (SI_KERNEL): a thread can meet a
     * site's int3 re-armed by another thread while it steps over it. */
    const int kind = (info->si_code == TRAP_TRACE && stepping && *stepping >= 0) ? TRAP_STEP : TRAP_BREAKPOINT;
    if (kind == TRAP_STEP) { /* the instruction ran */
        write_byte(sites[*stepping].code, 0xcc);
        *stepping = -1;
        platform_context_clear_trap(&uc);
        return;
    }
    for (int i = 0; i < site_count; ++i) {
        if ((uint64_t)(uintptr_t)sites[i].code == rip - 1) {
            atomic_fetch_add_explicit(&sites[i].hits, 1, memory_order_relaxed);
            if (reg_index >= 0 && sites[i].offset == reg_offset) {
                uint64_t v = platform_context_register(&uc,reg_index);
                if (reg_low) v &= 0xffffffffull;
                for (int k = 0; k < 16; ++k) {
                    uint64_t seen = atomic_load(&reg_values[k]);
                    if (seen == v + 1) { atomic_fetch_add(&reg_counts[k], 1); break; }
                    if (seen == 0 && atomic_compare_exchange_strong(&reg_values[k], &seen, v + 1)) {
                        atomic_fetch_add(&reg_counts[k], 1); break;
                    }
                }
            }
            if (list_index >= 0 && sites[i].offset == list_offset) {
                const uint32_t *e = (const uint32_t *)(uintptr_t)platform_context_register(&uc,list_index);
                int n = 0;
                while (n < 0x4000 && e[n * 8] <= 0xfff && e[n * 8 + 1] != 0) ++n;
                atomic_fetch_add(&list_hist[n == 0 ? 0 : n == 1 ? 1 : n <= 4 ? 2 : n <= 16 ? 3 : n <= 64 ? 4 : 5], 1);
                memcpy(list_last, e, sizeof(list_last));
            }
            if (trans_key >= 0 && trans_value >= 0 && sites[i].offset == trans_offset) note_transition(&uc);
            if (delay_us && sites[i].offset == delay_offset) {
                platform_sleep_ns((uint64_t)delay_us * 1000ull);
            }
            /* BB_GUEST_BACKTRACE=1: at a site that is a function's first instruction, the chain of
             * callers (the game keeps frame pointers: rbp is still the caller's), the first 8 hits. */
            static int backtrace = -1;
            if (backtrace < 0) { const char *v = getenv("BB_GUEST_BACKTRACE"); backtrace = v && *v == '1'; }
            if (backtrace) {
                static _Atomic int printed;
                if (atomic_fetch_add(&printed, 1) < 8) {
                    char line[512]; int n = snprintf(line, sizeof(line), "Guest backtrace +%#" PRIx64 ":", sites[i].offset);
                    const uint64_t base = (uint64_t)(uintptr_t)image_base;
                    uint64_t r = *(const uint64_t *)(uintptr_t)platform_context_stack(&uc);
                    const uint64_t *frame = (const uint64_t *)(uintptr_t)platform_context_frame(&uc);
                    for (int d = 0; d < 14 && n < (int)sizeof(line) - 24; ++d) {
                        n += snprintf(line + n, sizeof(line) - (size_t)n, " +%#" PRIx64, r - base);
                        if (!frame || (uintptr_t)frame & 7 || !runtime_memory_is_mapped((uintptr_t)frame, 16)) break;
                        r = frame[1]; frame = (const uint64_t *)(uintptr_t)frame[0];
                        if (r < base || r >= base + 0x5000000) break;
                    }
                    snprintf(line + n, sizeof(line) - (size_t)n, "\n");
                    platform_write_stdout(line, strlen(line));
                }
            }
            const uint64_t ret = *(const uint64_t *)(uintptr_t)platform_context_stack(&uc);
            for (int k = 0; k < MAX_CALLERS; ++k) {
                uint64_t seen = atomic_load(&sites[i].callers[k]);
                if (seen == ret) break;
                if (seen == 0 && atomic_compare_exchange_strong(&sites[i].callers[k], &seen, ret)) break;
            }
            const uint64_t rdi = platform_context_register(&uc,REG_RDI);
            for (int k = 0; k < MAX_CALLERS; ++k) {
                uint64_t seen = atomic_load(&sites[i].rdis[k]);
                if (seen == rdi) break;
                if (seen == 0 && atomic_compare_exchange_strong(&sites[i].rdis[k], &seen, rdi)) break;
            }
            write_byte(sites[i].code, sites[i].original);
            /* The instruction is run again with the trap flag set, so the host stops right
             * after it and the int3 is put back. */
            platform_context_set_rip(&uc, rip - 1);
            platform_context_set_trap(&uc);
            if (!stepping) stepping = stepper_slot(tid, 1);
            if (stepping) *stepping = i;
            return;
        }
    }
#ifndef _WIN32
    if (previous.sa_flags & SA_SIGINFO) {
        if (previous.sa_sigaction) previous.sa_sigaction(sig, info, context);
    } else if (previous.sa_handler != SIG_IGN && previous.sa_handler != SIG_DFL && previous.sa_handler) {
        previous.sa_handler(sig);
    } else {
        signal(SIGTRAP, SIG_DFL);
        raise(SIGTRAP);
    }
#endif
}
#endif /* !_WIN32 */

static void *report(void *arg) {
    (void)arg;
    for (;;) {
        platform_sleep_ns(2000000000ull);
        printf("Guest counts (2 s):");
        for (int i = 0; i < site_count; ++i)
            printf(" +%#" PRIx64 "=%" PRIu64, sites[i].offset, atomic_exchange(&sites[i].hits, 0));
        printf("\n");
        /* Callers (return addresses in the image), for sites at function entries. */
        printf("Guest callers:");
        for (int i = 0; i < site_count; ++i) {
            printf(" +%#" PRIx64 "<", sites[i].offset);
            for (int k = 0; k < MAX_CALLERS; ++k) {
                const uint64_t ret = atomic_load(&sites[i].callers[k]);
                if (!ret) break;
                const uint64_t base = (uint64_t)(uintptr_t)image_base;
                if (ret >= base && ret < base + 0x5000000) printf("%s+%#" PRIx64, k ? "," : "", ret - base);
                else printf("%s?", k ? "," : "");
            }
        }
        printf("\n");
        if (reg_index >= 0) {
            printf("Guest reg at +%#" PRIx64 ":", reg_offset);
            for (int k = 0; k < 16; ++k) {
                const uint64_t v = atomic_load(&reg_values[k]);
                if (!v) break;
                printf(" %#" PRIx64 "x%" PRIu64, v - 1, atomic_exchange(&reg_counts[k], 0));
            }
            printf("\n");
        }
        if (list_index >= 0) {
            printf("Guest list at +%#" PRIx64 ": valid 0:%" PRIu64 " 1:%" PRIu64 " 2-4:%" PRIu64
                   " 5-16:%" PRIu64 " 17-64:%" PRIu64 " 65+:%" PRIu64 "; last", list_offset,
                   atomic_exchange(&list_hist[0], 0), atomic_exchange(&list_hist[1], 0),
                   atomic_exchange(&list_hist[2], 0), atomic_exchange(&list_hist[3], 0),
                   atomic_exchange(&list_hist[4], 0), atomic_exchange(&list_hist[5], 0));
            for (int k = 0; k < 24; ++k) printf("%s%08x", k % 8 ? " " : " | ", list_last[k]);
            printf("\n");
        }
        printf("Guest rdi:");
        for (int i = 0; i < site_count; ++i) {
            printf(" +%#" PRIx64 "<", sites[i].offset);
            for (int k = 0; k < MAX_CALLERS; ++k) {
                const uint64_t v = atomic_load(&sites[i].rdis[k]);
                if (!v) break;
                printf("%s%#" PRIx64, k ? "," : "", v);
            }
        }
        printf("\n");
        fflush(stdout);
    }
    return NULL;
}

#ifdef _WIN32
/* The Windows build has no signals: the exception handler (probe.c) hands the interrupt to
 * the runtime, which tells whether it was an int3 the runtime put in the game's code or the
 * single step that followed one. */
int runtime_count_trap(void *exception_context) {
    if (!site_count) return 0;
    PlatformContext context = { (EXCEPTION_POINTERS *)exception_context, NULL };
    /* The CONTEXT the exception carries. */
    context.context = ((EXCEPTION_POINTERS *)exception_context)->ContextRecord;
    const int tid = (int)platform_context_thread_id(&context);
    int *stepping = stepper_slot(tid, 0);
    const uint64_t rip = platform_context_rip(&context);
    /* A single step leaves the trap flag set in the flags: that is how it is told from an
     * int3 (Windows does not say which one it was). */
    const int single_step = (platform_context_flags(&context) & TRAP_FLAG) != 0;
    if (single_step && stepping && *stepping >= 0) {
        write_byte(sites[*stepping].code, 0xcc);
        *stepping = -1;
        platform_context_clear_trap(&context);
        return 1;
    }
    for (int i = 0; i < site_count; ++i) {
        if ((uint64_t)(uintptr_t)sites[i].code != rip - 1) continue;
        atomic_fetch_add_explicit(&sites[i].hits, 1, memory_order_relaxed);
        if (reg_index >= 0 && sites[i].offset == reg_offset) {
            uint64_t v = platform_context_register(&context, reg_index);
            if (reg_low) v &= 0xffffffffull;
            for (int k = 0; k < 16; ++k) {
                uint64_t seen = atomic_load(&reg_values[k]);
                if (seen == v + 1) { atomic_fetch_add(&reg_counts[k], 1); break; }
                if (seen == 0 && atomic_compare_exchange_strong(&reg_values[k], &seen, v + 1)) {
                    atomic_fetch_add(&reg_counts[k], 1); break;
                }
            }
        }
        if (trans_key >= 0 && trans_value >= 0 && sites[i].offset == trans_offset) note_transition(&context);
        if (delay_us && sites[i].offset == delay_offset) platform_sleep_ns((uint64_t)delay_us * 1000ull);
        write_byte(sites[i].code, sites[i].original);
        platform_context_set_rip(&context, rip - 1);
        platform_context_set_trap(&context);
        if (!stepping) stepping = stepper_slot(tid, 1);
        if (stepping) *stepping = i;
        return 1;
    }
    return 0;
}
#endif

void runtime_count_install(unsigned char *image) {
    const char *list = getenv("BB_GUEST_COUNT");
    if (!list || !*list) return;
    image_base = image;
    for (int which = 0; which < 2; ++which) {
        const char *spec = getenv(which ? "BB_GUEST_LIST" : "BB_GUEST_REG");
        if (!spec || !*spec) continue;
        char *end = NULL;
        const uint64_t offset = strtoull(spec, &end, 16);
        int low = 0;
        const int index = end && *end == ':' ? reg_by_name(end + 1, &low) : -1;
        if (which) { list_offset = offset; list_index = index; }
        else { reg_offset = offset; reg_index = index; reg_low = low; }
    }
    const char *transitions = getenv("BB_GUEST_TRANSITIONS");
    if (transitions && *transitions) {
        char *end = NULL; int low = 0;
        trans_offset = strtoull(transitions, &end, 16);
        if (end && *end == ':') {
            char key[8] = {0}; const char *colon = strchr(end + 1, ':');
            if (colon && colon - end - 1 < (long)sizeof(key)) {
                memcpy(key, end + 1, (size_t)(colon - end - 1));
                trans_key = reg_by_name(key, &low);
                trans_value = reg_by_name(colon + 1, &low);
            }
        }
    }
    const char *delay = getenv("BB_GUEST_DELAY");
    if (delay && *delay) {
        char *end = NULL;
        delay_offset = strtoull(delay, &end, 16);
        delay_us = end && *end == ':' ? strtoull(end + 1, NULL, 10) : 0;
    }
    for (const char *at = list; at && *at && site_count < MAX_SITES;) {
        char *end = NULL;
        const uint64_t offset = strtoull(at, &end, 16);
        sites[site_count].offset = offset;
        sites[site_count].code = image + offset;
        sites[site_count].original = image[offset];
        ++site_count;
        at = end && *end == ',' ? end + 1 : NULL;
    }
#ifndef _WIN32
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_trap;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTRAP, &action, &previous);
#endif
    for (int i = 0; i < site_count; ++i) {
        const uintptr_t page = (uintptr_t)sites[i].code & ~(uintptr_t)4095;
        platform_protect((void *)page, 8192, BB_PROT_READ | BB_PROT_WRITE | BB_PROT_EXEC);
        write_byte(sites[i].code, 0xcc);
    }
    pthread_t thread;
    if (!pthread_create(&thread, NULL, report, NULL)) pthread_detach(thread);
    printf("Guest counts: %d sites (BB_GUEST_COUNT)\n", site_count);
}
