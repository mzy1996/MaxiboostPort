/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <cpuid.h>
#include "runtime.h"
#include "asm.h"
#include "platform.h"
#include "gpu/bbgpu.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <wchar.h>
#include <pthread.h>
#include "runtime_tls.h"
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/prctl.h>
#endif

#ifdef _WIN32
/* The single-step counter's trap (the Windows build counts the int3s in the exception
 * handler, not in a signal handler). */
int runtime_count_trap(void *exception_context);
#endif
typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
/* The import slots of the image: where the game keeps the address of every symbol it
 * imports. The guest's own linker fills them; a slot that is still empty when the game
 * calls through it is a NULL call, and the name of the symbol says which one. */
static struct ImportSlot { uint64_t target; uint64_t name; uint64_t kind; } *import_slots;
static uint64_t import_slot_count;
static unsigned char *image;
static uint64_t image_bytes;   /* the image's length (the crash diagnostics search in it) */
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }

/* The game's code was compiled for the PS4's CPU (AMD Jaguar) and runs as it is: AVX, BMI1
 * (andn/bextr/blsr/tzcnt), MOVBE, LZCNT and POPCNT, thousands of each in eboot.bin. A CPU without
 * them stops at the first one with SIGILL (exit code 132, issue #26), and lzcnt/tzcnt even run as
 * bsr/bsf there, with other results. Said before the game starts; BB_SKIP_CPU_CHECK=1 skips it. */
static void check_cpu(void) {
    const char *skip = getenv("BB_SKIP_CPU_CHECK");
    if (skip && !strcmp(skip, "1")) return;
    unsigned a, b, c, d, leaf1_c = 0, leaf7_b = 0, ext1_c = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) leaf1_c = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) leaf7_b = b;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d)) ext1_c = c;
    int avx = (leaf1_c & bit_AVX) && (leaf1_c & bit_OSXSAVE);
    if (avx) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        avx = (lo & 6) == 6; /* the OS saves SSE and AVX state */
    }
    const struct { int present; const char *name; } features[] = {
        {avx, "AVX"}, {(leaf7_b & bit_BMI) != 0, "BMI1"}, {(leaf1_c & bit_MOVBE) != 0, "MOVBE"},
        {(ext1_c & bit_LZCNT) != 0, "LZCNT"}, {(leaf1_c & bit_POPCNT) != 0, "POPCNT"},
        {(leaf1_c & bit_SSE4_2) != 0, "SSE4.2"},
    };
    char missing[64] = "";
    for (unsigned i = 0; i < sizeof(features) / sizeof(features[0]); ++i) {
        if (features[i].present) continue;
        if (missing[0]) strcat(missing, ", ");
        strcat(missing, features[i].name);
    }
    if (!missing[0]) return;
    fprintf(stderr,
            "ERROR: this CPU lacks %s. Bloodborne's code was compiled for the PS4's CPU and uses "
            "these instructions directly: it needs an Intel Haswell (4th generation Core, 2013) or "
            "newer, or an AMD Ryzen. BB_SKIP_CPU_CHECK=1 starts anyway.\n", missing);
    exit(1);
}
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
/* bbport: the blocks the runtime allocates for the game (the import traps, the data traps, the
 * guest's main stack). A guest address that lands in one of them says the game and the runtime
 * share that memory, which is one way a host address ends up in a guest object. */
#define RUNTIME_ALLOCS 128
static struct { void *address; size_t size; } runtime_allocs[RUNTIME_ALLOCS];
static int runtime_alloc_count;
static void note_runtime_alloc(void *address, size_t size) {
    if (runtime_alloc_count < RUNTIME_ALLOCS) {
        runtime_allocs[runtime_alloc_count].address = address;
        runtime_allocs[runtime_alloc_count].size = size;
        ++runtime_alloc_count;
    }
}
static void *allocate(size_t size) {
#ifndef _WIN32
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) { note_runtime_alloc(low,size); return low; }
#endif
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    note_runtime_alloc(p,size);
    return p;
}
/* The relink bakes vtable / function / static-data pointers as absolute
 * 0x10000000+offset values (the PS4 eboot load base), and the game itself
 * computes module-internal pointers as 0x10000000+offset. Both are only valid
 * when the image is mapped at exactly that address, so pin it there instead of
 * letting the OS choose a random (often >40-bit) address -- otherwise every
 * 0x10000000-based pointer is wrong and CApplicationBase null-jumps (rip 0). */
static void *allocate_image(size_t size) {
    size = round_page(size);
#ifdef _WIN32
    void *p = VirtualAlloc((void *)0x10000000, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p) return p;
#else
    void *p = mmap((void *)0x10000000, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == (void *)0x10000000) return p;
    if (p != MAP_FAILED) munmap(p, size);
#endif
    fprintf(stderr, "bb-probe: WARNING: could not reserve guest image at 0x10000000; "
                    "0x10000000-based pointers will be invalid\n");
    return allocate(size);
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
#ifdef _WIN32
/* The game's entry and the stack it runs on (the host's main thread has a stack of one
 * megabyte, which the game overflows while it starts). */
typedef struct { void *entry; void *params; void *exit_fn; unsigned char *stack; } GuestMain;
static void *guest_main_start(void *argument) {
    const GuestMain *main_thread=(const GuestMain *)argument;
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(main_thread->entry))(main_thread->params,main_thread->exit_fn);
    fprintf(stderr,"Runtime: the game's entry returned\n");
    return NULL;
}
#endif
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack.
 * The hand-written code takes its arguments in %rdi, %rsi, %rdx and %rcx (the System V way,
 * which the guest's code follows), so the declaration says so: the host's compiler has to put
 * them there (its own convention puts them in %rcx, %rdx, %r8 and %r9). */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top) ABI;
__asm__(BB_ASM_TEXT BB_ASM_GLOBAL("enter_on_stack") "enter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
/* The jump that does not unwind (runtime.h): it puts the registers and the stack pointer back
 * and continues there. The C library's long jump walks the frames between the two points, and
 * the guest's frames have no unwind information for it.
 * The hand-written code takes its arguments in %rdi (the System V way), so the declarations in
 * runtime.h say so: the host's compiler has to put them there (its own convention puts them in
 * %rcx). */
__asm__(BB_ASM_TEXT BB_ASM_GLOBAL("runtime_setjmp") "runtime_setjmp:\n"
        " movq %rax,0(%rdi)\n"
        " movq %rbx,8(%rdi)\n"
        " movq %rcx,16(%rdi)\n"
        " movq %rdx,24(%rdi)\n"
        " movq %rsi,32(%rdi)\n"
        " movq %rdi,40(%rdi)\n"
        " movq %rbp,48(%rdi)\n"
        " leaq 8(%rsp),%rax\n"         /* the stack of the caller, once this returns */
        " movq %rax,56(%rdi)\n"
        " movq %r8,64(%rdi)\n"
        " movq %r9,72(%rdi)\n"
        " movq %r10,80(%rdi)\n"
        " movq %r11,88(%rdi)\n"
        " movq %r12,96(%rdi)\n"
        " movq %r13,104(%rdi)\n"
        " movq %r14,112(%rdi)\n"
        " movq %r15,120(%rdi)\n"
        " movq (%rsp),%rax\n"          /* where to continue: the return address */
        " movq %rax,128(%rdi)\n"
        " pushfq\n"
        " popq %rax\n"
        " movq %rax,136(%rdi)\n"
        " stmxcsr 144(%rdi)\n"
        " fnstcw 150(%rdi)\n"
        " xorl %eax,%eax\n"
        " ret\n");
__asm__(BB_ASM_TEXT BB_ASM_GLOBAL("runtime_longjmp") "runtime_longjmp:\n"
        " movq %rdi,%r11\n"            /* the buffer: kept until the last register is back */
        " movq 56(%r11),%rsp\n"        /* the stack to continue on */
        " movl %esi,%eax\n"            /* the value the jump returns */
        " movq %rax,-16(%rsp)\n"       /* kept there: %rax is put back below */
        " ldmxcsr 144(%r11)\n"
        " fldcw 150(%r11)\n"
        " pushq 136(%r11)\n"
        " popfq\n"
        " movq 128(%r11),%rax\n"       /* where to continue */
        " movq %rax,-8(%rsp)\n"        /* the return takes it from there */
        " movq 0(%r11),%rax\n"
        " movq 8(%r11),%rbx\n"
        " movq 16(%r11),%rcx\n"
        " movq 24(%r11),%rdx\n"
        " movq 32(%r11),%rsi\n"
        " movq 40(%r11),%rdi\n"
        " movq 48(%r11),%rbp\n"
        " movq 64(%r11),%r8\n"
        " movq 72(%r11),%r9\n"
        " movq 80(%r11),%r10\n"
        " movq 96(%r11),%r12\n"
        " movq 104(%r11),%r13\n"
        " movq 112(%r11),%r14\n"
        " movq 120(%r11),%r15\n"
        " movq 88(%r11),%r11\n"        /* the buffer's own register is the last one back */
        " movq -16(%rsp),%rax\n"       /* the value the jump returns */
        " ret\n");
#ifndef _WIN32
/* BB_FAULT_WAIT=1: a fatal fault or a GPU-library assertion leaves the process alive, stopped
 * in its handler, so a debugger can attach to the faulting thread with its registers and stack
 * still there: bash tools/debug_attach.sh <pid>. Any process of this user may attach
 * (PR_SET_PTRACER_ANY, set at startup). Exported (-rdynamic): libbbgpu.so calls it too. */
void bb_fault_wait(const char *what) {
    const char *wait = getenv("BB_FAULT_WAIT");
    if (!wait || strcmp(wait, "1")) return;
    char line[256];
    snprintf(line, sizeof(line), "bbport: %s; BB_FAULT_WAIT=1: the process stays alive for a debugger:"
             " bash tools/debug_attach.sh %d\n", what, (int)getpid());
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (fork() == 0) {
        snprintf(line, sizeof(line), "%s. Waiting for a debugger: bash tools/debug_attach.sh %d", what, (int)getppid());
        execlp("notify-send", "notify-send", "-u", "critical", "bbport stopped", line, (char *)NULL);
        _exit(0);
    }
    for (;;) pause();
}
#else
#define BB_FAULT_WAIT_UNAVAILABLE 1
#endif
/* The fault report, shared by both hosts: the guest's offset, the registers, the thread and the
 * host's call chain. The context is the host's own (a ucontext on Linux, a CONTEXT on
 * Windows); the address is the one the fault was about. */
void bb_fault_wait(const char *what);

/* Diagnostic: dump the game's object registry R1 (buckets 0x160b600/0x160b700,
 * sentinel 0x160b698, node {+0x0,+0x8}=BST links,+0x10=key(dw),+0x18=value}) so a fault
 * shows whether the id the walker wanted (0x1001 and friends) is actually present. The
 * registry is walked with the same hash/compare the game uses; platform_read_memory makes
 * every read bounds-safe. */
static void dump_registry_r1(uint32_t key) {
    char line[256];
    uint64_t v;
    uint32_t d;
    /* The guest RAM is identity-mapped at the image base: a guest virtual address X lives at
     * host address (image + X). The game stores node/sentinel addresses as guest VAs, so every
     * read of registry memory must be translated through `image`, otherwise platform_read_memory
     * probes the wrong (often unmapped) place and returns UNREADABLE. rsp is already a true host
     * pointer so the stack dump needs no translation; the registry's fixed addresses do. */
    uintptr_t base = (uintptr_t)image;
    if (platform_read_memory((void *)(base + 0x160b688), &v, 8) == 0)
        snprintf(line, sizeof(line), "  [R1] free_head=%#llx", (unsigned long long)v);
    else
        snprintf(line, sizeof(line), "  [R1] free_head=UNREADABLE");
    platform_write_stdout(line, strlen(line));
    if (platform_read_memory((void *)(base + 0x160b600), &d, 4) == 0) {
        snprintf(line, sizeof(line), " next_id=%#x", d);
        platform_write_stdout(line, strlen(line));
    }
    if (platform_read_memory((void *)(base + 0x160b694), &d, 4) == 0) {
        snprintf(line, sizeof(line), " used=%#x", d);
        platform_write_stdout(line, strlen(line));
    }
    if (platform_read_memory((void *)(base + 0x160b690), &d, 4) == 0) {
        snprintf(line, sizeof(line), " limit=%#x\n", d);
        platform_write_stdout(line, strlen(line));
    } else
        platform_write_stdout("\n", 1);
    /* The node pool and the free list. The pool is 0x19000 nodes of 0x20 bytes; a free
     * node keeps the next one in its first word. A free list that is empty (the head's
     * next is zero) means the register function hands out ids without ever putting a
     * node in the table: every lookup then finds nothing. The pool is in the guest's
     * own heap, not in the image, so its address is a host address already. */
    {
        uint64_t pool = 0, pool2 = 0, head = 0;
        platform_read_memory((void *)(base + 0x161d080), &pool, 8);
        platform_read_memory((void *)(base + 0x160b680), &pool2, 8);
        platform_read_memory((void *)(base + 0x160b688), &head, 8);
        snprintf(line, sizeof(line), "  [R1] pool=%#llx pool2=%#llx free_head=%#llx\n",
                 (unsigned long long)pool, (unsigned long long)pool2, (unsigned long long)head);
        platform_write_stdout(line, strlen(line));
        if (head) {
            uint64_t words[4] = {0, 0, 0, 0};
            if (platform_read_memory((void *)(uintptr_t)head, words, sizeof(words)) == 0) {
                snprintf(line, sizeof(line), "    head words: %#llx %#llx %#llx %#llx\n",
                         (unsigned long long)words[0], (unsigned long long)words[1],
                         (unsigned long long)words[2], (unsigned long long)words[3]);
                platform_write_stdout(line, strlen(line));
            } else
                platform_write_stdout("    head words: UNREADABLE\n", 24);
            uint64_t node = head;
            for (int n = 0; n < 4; ++n) {
                uint64_t next = 0;
                if (platform_read_memory((void *)(uintptr_t)node, &next, 8) != 0) break;
                snprintf(line, sizeof(line), "    node %#llx next=%#llx\n", (unsigned long long)node,
                         (unsigned long long)next);
                platform_write_stdout(line, strlen(line));
                if (!next) break;
                node = next;
            }
        }
    }
    /* Walk the bucket for `key` with the game's own hash. */
    uint64_t n = (uint64_t)key * 0x747ed91f; n >>= 0x2c;
    uint32_t eax = (uint32_t)(n * 0x2329);
    int32_t bkt = (int32_t)key - (int32_t)eax;
    uint64_t node = 0x160b698; /* sentinel (guest VA, matches the values the game stores) */
    if (platform_read_memory((void *)(base + 0x160b700 + (uint64_t)(uint32_t)bkt * 8), &node, 8) == 0) {
        int depth = 0, found = 0;
        uint64_t val = 0;
        while (node != 0x160b698 && depth < 4096) {
            uint32_t k = 0;
            if (platform_read_memory((void *)(base + node + 0x10), &k, 4) != 0) { node = 0x160b698; break; }
            if (k == key) { found = 1; platform_read_memory((void *)(base + node + 0x18), &val, 8); break; }
            uint64_t child_off = (k > key) ? node + 0x8 : node + 0x0;
            if (platform_read_memory((void *)(base + child_off), &node, 8) != 0) { node = 0x160b698; break; }
            ++depth;
        }
        if (found)
            snprintf(line, sizeof(line), "  [R1] key %#x: FOUND value=%#llx (bucket %d, depth %d)\n",
                     key, (unsigned long long)val, bkt, depth);
        else
            snprintf(line, sizeof(line), "  [R1] key %#x: ABSENT (bucket %d)\n", key, bkt);
        platform_write_stdout(line, strlen(line));
    }
}

/* Print the guest call stack (the faulting thread's Rsp, read with platform_read_memory, which
 * maps guest virtual addresses directly). The runtime executes the guest natively, so the
 * return address into the caller of a leaf helper (like the 0x66b280 node destructor) is the
 * word at [Rsp]; the frame chain below it is the call chain. This is how the indirect callers
 * that leave stale node pointers get found. */
static void dump_guest_stack(uint64_t rsp) {
    if (!rsp) return;
    char line[256];
    snprintf(line, sizeof(line), "  guest stack @ %#llx:\n", (unsigned long long)rsp);
    platform_write_stdout(line, strlen(line));
    uint64_t words[32];
    if (platform_read_memory((void *)(uintptr_t)rsp, words, sizeof(words)) != 0) {
        platform_write_stdout("    (unreadable)\n", 15);
        return;
    }
    for (int i = 0; i < 32; ++i) {
        uint64_t v = words[i];
        char tag[80] = "";
        uint64_t off = 0; const char *mod = NULL;
        if (runtime_guest_address(v, &off, &mod) && mod)
            snprintf(tag, sizeof(tag), "  <%s+%#llx>", mod, (unsigned long long)off);
        snprintf(line, sizeof(line), "    [%d] %#llx%s\n", i, (unsigned long long)v, tag);
        platform_write_stdout(line, strlen(line));
    }
}

/* The guest's frame chain: the game keeps its frame pointer, so every frame's saved rbp
 * points at the frame of its caller, and the return address of each frame is just above
 * it. This is the way to find the code that made a call the guest could not return from
 * (a call through a null pointer leaves no return address of its own on the stack). */
static void dump_guest_frames(uint64_t rbp) {    if (!rbp) return;
    char line[256];
    platform_write_stdout("  guest frames (rbp chain):\n", 24);
    for (int depth = 0; depth < 40; ++depth) {
        uint64_t frame[2];
        if (platform_read_memory((void *)(uintptr_t)rbp, frame, sizeof(frame)) != 0) {
            snprintf(line, sizeof(line), "    #%d rbp %#llx (unreadable)\n", depth,
                     (unsigned long long)rbp);
            platform_write_stdout(line, strlen(line));
            return;
        }
        const uint64_t saved = frame[0], ret = frame[1];
        uint64_t off = 0; const char *mod = NULL;
        char tag[80] = "";
        if (runtime_guest_address(ret, &off, &mod) && mod)
            snprintf(tag, sizeof(tag), "  <%s+%#llx>", mod, (unsigned long long)off);
        snprintf(line, sizeof(line), "    #%d rbp %#llx ret %#llx%s\n", depth,
                 (unsigned long long)rbp, (unsigned long long)ret, tag);
        platform_write_stdout(line, strlen(line));
        if (!saved || saved == rbp) return;
        rbp = saved;
    }
}

/* A wider scan of the guest stack than dump_guest_stack: the fatal call's return address may
 * sit *below* rsp (a `ret`-to-0 pops it, a `jmp [null]`/`call [null]` leaves its caller's
 * return address at [rsp-8]), and the 32 words above rsp miss it. Walk the whole stack mapping
 * (bounded to a window around rsp so we do not scan a multi-GiB direct region) and print every
 * qword that resolves to a guest module, tagging the one at rsp. */
static void dump_guest_stack_wide(uint64_t rsp) {
    if (!rsp) return;
    char line[256];
    snprintf(line, sizeof(line), "  guest stack wide scan (rsp %#llx):\n", (unsigned long long)rsp);
    platform_write_stdout(line, strlen(line));
    uint64_t mstart = 0, mend = 0;
    char mapping[512];
    if (platform_mapping_of((void *)(uintptr_t)rsp, mapping, sizeof(mapping)) && mapping[0])
        sscanf(mapping, "%llx-%llx", &mstart, &mend);
    if (!mstart) { mstart = rsp - 0x4000; mend = rsp + 0x200; }
    /* Bound the scan to a window around rsp; a thread stack is at most a few MiB but the
     * mapping may be a large region, so clamp to 0x20000 below / 0x8000 above. */
    uint64_t lo = mstart;
    if (rsp > lo + 0x20000) lo = rsp - 0x20000;
    uint64_t hi = mend;
    if (hi > rsp + 0x8000) hi = rsp + 0x8000;
    const size_t n = (size_t)(hi - lo) / 8;
    uint64_t *buf = (uint64_t *)malloc((n + 1) * sizeof(uint64_t));
    if (!buf) { platform_write_stdout("    (no memory)\n", 15); return; }
    if (platform_read_memory((void *)(uintptr_t)lo, buf, n * sizeof(uint64_t)) != 0) {
        free(buf);
        platform_write_stdout("    (unreadable)\n", 15);
        return;
    }
    unsigned shown = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t addr = lo + (uint64_t)i * 8;
        uint64_t v = buf[i];
        uint64_t off = 0; const char *mod = NULL;
        if (runtime_guest_address(v, &off, &mod) && mod) {
            char tag[80];
            snprintf(tag, sizeof(tag), "<%s+%#llx>", mod, (unsigned long long)off);
            snprintf(line, sizeof(line), "    %#llx: %#llx %s%s\n",
                     (unsigned long long)addr, (unsigned long long)v, tag,
                     (addr == rsp) ? "  <== rsp" : "");
            platform_write_stdout(line, strlen(line));
            if (++shown >= 512) break;
        }
    }
    free(buf);
    if (!shown) platform_write_stdout("    (no guest addresses in window)\n", 33);
}

/* bbport: what kind of host memory is this address in? The guest's memory is the host's, so an
 * address that the runtime does not track as a region of the guest's own is still a piece of the
 * host's memory, and what it is (the C library's heap, a private region, a module's data) says
 * who handed it out. */
static void describe_host_address(uintptr_t address, char *buffer, size_t size) {
    if (!buffer || !size) return;
    buffer[0]=0;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((const void *)address, &mbi, sizeof(mbi))) {
        static const char *const states[3] = {"commit","reserve","free"};
        const char *state = mbi.State<=2 ? states[mbi.State] : "?";
        char prot[8]={0};
        const DWORD p=mbi.Protect & 0xff;
        if (p==PAGE_READWRITE) snprintf(prot,sizeof(prot),"rw");
        else if (p==PAGE_READONLY) snprintf(prot,sizeof(prot),"r");
        else if (p==PAGE_EXECUTE_READWRITE) snprintf(prot,sizeof(prot),"rwx");
        else if (p==PAGE_EXECUTE_READ) snprintf(prot,sizeof(prot),"rx");
        else if (p==PAGE_NOACCESS) snprintf(prot,sizeof(prot),"---");
        else if (p==PAGE_WRITECOPY) snprintf(prot,sizeof(prot),"rwc");
        else snprintf(prot,sizeof(prot),"p%lu",(unsigned long)p);
        snprintf(buffer,size,"host %p-%p %s %s type=%lu",
                 mbi.BaseAddress,(char *)mbi.BaseAddress+mbi.RegionSize,state,prot,
                 (unsigned long)mbi.Type);
        return;
    }
    snprintf(buffer,size,"host region unknown");
#else
    snprintf(buffer,size,"host region unknown");
#endif
}
/* bbport: where does this value live? The guest's memory is the host's memory (the guest's
 * address space is the host's), so a value that leaked from the host into the guest's heap is
 * found by the same search: the guest's own copy (the corrupted one) and the host's own (the
 * table it was copied from) both show up, and the distance between them says what happened. */
static void dump_value_places(uint64_t value, const char *what) {
    uintptr_t hits[12];
    uint64_t scanned=0;
    const int found=platform_scan_memory(value, hits, 12, &scanned);
    char line[320];
    int n=snprintf(line,sizeof(line),"  %s = %#llx lives in %d place(s) (%llu MiB looked at):\n",
                   what,(unsigned long long)value,found,(unsigned long long)(scanned>>20));
    if (n>0 && n<(int)sizeof(line)) platform_write_stdout(line,(size_t)n);
    for (int i=0;i<found;++i) {
        char where[192]={0};
        uint64_t offset=0; const char *module=NULL;
        if (runtime_guest_address(hits[i],&offset,&module) && module)
            snprintf(where,sizeof(where),"in the image (%s+%#llx)",module,(unsigned long long)offset);
        else {
            char region[160]={0};
            runtime_memory_describe(hits[i],region,sizeof(region));
            snprintf(where,sizeof(where),"%s",region);
        }
        char host[160]={0};
        describe_host_address(hits[i],host,sizeof(host));
        n=snprintf(line,sizeof(line),"    %#llx  %s | %s\n",(unsigned long long)hits[i],where,host);
        if (n>0 && n<(int)sizeof(line)) platform_write_stdout(line,(size_t)n);
    }
}
/* bbport: the same search, but only in the guest's image: the loader's bookkeeping (a module's
 * base address, its path) is in there, so a value that should be a vtable but is a module's base
 * address is found next to the other module entries. */
static void dump_value_in_image(uint64_t value, const char *what) {
    if (!image || !image_bytes) return;
    char line[320];
    int found=0;
    const unsigned char *p=image;
    for (uint64_t i=0;i+8<=image_bytes;++i) {
        uint64_t v; memcpy(&v,p+i,sizeof(v));
        if (v!=value) continue;
        if (found<12) {
            const int n=snprintf(line,sizeof(line),"    image+%#llx\n",(unsigned long long)i);
            if (n>0 && n<(int)sizeof(line)) platform_write_stdout(line,(size_t)n);
        }
        ++found;
        i+=7;
    }
    const int n=snprintf(line,sizeof(line),"  %s = %#llx lives in %d place(s) of the image\n",
                         what,(unsigned long long)value,found);
    if (n>0 && n<(int)sizeof(line)) platform_write_stdout(line,(size_t)n);
}
/* A null-jump that lands at rip=0 from a `jmp [rax+off]` / `call [rax+off]` vtable thunk: the
 * target read was zero. The first argument (rdi) is `this`; read it to tell a null vtable
 * pointer (object not constructed) from a null vtable slot (vtable present but empty). */
static void dump_null_jump_target(uint64_t rdi) {
    if (!rdi) return;
    char line[256];
    /* The 32 bytes *before* this (the wrapper's tail, if rdi is inside a std::function /
     * job object) often tells the dispatch flavour: a raw CApplicationBase* has nothing
     * interesting before it, a wrapper has the real object / executor table there. */
    uint64_t pre[4];
    if (platform_read_memory((void *)(uintptr_t)(rdi - 0x20), pre, sizeof(pre)) == 0) {
        snprintf(line, sizeof(line),
                 "  before 'this' (-0x20..-0x01): %#llx %#llx %#llx %#llx\n",
                 (unsigned long long)pre[0], (unsigned long long)pre[1],
                 (unsigned long long)pre[2], (unsigned long long)pre[3]);
        platform_write_stdout(line, strlen(line));
    }
    uint64_t obj[4];
    if (platform_read_memory((void *)(uintptr_t)rdi, obj, sizeof(obj)) != 0) {
        snprintf(line, sizeof(line), "  (cannot read object at %#llx)\n", (unsigned long long)rdi);
        platform_write_stdout(line, strlen(line));
        return;
    }
    uint64_t off = 0; const char *mod = NULL;
    char tag[80] = "";
    if (runtime_guest_address(obj[0], &off, &mod) && mod)
        snprintf(tag, sizeof(tag), "  <%s+%#llx>", mod, (unsigned long long)off);
    snprintf(line, sizeof(line),
             "  null-jump 'this' @ %#llx: vtable=%#llx%s  [+8]=%#llx  [+0x10]=%#llx  [+0x18]=%#llx\n",
             (unsigned long long)rdi, (unsigned long long)obj[0], tag,
             (unsigned long long)obj[1], (unsigned long long)obj[2], (unsigned long long)obj[3]);
    platform_write_stdout(line, strlen(line));
    const uint64_t vtable = obj[0];
    if (vtable) {
        uint64_t slot = 0;
        if (platform_read_memory((void *)(uintptr_t)(vtable + 0x10), &slot, 8) == 0)
            snprintf(line, sizeof(line), "  vtable+0x10 (the jumped slot) = %#llx%s\n",
                     (unsigned long long)slot, slot ? "" : "  <== ZERO (this is the null jump)");
        else
            snprintf(line, sizeof(line), "  vtable+0x10 unreadable\n");
        platform_write_stdout(line, strlen(line));
    } else {
        snprintf(line, sizeof(line),
                 "  vtable pointer is ZERO -> object at %#llx was never constructed (vtable not set)\n",
                 (unsigned long long)rdi);
        platform_write_stdout(line, strlen(line));
    }
    /* Which region of the guest's own address space is the object in? */
    { char region[192]={0};
      if (runtime_memory_describe((uintptr_t)rdi,region,sizeof(region)) && region[0]) {
          snprintf(line,sizeof(line),"  'this' is in: %s\n",region);
          platform_write_stdout(line,strlen(line));
      } }
    /* ... and which host memory is it? The runtime does not track it as a region of the guest's
     * own, so it says what the host has there: the C library's heap, a private region, a
     * module's data. The nearest region of the guest's own on both sides is said too. */
    { char host[192]={0};
      describe_host_address((uintptr_t)rdi,host,sizeof(host));
      snprintf(line,sizeof(line),"  'this' is in %s\n",host);
      platform_write_stdout(line,strlen(line)); }
    /* Does one of the runtime's own blocks cover the object? (the import traps, the data traps,
     * the guest's main stack: the memory the game and the runtime share.) */
    { int inside=0;
      for (int i=0;i<runtime_alloc_count;++i) {
          const uintptr_t a=(uintptr_t)runtime_allocs[i].address;
          if (rdi>=a && rdi<a+runtime_allocs[i].size) { inside=1; break; }
      }
      snprintf(line,sizeof(line),"  the runtime's own blocks: %d; the object is %sinside one of them\n",
               runtime_alloc_count, inside ? "" : "NOT ");
      platform_write_stdout(line,strlen(line));
      for (int i=0;i<runtime_alloc_count && i<8;++i) {
          const int n=snprintf(line,sizeof(line),"    %p +%zu\n",
                               runtime_allocs[i].address,runtime_allocs[i].size);
          if (n>0 && n<(int)sizeof(line)) platform_write_stdout(line,(size_t)n);
      } }
    { uintptr_t starts[64], ends[64], phys[64]; int kinds[64];
      const int n=runtime_memory_regions(starts,ends,phys,kinds,64);
      uintptr_t below=0, above=0; int have_below=0, have_above=0;
      for (int i=0;i<n;++i) {
          if (ends[i]<=rdi) { below=starts[i]; have_below=1; }
          if (starts[i]>rdi && (!have_above || starts[i]<above)) { above=starts[i]; have_above=1; }
      }
      snprintf(line,sizeof(line),"  the guest's own regions: %d; below %#llx, above %#llx\n",
               n,(unsigned long long)below,(unsigned long long)above);
      platform_write_stdout(line,strlen(line)); }
#ifdef _WIN32
    /* The host region the object is in: what else lives there? The loader's module table is a
     * run of entries that begin with a module's base address, so a region full of the image's
     * base address says the object shares its block with that table. */
    { MEMORY_BASIC_INFORMATION mbi;
      if (VirtualQuery((const void *)rdi, &mbi, sizeof(mbi)) && mbi.RegionSize) {
          const uintptr_t base=(uintptr_t)mbi.BaseAddress, size=mbi.RegionSize;
          const unsigned char *p=(const unsigned char *)base;
          int hits=0; uintptr_t first=0;
          for (uintptr_t i=0;i+8<=size;++i) {
              uint64_t v; memcpy(&v,p+i,sizeof(v));
              if (v!=0x10000000ull) continue;
              if (!hits) first=base+i;
              ++hits;
          }
          snprintf(line,sizeof(line),"  that host region (%p+%zu): %d image-base word(s), first at %p\n",
                   (void *)base,size,hits,(void *)first);
          platform_write_stdout(line,strlen(line));
          /* The words around the object: its own neighbours say what structure it is. */
          const uintptr_t from=(rdi>base+0x40) ? rdi-0x40 : base;
          const uintptr_t to=(rdi+0x80<base+size) ? rdi+0x80 : base+size;
          uint64_t words[32]; const size_t count=(size_t)((to-from)/8);
          if (count && platform_read_memory((void *)from, words, count*8)==0) {
              for (size_t i=0;i<count;++i) {
                  const int n=snprintf(line,sizeof(line),"    %p: %#llx%s\n",
                                       (void *)(from+i*8),(unsigned long long)words[i],
                                       (from+i*8==rdi) ? "   <== 'this'" : "");
                  if (n>0 && n<(int)sizeof(line)) platform_write_stdout(line,(size_t)n);
              }
          }
      } }
#endif
    /* A wider window: the object is usually a part of a bigger structure, and what is around
     * it says who owns it. */
    { uint64_t more[4];
      if (platform_read_memory((void *)(uintptr_t)(rdi+0x20), more, sizeof(more)) == 0) {
          snprintf(line,sizeof(line),"  after 'this' (+0x20..+0x3f): %#llx %#llx %#llx %#llx\n",
                   (unsigned long long)more[0],(unsigned long long)more[1],
                   (unsigned long long)more[2],(unsigned long long)more[3]);
          platform_write_stdout(line,strlen(line));
      } }
    /* A value that is a host address says the host's memory leaked into the guest's heap; a
     * value that is the image's base says the loader's bookkeeping is here. Both are looked for
     * everywhere, so the place they come from is named. */
    for (int i=0;i<4;++i) {
        if (obj[i]>=0x7ff000000000ull) {
            char what[64];
            snprintf(what,sizeof(what),"the host address at +%#x",i*8);
            dump_value_places(obj[i],what);
        }
    }
    if (obj[0] && obj[0]>=0x10000000ull && obj[0]<0x30000000ull)
        dump_value_in_image(obj[0],"the image base at +0");
}

static void report_fault(void *context, void *fault_address, int sig) {
    /* The context is the exception's pointers on Windows and the ucontext on Linux:
     * the shape of the view of it is not the same. */
#ifdef _WIN32
    PlatformContext uc = { (EXCEPTION_POINTERS *)context, ((EXCEPTION_POINTERS *)context)->ContextRecord };
#else
    PlatformContext uc = { context };
#endif
    const uintptr_t rip = (uintptr_t)platform_context_rip(&uc);
    char line[512];
    char module[256] = {0};
    uint64_t module_offset = 0;
    const int in_module = platform_symbol_of((void *)rip, module, sizeof(module), &module_offset);
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), fault_address);
    else if (in_module)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx, address %p\n", sig, module,
                 (unsigned long)module_offset, fault_address);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, fault_address);
    platform_write_stdout(line, strlen(line));
    /* The thread, its stack pointer and the stack's host mapping: a stack overflow shows as a
     * fault just below the mapping that holds rsp. */
    {
        char thread[32] = "?";
        platform_thread_name_get(thread, sizeof(thread));
        const uintptr_t rsp = (uintptr_t)platform_context_stack(&uc);
        snprintf(line, sizeof(line), "  thread %s, rsp %p, rbp %p; stack mapping: ", thread, (void *)rsp,
                 (void *)(uintptr_t)platform_context_frame(&uc));
        platform_write_stdout(line, strlen(line));
        char mapping[512];
        if (platform_mapping_of((void *)rsp, mapping, sizeof(mapping)) && mapping[0]) {
            platform_write_stdout(mapping, strlen(mapping));
        } else {
            platform_write_stdout("none\n", 5);
        }
    }
    /* Where the faulting address is: a pointer into memory the guest has released shows
     * as an unmapped address, a page the GPU write-protected shows as read-only. The two
     * need different fixes, so the report says which one it is. */
    if (fault_address) {
        char mapping[512];
        if (platform_mapping_of(fault_address, mapping, sizeof(mapping)) && mapping[0]) {
            snprintf(line, sizeof(line), "  fault address %p mapping: ", fault_address);
            platform_write_stdout(line, strlen(line));
            platform_write_stdout(mapping, strlen(mapping));
        } else {
            snprintf(line, sizeof(line), "  fault address %p: no mapping\n", fault_address);
            platform_write_stdout(line, strlen(line));
        }
        /* The guest's own view of the address: its region and the protection the guest
         * asked for. A page the GPU left protected differs from a page the guest made
         * read-only, and only the first one is a fault of ours. */
        {
            char region[256];
            if (runtime_memory_describe((uintptr_t)fault_address, region, sizeof(region)) && region[0]) {
                snprintf(line, sizeof(line), "  fault address %p %s\n", fault_address, region);
                platform_write_stdout(line, strlen(line));
            }
        }
    }
    snprintf(line, sizeof(line),
             "  rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r12 %llx r13 %llx r14 %llx r15 %llx\n",
             (unsigned long long)platform_context_register(&uc, 0), (unsigned long long)platform_context_register(&uc, 3),
             (unsigned long long)platform_context_register(&uc, 1), (unsigned long long)platform_context_register(&uc, 2),
             (unsigned long long)platform_context_register(&uc, 6), (unsigned long long)platform_context_register(&uc, 7),
             (unsigned long long)platform_context_register(&uc, 12), (unsigned long long)platform_context_register(&uc, 13),
             (unsigned long long)platform_context_register(&uc, 14), (unsigned long long)platform_context_register(&uc, 15));
    platform_write_stdout(line, strlen(line));
    /* A jump to address zero: the first argument (rdi) is `this` for a vtable thunk, so read
     * the object to say whether its vtable pointer is null or its vtable slot is. */
    if (rip == 0)
        dump_null_jump_target((uintptr_t)platform_context_register(&uc, 7));
    /* The guest call chain: how a leaf helper like the 0x66b280 node destructor got here,
     * and which indirect caller left the stale node pointer the destructor trips on. */
    {
        const uintptr_t grsp = (uintptr_t)platform_context_stack(&uc);
        const uintptr_t grbp = (uintptr_t)platform_context_frame(&uc);
        /* The crashing thread's guest start PC: a null-pointer jump lands inside this
         * thread's code, and the entry names the subsystem (CApplicationBase, &c.). */
        {
            const uintptr_t entry = runtime_thread_current_entry();
            if (entry) {
                uint64_t off = 0; const char *mod = NULL;
                char tag[80] = "";
                if (runtime_guest_address(entry, &off, &mod) && mod)
                    snprintf(tag, sizeof(tag), "  <%s+%#llx>", mod, (unsigned long long)off);
                snprintf(line, sizeof(line), "  thread guest entry PC = %#llx%s\n",
                         (unsigned long long)entry, tag);
                platform_write_stdout(line, strlen(line));
            }
        }
        dump_guest_stack((uint64_t)grsp);
        dump_guest_frames((uint64_t)grbp);
        dump_guest_stack_wide((uint64_t)grsp);
    }
    /* Outside the image and any shared object (generated code, a freed mapping): the mapping
     * the address is in, and the thread. */
    if (rip - (uintptr_t)image >= 0x10000000 && !in_module) {
        char thread[32] = "?";
        platform_thread_name_get(thread, sizeof(thread));
        snprintf(line, sizeof(line), "  thread %s; mapping of RIP: ", thread);
        platform_write_stdout(line, strlen(line));
        char mapping[512];
        if (platform_mapping_of((void *)rip, mapping, sizeof(mapping)) && mapping[0]) {
            platform_write_stdout(mapping, strlen(mapping));
        } else {
            platform_write_stdout("none\n", 5);
        }
    }
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = platform_backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        char frame_module[256] = {0};
        uint64_t frame_offset = 0;
        if (platform_symbol_of(frames[i], frame_module, sizeof(frame_module), &frame_offset))
            snprintf(line, sizeof(line), "  #%d %s+0x%lx\n", i, frame_module, (unsigned long)frame_offset);
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        platform_write_stdout(line, strlen(line));
    }
    /* The registry's node pool: where it is and what it holds. The pool is the game's
     * own memory, and the free list that threads it is written into the pool itself,
     * so a pool whose memory was recycled leaves a chain of zeros. */
    {
        uint64_t pool = 0, head = 0, link = 0;
        platform_read_memory((void *)((uintptr_t)image + 0x161d080), &pool, 8);
        platform_read_memory((void *)((uintptr_t)image + 0x160b688), &head, 8);
        if (pool) platform_read_memory((void *)(uintptr_t)pool, &link, 8);
        char region[192];
        region[0] = 0;
        if (pool) runtime_memory_describe((uintptr_t)pool, region, sizeof(region));
        snprintf(line, sizeof(line), "  R1 pool=%#llx head=%#llx chain[0]=%#llx %s\n",
                 (unsigned long long)pool, (unsigned long long)head,
                 (unsigned long long)link, region);
        platform_write_stdout(line, strlen(line));
    }
    /* The import slots that are still empty: the game calls through one of them and
     * the address is zero, so the call goes to address zero. Which symbol it is says
     * what the linker failed to resolve. */
    {
        unsigned shown = 0, empty = 0;
        for (uint64_t i = 0; i < import_slot_count; ++i) {
            uint64_t at = 0;
            if (platform_read_memory(image + import_slots[i].target, &at, 8) != 0) continue;
            if (at) continue;
            ++empty;
            if (shown++ >= 24) continue;
            const uint64_t n = import_slots[i].name;
            const char *symbol = n < import_count ? runtime_symbol(names[n]) : NULL;
            snprintf(line, sizeof(line), "  import slot %s is empty (%s, kind %llu)\n",
                     n < import_count ? names[n] : "?", symbol ? symbol : "(no name)",
                     (unsigned long long)import_slots[i].kind);
            platform_write_stdout(line, strlen(line));
        }
        if (empty) {
            snprintf(line, sizeof(line), "  %u import slots are empty\n", empty);
            platform_write_stdout(line, strlen(line));
        }
    }
    snprintf(line, sizeof(line), "fault (signal %d)", sig);
    bb_fault_wait(line);
    /* The game's own linker: it looks the symbol up by its address, and the address is
     * in the table of the module it is linking. The table's entries say which address
     * it wanted, so the one that is not in the image is said out loud. */
    if (rip - (uintptr_t)image < 0x10000000) {
        const uintptr_t module=(uintptr_t)platform_context_register(&uc, 14);
        unsigned char entries[8*32];
        if (module && platform_read_memory((void *)(module+0x30),entries,sizeof(entries))==0) {
            for (unsigned i=0;i<8;++i) {
                uint32_t at;
                memcpy(&at,entries+i*32+0x2c,4);
                if (!at) continue;
                const int inside=runtime_guest_address((uintptr_t)at,NULL,NULL);
                snprintf(line,sizeof(line),"  the linker wanted the symbol at %#x (%s)%s\n",
                         at, inside ? "in the image" : "not in the image",
                         inside ? "" : " - the kernel's answer, which the image does not know");
                platform_write_stdout(line,strlen(line));
                dump_registry_r1(at);
            }
        }
    }
    _exit(128 + sig);
}
/* BB_CPU_WATCH / BB_POOL_WATCH (diagnostics): the copies the guest's own C library makes are
 * HLE'd by the runtime, so a `memset` of a page of the game's heap never shows in the game's
 * code: it is the runtime that does it, on the game's behalf. The runtime names the copy and
 * hands the place that asked for it here; the place is a guest address of the game's code
 * (the addresses of the guest are the host's own), so it is said the same way a fault is. */
void bb_cpu_copy_note(const char *what, const void *destination, uint64_t size, const void *caller) {
    const uintptr_t rip=(uintptr_t)caller;
    char line[256];
    int n=snprintf(line,sizeof(line),"CPU watch: %s of %llu bytes to %#llx from %#llx",
                   what?what:"copy",(unsigned long long)size,(unsigned long long)(uintptr_t)destination,
                   (unsigned long long)rip);
    if (n<=0) return;
    if (n>=(int)sizeof(line)) n=(int)sizeof(line)-1;
    if (image && rip>=(uintptr_t)image && rip<(uintptr_t)image+0x20000000) {
        const int m=snprintf(line+n,sizeof(line)-(size_t)n," (guest+%#llx)",
                             (unsigned long long)(rip-(uintptr_t)image));
        if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1;
    }
#ifdef _WIN32
    { HMODULE where=NULL;
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&where)) {
          char path[MAX_PATH];
          const DWORD length=GetModuleFileNameA(where,path,sizeof(path));
          if (length && length<sizeof(path)) {
              const int m=snprintf(line+n,sizeof(line)-(size_t)n," (in %s+%#llx)",path,
                                   (unsigned long long)(rip-(uintptr_t)where));
              if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1;
          }
      } }
    { const int m=snprintf(line+n,sizeof(line)-(size_t)n," thread %lu\n",
                           (unsigned long)GetCurrentThreadId());
      if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1; }
#endif
    if (n>0) platform_write_stdout(line,(size_t)n);
}
#ifdef _WIN32
/* BB_POOL_WATCH=1 (diagnostics): the registry's node pool is watched for writes, so the place
 * that erases it is named. The pool is the game's own operator-new block (its base is held by the
 * image at 0x161d080); the free list of the registry is threaded through the nodes in it, and a
 * node whose memory is zeroed breaks the list: the register function then hands out ids without
 * ever putting a node in the table, every lookup of one of them finds nothing, and the game calls
 * a symbol that resolved to NULL (rip 0).
 *
 * The earlier watch - a guard page on the pool's first page - never fired, and the pool was all
 * zeros by the time it was armed: the pool's page is read constantly (every lookup reads a node),
 * and Windows takes a guard away when it fires whatever the access was, so the reads consumed the
 * guard before any write arrived. Nothing is watched through page protection now:
 *   - the four words of the pool's first node (the pool's base is the node's first word) are
 *     watched by the processor's own breakpoints (Dr0..Dr3: write, eight bytes each), carried by
 *     every thread of the process: a write through any name of those bytes is caught, and the
 *     place that wrote it (guest offset, module, thread) is said;
 *   - the runtime names the copies its HLE'd C library makes into the pool's page (a `memset` by
 *     the game's own libc does not appear in the game's code at all: the runtime does it for the
 *     game), handing over the place of the game that asked for it;
 *   - the node's words are read every five milliseconds and every change is said, with the number
 *     of writes seen, so the moment the chain is erased is dated;
 *   - the runtime's mapping operations touching the pool's bytes are named (the WATCH lines of
 *     runtime_memory.c), and the other names those bytes are mapped at are listed: the breakpoints
 *     do not care about pages, so a write through another name is caught all the same.
 * A pool that goes to zero with no write, no copy and no mapping operation seen was not written at
 * all: the pages behind it were replaced or discarded by the host. */
static volatile uintptr_t pool_watch_base; /* the pool: its base is the first node */
static volatile uintptr_t pool_watch_page;
static volatile LONG pool_watch_lock;
static _Atomic long pool_watch_writes;
static _Atomic int pool_watch_lines;
static _Atomic int pool_watch_threads;
#define POOL_WATCH_PLACES 24
typedef struct { uintptr_t rip, at; long count; } PoolWatchPlace;
static PoolWatchPlace pool_watch_places[POOL_WATCH_PLACES];
static int pool_watch_place_count;
/* The pool's base through the host's own view of the same bytes (the runtime's second name of the
 * physical memory), and the protection of both names, asked about in every round: a write into a
 * page that something else has silently made writable again (the GPU library protects the pages it
 * writes, and both libraries protect their own code) does not fault at all, so the read-only trap
 * never sees it and the watch goes on believing the trap is armed. See pool_watch_prot_poll. */
static uintptr_t pool_watch_host_base;
/* The physical address of the pool's page (the section offset the guest's name of it begins at): a
 * third name of the same bytes carries the pool's page at that offset inside itself when it begins
 * at the section's beginning, and at its own beginning when it begins at the page. */
static uintptr_t pool_watch_phys;
typedef struct { uintptr_t address; unsigned mask; int seen; } PoolWatchProt;
static PoolWatchProt pool_watch_prot_guest, pool_watch_prot_host;
static uint64_t pool_watch_started_ms;

/* The pool's page is also watched with the processor's own page protection, and this is what
 * catches a writer the breakpoints cannot: a write from a thread that was never adopted, or a
 * write through the host's second view of the same bytes. The page is made read-only, so a write
 * is an access violation (a read is not - this is why the guard page failed: the guard is taken
 * away by the first access, read or write, and the pool is read constantly). The violation names
 * the writer; the runtime's own recovery then gives the page back, and the watch arms it again. */
static volatile LONG pool_trap_lock;
static _Atomic int pool_trap_on;
static _Atomic long pool_trap_arms;
static _Atomic long pool_trap_hits;
#define POOL_TRAP_SITES 24
typedef struct { uintptr_t rip, at; long count; } PoolTrapSite;
static PoolTrapSite pool_trap_sites[POOL_TRAP_SITES];
static int pool_trap_site_count;

/* The page is made read-only; the guest's own protection of it allows the write, so the runtime's
 * recovery puts the protection back after the writer has been named. */
static int pool_trap_arm(void) {
    if (!pool_watch_page) return 0;
    DWORD old=0;
    if (!VirtualProtect((void *)(uintptr_t)pool_watch_page,4096,PAGE_READONLY,&old)) return 0;
    atomic_store(&pool_trap_on,1);
    atomic_fetch_add(&pool_trap_arms,1);
    return 1;
}

/* The same page protection is put on the host's own view of those bytes (the runtime's second
 * name for the same physical memory), because a store through that name is a store the guest's
 * page protection and the breakpoints do not see at all: the guest's name of the bytes is a
 * different view, so the fault never happens there, and the linear address the breakpoints watch
 * is never stored to. Everything the GPU side keeps in host memory is written through this view. */
static volatile uintptr_t pool_trap_backing_page;
static _Atomic int pool_trap_backing_on;
static _Atomic long pool_trap_backing_hits;
static PoolTrapSite pool_trap_backing_sites[POOL_TRAP_SITES];
static int pool_trap_backing_site_count;

/* When a write faults on a pool page we let exactly one store through: the page is made writable
 * and the faulting thread's Trap Flag is set, so that after the store the processor takes a
 * single-step fault (#DB), in which we re-protect the page read-only and re-arm the trap. This
 * closes the disarm window the old code left - it made the page permanently writable until the
 * pool-watch thread swept the address space (~100 ms later), during which the zeroing slipped by
 * with no write named. pool_trap_on / pool_trap_backing_on stay 1 across the one-instruction
 * window, so the watch thread never races to re-protect underneath us. */
static volatile uintptr_t pool_trap_reprotect_guest;
static volatile uintptr_t pool_trap_reprotect_backing;
/* Safety: if a pool page faults an unreasonable number of times (a tight write loop rather than
 * the occasional allocator store), the single-step re-protect would cost too much and could stall
 * the game before the zeroing is ever reached. Past the threshold we fall back to the old
 * disarm-and-let-the-watch-thread-re-arm behaviour for the rest of the run. */
static _Atomic long pool_trap_storm;
#define POOL_TRAP_STORM_LIMIT 4000000L

static void pool_watch_enter(void);
static void pool_watch_leave(void);

static int pool_trap_arm_backing(void) {
    if (!pool_trap_backing_page) return 0;
    DWORD old=0;
    if (!VirtualProtect((void *)(uintptr_t)pool_trap_backing_page,4096,PAGE_READONLY,&old)) return 0;
    atomic_store(&pool_trap_backing_on,1);
    atomic_fetch_add(&pool_trap_arms,1);
    return 1;
}
/* The place that wrote through the host's view: named once, then counted. */
static void pool_trap_backing_note(uintptr_t rip, uintptr_t at) {
    const long hits=atomic_fetch_add(&pool_trap_backing_hits,1)+1;
    pool_watch_enter();
    int slot=-1;
    for (int i=0;i<pool_trap_backing_site_count;++i)
        if (pool_trap_backing_sites[i].rip==rip) { ++pool_trap_backing_sites[i].count; slot=i; break; }
    if (slot<0 && pool_trap_backing_site_count<POOL_TRAP_SITES) {
        slot=pool_trap_backing_site_count++;
        pool_trap_backing_sites[slot].rip=rip; pool_trap_backing_sites[slot].at=at;
        pool_trap_backing_sites[slot].count=1;
    }
    if (slot>=0 && pool_trap_backing_sites[slot].count==1) {
        char line[320];
        int n=snprintf(line,sizeof(line),
                       "Pool trap: a write into the host's view of the pool+%#llx from rip %#llx",
                       (unsigned long long)(at-(uintptr_t)pool_trap_backing_page),
                       (unsigned long long)rip);
        if (n<=0) n=0;
        if (n>=(int)sizeof(line)) n=(int)sizeof(line)-1;
        { HMODULE where=NULL;
          if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&where)) {
              char path[MAX_PATH];
              const DWORD length=GetModuleFileNameA(where,path,sizeof(path));
              if (length && length<sizeof(path)) {
                  const int m=snprintf(line+n,sizeof(line)-(size_t)n," (in %s+%#llx)",path,
                                       (unsigned long long)(rip-(uintptr_t)where));
                  if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1;
              }
          } }
        { const int m=snprintf(line+n,sizeof(line)-(size_t)n," thread %lu hit %ld\n",
                               (unsigned long)GetCurrentThreadId(),hits);
          if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1; }
        if (n>0) platform_write_stdout(line,(size_t)n);
    }
    pool_watch_leave();
}

static void pool_watch_enter(void) {
    while (InterlockedCompareExchange(&pool_watch_lock,1,0)) Sleep(0);
}
static void pool_watch_leave(void) { InterlockedExchange(&pool_watch_lock,0); }
static void pool_watch_say(const char *format, ...) {
    if (atomic_load(&pool_watch_lines)>=2048) return;
    char text[256];
    va_list arguments;
    va_start(arguments,format);
    int n=vsnprintf(text,sizeof(text),format,arguments);
    va_end(arguments);
    if (n<=0) return;
    if (n>=(int)sizeof(text)) n=(int)sizeof(text)-1;
    atomic_fetch_add(&pool_watch_lines,1);
    platform_write_stdout(text,(size_t)n);
}
/* BB_POOL_WATCH (diagnostics): four words of the pool's bytes, read through one of the names they
 * have (the guest's address, or the host's own view of the same physical memory). The same words
 * coming out of both names is the same memory; a change in one name while the other still says the
 * old words means the mapping behind that name was replaced, not written. */
static void pool_watch_words_say(const char *what, uintptr_t address) {
    if (!address) return;
    uint64_t words[4]={0,0,0,0};
    if (platform_read_memory((void *)address,words,sizeof(words)))
        pool_watch_say("Pool watch: %s %#llx: not readable\n",what,(unsigned long long)address);
    else
        pool_watch_say("Pool watch: %s %#llx: %#llx %#llx %#llx %#llx\n",what,
                       (unsigned long long)address,(unsigned long long)words[0],
                       (unsigned long long)words[1],(unsigned long long)words[2],
                       (unsigned long long)words[3]);
}
/* BB_POOL_WATCH (diagnostics): the state, the protection and the type of one name of the pool's
 * page, asked about in every round and said whenever any of them changes. The read-only trap holds
 * only for as long as nothing else flips the page back: a library that protects a page before it
 * writes it, or a host call that does, takes the trap away without ever telling this watch, and the
 * write then arrives on a writable page with the trap still believing it is armed. */
static void pool_watch_prot_poll(const char *what, PoolWatchProt *watched) {
    if (!watched->address) return;
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery((void *)watched->address,&info,sizeof(info))!=sizeof(info)) {
        if (watched->seen!=2) {
            watched->seen=2;
            pool_watch_say("Pool watch: the %s page %#llx cannot be asked about\n",what,
                           (unsigned long long)watched->address);
        }
        return;
    }
    const unsigned mask=((unsigned)(info.State&0xfff))|((unsigned)(info.Protect&0xffff)<<12)|
                        ((unsigned)(info.Type&0xffff)<<28);
    if (watched->seen==1 && mask==watched->mask) return;
    const unsigned was=watched->mask;
    watched->mask=mask; watched->seen=1;
    pool_watch_say("Pool watch: the %s page %#llx is state %#x protect %#x type %#x alloc %#x "
                   "(before %#x) at +%llu ms\n",what,(unsigned long long)watched->address,
                   (unsigned)info.State,(unsigned)info.Protect,(unsigned)info.Type,
                   (unsigned)info.AllocationProtect,was,
                   (unsigned long long)(GetTickCount64()-(unsigned long long)pool_watch_started_ms));
}
/* BB_POOL_WATCH (diagnostics): every other name the pool's bytes are mapped at, and the exact
 * shape of the erasure. The runtime maps the pool's section once for the guest (at the address the
 * game asked for) and keeps one view of its own (the "second name" this watch protects). A mapping
 * the runtime makes at an address the host picks itself - runtime_memory.c's map_phys falls back to
 * one when the address the game asked for is already taken, which is what happens when the same
 * range is mapped twice - is a third view of the very same physical bytes, and a store through it
 * reaches the pool without touching either name this watch holds: a different view has a page
 * protection of its own, and the processor's breakpoints watch linear addresses, not physical ones.
 * A store through such a view is exactly what "the words changed while their page was read-only,
 * with no write seen" means. The views are found by their content, not by their name (a
 * pagefile-backed section has none: see pool_watch_sweep_views), and the pool's page inside a view
 * is recognised by the page's own four words: the same four words do not occur by chance. The page
 * inside such a view is made read-only as soon as it is found, so the write through it is an access
 * violation that names the writer, exactly like the other two. */
#define POOL_EXTRA_VIEWS 8
typedef struct { uintptr_t base, size, page; } PoolExtraView;
static PoolExtraView pool_extra_views[POOL_EXTRA_VIEWS];
static _Atomic int pool_extra_count;
static _Atomic long pool_extra_hits;
static PoolTrapSite pool_extra_sites[POOL_TRAP_SITES];
static int pool_extra_site_count;
static uint64_t pool_extra_mark[4];       /* the first node's words as they were published */
static uint64_t pool_watch_snapshot[512]; /* the pool's first page as it was published */

static uint64_t pool_extra_view_size(uintptr_t base) {
    uint64_t size=0;
    for (int guard=0;guard<8192;++guard) {
        MEMORY_BASIC_INFORMATION part;
        if (VirtualQuery((void *)(base+size),&part,sizeof(part))!=sizeof(part)) break;
        if (!part.RegionSize || (uintptr_t)part.AllocationBase!=base) break;
        size+=part.RegionSize;
    }
    return size;
}
/* The place that wrote through a third view of the pool's bytes: named once, then counted. */
static void pool_extra_note(uintptr_t rip, uintptr_t at, uintptr_t page) {
    const long hits=atomic_fetch_add(&pool_extra_hits,1)+1;
    pool_watch_enter();
    int slot=-1;
    for (int i=0;i<pool_extra_site_count;++i)
        if (pool_extra_sites[i].rip==rip) { ++pool_extra_sites[i].count; slot=i; break; }
    if (slot<0 && pool_extra_site_count<POOL_TRAP_SITES) {
        slot=pool_extra_site_count++;
        pool_extra_sites[slot].rip=rip; pool_extra_sites[slot].at=at;
        pool_extra_sites[slot].count=1;
    }
    if (slot>=0 && pool_extra_sites[slot].count==1) {
        char line[320];
        int n=snprintf(line,sizeof(line),"Pool trap: a write into ANOTHER name of the pool (%#llx of "
                                       "%#llx) from rip %#llx",
                       (unsigned long long)at,(unsigned long long)page,(unsigned long long)rip);
        if (n<=0) n=0;
        if (n>=(int)sizeof(line)) n=(int)sizeof(line)-1;
        if (image && rip>=(uintptr_t)image && rip<(uintptr_t)image+0x20000000) {
            const int m=snprintf(line+n,sizeof(line)-(size_t)n," (guest+%#llx)",
                                 (unsigned long long)(rip-(uintptr_t)image));
            if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1;
        }
        { HMODULE where=NULL;
          if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&where)) {
              char path[MAX_PATH];
              const DWORD length=GetModuleFileNameA(where,path,sizeof(path));
              if (length && length<sizeof(path)) {
                  const int m=snprintf(line+n,sizeof(line)-(size_t)n," (in %s+%#llx)",path,
                                       (unsigned long long)(rip-(uintptr_t)where));
                  if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1;
              }
          } }
        { const int m=snprintf(line+n,sizeof(line)-(size_t)n," thread %lu hit %ld\n",
                               (unsigned long)GetCurrentThreadId(),hits);
          if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1; }
        if (n>0) platform_write_stdout(line,(size_t)n);
    }
    pool_watch_leave();
}
/* Does this name of some bytes hold the pool's page? The page is recognised by its own first four
 * words, which no other mapped range of the process has by chance: the words the page holds now (a
 * name made after the store), or the words it held when they were published (a name made before
 * it). Four words of that shape, eight-byte for eight-byte, are the mark. */
static int pool_extra_page_here(uintptr_t where) {
    uint64_t live[4], seen[4];
    if (!pool_watch_page || !where) return 0;
    if (platform_read_memory((void *)(uintptr_t)pool_watch_page,live,sizeof(live))) return 0;
    if (platform_read_memory((void *)where,seen,sizeof(seen))) return 0;
    if (!memcmp(seen,live,sizeof(live))) return (seen[0]|seen[1]|seen[2]|seen[3])!=0;
    return !memcmp(seen,pool_extra_mark,sizeof(pool_extra_mark));
}
/* Every other name of the pool's bytes. The views are found by content, not by name: the port's
 * sections are made with CreateFileMappingW(INVALID_HANDLE_VALUE,...), a pagefile-backed section
 * that has no name at all, so GetMappedFileNameW says nothing about them - it answered for the
 * images of the loaded libraries and for none of the port's own views, which is why the earlier
 * sweep, asking it for "bb-guest-memory", never found a view and never found a third one.
 *
 * A view of the pool's bytes holds the pool's page: at the beginning of the view when the view
 * begins at the page (a mapping of the pool's own range), or at the offset the page lies at inside
 * the section when the view begins at the section's beginning. Both are looked for, and the page
 * inside such a view is made read-only as soon as it is found, so a write through it is an access
 * violation that names the writer, exactly like the other two names. */
static void pool_watch_sweep_views(void) {
    static uintptr_t said[24];
    static int said_count, names_reported;
    static int sweeps_entered;
    unsigned char *at=(unsigned char *)(uintptr_t)0x10000;
    unsigned looked=0, candidates=0;
    const int report_names=!names_reported;
    /* The walk is long and the watch's first round cannot begin until it ends: its entry and its
     * leaving are said, rate-limited to the first few walks, so that a walk that never leaves (the
     * earlier runs showed the loop's first round never came) is told apart from a walk that simply
     * found nothing. */
    ++sweeps_entered;
    if (sweeps_entered<=6)
        pool_watch_say("Pool watch: sweep: entered (walk %d), looking through the address space\n",
                       sweeps_entered);
    for (;;) {
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(at,&info,sizeof(info))!=sizeof(info)) break;
        if (!info.RegionSize || !info.BaseAddress) break;
        unsigned char *next=(unsigned char *)info.BaseAddress+info.RegionSize;
        if (next<=(unsigned char *)info.BaseAddress) break;
        if (info.State==MEM_COMMIT && info.Type==MEM_MAPPED && info.AllocationBase &&
            (uintptr_t)info.BaseAddress==(uintptr_t)info.AllocationBase) {
            ++looked;
            const uintptr_t alloc=(uintptr_t)info.AllocationBase;
            const uint64_t size=pool_extra_view_size(alloc);
            const int guest_name=(uintptr_t)pool_watch_page>=alloc &&
                                 (uintptr_t)pool_watch_page<alloc+size;
            const int host_name=(uintptr_t)pool_trap_backing_page>=alloc &&
                                (uintptr_t)pool_trap_backing_page<alloc+size;
            if (report_names) {
                wchar_t name[256];
                const DWORD n=GetMappedFileNameW(GetCurrentProcess(),info.AllocationBase,name,
                                                 (DWORD)(sizeof(name)/sizeof(name[0])));
                if (n) {
                    DWORD i;
                    int ours=0;
                    for (i=0;i<n;++i) if (name[i]=='b' && i+1<n && name[i+1]=='b') { ours=1; break; }
                    if (ours && said_count<24) {
                        int already=0;
                        for (int j=0;j<said_count;++j) if (said[j]==alloc) { already=1; break; }
                        if (!already) {
                            char plain[256];
                            said[said_count++]=alloc;
                            for (i=0;i<n && i<sizeof(plain)-1;++i)
                                plain[i]=(char)((name[i]>32 && name[i]<127) ? name[i] : '?');
                            plain[i]=0;
                            pool_watch_say("Pool watch: the mapped range at %#llx is backed by %s\n",
                                           (unsigned long long)alloc,plain);
                        }
                    }
                }
            }
            if (guest_name || host_name) continue;   /* the two names the watch already holds */
            ++candidates;
            {
                uintptr_t where=0;
                uint64_t offsets[4];
                unsigned count=1;
                offsets[0]=0;
                if (pool_watch_phys) {
                    offsets[1]=pool_watch_phys;
                    offsets[2]=pool_watch_phys+(uintptr_t)(pool_watch_base-pool_watch_page);
                    offsets[3]=(uintptr_t)(pool_watch_base-pool_watch_page);
                    count=4;
                }
                for (unsigned k=0;k<count && !where;++k)
                    if (offsets[k]+0x40<=size && pool_extra_page_here(alloc+(uintptr_t)offsets[k]))
                        where=alloc+(uintptr_t)offsets[k];
                if (where) {
                    int known=0;
                    for (int j=0;j<atomic_load(&pool_extra_count);++j)
                        if (pool_extra_views[j].base==alloc) {
                            known=1; pool_extra_views[j].page=where; break;
                        }
                    if (!known) {
                        const int count2=atomic_load(&pool_extra_count);
                        if (count2<POOL_EXTRA_VIEWS) {
                            pool_extra_views[count2].base=alloc;
                            pool_extra_views[count2].size=size;
                            pool_extra_views[count2].page=where;
                            atomic_store(&pool_extra_count,count2+1);
                        }
                        pool_watch_say("Pool watch: ANOTHER name of the pool's bytes is %#llx+%#llx "
                                       "(the guest's name is %#llx, the host's is %#llx); the pool's "
                                       "page inside it is %#llx, read-only now: a write through it "
                                       "will be named\n",
                                       (unsigned long long)alloc,(unsigned long long)size,
                                       (unsigned long long)pool_watch_page,
                                       (unsigned long long)pool_trap_backing_page,
                                       (unsigned long long)where);
                    }
                    { DWORD old=0;
                      VirtualProtect((void *)where,0x4000,PAGE_READONLY,&old); }
                }
            }
        }
        if (looked && (looked%4096)==0 && sweeps_entered<=2)
            pool_watch_say("Pool watch: sweep: %u mapped ranges looked through so far, at %#llx\n",
                           looked,(unsigned long long)at);
        at=next;
    }
    names_reported=1;
    if (sweeps_entered<=6)
        pool_watch_say("Pool watch: sweep: left after %u mapped ranges\n",looked);
    pool_watch_say("Pool watch: the address space was looked through (%u mapped ranges, %u other "
                   "names, %d names of the pool's bytes known)\n",
                   looked,candidates,atomic_load(&pool_extra_count));
}
/* The place that wrote into the pool: named once, then counted. The breakpoint fires after the
 * store, so the place said is the instruction after the one that wrote (the store itself is the
 * instruction that ends just before it). The name says which of the two names of the bytes was
 * stored through, and the offset is counted from that name's own first byte. */
static void pool_watch_say_write(uintptr_t rip, uintptr_t at, uintptr_t origin, const char *name) {
    pool_watch_enter();
    atomic_fetch_add(&pool_watch_writes,1);
    int slot=-1;
    for (int i=0;i<pool_watch_place_count;++i)
        if (pool_watch_places[i].rip==rip) { ++pool_watch_places[i].count; slot=i; break; }
    if (slot<0 && pool_watch_place_count<POOL_WATCH_PLACES) {
        slot=pool_watch_place_count++;
        pool_watch_places[slot].rip=rip; pool_watch_places[slot].at=at;
        pool_watch_places[slot].count=1;
    }
    if (slot>=0 && pool_watch_places[slot].count==1 && atomic_fetch_add(&pool_watch_lines,1)<512) {
        char line[256];
        int n=snprintf(line,sizeof(line),"Pool watch: write into the pool+%#llx through the %s name "
                                       "from rip %#llx",
                       (unsigned long long)(at-origin),name,(unsigned long long)rip);
        if (n<=0) n=0;
        if (n>=(int)sizeof(line)) n=(int)sizeof(line)-1;
        if (image && rip>=(uintptr_t)image && rip<(uintptr_t)image+0x20000000) {
            const int m=snprintf(line+n,sizeof(line)-(size_t)n," (guest+%#llx)",
                                 (unsigned long long)(rip-(uintptr_t)image));
            if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1;
        }
        { HMODULE where=NULL;
          if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&where)) {
              char path[MAX_PATH];
              const DWORD length=GetModuleFileNameA(where,path,sizeof(path));
              if (length && length<sizeof(path)) {
                  const int m=snprintf(line+n,sizeof(line)-(size_t)n," (in %s+%#llx)",path,
                                       (unsigned long long)(rip-(uintptr_t)where));
                  if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1;
              }
          } }
        { const int m=snprintf(line+n,sizeof(line)-(size_t)n," thread %lu\n",
                               (unsigned long)GetCurrentThreadId());
          if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1; }
        if (n>0) platform_write_stdout(line,(size_t)n);
        /* bbport: also name the guest call chain that led to this store, so a write through the
         * host's backing view is traced back to the guest function that asked for it. The VEH runs
         * on the faulting thread, so runtime_guest_call_sites reads its guest context. */
        uint64_t gs[3]={0,0,0};
        runtime_guest_call_sites(gs);
        if (gs[0]||gs[1]||gs[2]) {
            char g[256];
            const int gn=snprintf(g,sizeof g,"  host-write guest frames: +%#llx < +%#llx < +%#llx\n",
                                 (unsigned long long)gs[0],(unsigned long long)gs[1],
                                 (unsigned long long)gs[2]);
            if (gn>0) platform_write_stdout(g,(size_t)(gn>=(int)sizeof g?sizeof g-1:gn));
        }
    } else if (slot>=0 && pool_watch_places[slot].count==2) {
        pool_watch_say("Pool watch: the place at rip %#llx writes into the pool again\n",
                       (unsigned long long)rip);
    }
    /* bbport: report the value actually stored on every trapped write (rate-limited to 32 per
     * place), so a zero (the erasure) is told apart from the registry's own free-list write
     * (0x800410060). `at` is a mapped guest or host virtual address and reads directly. */
    if (slot>=0 && pool_watch_places[slot].count>=1 && pool_watch_places[slot].count<=32) {
        uint64_t val=0;
        if (at) { const unsigned char *p=(const unsigned char *)at;
                  for (int i=0;i<8;++i) ((unsigned char *)&val)[i]=p[i]; }
        char v[64];
        const int vn=snprintf(v,sizeof v,"  stored value %#llx (hit %ld)\n",
                              (unsigned long long)val,(long)pool_watch_places[slot].count);
        if (vn>0) platform_write_stdout(v,(size_t)(vn>=(int)sizeof v?sizeof v-1:vn));
    }
    pool_watch_leave();
}
/* The page-protection watch's own record of a write: the place, the module, the thread. Each
 * place is said once and then counted. */
static void pool_trap_note(uintptr_t rip, uintptr_t at) {
    const long hits=atomic_fetch_add(&pool_trap_hits,1)+1;
    pool_watch_enter();
    int slot=-1;
    for (int i=0;i<pool_trap_site_count;++i)
        if (pool_trap_sites[i].rip==rip) { ++pool_trap_sites[i].count; slot=i; break; }
    if (slot<0 && pool_trap_site_count<POOL_TRAP_SITES) {
        slot=pool_trap_site_count++;
        pool_trap_sites[slot].rip=rip; pool_trap_sites[slot].at=at;
        pool_trap_sites[slot].count=1;
    }
    if (slot>=0 && pool_trap_sites[slot].count==1) {
        char line[320];
        int n=snprintf(line,sizeof(line),"Pool trap: a write into the pool+%#llx from rip %#llx",
                       (unsigned long long)(at-(uintptr_t)pool_watch_page),(unsigned long long)rip);
        if (n<=0) n=0;
        if (n>=(int)sizeof(line)) n=(int)sizeof(line)-1;
        if (image && rip>=(uintptr_t)image && rip<(uintptr_t)image+0x20000000) {
            const int m=snprintf(line+n,sizeof(line)-(size_t)n," (guest+%#llx)",
                                 (unsigned long long)(rip-(uintptr_t)image));
            if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1;
        }
        { HMODULE where=NULL;
          if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&where)) {
              char path[MAX_PATH];
              const DWORD length=GetModuleFileNameA(where,path,sizeof(path));
              if (length && length<sizeof(path)) {
                  const int m=snprintf(line+n,sizeof(line)-(size_t)n," (in %s+%#llx)",path,
                                       (unsigned long long)(rip-(uintptr_t)where));
                  if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1;
              }
          } }
        { const int m=snprintf(line+n,sizeof(line)-(size_t)n," thread %lu hit %ld\n",
                               (unsigned long)GetCurrentThreadId(),hits);
          if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1; }
        if (n>0) platform_write_stdout(line,(size_t)n);
    }
    pool_watch_leave();
}
/* The breakpoints, two names of the pool's first node words each, watched for writes: the guest's
 * name of the words (the pool's own address) and the host's own view of the same physical memory.
 * A store through the host's view does not fault the guest's page at all - it is a different view
 * of the bytes - so it is watched where it does happen. If the host's view is not known yet, the
 * four guest words are watched as before. */
static void pool_watch_registers(CONTEXT *context) {
    const uintptr_t base=(uintptr_t)pool_watch_base;
    const uintptr_t host=(uintptr_t)pool_watch_host_base;
    context->Dr0=base;
    context->Dr1=host?host:base+0x8;
    context->Dr2=host?base+0x8:base+0x10;
    context->Dr3=host?host+0x8:base+0x18;
    /* bbport fix: all four breakpoints are write-only (RW=01b) and eight bytes long (LEN=10b).
     * The previous setting had the host-view slots (Dr1/Dr3) configured as RW=read and the
     * guest-view slots (Dr0/Dr2) as LEN=1 byte, so a store through the host's backing view -
     * exactly how the registry pool is erased (store_once writes via backing_base+phys, a
     * different virtual address than the guest page the guest-view breakpoint watched) - was
     * never trapped. Enable is GLOBAL (bits 8..11), not local: the CPU clears local (L0..L3)
     * breakpoint enables on every thread/context switch, so local breakpoints are off in every
     * thread except the one whose context was just loaded, which is why the trap never fired.
     * Global breakpoints survive switches and watch every thread. RWn=01 at bits 16+2n,
     * LENn=10 at bits 25+2n, reserved bit 10 kept, GE/LE clear. */
    context->Dr7=(1u<<10)|0xF00u|0xFu|
                 (1u<<16)|(1u<<18)|(1u<<20)|(1u<<22)|
                 (1u<<25)|(1u<<27)|(1u<<29)|(1u<<31);
}
/* The processor's breakpoints, proved before they are trusted: this thread arms Dr0 on a word of
 * its own (eight bytes long, write-only) and stores into it, and the handler has to name the
 * store. A breakpoint set that never fires would make the silence that follows - no write into
 * the pool seen - mean nothing at all, and it did: the length field was written wrong (see
 * pool_watch_registers), so every store of eight bytes went past all four breakpoints. The words
 * of the pool are eight-byte words, so the proof uses an eight-byte word too. The store is done
 * on the thread that owns the registers, which is how a runaway SetThreadContext is ruled out. */
static volatile uint64_t pool_dr_probe[4];
static void pool_watch_dr_prove(void) {
    CONTEXT context;
    memset(&context,0,sizeof(context));
    context.ContextFlags=CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(GetCurrentThread(),&context)) {
        pool_watch_say("Pool watch: the breakpoints' own state was not read, they are NOT proved\n");
        return;
    }
    context.Dr0=(uintptr_t)&pool_dr_probe[0];
    context.Dr7|=0x1u|(1u<<16)|(1u<<19); /* Dr0: enabled, write-only, eight bytes long */
    context.ContextFlags=CONTEXT_DEBUG_REGISTERS;
    if (!SetThreadContext(GetCurrentThread(),&context)) {
        pool_watch_say("Pool watch: the breakpoints' own state was not written, they are NOT proved\n");
        return;
    }
    const long before=atomic_load(&pool_watch_writes);
    pool_dr_probe[0]=UINT64_C(0x5a5a5a5a5a5a5a5a);
    const long after=atomic_load(&pool_watch_writes);
    pool_watch_say("Pool watch: the breakpoints' own probe write was %s (%ld places named by it)\n",
                   after>before?"seen":"NOT seen",after-before);
    /* The watch's own breakpoints are put back on this thread (the store above left the probe's). */
    context.ContextFlags=CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(GetCurrentThread(),&context)) {
        pool_watch_registers(&context);
        context.ContextFlags=CONTEXT_DEBUG_REGISTERS;
        SetThreadContext(GetCurrentThread(),&context);
    }
}
static void pool_watch_dump(const char *path, uintptr_t address);
static int pool_tight_dumped;
/* A poll of its own, closer than the watch's loop can be: the loop carries the sweep (which walks
 * the whole address space and costs a hundred milliseconds) and the adoption of every thread, so
 * its first round can come a hundred milliseconds after the words were published - long after the
 * words were already gone, which is what the earlier runs showed (+203 and +218 ms). This thread
 * stands behind nothing: it asks about the protection of both names and reads the node's words as
 * fast as the machine allows, and says the instant either name stops being read-only and the
 * instant the words change, with the protection and the traps of that same instant. */
static unsigned pool_tight_mask(uintptr_t address) {
    MEMORY_BASIC_INFORMATION info;
    if (!address || VirtualQuery((void *)address,&info,sizeof(info))!=sizeof(info)) return 0xffffffffu;
    return ((unsigned)(info.State&0xfff))|((unsigned)(info.Protect&0xffff)<<12)|
           ((unsigned)(info.Type&0xffff)<<28);
}
static DWORD WINAPI pool_tight_thread(LPVOID parameter) {
    (void)parameter;
    while (!pool_watch_page || !pool_watch_started_ms) {
        if (atomic_load(&pool_watch_lines)>=2048) return 0;
        Sleep(1);
    }
    uint64_t last[4]={0,0,0,0};
    int have=0, said_masks=0;
    unsigned mask_g=0, mask_h=0;
    for (;;) {
        const uint64_t now_ms=(uint64_t)GetTickCount64();
        if (now_ms-pool_watch_started_ms>10000) break; /* the start-up is over: the loop is enough */
        const unsigned mg=pool_tight_mask((uintptr_t)pool_watch_page);
        const unsigned mh=pool_tight_mask((uintptr_t)pool_trap_backing_page);
        if (!said_masks || mg!=mask_g || mh!=mask_h) {
            said_masks=1; mask_g=mg; mask_h=mh;
            pool_watch_say("Pool watch: tight: the guest's page protect %#x state %#x, the host's "
                           "protect %#x state %#x (traps %d/%d, writes %ld, traps hit %ld+%ld+%ld) "
                           "at +%llu ms\n",
                           (mg>>12)&0xffff, mg&0xfff, (mh>>12)&0xffff, mh&0xfff,
                           atomic_load(&pool_trap_on),atomic_load(&pool_trap_backing_on),
                           (long)atomic_load(&pool_watch_writes),
                           (long)atomic_load(&pool_trap_hits),
                           (long)atomic_load(&pool_trap_backing_hits),
                           (long)atomic_load(&pool_extra_hits),
                           (unsigned long long)(now_ms-(unsigned long long)pool_watch_started_ms));
        }
        uint64_t words[4]={0,0,0,0};
        if (pool_watch_base &&
            platform_read_memory((void *)(uintptr_t)pool_watch_base,words,sizeof(words))==0) {
            if (!have || memcmp(words,last,sizeof(words))) {
                const int first=!have;
                have=1;
                pool_watch_say("Pool watch: tight: the words are %#llx %#llx %#llx %#llx at +%llu ms "
                               "(masks %#x/%#x, traps %d/%d, writes %ld, traps hit %ld+%ld+%ld)\n",
                               (unsigned long long)words[0],(unsigned long long)words[1],
                               (unsigned long long)words[2],(unsigned long long)words[3],
                               (unsigned long long)(now_ms-(unsigned long long)pool_watch_started_ms),
                               mask_g,mask_h,atomic_load(&pool_trap_on),
                               atomic_load(&pool_trap_backing_on),(long)atomic_load(&pool_watch_writes),
                               (long)atomic_load(&pool_trap_hits),
                               (long)atomic_load(&pool_trap_backing_hits),
                               (long)atomic_load(&pool_extra_hits));
                if (!first && !pool_tight_dumped) {
                    pool_tight_dumped=1;
                    pool_watch_dump("pool-page-after.bin",(uintptr_t)pool_watch_page);
                    if (pool_watch_host_base)
                        pool_watch_dump("pool-page-host-after.bin",(uintptr_t)pool_watch_host_base);
                }
                memcpy(last,words,sizeof(last));
            }
        }
        Sleep(0);
    }
    return 0;
}
/* The pool's page as it is, written out whole: the shape of a store cannot be read against a
 * snapshot of four words, and the page as it was before and after can be compared offline with the
 * images and the patches, which is what tells where the bytes that took the page's place came
 * from. */
static void pool_watch_dump(const char *path, uintptr_t address) {
    if (!address) return;
    unsigned char page[0x4000];
    if (platform_read_memory((void *)address,page,sizeof(page))) {
        pool_watch_say("Pool watch: the page at %#llx was not read for %s\n",
                       (unsigned long long)address,path);
        return;
    }
    FILE *file=fopen(path,"wb");
    if (!file) {
        pool_watch_say("Pool watch: %s was not written\n",path);
        return;
    }
    fwrite(page,1,sizeof(page),file);
    fclose(file);
    pool_watch_say("Pool watch: the page at %#llx was written to %s\n",
                   (unsigned long long)address,path);
}
static int pool_watch_take_thread(HANDLE thread) {
    CONTEXT context;
    memset(&context,0,sizeof(context));
    context.ContextFlags=CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(thread,&context)) return 0;
    const uintptr_t base=(uintptr_t)pool_watch_base;
    const uintptr_t host=(uintptr_t)pool_watch_host_base;
    if (context.Dr0==base && context.Dr1==(host?host:base+0x8) &&
        context.Dr3==(host?host+0x8:base+0x18)) return 1; /* already carried */
    pool_watch_registers(&context);
    context.ContextFlags=CONTEXT_DEBUG_REGISTERS;
    return SetThreadContext(thread,&context)?1:0;
}
/* Every thread of the process carries the breakpoints; the ones that start while the watch is on
 * take them in the sweep that follows. */
static void pool_watch_adopt(void) {
    HANDLE all=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if (all==INVALID_HANDLE_VALUE) return;
    const DWORD process=GetCurrentProcessId(), mine=GetCurrentThreadId();
    THREADENTRY32 one;
    one.dwSize=sizeof(one);
    int carried=0;
    if (Thread32First(all,&one)) do {
        if (one.th32OwnerProcessID!=process || one.th32ThreadID==mine) continue;
        HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,
                                 one.th32ThreadID);
        if (!thread) continue;
        if (SuspendThread(thread)!=(DWORD)-1) {
            if (pool_watch_take_thread(thread)) ++carried;
            ResumeThread(thread);
        }
        CloseHandle(thread);
    } while (Thread32Next(all,&one));
    CloseHandle(all);
    atomic_store(&pool_watch_threads,carried);
}
/* The pool's own watch thread: it waits for the game to allocate the pool, takes the breakpoints
 * on its first node, and reads the node's words every five milliseconds. The watchdog (the other
 * thread) is far too slow for this; nothing here waits on the runtime, so it cannot deadlock. */
/* bbport: once the game publishes the R1 registry pool's base (image+0x161d080), relocate the pool
 * into its own section (runtime_memory_protect_r1_pool) so the GPU's in-place import of direct-memory
 * chunks and any discard of the shared pool section cannot reach it. Always on (not gated by
 * BB_POOL_WATCH); polls briefly then runs once and exits. */
static DWORD WINAPI pool_protect_thread(LPVOID parameter) {
    (void)parameter;
    uintptr_t base=0;
    for (unsigned tries=0;tries<600000;++tries) { /* ten minutes is more than the boot takes */
        base=0;
        if (image && platform_read_memory((void *)((uintptr_t)image+0x161d080),&base,8)==0 &&
            base>=0x10000 && base<0x0000ffffffffffffull)
            break;
        base=0;
        Sleep(1);
    }
    if (!base) return 0;
    uint64_t phys=0; uintptr_t end=0;
    if (runtime_memory_direct_phys((uintptr_t)base,&phys,&end))
        runtime_memory_protect_r1_pool(phys, 0x320000);
    return 0;
}
static DWORD WINAPI pool_watch_thread(LPVOID parameter) {
    (void)parameter;
    uintptr_t base=0;
    for (unsigned tries=0;tries<600000;++tries) { /* ten minutes is more than the boot takes */
        base=0;
        /* The pool's base is held by the registry's own control block, in the image. */
        if (platform_read_memory((void *)((uintptr_t)image+0x161d080),&base,8)==0 &&
            base>=0x10000 && base<0x0000ffffffffffffull) break;
        base=0;
        Sleep(1);
    }
    if (!base) { pool_watch_say("Pool watch: the pool's base never appeared\n"); return 0; }
    pool_watch_base=base;
    pool_watch_page=base&~(uintptr_t)4095;
    pool_watch_say("Pool watch: the registry pool is at %#llx (page %#llx); the four words of its "
                   "first node are watched for writes\n",
                   (unsigned long long)base,(unsigned long long)pool_watch_page);
    /* The node's words the instant the base is published: the free list is threaded a few
     * instructions after the base is stored, so a zero here means the threading never happened
     * or was already undone. Read before anything else, so nothing this thread does can hide
     * the state at publication. */
    {
        uint64_t words[4]={0,0,0,0};
        uint64_t head=0;
        platform_read_memory((void *)((uintptr_t)image+0x160b688),&head,8);
        const int read=platform_read_memory((void *)(uintptr_t)pool_watch_base,words,sizeof(words))==0;
        pool_watch_say("Pool watch: at publication free_head=%#llx node %#llx %#llx %#llx %#llx%s\n",
                       (unsigned long long)head,(unsigned long long)words[0],
                       (unsigned long long)words[1],(unsigned long long)words[2],
                       (unsigned long long)words[3],read?"":" (unreadable)");
        /* Those four words are the mark that finds the pool inside every other view of its
         * section; the whole first page is kept as it is now, so that the shape of an erasure can
         * be read against it (how far it reaches, and where exactly). */
        memcpy(pool_extra_mark,words,sizeof(pool_extra_mark));
        if (platform_read_memory((void *)(uintptr_t)pool_watch_page,pool_watch_snapshot,
                                 sizeof(pool_watch_snapshot))!=0)
            memset(pool_watch_snapshot,0,sizeof(pool_watch_snapshot));
    }
    /* The page is made read-only before the heavier work that follows (the runtime's watch is
     * taken under its own lock, and adopting the threads takes a while): the trap is armed in
     * microseconds, the free list is not threaded yet when the base is published, so a trap
     * armed here is armed in time. */
    {
        const int armed=pool_trap_arm();
        pool_watch_say("Pool watch: the page protection trap is %s (page %#llx)\n",
                       armed?"armed":"NOT armed",(unsigned long long)pool_watch_page);
    }
    /* The host's own view of the very same bytes, made read-only as well (pool_trap_arm_backing
     * above says why): a store through that name lands on these bytes without ever touching the
     * guest's page, so this is the only watch that can name it. */
    {
        uint64_t phys=0;
        uintptr_t end=0;
        void *view=runtime_memory_direct_phys(pool_watch_page,&phys,&end)
                       ? runtime_memory_backing_pointer(phys) : NULL;
        if (view) {
            pool_trap_backing_page=(uintptr_t)view;
            const int armed=pool_trap_arm_backing();
            pool_watch_say("Pool watch: the host's own view of the pool is %#llx (phys %#llx), %s\n",
                           (unsigned long long)(uintptr_t)view,(unsigned long long)phys,
                           armed?"read-only now":"NOT protected");
        } else {
            pool_watch_say("Pool watch: the host's own view of the pool was not found\n");
        }
    }
    /* Both traps are proved with a store of the value that is already there: the page is
     * read-only, so the store must fault and the handler must name it. A trap that stays silent
     * here is silent about every write it did not see as well, so this proof is what makes the
     * silence that follows mean something. The value stored is the value read, so the bytes are
     * left exactly as they were. */
    {
        if (pool_watch_base) {
            volatile uint32_t *word=(volatile uint32_t *)(uintptr_t)pool_watch_base;
            const uint32_t was=*word;
            *word=was;
            pool_watch_say("Pool watch: the guest page trap's own probe write was %s\n",
                           atomic_load(&pool_trap_hits)?"seen":"NOT seen");
        }
        if (pool_trap_backing_page) {
            volatile uint32_t *word=(volatile uint32_t *)(uintptr_t)(pool_trap_backing_page+0x40);
            const uint32_t was=*word;
            *word=was;
            pool_watch_say("Pool watch: the host view trap's own probe write was %s\n",
                           atomic_load(&pool_trap_backing_hits)?"seen":"NOT seen");
        }
    }
    /* The self-tests above took both traps away: the handler gives the page back so that the write
       it just named can go through, and the watch's own loop only arms them again once it starts -
       and the loop starts only after every thread has been adopted. The game's startup runs through
       that window, so the traps are put back here, at once, before anything else. */
    pool_trap_arm();
    pool_trap_arm_backing();
    /* The host's own view of the pool's bytes and the protection of both names, taken here and
       then asked about in every round: whatever takes the read-only away from either name - the
       GPU library protecting a page it writes, the runtime protecting a guest page, a mapping
       operation putting the view back - is said with its time, so a write that arrives on a page
       the trap no longer holds is not counted as a write the trap did not see. */
    pool_watch_host_base=pool_trap_backing_page
                             ?(uintptr_t)pool_trap_backing_page+(uintptr_t)(pool_watch_base-pool_watch_page)
                             :(uintptr_t)0;
    pool_watch_started_ms=(uint64_t)GetTickCount64();
    pool_watch_prot_guest.address=(uintptr_t)pool_watch_page;
    pool_watch_prot_host.address=(uintptr_t)pool_trap_backing_page;
    pool_watch_words_say("the pool's first words through the guest's name",(uintptr_t)pool_watch_base);
    pool_watch_words_say("the pool's first words through the host's name",pool_watch_host_base);
    pool_watch_prot_poll("guest's",&pool_watch_prot_guest);
    pool_watch_prot_poll("host's",&pool_watch_prot_host);
    runtime_memory_watch_range(pool_watch_page,0x4000);
    {   /* The other names of the same bytes: a write through one of them is not a fault at all
           (no page protection is used), the breakpoints catch it wherever it comes from. */
        uintptr_t starts[8], ends[8];
        const int n=runtime_memory_aliases(pool_watch_page,0x4000,starts,ends,8);
        if (!n) pool_watch_say("Pool watch: the pool's bytes have no other name\n");
        for (int i=0;i<n;++i)
            pool_watch_say("Pool watch: the pool's bytes are also named %#llx-%#llx\n",
                           (unsigned long long)starts[i],(unsigned long long)ends[i]);
    }
    /* The copies the runtime's HLE'd C library makes into the pool's page are named too. */
    { extern uintptr_t runtime_cpu_watch_lo, runtime_cpu_watch_hi;
      runtime_cpu_watch_lo=pool_watch_page;
      runtime_cpu_watch_hi=pool_watch_page+0x4000; }
    pool_watch_adopt();
    pool_watch_say("Pool watch: %d threads carry the breakpoints\n",
                   atomic_load(&pool_watch_threads));
    uint64_t last[4]={~0ull,0,0,0};
    long last_writes=0, last_trap_hits=0, last_backing_hits=0, last_extra_hits=0;
    /* Every other view of the pool's section, before the game runs on: a view the watch does not
     * hold is a name of the pool's bytes through which a store reaches the pool silently. */
    pool_watch_sweep_views();
    uint64_t last_adopt=(uint64_t)GetTickCount64();
    uint64_t last_sweep=last_adopt;
    for (unsigned round=0;;++round) {
        /* The page the runtime put back (or the trap that was just fired) is armed again first of
         * all, before the adoption: the watch has to stay on for the write that erases the node to
         * be caught whatever it is. */
        if (!atomic_load(&pool_trap_on)) pool_trap_arm();
        if (!atomic_load(&pool_trap_backing_on)) pool_trap_arm_backing();
        /* The pages of the other views found by the sweep are held read-only too, until the first
         * write through one of them has been named: after that they are left alone (the trap is
         * for the naming, not for stopping the game). */
        if (!atomic_load(&pool_extra_hits))
            for (int i=0;i<atomic_load(&pool_extra_count);++i)
                if (pool_extra_views[i].page) {
                    DWORD old=0;
                    VirtualProtect((void *)pool_extra_views[i].page,0x4000,PAGE_READONLY,&old);
                }
        {   /* The adoption and the sweep cost milliseconds, so they are kept to their own times
               (the rounds themselves cost microseconds: the words are watched as closely as the
               machine allows, so that the instant of the erasure is known to the microsecond). */
            const uint64_t now_ms=(uint64_t)GetTickCount64();
            if (now_ms-last_adopt>=10) { last_adopt=now_ms; pool_watch_adopt(); }
            if (now_ms-last_sweep>=(now_ms-pool_watch_started_ms<3000?100:1000)) {
                last_sweep=now_ms;
                pool_watch_sweep_views();
            }
        }
        {
            const long hits=atomic_load(&pool_trap_hits);
            if (hits!=last_trap_hits) {
                last_trap_hits=hits;
                if (hits<=8 || hits%256==0)
                    pool_watch_say("Pool watch: %ld writes into the pool's page caught by the "
                                   "trap so far\n",hits);
            }
            const long backing=atomic_load(&pool_trap_backing_hits);
            if (backing!=last_backing_hits) {
                last_backing_hits=backing;
                if (backing<=8 || backing%256==0)
                    pool_watch_say("Pool watch: %ld writes into the host's view of the pool caught "
                                   "by the trap so far\n",backing);
            }
            const long extra=atomic_load(&pool_extra_hits);
            if (extra!=last_extra_hits) {
                last_extra_hits=extra;
                if (extra<=8 || extra%256==0)
                    pool_watch_say("Pool watch: %ld writes into another name of the pool caught by "
                                   "the trap so far\n",extra);
            }
        }
        /* The protection of both names is asked about every round, before the words are read: a
         * change while either name reads read-only is not a write through that name at all. */
        pool_watch_prot_poll("guest's",&pool_watch_prot_guest);
        pool_watch_prot_poll("host's",&pool_watch_prot_host);
        uint64_t words[4]={0,0,0,0};
        const int read=platform_read_memory((void *)(uintptr_t)pool_watch_base,words,sizeof(words))==0;
        if (read && (words[0]!=last[0]||words[1]!=last[1]||words[2]!=last[2]||words[3]!=last[3])) {
            uint32_t used=0, next_id=0;
            if (image) {
                platform_read_memory((void *)((uintptr_t)image+0x160b694),&used,4);
                platform_read_memory((void *)((uintptr_t)image+0x160b600),&next_id,4);
            }
            /* bbport: the verdict on WHO erased the pool. If the guest VA 0x800410040 now maps a
             * DIFFERENT physical page than the one the registry was built on (0x4900040), the
             * runtime/GPU repointed the mapping (a remap that the stale phys-window of watch_note
             * would miss). If the phys is unchanged but the bytes are zero/other, it is an in-place
             * reset (MEM_RESET / punch hole) or a write through an untrapped name. */
            {   char dg[200], dh[200];
                runtime_memory_describe((uintptr_t)pool_watch_base,dg,sizeof dg);
                runtime_memory_describe(pool_watch_host_base?pool_watch_host_base:(uintptr_t)pool_watch_base,
                                       dh,sizeof dh);
            pool_watch_say("Pool watch: mapping at erasure: guest(%#llx) %s | host(%#llx) %s "
                           "[original phys 0x4900040]\n",
                           (unsigned long long)pool_watch_base,dg,
                           (unsigned long long)(pool_watch_host_base?pool_watch_host_base:
                                                (uintptr_t)pool_watch_base),dh);
            {   char ops[4096];
                if (runtime_memory_op_dump(ops,sizeof ops)>0)
                    pool_watch_say("Pool watch: last memory operations (most recent last):\n%s",ops);
            }
            }
            pool_watch_say("Pool watch: the pool's words are now %#llx %#llx %#llx %#llx through the "
                           "guest's name (writes seen %ld, trap on %d arms %ld hits %ld+%ld, "
                           "used %#x next_id %#x) at +%llu ms\n",
                           (unsigned long long)words[0],(unsigned long long)words[1],
                           (unsigned long long)words[2],(unsigned long long)words[3],
                           (long)atomic_load(&pool_watch_writes),atomic_load(&pool_trap_on),
                           (long)atomic_load(&pool_trap_arms),(long)atomic_load(&pool_trap_hits),
                           (long)atomic_load(&pool_trap_backing_hits),used,next_id,
                           (unsigned long long)(GetTickCount64()-(unsigned long long)pool_watch_started_ms));
            /* The protection of both names at the instant the words were read (the poll above runs
             * a few microseconds before the read): words that changed while their page was
             * read-only were not written through that page at all - something wrote the physical
             * bytes through a name this watch does not hold. Words that changed while the page was
             * writable are a write the trap was not armed for. */
            pool_watch_say("Pool watch: at that instant the guest's name is protect %#x and the "
                           "host's name is protect %#x (the traps %d/%d)\n",
                           pool_watch_prot_guest.mask,pool_watch_prot_host.mask,
                           atomic_load(&pool_trap_on),atomic_load(&pool_trap_backing_on));
            /* The shape of the erasure: the pool's first page as it was published against the page
             * now. Four bytes of zeros is one store; the free-list region is the allocator reusing
             * the nodes; the whole page is a bulk copy or the memory having been given back. */
            { uint64_t now[512];
              unsigned changed=0, first=0, lastoff=0, shown=0;
              if (platform_read_memory((void *)(uintptr_t)pool_watch_page,now,sizeof(now))==0) {
                  for (unsigned i=0;i<512;++i)
                      if (now[i]!=pool_watch_snapshot[i]) {
                          if (!changed) first=i;
                          lastoff=i; ++changed;
                      }
                  pool_watch_say("Pool watch: %u of the page's 512 words differ from the page as it "
                                 "was published (first +%#llx, last +%#llx)\n",changed,
                                 (unsigned long long)(first*8),(unsigned long long)(lastoff*8));
                  for (unsigned i=0;i<512 && shown<6;++i)
                      if (now[i]!=pool_watch_snapshot[i]) {
                          pool_watch_say("Pool watch:   +%#llx was %#llx now %#llx\n",
                                         (unsigned long long)(i*8),
                                         (unsigned long long)pool_watch_snapshot[i],
                                         (unsigned long long)now[i]);
                          ++shown;
                      }
              } }
            /* The same words through the host's own name: the same bytes have to come out of both
             * names. Only one of the two names changing means the mapping behind that name was
             * replaced, not the memory written. */
            pool_watch_words_say("the same words through the host's name",pool_watch_host_base);
            /* How far the erasure reaches: the node's own page, and the pool's second page. A
             * single node's worth of zeros is a targeted store; a whole page (or the whole pool)
             * of zeros is a bulk operation or the memory having been given back. */
            { unsigned nonzero_first=0, nonzero_second=0; uint32_t word=0;
              for (unsigned i=0;i<4096;i+=16)
                  if (!platform_read_memory((void *)((uintptr_t)pool_watch_base+i),&word,4) && word)
                      ++nonzero_first;
              for (unsigned i=0;i<4096;i+=16)
                  if (!platform_read_memory((void *)((uintptr_t)pool_watch_base+0x1000+i),&word,4) && word)
                      ++nonzero_second;
              pool_watch_say("Pool watch: of the pool's first page %u of 256 sampled words are not "
                             "zero, of the second page %u of 256\n",nonzero_first,nonzero_second); }
            pool_watch_prot_poll("guest's",&pool_watch_prot_guest);
            pool_watch_prot_poll("host's",&pool_watch_prot_host);
            memcpy(last,words,sizeof(last));
        }
        const long writes=atomic_load(&pool_watch_writes);
        if (writes!=last_writes) {
            last_writes=writes;
            if (writes<=8 || writes%1024==0)
                pool_watch_say("Pool watch: %ld writes into the pool's first node so far\n",writes);
        }
        /* The round costs microseconds: the shifts of the words are dated as closely as the
           machine allows, and no protection change can hide between two reads. */
        Sleep(0);
    }
    return 0;
}
/* The Windows build has no signals: the vectored exception handler sees the access violations,
 * the illegal instructions and the breakpoints the runtime and the game cause. */
void bb_fault_wait(const char *what) {
    /* There is no debugger to attach to on Windows: the process stops with the fault's report. */
    (void)what;
}
/* BB_CPU_WATCH on Windows: the page the watch protects has no signal handler here (the signal
 * path is the POSIX build's), so the vectored handler does the job the way the pool's trap
 * does it: the write is named, one store is let through, and the Trap Flag's single-step
 * (handled at the top of this handler) re-protects the page.
 *
 * The watch is a small table of pages, so the image's own pages (BB_CPU_WATCH=<image offset>)
 * and any page of the guest's memory (BB_GUEST_WATCH=<guest address>) can be watched at the
 * same time. The guest's address space is the host's, so a guest address is a host address. */
#define CPU_WATCH_PAGES 4
typedef struct { unsigned char *page; uintptr_t lo, hi; int is_guest; } CpuWatch;
static CpuWatch cpu_watch[CPU_WATCH_PAGES];
static int cpu_watch_count;
static _Atomic int cpu_watch_hits;
static volatile uintptr_t cpu_watch_reprotect;
static volatile uintptr_t cpu_watch_open;   /* the page that was opened for one store (POSIX) */
#define CPU_WATCH_SITES 24
#define CPU_WATCH_STORM_LIMIT 10000L
static struct { uintptr_t rip, at; long count; } cpu_watch_sites[CPU_WATCH_SITES];
static int cpu_watch_site_count;
static _Atomic long cpu_watch_storm;
static void cpu_watch_note(uintptr_t rip, uintptr_t at, const CpuWatch *watch) {
    const long hits=atomic_fetch_add(&cpu_watch_hits,1)+1;
    int slot=-1;
    for (int i=0;i<cpu_watch_site_count;++i)
        if (cpu_watch_sites[i].rip==rip) { ++cpu_watch_sites[i].count; slot=i; break; }
    if (slot<0 && cpu_watch_site_count<CPU_WATCH_SITES) {
        slot=cpu_watch_site_count++;
        cpu_watch_sites[slot].rip=rip; cpu_watch_sites[slot].at=at; cpu_watch_sites[slot].count=1;
    }
    if (slot>=0 && cpu_watch_sites[slot].count==1) {
        char line[320];
        int n=snprintf(line,sizeof(line),"CPU watch: write to %s",
                       (watch && watch->is_guest) ? "guest" : "image");
        if (n<=0) n=0;
        if (n>=(int)sizeof(line)) n=(int)sizeof(line)-1;
        if (watch && watch->is_guest) {
            const int m=snprintf(line+n,sizeof(line)-(size_t)n,"+%#llx",
                                 (unsigned long long)(at-(uintptr_t)watch->page));
            if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1;
        } else {
            const int m=snprintf(line+n,sizeof(line)-(size_t)n,"+%#llx",
                                 (unsigned long long)(at-(uintptr_t)image));
            if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1;
        }
        const int m=snprintf(line+n,sizeof(line)-(size_t)n," from rip %#llx",
                             (unsigned long long)rip);
        if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1;
        if (rip>=(uintptr_t)image && rip<(uintptr_t)image+0x20000000) {
            const int m=snprintf(line+n,sizeof(line)-(size_t)n," (guest+%#llx)",
                                 (unsigned long long)(rip-(uintptr_t)image));
            if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1;
        }
        { HMODULE where=NULL;
          if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&where)) {
              char path[MAX_PATH];
              const DWORD length=GetModuleFileNameA(where,path,sizeof(path));
              if (length && length<sizeof(path)) {
                  const int m=snprintf(line+n,sizeof(line)-(size_t)n," (in %s+%#llx)",path,
                                       (unsigned long long)(rip-(uintptr_t)where));
                  if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1;
              }
          } }
        { const int m=snprintf(line+n,sizeof(line)-(size_t)n," thread %lu hit %ld\n",
                               (unsigned long)GetCurrentThreadId(),hits);
          if (m>0 && m<(int)sizeof(line)-n) n+=m; else n=(int)sizeof(line)-1; }
        if (n>0) platform_write_stdout(line,(size_t)n);
    }
}
static LONG CALLBACK fault_handler(EXCEPTION_POINTERS *exception) {
    const DWORD code = exception->ExceptionRecord->ExceptionCode;
    /* The pool's breakpoints (BB_POOL_WATCH=1): a write into the first node of the game's registry
     * pool is named - the place that wrote it, the module it is in and the thread. The processor
     * fires the breakpoint after the store has happened, so the place said is the instruction after
     * the store (the store is the instruction that ends just before it); nothing is undone and the
     * game runs on. */
    if (code==EXCEPTION_SINGLE_STEP) {
        /* Re-protect the cpu-watch page we had opened for exactly one store (BB_CPU_WATCH):
         * the same shape as the pool's re-protection below. */
        if (cpu_watch_reprotect) {
            const uintptr_t pg=cpu_watch_reprotect;
            cpu_watch_reprotect=0;
            { DWORD old=0;
              VirtualProtect((void *)pg,4096,PAGE_READONLY,&old); }
            exception->ContextRecord->Dr6 &= ~0x4000u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        /* Re-protect a pool page we had opened for exactly one store (see the fault handlers
         * below). The Trap Flag we set there is the reason this single-step arrived: its BS bit
         * in Dr6 is cleared and the data-breakpoint path below is left untouched. (The processor
         * clears TF itself on the #DB, so it does not keep single-stepping.) */
        if (pool_trap_reprotect_guest) {
            const uintptr_t pg=pool_trap_reprotect_guest;
            pool_trap_reprotect_guest=0;
            { DWORD old=0;
              if (VirtualProtect((void *)pg,4096,PAGE_READONLY,&old))
                  atomic_store(&pool_trap_on,1); }
            exception->ContextRecord->Dr6 &= ~0x4000u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (pool_trap_reprotect_backing) {
            const uintptr_t pg=pool_trap_reprotect_backing;
            pool_trap_reprotect_backing=0;
            { DWORD old=0;
              if (VirtualProtect((void *)pg,4096,PAGE_READONLY,&old))
                  atomic_store(&pool_trap_backing_on,1); }
            exception->ContextRecord->Dr6 &= ~0x4000u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (pool_watch_base) {
            CONTEXT *context=exception->ContextRecord;
            const ULONG reason=(ULONG)context->Dr6;
            const uintptr_t host=(uintptr_t)pool_watch_host_base;
            uintptr_t at=(uintptr_t)pool_watch_base, origin=(uintptr_t)pool_watch_page;
            const char *name="guest's";
            if (reason&1) at=(uintptr_t)context->Dr0;
            else if (reason&2) { at=(uintptr_t)context->Dr1; origin=host; name="host's"; }
            else if (reason&4) at=(uintptr_t)context->Dr2;
            else if (reason&8) { at=(uintptr_t)context->Dr3; origin=host; name="host's"; }
            pool_watch_say_write((uintptr_t)context->Rip,at,origin?origin:(uintptr_t)pool_watch_page,name);
            context->Dr6=0; /* the reason was taken: it is not a reason again */
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    /* The cpu-watch page (BB_CPU_WATCH=<image offset>, BB_GUEST_WATCH=<guest address>): a write
     * into it is named - the place, the module, the thread - and exactly one store is let through
     * (the page is made writable and the thread's Trap Flag is set; the single-step at the top of
     * this handler re-protects it read-only). This runs before the runtime's own recovery below,
     * which would give the page back without naming anybody. */
    if ((code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_IN_PAGE_ERROR) && cpu_watch_count) {
        const EXCEPTION_RECORD *record=exception->ExceptionRecord;
        const int is_write=(record->NumberParameters>=2 && record->ExceptionInformation[0]==1);
        const uintptr_t at=(record->NumberParameters>=2)
                               ? (uintptr_t)record->ExceptionInformation[1] : 0;
        for (int w=0;w<cpu_watch_count;++w) {
            const uintptr_t page=(uintptr_t)cpu_watch[w].page;
            if (!page || !is_write || at<page || at>=page+4096) continue;
            cpu_watch_note((uintptr_t)exception->ContextRecord->Rip,at,&cpu_watch[w]);
            const long seen=atomic_fetch_add(&cpu_watch_storm,1)+1;
            if (seen>CPU_WATCH_STORM_LIMIT) {
                /* Too many faults: leave the page writable for the rest of the run. */
                static _Atomic int storm_said;
                if (!atomic_exchange(&storm_said,1)) {
                    static const char storm_msg[]="CPU watch: storm limit reached; the page stays "
                                                 "writable for the rest of the run\n";
                    platform_write_stdout(storm_msg,sizeof(storm_msg)-1);
                }
                { DWORD old=0;
                  VirtualProtect((void *)page,4096,PAGE_READWRITE,&old); }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            cpu_watch_reprotect=page;
            exception->ContextRecord->EFlags |= 0x100u; /* Trap Flag: #DB after one instruction */
            { DWORD old=0;
              VirtualProtect((void *)page,4096,PAGE_READWRITE,&old); }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    /* The pool's page-protection trap (BB_POOL_WATCH=1): the page was made read-only, so the
     * write into it is an access violation, and the violation names the writer - the place, the
     * module, the thread - whatever thread it came from. Exactly one store is let through (the
     * page is made writable and the thread's Trap Flag is set); the resulting single-step below
     * re-protects the page read-only and re-arms the trap, so the write is named and the page is
     * closed again within a single instruction - the disarm window the old code left is gone. */
    if ((code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_IN_PAGE_ERROR) &&
        atomic_load(&pool_trap_on)) {
        const EXCEPTION_RECORD *record=exception->ExceptionRecord;
        const int is_write=(record->NumberParameters>=2 && record->ExceptionInformation[0]==1);
        const uintptr_t at=(record->NumberParameters>=2)
                               ? (uintptr_t)record->ExceptionInformation[1] : 0;
        const uintptr_t page=(uintptr_t)pool_watch_page;
        if (is_write && at>=page && at<page+4096) {
            pool_trap_note((uintptr_t)exception->ContextRecord->Rip,at);
            const long seen=atomic_fetch_add(&pool_trap_storm,1)+1;
            if (seen>POOL_TRAP_STORM_LIMIT) {
                /* Too many faults: stop re-protecting each store, leave it writable and let the
                 * watch thread re-arm (the old behaviour). The writer has already been named. */
                if (seen==POOL_TRAP_STORM_LIMIT+1)
                    pool_watch_say("Pool trap: guest page faulted %ld times; switching to "
                                   "disarm-and-poll re-arm for the rest of the run\n",seen);
                atomic_store(&pool_trap_on,0);
                { DWORD old=0;
                  VirtualProtect((void *)page,4096,PAGE_READWRITE,&old); }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            /* Let exactly this one store through, then re-protect on the single-step below. */
            pool_trap_reprotect_guest=page;
            exception->ContextRecord->EFlags |= 0x100u; /* Trap Flag: #DB after one instruction */
            { DWORD old=0;
              VirtualProtect((void *)page,4096,PAGE_READWRITE,&old); }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    /* The same trap on the host's own view of those bytes: the store that never touches the
     * guest's page is named here, and the page is given back the way the runtime has it. */
    if ((code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_IN_PAGE_ERROR) &&
        atomic_load(&pool_trap_backing_on)) {
        const EXCEPTION_RECORD *record=exception->ExceptionRecord;
        const int is_write=(record->NumberParameters>=2 && record->ExceptionInformation[0]==1);
        const uintptr_t at=(record->NumberParameters>=2)
                               ? (uintptr_t)record->ExceptionInformation[1] : 0;
        const uintptr_t page=(uintptr_t)pool_trap_backing_page;
        if (is_write && page && at>=page && at<page+4096) {
            pool_trap_backing_note((uintptr_t)exception->ContextRecord->Rip,at);
            const long seen=atomic_fetch_add(&pool_trap_storm,1)+1;
            if (seen>POOL_TRAP_STORM_LIMIT) {
                if (seen==POOL_TRAP_STORM_LIMIT+1)
                    pool_watch_say("Pool trap: host page faulted %ld times; switching to "
                                   "disarm-and-poll re-arm for the rest of the run\n",seen);
                atomic_store(&pool_trap_backing_on,0);
                { DWORD old=0;
                  VirtualProtect((void *)page,4096,PAGE_READWRITE,&old); }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            pool_trap_reprotect_backing=page;
            exception->ContextRecord->EFlags |= 0x100u;
            { DWORD old=0;
              VirtualProtect((void *)page,4096,PAGE_READWRITE,&old); }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    /* The same trap on any other view of the pool's bytes the sweep found (a third view: a mapping
     * of the pool's section at an address the host picked). A store through it is a store onto the
     * pool that neither of the two names this watch holds would ever see - which is exactly how the
     * node's words can change with both known pages read-only and no write named. The page is given
     * back the way the runtime has it, so the game runs on. */
    if ((code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_IN_PAGE_ERROR) &&
        !atomic_load(&pool_extra_hits)) {
        const EXCEPTION_RECORD *record=exception->ExceptionRecord;
        const int is_write=(record->NumberParameters>=2 && record->ExceptionInformation[0]==1);
        const uintptr_t at=(record->NumberParameters>=2)
                               ? (uintptr_t)record->ExceptionInformation[1] : 0;
        if (is_write)
            for (int i=0;i<atomic_load(&pool_extra_count);++i) {
                const uintptr_t page=pool_extra_views[i].page;
                if (!page || at<page || at>=page+0x4000) continue;
                pool_extra_note((uintptr_t)exception->ContextRecord->Rip,at,page);
                { DWORD old=0;
                  VirtualProtect((void *)page,0x4000,PAGE_READWRITE,&old); }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
    }
    /* A write into either of the two pages that neither trap took: the page was left read-only by
     * something else, so it is said before the runtime's own recovery gives it back. */
    if (code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_IN_PAGE_ERROR) {
        const EXCEPTION_RECORD *record=exception->ExceptionRecord;
        const int is_write=(record->NumberParameters>=2 && record->ExceptionInformation[0]==1);
        const uintptr_t at=(record->NumberParameters>=2)
                               ? (uintptr_t)record->ExceptionInformation[1] : 0;
        const uintptr_t guest=(uintptr_t)pool_watch_page;
        const uintptr_t backing=(uintptr_t)pool_trap_backing_page;
        const int in_guest=guest && at>=guest && at<guest+4096;
        const int in_backing=backing && at>=backing && at<backing+4096;
        if (is_write && (in_guest || in_backing)) {
            static _Atomic unsigned notes;
            const unsigned n=atomic_fetch_add(&notes,1)+1;
            if (n<=8 || !(n&(n-1)))
                pool_watch_say("Pool watch: a write into the pool's %s page (%#llx) faulted while "
                               "the trap was off (%u so far)\n",
                               in_guest?"guest":"host view",(unsigned long long)at,n);
        }
    }
    /* The exception's own view: the address it was about, and the context it carries. */
    PlatformContext uc0 = { exception, exception->ContextRecord };

    /* A fault the runtime can resolve itself is resolved before the report, because the
     * report ends the game. A page the GPU page tracking left write-protected is such a
     * fault: the guest's own protection allows the write (the report says which region it
     * is and what protection it has), and the POSIX build resolves it before the report
     * too. Here the report came first, so the recovery was dead code, and a write of the
     * game into a page the tracker had protected ended the game instead of being let
     * through (guest+0x66b346: the game's own allocator writing into its heap).
     *
     * Only the protection of the page is put back: the thread's context is left alone, so
     * the thread resumes exactly where it faulted. (The GPU library's own resolution is not
     * used here: on Windows it expects the exception's pointers in one place and the
     * context in another, and it rewrites the context to retry the instruction - driven
     * from this handler it faulted inside itself, and the handler called itself until the
     * thread's stack was gone.) */
    if (code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_IN_PAGE_ERROR) {
        /* BB_FAULT_RECOVER=0 turns the recovery off (the fault ends the game as before). */
        static int recover=-1;
        if (recover<0) { const char *e=getenv("BB_FAULT_RECOVER"); recover=!(e && e[0]=='0'); }
        if (recover) {
            const uintptr_t where=(uintptr_t)platform_context_fault_address(&uc0);
            /* The page gets the guest's own protection back: the write the guest is doing
             * is one that protection allows. */
            if (runtime_memory_restore_access(exception, where, 0))
                return EXCEPTION_CONTINUE_EXECUTION;
            {   /* The ones that were not resolved are said out loud: which page faulted,
                 * what the guest's own protection of it is, and which thread it was. */
                static unsigned notes;
                const unsigned n=__atomic_fetch_add(&notes,1,__ATOMIC_RELAXED)+1;
                if (n<=8 || !(n&(n-1))) {
                    char note[256];
                    const uintptr_t rip=(uintptr_t)platform_context_rip(&uc0);
                    char region[192];
                    char thread[32]="?";
                    platform_thread_name_get(thread,sizeof(thread));
                    runtime_memory_describe(where,region,sizeof(region));
                    int length=snprintf(note,sizeof(note),
                        "Runtime: access violation at %#llx (rip %#llx) in thread %s: %s (%u so far)\n",
                        (unsigned long long)where,(unsigned long long)rip,thread,region,n);
                    if (length>0) platform_write_stdout(note,(size_t)length);
                    /* The registry's state: how many objects are in it and what the next
                     * id is. The ids the linker hands out start at 0x1001, so a registry
                     * that is empty says that no entry was ever converted. */
                    dump_registry_r1(0);
                }
            }
        }
    }

    /* The first line is written with almost no stack: a stack overflow leaves the
     * thread nearly without one, and the report has to survive it (the rest of the
     * report needs a kilobyte of stack and would take the process down with it). */
    {
        static char first[200];
        static const char digits[]="0123456789abcdef";
        char *at=first;
        const char *label="Fault: code 0x";
        while (*label) *at++=*label++;
        for (int shift=28;shift>=0;shift-=4) *at++=digits[(code>>shift)&0xf];
        const uintptr_t rip=(uintptr_t)exception->ContextRecord->Rip;
        const uintptr_t where=(uintptr_t)exception->ExceptionRecord->ExceptionInformation[1];
        const char *what=" rip 0x";
        while (*what) *at++=*what++;
        for (int shift=60;shift>=0;shift-=4) *at++=digits[(rip>>shift)&0xf];
        const char *address=" at 0x";
        while (*address) *at++=*address++;
        for (int shift=60;shift>=0;shift-=4) *at++=digits[(where>>shift)&0xf];
        if (rip-(uintptr_t)image<0x10000000) {
            const char *in=" (guest+0x";
            while (*in) *at++=*in++;
            for (int shift=60;shift>=0;shift-=4) *at++=digits[((rip-(uintptr_t)image)>>shift)&0xf];
            *at++=')';
        }
        {   /* The module the place is in: the base and the offset in it. The module
             * handle is asked for (it is a few words of stack, the report has to
             * survive a thread that has hardly any stack left). */
            HMODULE module=NULL;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)rip,&module)) {
                const char *mod=" mod 0x";
                while (*mod) *at++=*mod++;
                for (int shift=60;shift>=0;shift-=4) *at++=digits[(((uintptr_t)module)>>shift)&0xf];
                const char *off="+0x";
                while (*off) *at++=*off++;
                for (int shift=60;shift>=0;shift-=4) *at++=digits[((rip-(uintptr_t)module)>>shift)&0xf];
                /* The name of the module, so the place can be looked up in it. */
                static char path[MAX_PATH];
                const DWORD length=GetModuleFileNameA(module,path,sizeof(path));
                if (length && length<sizeof(path)) {
                    const char *name=" ";
                    while (*name) *at++=*name++;
                    for (DWORD i=0;i<length;++i) *at++=path[i];
                }
            }
            /* The program's own module: the offset in it is what the source can be
             * looked up by (the module the place is in may be a library). */
            { const uintptr_t own=(uintptr_t)GetModuleHandle(NULL);
              const char *self=" self 0x";
              while (*self) *at++=*self++;
              for (int shift=60;shift>=0;shift-=4) *at++=digits[(own>>shift)&0xf];
              const char *at2="+0x";
              while (*at2) *at++=*at2++;
              for (int shift=60;shift>=0;shift-=4) *at++=digits[((rip-own)>>shift)&0xf];
            }
        }
        *at++='\n';
        HANDLE handle=GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written=0;
        if (handle && handle!=INVALID_HANDLE_VALUE)
            WriteFile(handle,first,(DWORD)(at-first),&written,NULL);
    }
/* The report of the fault (the thread, its registers and its stack) is written for
     * every kind of exception but the debug ones: a program that writes to the debug
     * output raises one of those on every line (the graphics driver's overlay does it
     * constantly), and they say nothing about a fault. */
    { const DWORD quiet[]={0x40010006u,0x4001000au,0x406D1388u};
      int say=1;
      for (size_t q=0;q<sizeof(quiet)/sizeof(*quiet);++q) if (code==quiet[q]) say=0;
      if (say) {
          PlatformContext uc = { exception, exception->ContextRecord };
          report_fault(exception, (void *)platform_context_fault_address(&uc), (int)code);
      } }
    /* The guest's code is counted (the single-step counter's int3s). */
    if (runtime_count_trap(exception)) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
/* BB_CPU_WATCH=<image offset>[:<length>] (diagnostics): the game's code that writes those bytes of
 * its image (data, bss). Their page is write-protected; a write anywhere in it is let through by
 * single-stepping that one instruction (the page is closed again after it); writes into the range
 * are logged with their code offset (the first 64).
 *
 * BB_GUEST_WATCH=<guest address>[:<length>][;<guest address>...] watches a page of the guest's
 * own memory the same way. The guest's address space is the host's, so a guest address is a host
 * address and its page can be protected the same way. This is how the game's heap can be watched:
 * the address of the block that was corrupted is not known before the run that corrupts it, but
 * once a run has said where it was, the next run can watch exactly that place. The copies the
 * runtime does on the game's behalf (memset, memcpy, memmove) are named as well
 * (bb_cpu_copy_note), which catches the bulk of the writes without touching the protection. */
#ifndef _WIN32
static void cpu_watch_step(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    ucontext_t *uc = context;
    uc->uc_mcontext.gregs[REG_EFL] &= ~0x100ll;
    if (cpu_watch_open) mprotect((void *)cpu_watch_open, 4096, PROT_READ);
    cpu_watch_open = 0;
}
#endif
static void cpu_watch_install(void) {
#ifndef _WIN32
    { struct sigaction step = {0};
      step.sa_sigaction = cpu_watch_step; step.sa_flags = SA_SIGINFO;
      sigaction(SIGTRAP, &step, NULL); }
#endif
    /* BB_CPU_WATCH: an offset in the image. */
    const char *env = getenv("BB_CPU_WATCH");
    if (env && *env) {
        char *end = NULL;
        const unsigned long long at = strtoull(env, &end, 16);
        const unsigned long long length = end && *end == ':' ? strtoull(end + 1, NULL, 0) : 4;
        if (cpu_watch_count < CPU_WATCH_PAGES) {
            CpuWatch *w = &cpu_watch[cpu_watch_count++];
            w->page = image + (at & ~0xfffull);
            w->lo = (uintptr_t)(image + at);
            w->hi = w->lo + length;
            w->is_guest = 0;
            { extern uintptr_t runtime_cpu_watch_lo, runtime_cpu_watch_hi;
              runtime_cpu_watch_lo = w->lo; runtime_cpu_watch_hi = w->hi; }
            platform_protect(w->page, 4096, BB_PROT_READ);
            printf("CPU watch: writes to image+%#llx (%llu bytes)\n", at, length);
        }
    }
    /* BB_GUEST_WATCH: addresses in the guest's own memory (';' separates them). */
    const char *guest = getenv("BB_GUEST_WATCH");
    if (guest && *guest) {
        char buffer[512];
        snprintf(buffer,sizeof(buffer),"%s",guest);
        char *cursor = buffer;
        while (cursor && *cursor) {
            char *sep = strchr(cursor, ';');
            if (sep) *sep = 0;
            char *end = NULL;
            const unsigned long long at = strtoull(cursor, &end, 16);
            const unsigned long long length = (end && *end == ':') ? strtoull(end + 1, NULL, 0) : 8;
            if (at && cpu_watch_count < CPU_WATCH_PAGES) {
                CpuWatch *w = &cpu_watch[cpu_watch_count++];
                w->page = (unsigned char *)((uintptr_t)at & ~(uintptr_t)0xfffull);
                w->lo = (uintptr_t)at;
                w->hi = w->lo + length;
                w->is_guest = 1;
                { extern uintptr_t runtime_guest_watch_lo, runtime_guest_watch_hi;
                  if (!runtime_guest_watch_lo || w->lo < runtime_guest_watch_lo) runtime_guest_watch_lo = w->lo;
                  if (w->hi > runtime_guest_watch_hi) runtime_guest_watch_hi = w->hi; }
                const int protected_ok = platform_protect(w->page, 4096, BB_PROT_READ);
                printf("CPU watch: writes to guest+%#llx (%llu bytes), page %p %s\n",
                       at, length, (void *)w->page, protected_ok ? "protected" : "NOT PROTECTED");
            }
            cursor = sep ? sep + 1 : NULL;
        }
    }
}
#ifndef _WIN32
static void fault(int sig, siginfo_t *info, void *context) {
    if (cpu_watch_count && sig == SIGSEGV) {
        for (int w=0;w<cpu_watch_count;++w) {
            const unsigned char *page = cpu_watch[w].page;
            if (!page || (unsigned char *)info->si_addr < page ||
                (unsigned char *)info->si_addr >= page + 4096) continue;
            ucontext_t *uc = context;
            const uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
            const uintptr_t addr = (uintptr_t)info->si_addr;
            if (addr + 64 > cpu_watch[w].lo && addr < cpu_watch[w].hi && cpu_watch_hits < 64) {
                char line[160];
                snprintf(line, sizeof(line), "CPU watch: write to %s+%#lx from guest+%#lx\n",
                         cpu_watch[w].is_guest ? "guest" : "image",
                         (unsigned long)(addr - (cpu_watch[w].is_guest ? 0 : (uintptr_t)image)),
                         (unsigned long)(rip - (uintptr_t)image));
                { ssize_t w2 = write(1, line, strlen(line)); (void)w2; }
                ++cpu_watch_hits;
            }
            mprotect((void *)page, 4096, PROT_READ | PROT_WRITE);
            cpu_watch_open = (uintptr_t)page;
            uc->uc_mcontext.gregs[REG_EFL] |= 0x100;
            return;
        }
    }
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (gpu_enabled && sig == SIGSEGV && bbgpu_handle_fault(context, info->si_addr)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    report_fault(context, info->si_addr, sig);
}
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)uc->uc_mcontext.gregs[REG_RIP], rbp=(uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,(uint64_t)uc->uc_mcontext.gregs[REG_RDI]);
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    /* The registers a guest wait loop keeps its pointers in (a polled GPU label, its object). */
    { static const int regs[]={REG_RAX,REG_RBX,REG_RCX,REG_RDX,REG_RSI,REG_R12,REG_R13,REG_R14,REG_R15};
      static const char names[][4]={"rax","rbx","rcx","rdx","rsi","r12","r13","r14","r15"};
      char text[]="  reg=0000000000000000\n";
      for (unsigned i=0;i<sizeof(regs)/sizeof(*regs);++i) {
          memcpy(text+2,names[i],3); write_hex(text+6,(uint64_t)uc->uc_mcontext.gregs[regs[i]]);
          ssize_t written_=write(2,text,sizeof(text)-1); (void)written_;
      } }
    /* The thread's name (/proc/self/task/<tid>/comm; open and read are async-signal-safe). */
    { char path[]="/proc/self/task/0000000000/comm", name[40]="  name=";
      uint64_t v=tid; for (int i=25;i>=16;--i) { path[i]=(char)('0'+v%10); v/=10; }
      int start=16; while (path[start]=='0' && start<25) ++start;
      memmove(path+16,path+start,sizeof(path)-start);
      int fd=open(path,O_RDONLY);
      if (fd>=0) { ssize_t n=read(fd,name+7,sizeof(name)-8); close(fd); if (n>0) { ssize_t w=write(2,name,(size_t)(7+n)); (void)w; } } }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
    /* The host side of the thread (the runtime function a guest thread waits in): unwound
     * through the signal frame by the host's unwind tables, up to the first guest frame. */
    void *frames[16];
    int depth=backtrace(frames,16);
    for (int i=2;i<depth;++i) {
        Dl_info where;
        char text[256];
        if (dladdr(frames[i],&where) && where.dli_fname)
            snprintf(text,sizeof(text),"  host #%d %s+0x%lx (%s)\n",i,strrchr(where.dli_fname,'/') ? strrchr(where.dli_fname,'/')+1 : where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i]-(uintptr_t)where.dli_fbase),where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(text,sizeof(text),"  host #%d guest+0x%lx\n",i,(unsigned long)((uintptr_t)frames[i]-(uintptr_t)image));
        ssize_t w=write(2,text,strlen(text)); (void)w;
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
    dump_frames(context);
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
    usleep(100000);
    _exit(128 + sig);
}
#endif
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
/* The guest reads its thread pointer with `mov rax, gs:[0]` (the loader turns the game's
 * `fs:[0]` into it, because the C library of the host owns FS). On Linux the runtime
 * points the GS base at the guest's thread block, so the read gives the right thing.
 * Windows keeps the GS base for itself: GS:[0] is the kernel's exception list, and the
 * guest read that instead (nothing), then used it as a pointer.
 *
 * Windows' thread block has a word of its own for the program (ArbitraryUserPointer);
 * the runtime keeps the guest's thread block pointer in it. The read is pointed there
 * instead, which is the same length, so nothing else moves. */
static void rewrite_tls_reads(unsigned char *image, uint64_t size) {
#ifdef _WIN32
    /* mov rax, gs:[disp32]: 65 48 8b 04 25 <disp32> */
    static const unsigned char load[]={0x65,0x48,0x8b,0x04,0x25};
    const uint32_t slot=runtime_tls_guest_index();
    unsigned patched=0;
    for (uint64_t at=0;at+9<=size;++at) {
        if (memcmp(image+at,load,sizeof(load))) continue;
        uint32_t offset;
        memcpy(&offset,image+at+5,4);
        if (offset) continue; /* not the thread pointer read */
        memcpy(image+at+5,&slot,4);
        ++patched;
    }
    if (patched) printf("Runtime: the guest's thread pointer reads point at the thread's own word (%u places)\n",patched);
#else
    (void)image; (void)size;
#endif
}
/* BBPATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"BBPATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
volatile int runtime_restarting;
void runtime_restart(void) {
    fflush(NULL);
    puts("Runtime: restarting through run.sh");
    /* The GPU threads still run until exec: their Vulkan calls fail once the device fd is
     * closed below, and an assertion there must not end the process (exit 23) before exec. */
    runtime_restarting = 1;
    __sync_synchronize();
#ifndef _WIN32
    syscall(SYS_close_range, 3u, ~0u, 0u);
    execlp("bash", "bash", "run.sh", (char *)NULL);
    perror("runtime_restart: exec");
    _exit(1);
#endif
}

#ifdef _WIN32
/* The kernel's own call for the start address of a thread (the headers of the host do
 * not have the newer wrapper). */
typedef LONG (NTAPI *NtQueryThreadStartFn)(HANDLE, ULONG, void*, ULONG, ULONG*);
static NtQueryThreadStartFn thread_start_of(void) {
    static NtQueryThreadStartFn fn;
    if (!fn) {
        HMODULE ntdll=GetModuleHandleA("ntdll.dll");
        fn=(NtQueryThreadStartFn)(ntdll?GetProcAddress(ntdll,"NtQueryInformationThread"):NULL);
    }
    return fn;
}
#ifdef _WIN32
/* Whether an address is in the program itself (the game's threads) or in the system. */
static uintptr_t watchdog_image_lo=0, watchdog_image_hi=0;
void watchdog_set_image(uintptr_t lo,uintptr_t hi) { watchdog_image_lo=lo; watchdog_image_hi=hi; }
static int watchdog_in_program(uintptr_t address) {
    static uintptr_t lo=0, hi=0;
    if (!lo) {
        HMODULE base=GetModuleHandleA(NULL);
        if (!base) return 0;
        lo=(uintptr_t)base;
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery((void *)base,&info,sizeof(info))==sizeof(info)) hi=lo+info.RegionSize;
        else hi=lo+(1u<<20);
    }
    if (address>=watchdog_image_lo && address<watchdog_image_hi) return 1;
    return address>=lo && address<hi;
}
/* The number of threads there are (Windows): the tool help knows them all. */
static unsigned thread_count(void) {
    unsigned count=0;
    HANDLE all=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if (all!=INVALID_HANDLE_VALUE) {
        THREADENTRY32 one;
        one.dwSize=sizeof(one);
        if (Thread32First(all,&one)) do { ++count; } while (Thread32Next(all,&one));
        CloseHandle(all);
    }
    return count;
}
#define THREAD_COUNT_NOTE(where) fprintf(stderr,"Threads %s: %u\n",where,thread_count())
#else
#define THREAD_COUNT_NOTE(where)
#endif
/* The watchdog (Windows): the game can wait for something that never comes, and then
 * nothing is said. The threads are looked at after the time is up: where each of them
 * is, and (for the ones that are in the game's own code) which place of the game. */
static volatile int watchdog_fired;
static void *watchdog_image;
static unsigned watchdog_seconds;
/* Where the game's threads are (the watchdog's answer): the place each thread was in when
 * it was looked at, counted. A thread that spins in the game shows up with one place over
 * and over; a thread that waits shows up all over the place. */
typedef struct { uintptr_t rip; unsigned count; char name[32]; } ThreadPlace;
static ThreadPlace thread_places[256];
static unsigned thread_place_count;
static void note_thread_place(const char *name, uintptr_t rip) {
    for (unsigned i=0;i<thread_place_count;++i)
        if (thread_places[i].rip==rip) { ++thread_places[i].count; return; }
    if (thread_place_count>=sizeof(thread_places)/sizeof(*thread_places)) return;
    ThreadPlace *p=&thread_places[thread_place_count++];
    p->rip=rip; p->count=1;
    snprintf(p->name,sizeof(p->name),"%s",name?name:"?");
}
static void report_thread_places(FILE *out) {
    if (!thread_place_count) return;
    for (unsigned i=0;i<thread_place_count;++i)
        for (unsigned j=i;j>0 && thread_places[j].count>thread_places[j-1].count;--j) {
            ThreadPlace t=thread_places[j]; thread_places[j]=thread_places[j-1]; thread_places[j-1]=t;
        }
    fprintf(out,"  where the threads were (the most looked-at places):\n");
    for (unsigned i=0;i<thread_place_count && i<16;++i) {
        const uintptr_t rip=thread_places[i].rip;
        fprintf(out,"    %-28s %5u times",thread_places[i].name,thread_places[i].count);
        if (watchdog_image && rip>=(uintptr_t)watchdog_image && rip<(uintptr_t)watchdog_image+0x80000000)
            fprintf(out," image+%#llx",(unsigned long long)(rip-(uintptr_t)watchdog_image));
        fprintf(out,"\n");
    }
}
static DWORD WINAPI watchdog_thread(LPVOID parameter) {
    (void)parameter;
    const unsigned interval=watchdog_seconds?watchdog_seconds:10;
    FILE *out=fopen("watchdog.txt","w");
    if (!out) out=stderr;
    /* The registry's node pool: its free list is threaded through the nodes themselves, so
     * a node whose memory is recycled (zeroed) breaks the chain. The register function then
     * hands out ids without ever putting a node in the table, and every lookup of one of
     * them finds nothing. The watch shows the chain and the counters as they change. */
    const int watch_r1 = getenv("BB_R1_WATCH") != NULL;
    const int watch_pool = getenv("BB_POOL_WATCH") != NULL;
    unsigned last_used = 0xffffffffu, last_next = 0xffffffffu;
    uint64_t last_head = ~0ull, last_link = 0, last_link_was = 0;
    for (unsigned round=0;;++round) {
        if (watch_r1 || watch_pool) {
            uint64_t head = 0, next = 0;
            platform_read_memory((void *)((uintptr_t)image + 0x160b688), &head, 8);
            if (head) platform_read_memory((void *)(uintptr_t)head, &next, 8);
            uint32_t used = 0, next_id = 0;
            platform_read_memory((void *)((uintptr_t)image + 0x160b694), &used, 4);
            platform_read_memory((void *)((uintptr_t)image + 0x160b600), &next_id, 4);
            if (used != last_used || next_id != last_next || head != last_head) {
                last_used = used;
                last_next = next_id;
                last_head = head;
                char note[192];
                int length = snprintf(note, sizeof(note),
                    "Runtime: R1 free_head=%#llx head.next=%#llx used=%#x next_id=%#x\n",
                    (unsigned long long)head, (unsigned long long)next, used, next_id);
                if (length > 0) platform_write_stdout(note, (size_t)length);
                /* The head node's own words: the chain is threaded through them, so they
                 * say whether the pool still holds what the init wrote. */
                if (head) {
                    uint64_t words[4] = {0,0,0,0};
                    if (platform_read_memory((void *)(uintptr_t)head, words, sizeof(words)) == 0) {
                        length = snprintf(note, sizeof(note),
                            "    head node %#llx: %#llx %#llx %#llx %#llx\n",
                            (unsigned long long)head, (unsigned long long)words[0],
                            (unsigned long long)words[1], (unsigned long long)words[2],
                            (unsigned long long)words[3]);
                        if (length > 0) platform_write_stdout(note, (size_t)length);
                    }
                }
            }
            /* The chain's head word on its own: it is the first thing that goes. */
            if (head) {
                uint64_t link = 1;
                if (platform_read_memory((void *)(uintptr_t)head, &link, 8) == 0 && link != last_link) {
                    last_link = link;
                    char note[128];
                    int length = snprintf(note, sizeof(note),
                        "Runtime: R1 chain head word = %#llx (was %#llx)\n",
                        (unsigned long long)link, (unsigned long long)last_link_was);
                    if (length > 0) platform_write_stdout(note, (size_t)length);
                    last_link_was = link;
                    /* bbport: the pool's free-list just broke (or restored). Dump the ring of
                     * recent mapping operations (with guest call sites) so the zeroing/remapping
                     * of the pool's physical range (0x4900000..) is named - the pool watch's own
                     * erasure detector can miss a transient one. Fire on the broken state (link 0)
                     * so it also catches a chain that was already broken at the first sample. */
                    static _Atomic int r1_dumped;
                    if (link == 0 && !atomic_load(&r1_dumped)) {
                        atomic_store(&r1_dumped,1);
                        char ops[6000];
                        if (runtime_memory_op_dump(ops,sizeof ops)>0) {
                            platform_write_stdout(
                                "Runtime: R1 chain break - last memory operations:\n", 46);
                            platform_write_stdout(ops, (size_t)strlen(ops));
                        }
                        char dg[200];
                        runtime_memory_describe(0x800410040u,dg,sizeof dg);
                        platform_write_stdout("Runtime: at R1 break, pool VA 0x800410040 -> ", 42);
                        platform_write_stdout(dg,(size_t)strlen(dg));
                        platform_write_stdout("\n",1);
                        /* bbport: report which breakpoint/trap caught a write into the pool, as a
                         * fallback in case the single-step print was rate-limited. A non-zero host
                         * count means the erasure was a store through the runtime's backing view. */
                        char fb[256];
                        int fbn=snprintf(fb,sizeof fb,
                            "Runtime: R1 break trap tallies: guest=%ld host=%ld extra=%ld "
                            "(writes observed=%ld)\n",
                            (long)atomic_load(&pool_trap_hits),
                            (long)atomic_load(&pool_trap_backing_hits),
                            (long)atomic_load(&pool_extra_hits),
                            (long)atomic_load(&pool_watch_writes));
                        if (fbn>0) platform_write_stdout(fb,(size_t)(fbn>=(int)sizeof fb?sizeof fb-1:fbn));
                    }
                    if (link == 0x800410060) atomic_store(&r1_dumped,0); /* re-arm if restored */
                }
            }
            Sleep(20);
            continue;
        }
        Sleep(interval*200);
        fprintf(out,"--- round %u ---\n",round);
        fflush(out); /* the round is said before the threads are looked at: a hang in
                      * the looking must not take the rounds that came before it */
        /* The threads of the program itself: the many others belong to the system. */
        HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
        unsigned count=0, mine=0;
        if (snapshot!=INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry;
            entry.dwSize=sizeof(entry);
            /* The game's own threads are asked: the runtime knows their numbers (the
             * system's nine thousand threads are not asked about). */
            uint64_t ids[256];
            char names[256][32];
            const size_t guests=runtime_thread_guest_threads(ids,names[0],32,256);
            fprintf(out,"  %zu guest threads\n",guests);
            fflush(out);
            for (size_t g=0;g<guests;++g) {
                /* What the thread waits for is asked before the thread is held: the
                 * answer comes from a lock the thread itself takes, and a thread that
                 * is held while it holds that lock never gives it back. */
                const char *waits=runtime_wait_marked_of(ids[g]);
                HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT,FALSE,(DWORD)ids[g]);
                if (!thread) continue;
                SuspendThread(thread);
                CONTEXT context;
                memset(&context,0,sizeof(context));
                context.ContextFlags=CONTEXT_CONTROL;
                if (GetThreadContext(thread,&context)) {
                    const uintptr_t rip=(uintptr_t)context.Rip;
                    ++mine;
                    note_thread_place(names[g],rip);
                    fprintf(out,"  guest '%s' tid=%llu rip=%p",names[g],(unsigned long long)ids[g],(void *)rip);
                    if (waits) fprintf(out," waits for %s",waits);
                    if (watchdog_image)
                        fprintf(out," image+%#llx",
                                (unsigned long long)(rip-(uintptr_t)watchdog_image));
                    {   /* The module the place is in. */
                        HANDLE modules=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,
                                                                 GetCurrentProcessId());
                        if (modules!=INVALID_HANDLE_VALUE) {
                            MODULEENTRY32 module;
                            module.dwSize=sizeof(module);
                            if (Module32First(modules,&module)) do {
                                if (rip>=(uintptr_t)module.modBaseAddr &&
                                    rip<(uintptr_t)module.modBaseAddr+module.modBaseSize) {
                                    fprintf(out," in %s+%#llx",module.szModule,
                                            (unsigned long long)(rip-(uintptr_t)module.modBaseAddr));
                                    break;
                                }
                            } while (Module32Next(modules,&module));
                            CloseHandle(modules);
                        }
                    }
                    fprintf(out,"\n");
                }
                ResumeThread(thread);
                CloseHandle(thread);
            }
            CloseHandle(snapshot);
        }
        fprintf(out,"  %u threads in all, %u of them the program's\n",count,mine);
        if (round%10==9) report_thread_places(out);
        fflush(out);
    }
    return 0;
}

#endif

#ifdef _WIN32
/* The way the process ends is reported: the game can end in many ways, and the one
 * that happened is what says where it stopped. */
static void report_exit(void) {
    fflush(stdout);
    fputs("Runtime: the process is ending\n",stderr);
    fflush(stderr);
}
#endif
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    atexit(report_exit);
#endif
#ifndef _WIN32
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
    /* The launcher's controls page: the input devices that are there, and the buttons of
     * one of them. The GPU library reads the pads, so it is asked. */
    if (argc >= 2 && !strcmp(argv[1], "--input-devices")) {
        char buffer[64*512];
        const int length=bbgpu_input_devices(buffer,sizeof buffer);
        if (length<0) return 1;
        fwrite(buffer,1,(size_t)length,stdout);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--input-buttons")) {
        char buffer[4096];
        const int length=bbgpu_input_buttons(argv[2],buffer,sizeof buffer);
        if (length<0) return 1;
        fwrite(buffer,1,(size_t)length,stdout);
        return 0;
    }
    THREAD_COUNT_NOTE("at the start");
    check_cpu();
    int cpu_only = 0, strict_imports = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--timeout seconds] | --vulkan-only\n", argv[0]);
        return 1;
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"BBCONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
        char serial[16]="", title[128]="", version[16]="";
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"APP_VER",version,sizeof(version),NULL);
        runtime_net_configure(app0,serial,title,version);
    }
    bbgpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    /* The fault handler: the vectored exception handler sees the access violations, the illegal
     * instructions and the breakpoints before the process is torn down. */
    AddVectoredExceptionHandler(1, fault_handler);
    /* The modules of the process, so a fault's place can be looked up in one of them. */
    if (getenv("BB_LIST_MODULES")) {
        HMODULE modules[256];
        DWORD count=GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,NULL,NULL);
        (void)count;
        HMODULE *list=(HMODULE *)malloc(sizeof(modules));
        DWORD needed=0;
        if (list && EnumProcessModules(GetCurrentProcess(),list,sizeof(modules),&needed)) {
            printf("Modules (%lu):\n",(unsigned long)needed);
            for (DWORD i=0;i<needed/sizeof(HMODULE);++i) {
                char path[MAX_PATH]="?";
                GetModuleFileNameA(list[i],path,sizeof(path));
                const char *slash=strrchr(path,'\\');
                printf("  %p %s\n",(void *)list[i],slash?slash+1:path);
            }
        }
        free(list);
    }
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    { const char *w = getenv("BB_FAULT_WAIT"); if (w && !strcmp(w, "1")) prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0); }
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
#endif
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "BBPROBE1", 8) && memcmp(magic, "BBPROBE2", 8) && memcmp(magic,"BBPROBE3",8) && memcmp(magic,"BBPROBE4",8) && memcmp(magic,"BBPROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "BBPROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    THREAD_COUNT_NOTE("after the runtime starts");
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"BBPROBE5",8);
    int linked=!memcmp(magic,"BBPROBE3",8) || !memcmp(magic,"BBPROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* BBPROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"BBPROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    /* What the linked library of the game exports: the same symbol can be imported by
     * more than one module (the library itself and the module of the system's C library
     * both want malloc, for instance), and the module that imports it is not the one
     * that has it. The export is found by its name, so an import the runtime does not
     * provide is still answered - the game's own linker looks the symbol up by name too,
     * and it found nothing (the module's import was then resolved to nothing, and the
     * game died dereferencing it). */
    struct { const char *name; uint64_t address; int kind; } exported[512];
    unsigned exported_count=0;
    if (native_libc) {
        for (uint64_t i = 0; i < import_count && exported_count < sizeof(exported)/sizeof(*exported); ++i) {
            if (!bindings[i]) continue;
            const char *symbol=runtime_symbol(names[i]);
            if (!symbol) continue;
            int known=0;
            for (unsigned k = 0; k < exported_count; ++k)
                if (!strcmp(exported[k].name,symbol)) { known=1; break; }
            if (known) continue;
            exported[exported_count].name=symbol;
            exported[exported_count].address=bindings[i];
            exported[exported_count].kind=(int)binding_kinds[i];
            ++exported_count;
        }
    }
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    image = allocate_image(size);
    image_bytes = size;
    runtime_trace_set_image(image);
    { /* link_modules.py's unwind.txt beside the boot image: the modules' unwind tables */
        char unwind[4096];
        snprintf(unwind,sizeof(unwind),"%s",argv[1]);
        /* The folder of the path, whichever way it is written (Windows uses the backslash). */
        char *slash=NULL;
        for (char *at=unwind;*at;++at) if (*at=='/' || *at=='\\') slash=at;
        snprintf(slash ? slash+1 : unwind,sizeof(unwind)-(size_t)(slash ? slash+1-unwind : 0),"unwind.txt");
        runtime_process_load_unwind(unwind,image);
    }
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (!cpu_only) {
        char title[128]="MAXIBOOST ON", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        BbGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (bbgpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,bbgpu_symbol_count());
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    import_slots = calloc(nr ? nr : 1, sizeof(*import_slots));
    if (!import_slots) fail("allocation failed");
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        uintptr_t handler = (uintptr_t)unresolved;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            if (resolved && relocs[i].kind == 1) /* BB_IMPORT_TRACE: a logging thunk in between */
                resolved = runtime_trace_wrap(runtime_symbol(names[relocs[i].value]), resolved);
            if (resolved) value = resolved + relocs[i].addend;
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
            } else if (native_libc) {
                /* The same symbol, imported by another module: the library of the game
                 * exports it, so it is taken from there. */
                const char *symbol=runtime_symbol(names[relocs[i].value]);
                for (unsigned k = 0; symbol && k < exported_count; ++k) {
                    if (strcmp(exported[k].name,symbol) || exported[k].kind!=(int)relocs[i].kind) continue;
                    if (!mapped(segments,ns,exported[k].address,relocs[i].addend+1)) break;
                    value=(uintptr_t)image+exported[k].address+relocs[i].addend;
                    break;
                }
            }
        }
        memcpy(image + relocs[i].target, &value, 8);
        if (relocs[i].kind) {
            /* The slot is remembered: the guest's own linker overwrites it when it
             * resolves the import, and a slot that is still empty when the game calls
             * through it is the NULL call the fault report has to name. */
            if (import_slot_count < nr) {
                import_slots[import_slot_count].target = relocs[i].target;
                import_slots[import_slot_count].name = relocs[i].value;
                import_slots[import_slot_count].kind = relocs[i].kind;
                ++import_slot_count;
            }
        }
        if (relocs[i].kind==1 && value>=(uintptr_t)traps &&
            value<(uintptr_t)traps+(import_count+1)*32) {
            /* The import has no address: the game will find the trap when its own
             * linker looks the symbol up (by its address), and it stops there. */
            static int reported;
            if (reported++ < 40)
                printf("IMPORT left to the trap: %s (%s)\n",names[relocs[i].value],
                       runtime_import_name(names[relocs[i].value]));
        }
        if (getenv("BB_DUMP_IMPORTS")) {
            const char *name=names[relocs[i].value];
            const char *symbol=runtime_symbol(name);
            const uintptr_t resolved=runtime_resolve(name,relocs[i].kind==2);
            printf("IMPORT %s -> %s%s (kind %d addend %#llx)%s\n",name,
                   symbol?symbol:"(no name)",resolved?" resolved":"",
                   relocs[i].kind,(unsigned long long)relocs[i].addend,
                   (relocs[i].kind==1&&!resolved&&!native_libc)?" TRAP":"");
        }
    }
    /* BB_LIST_UNRESOLVED=1: every import neither the runtime nor a linked module provides, then
     * exit; the whole list at once instead of the first one the game happens to call. */
    { const char *list=getenv("BB_LIST_UNRESOLVED");
      if (list && !strcmp(list,"1")) {
        unsigned missing=0;
        for (uint64_t i=0;i<import_count;++i) {
            if (runtime_resolve(names[i],0) || runtime_resolve(names[i],1) || (native_libc && bindings[i])) continue;
            /* The same symbol, exported by the library of the game: answered from there. */
            const char *symbol=runtime_symbol(names[i]);
            int found=0;
            for (unsigned k=0;symbol && k<exported_count;++k)
                if (!strcmp(exported[k].name,symbol)) { found=1; break; }
            if (found) continue;
            printf("UNRESOLVED %s %s\n",names[i],runtime_import_name(names[i]));
            ++missing;
        }
        printf("UNRESOLVED total: %u of %" PRIu64 " imports\n",missing,import_count);
        exit(0);
      } }
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    if (!cpu_only) bbgpu_patch_image(image, size);
    rewrite_tls_reads(image, size);
    protect(traps, round_page((import_count + 1) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
#ifdef _WIN32
    runtime_tls_init();
#endif
    runtime_count_install(image);
    cpu_watch_install();
    THREAD_COUNT_NOTE("after the modules are loaded");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        if (module_count) runtime_set_libc_module(image+modules[0].base,modules[0].size); /* libc links first */
        for (uint64_t i=0;i<import_count;++i) /* link_modules.py RUNTIME_EXPORTS */
            if (bindings[i] && binding_kinds[i]==1 && runtime_nid_is(names[i],"CC-BLMBu9-I#libc"))
                runtime_set_libc_malloc_stats(image+bindings[i]);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    THREAD_COUNT_NOTE("before the game");
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
#ifdef _WIN32
    /* The watchdog: the game can wait for something that never comes, and then nothing
     * is said at all. */
    watchdog_image=image;
    watchdog_seconds=timeout_seconds;
    {   /* The image is the game's own code: the threads that are in it are the game's. */
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(image,&info,sizeof(info))==sizeof(info))
            watchdog_set_image((uintptr_t)info.BaseAddress,(uintptr_t)info.BaseAddress+info.RegionSize);
        else watchdog_set_image((uintptr_t)image,(uintptr_t)image+64*1024*1024);
    }
    if (timeout_seconds || getenv("BB_R1_WATCH") || getenv("BB_POOL_WATCH")) {
        HANDLE watchdog=CreateThread(NULL,0,watchdog_thread,NULL,0,NULL);
        if (watchdog) CloseHandle(watchdog);
    }
    /* bbport: always relocate the R1 registry pool into its own section the instant the game
     * publishes it, so the GPU's in-place import and any discard of the shared pool section
     * cannot corrupt it. Not gated by BB_POOL_WATCH (the fix must apply without diagnostics). */
    {
        HANDLE pool_protect=CreateThread(NULL,0,pool_protect_thread,NULL,0,NULL);
        if (pool_protect) CloseHandle(pool_protect);
    }
    /* The pool's own watch: the watchdog's rounds are far too slow to catch the moment the
     * registry's free list is erased, and the pool's page is read too often for a guard page. */
    if (getenv("BB_POOL_WATCH")) {
        HANDLE pool_watch=CreateThread(NULL,0,pool_watch_thread,NULL,0,NULL);
        if (pool_watch) CloseHandle(pool_watch);
    }
#endif
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,BB_PROT_READ|BB_PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
#ifdef _WIN32
    /* The host's main thread has a stack of one megabyte, which the game overflows while it
     * starts (its own initialization is deep). The game's entry therefore runs on a thread
     * of its own with the same big stack as on the other platforms. */
    GuestMain main_thread = { image + entry, &params, (void *)guest_exit, stack };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, stack, MAIN_STACK);
    pthread_t host_thread;
    const int created=pthread_create(&host_thread,&attr,guest_main_start,&main_thread);
    pthread_attr_destroy(&attr);
    if (created) fail("cannot create the guest's main thread");
    pthread_join(host_thread,NULL);
#else
    /* 64 KiB above the entry's stack pointer stay mapped (zeros): a PS4's main thread starts below
     * the process's start-up data, and MAXIBOOST ON's main thread reads ~0x340 bytes above where
     * it was entered (guest+0x192b10, once signed in to PSN). */
    enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-0x10000);
#endif
    fail("entry unexpectedly returned");
}
