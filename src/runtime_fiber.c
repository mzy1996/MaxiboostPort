/* libSceFiber: user-mode fibers (GUNDAM VERSUS' job system runs on them).
 *
 * A fiber is the game's own SceFiber object (256 bytes, 8-aligned); the runtime keeps its state
 * in it: signatures, state, entry, arguments, its stack (the game's context memory) and, while it
 * is not running, the stack pointer where it stopped. A switch saves the callee-saved registers,
 * MXCSR and the x87 control word on the stack it leaves and restores them from the one it enters
 * (fiber_swap), so a fiber continues exactly where it called Switch or ReturnToThread.
 *
 * sceFiberRun is called by a thread (not a fiber): the thread's context is kept in the thread's
 * state; ReturnToThread goes back to it, and Run returns. A fiber that was left with
 * ReturnToThread may be run again later by any thread (job systems move fibers between workers),
 * so nothing about the thread is kept in the fiber. A fiber's entry must never return (the PS4
 * calls that an error too): the runtime stops with a message if one does.
 *
 * A fiber initialized without context memory gets a stack from the runtime (1 MiB below 1 TiB,
 * like the PS4's thread stacks) the first time it runs. */
#define _GNU_SOURCE
#include "runtime.h"
#include "asm.h"
#include "platform.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIBER_ERROR_NULL ((int32_t)0x80590001)
#define FIBER_ERROR_ALIGNMENT ((int32_t)0x80590002)
#define FIBER_ERROR_RANGE ((int32_t)0x80590003)
#define FIBER_ERROR_INVALID ((int32_t)0x80590004)
#define FIBER_ERROR_PERMISSION ((int32_t)0x80590005)
#define FIBER_ERROR_STATE ((int32_t)0x80590006)

enum { STATE_RUN = 1, STATE_IDLE = 2, STATE_TERMINATED = 3 };
#define SIGNATURE0 0xdef1649cu
#define SIGNATURE1 0xb37592a0u
#define OPT_SIGNATURE 0xbb40e64du
#define CONTEXT_MINIMUM 512u
#define OWN_STACK (1u << 20)
#define FLAG_SET_FPU 0x100u

/* The game's SceFiber (256 bytes): the runtime's layout of it. */
typedef struct {
    uint32_t magic_start;
    _Atomic uint32_t state;
    void (ABI *entry)(uint64_t arg_on_initialize, uint64_t arg_on_run);
    uint64_t arg_on_initialize;
    unsigned char *context;      /* the game's context memory (the fiber's stack), or NULL */
    uint64_t context_size;
    char name[32];
    void *saved_sp;              /* where it stopped (fiber_swap's frame), NULL before its first run */
    unsigned char *own_stack;    /* the runtime's stack for a fiber without context memory */
    uint32_t flags;
    uint32_t started;
    uint64_t reserved[12];
    uint32_t magic_end;
} Fiber;
_Static_assert(sizeof(Fiber) <= 256, "SceFiber is 256 bytes");

/* The fiber state of the thread now running: the fiber (NULL outside Run), where the thread's
 * own context waits, the value passed with the last switch into this context. */
typedef struct {
    Fiber *current;
    void *thread_sp;
    uint64_t transfer;
    Fiber *left;                 /* the fiber just left: Idle once its stack is no longer in use */
} ThreadFibers;
static _Thread_local ThreadFibers fibers;
static _Atomic uint64_t fibers_initialized, fiber_runs, fiber_switches;

/* After a switch the code may continue on another thread (a fiber resumed elsewhere): the thread's
 * state is looked up again each time, never kept across fiber_swap. */
static __attribute__((noinline)) ThreadFibers *here(void) {
    ThreadFibers *volatile state=&fibers;
    return state;
}

/* fiber_swap(save, next): pushes the callee-saved registers, MXCSR and the x87 control word, stores
 * the stack pointer in *save, then pops the same from the stack at next and returns there.
 * The registers are the ones the System V convention keeps (the guest's own code follows it),
 * so the declaration says so: the hand-written code takes its arguments in %rdi and %rsi, and
 * the host's compiler has to put them there (its own convention puts them in %rcx and %rdx). */
void fiber_swap(void **save, void *next) ABI;
void fiber_start(void);
void fiber_main(void);
void fiber_boot(void);
__asm__(
    BB_ASM_TEXT
    BB_ASM_GLOBAL("fiber_swap")
    BB_ASM_TYPE("fiber_swap")
    "fiber_swap:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    subq $8,%rsp\n"
    "    stmxcsr (%rsp)\n"
    "    fnstcw 4(%rsp)\n"
    "    movq %rsp,(%rdi)\n"
    "    movq %rsi,%rsp\n"
    "    ldmxcsr (%rsp)\n"
    "    fldcw 4(%rsp)\n"
    "    addq $8,%rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    ret\n"
    BB_ASM_SIZE("fiber_swap")
    /* First entry into a fiber: fiber_swap's ret lands here on the fiber's fresh stack. */
    BB_ASM_GLOBAL("fiber_start")
    BB_ASM_TYPE("fiber_start")
    "fiber_start:\n"
    "    andq $-16,%rsp\n"
    "    call fiber_boot\n"
    "    ud2\n"
    BB_ASM_SIZE("fiber_start"));

/* The first thing that runs on the fiber's own stack: it says so, so the place where a
 * fiber stops is known. */
__attribute__((used, noreturn)) void fiber_boot(void) {
    fprintf(stderr,"Fiber: boot (rsp %p)\n",(void *)__builtin_frame_address(0));
    fiber_main();
}

__attribute__((used, noreturn)) void fiber_main(void) {
    fprintf(stderr,"Fiber: entered (rsp %p)\n",(void *)__builtin_frame_address(0));
    ThreadFibers *t=here();
    Fiber *fiber=t->current;
    fprintf(stderr,"Fiber: entered (fiber %p, entry %p, arg %llu)\n",(void *)fiber,
            fiber?(void *)(uintptr_t)fiber->entry:NULL,
            (unsigned long long)(fiber?fiber->arg_on_initialize:0));
    if (t->left) { atomic_store(&t->left->state,STATE_IDLE); t->left=NULL; }
    fiber->entry(fiber->arg_on_initialize,t->transfer);
    fprintf(stderr,"STOP: fiber %.31s returned from its entry (the PS4 does not allow it)\n",fiber->name);
    exit(23);
}

/* The stack a fiber starts on: fiber_swap's frame (MXCSR/FPU word, six registers, fiber_start). */
static void *first_frame(Fiber *fiber) {
    unsigned char *top;
    if (fiber->context) top=fiber->context+fiber->context_size;
    else {
        if (!fiber->own_stack) {
            fiber->own_stack=runtime_low_map(OWN_STACK,BB_PROT_READ|BB_PROT_WRITE);
            if (!fiber->own_stack) { fputs("STOP: no memory for a fiber stack\n",stderr); exit(23); }
        }
        top=fiber->own_stack+OWN_STACK;
    }
    fprintf(stderr,"Fiber: first frame (context %p, size %llu, top %p)\n",(void *)fiber->context,
            (unsigned long long)fiber->context_size,(void *)top);
    uint64_t *sp=(uint64_t *)((uintptr_t)top&~(uintptr_t)15);
    *--sp=0;                          /* alignment */
    *--sp=(uint64_t)(uintptr_t)fiber_start;
    for (int i=0;i<6;++i) *--sp=0;    /* rbp rbx r12 r13 r14 r15 */
    uint32_t mxcsr; uint16_t fpucw;
    __asm__ volatile("stmxcsr %0; fnstcw %1" : "=m"(mxcsr), "=m"(fpucw));
    if (fiber->flags&FLAG_SET_FPU) { mxcsr=0x9fc0; fpucw=0x037f; }
    *--sp=(uint64_t)mxcsr|(uint64_t)fpucw<<32;
    return sp;
}

static void *resume_point(Fiber *fiber) {
    void *sp=fiber->saved_sp;
    fiber->saved_sp=NULL;
    if (!sp) sp=first_frame(fiber);
    /* The stack the fiber continues on is read before the switch: a stack that cannot
     * be read is found here, with this thread's own stack still in place, so the
     * report of it can be written (after the switch there would be no stack to
     * write it from). */
    { volatile uint64_t probe=*(volatile uint64_t *)sp; (void)probe; }
    /* And written: the fiber's first instructions run there. */
    { volatile uint64_t *w=(volatile uint64_t *)sp; *w=*w; }
    return sp;
}

static int valid(const Fiber *fiber) {
    return fiber->magic_start==SIGNATURE0 && fiber->magic_end==SIGNATURE1;
}

static ABI int32_t fiber_initialize(Fiber *fiber,const char *name,void (ABI *entry)(uint64_t,uint64_t),
                                    uint64_t arg_on_initialize,void *context,uint64_t context_size,
                                    const uint32_t *opt,uint32_t build_version) {
    if (!fiber || !name || !entry) return FIBER_ERROR_NULL;
    if (((uintptr_t)fiber&7) || ((uintptr_t)context&15) || ((uintptr_t)opt&7)) return FIBER_ERROR_ALIGNMENT;
    if (context_size && context_size<CONTEXT_MINIMUM) return FIBER_ERROR_RANGE;
    if ((context_size&15) || (!context)!=(!context_size)) return FIBER_ERROR_INVALID;
    if (opt && *opt!=OPT_SIGNATURE) return FIBER_ERROR_INVALID;
    memset(fiber,0,sizeof(*fiber));
    strncpy(fiber->name,name,31);
    fiber->entry=entry; fiber->arg_on_initialize=arg_on_initialize;
    fiber->context=context; fiber->context_size=context_size;
    fiber->flags=build_version>=0x03500000u ? FLAG_SET_FPU : 0; /* SDK 3.50 and later: a clean FPU state */
    fiber->magic_start=SIGNATURE0; fiber->magic_end=SIGNATURE1;
    atomic_store(&fiber->state,STATE_IDLE);
    if (!atomic_fetch_add(&fibers_initialized,1))
        printf("Runtime: first fiber %.31s (context %llu bytes)\n",fiber->name,(unsigned long long)context_size);
    return 0;
}

static ABI int32_t fiber_opt_initialize(uint32_t *opt) {
    if (!opt) return FIBER_ERROR_NULL;
    if ((uintptr_t)opt&7) return FIBER_ERROR_ALIGNMENT;
    *opt=OPT_SIGNATURE; return 0;
}

static ABI int32_t fiber_finalize(Fiber *fiber) {
    if (!fiber) return FIBER_ERROR_NULL;
    if ((uintptr_t)fiber&7) return FIBER_ERROR_ALIGNMENT;
    if (!valid(fiber)) return FIBER_ERROR_INVALID;
    uint32_t idle=STATE_IDLE;
    if (!atomic_compare_exchange_strong(&fiber->state,&idle,STATE_TERMINATED)) return FIBER_ERROR_STATE;
    if (fiber->own_stack) { platform_mapping_unmap(fiber->own_stack,OWN_STACK); fiber->own_stack=NULL; }
    return 0;
}

/* The stack a fiber runs on is the guest's own memory, not the thread's: the C library's
 * stack probe (which the host's own functions use for their big frames) walks down from
 * the thread's stack limit, which is somewhere else entirely, and touches pages that are
 * not the stack's. While the thread is on a stack of the guest's, the limit is put out of
 * the way (nothing is probed then), and it is put back when the thread is on its own. */
static uint64_t thread_stack_limit;
static void enter_guest_stack(void) {
    thread_stack_limit=platform_thread_stack_limit();
    platform_set_thread_stack_limit(0);
}
static void leave_guest_stack(void) {
    platform_set_thread_stack_limit(thread_stack_limit);
}
/* Run: from a thread, into a fiber, until a fiber returns to the thread. */
static ABI int32_t fiber_run(Fiber *fiber,uint64_t arg_on_run,uint64_t *arg_on_return) {
    if (!fiber) return FIBER_ERROR_NULL;
    if ((uintptr_t)fiber&7) return FIBER_ERROR_ALIGNMENT;
    if (!valid(fiber)) return FIBER_ERROR_INVALID;
    ThreadFibers *t=here();
    if (t->current) return FIBER_ERROR_PERMISSION;
    uint32_t idle=STATE_IDLE;
    if (!atomic_compare_exchange_strong(&fiber->state,&idle,STATE_RUN)) return FIBER_ERROR_STATE;
    atomic_fetch_add_explicit(&fiber_runs,1,memory_order_relaxed);
    t->current=fiber; t->transfer=arg_on_run; t->left=NULL;
    fprintf(stderr,"Fiber: run (fiber %p, arg %llu)\n",(void *)fiber,(unsigned long long)arg_on_run);
    enter_guest_stack();
    fiber_swap(&t->thread_sp,resume_point(fiber));
    leave_guest_stack();
    fprintf(stderr,"Fiber: back in the thread\n");
    t=here();                         /* back on this thread: ReturnToThread */
    if (t->left) { atomic_store(&t->left->state,STATE_IDLE); t->left=NULL; }
    t->current=NULL;
    if (arg_on_return) *arg_on_return=t->transfer;
    return 0;
}

/* Switch: from the running fiber to another; the caller continues when something runs it again. */
static ABI int32_t fiber_switch(Fiber *fiber,uint64_t arg_on_run,uint64_t *arg_on_run_back) {
    if (!fiber) return FIBER_ERROR_NULL;
    if ((uintptr_t)fiber&7) return FIBER_ERROR_ALIGNMENT;
    if (!valid(fiber)) return FIBER_ERROR_INVALID;
    ThreadFibers *t=here();
    Fiber *self=t->current;
    if (!self) return FIBER_ERROR_PERMISSION;
    uint32_t idle=STATE_IDLE;
    if (!atomic_compare_exchange_strong(&fiber->state,&idle,STATE_RUN)) return FIBER_ERROR_STATE;
    atomic_fetch_add_explicit(&fiber_switches,1,memory_order_relaxed);
    t->current=fiber; t->transfer=arg_on_run;
    /* This fiber becomes Idle only once its registers are saved (the next one marks it): another
     * thread must not run it while its stack is still in use here. */
    t->left=self;
    fiber_swap(&self->saved_sp,resume_point(fiber));
    t=here();
    if (t->left) { atomic_store(&t->left->state,STATE_IDLE); t->left=NULL; }
    if (arg_on_run_back) *arg_on_run_back=t->transfer;
    return 0;
}

/* ReturnToThread: the fiber stops here, sceFiberRun returns in the thread that ran it. */
static ABI int32_t fiber_return_to_thread(uint64_t arg_on_return,uint64_t *arg_on_run) {
    ThreadFibers *t=here();
    Fiber *self=t->current;
    if (!self) return FIBER_ERROR_PERMISSION;
    t->transfer=arg_on_return; t->left=self;
    fiber_swap(&self->saved_sp,t->thread_sp);
    t=here();                         /* run again (maybe by another thread) */
    if (t->left) { atomic_store(&t->left->state,STATE_IDLE); t->left=NULL; }
    if (arg_on_run) *arg_on_run=t->transfer;
    return 0;
}

static ABI int32_t fiber_get_self(Fiber **fiber) {
    if (!fiber) return FIBER_ERROR_NULL;
    ThreadFibers *t=here();
    if (!t->current) return FIBER_ERROR_PERMISSION;
    *fiber=t->current; return 0;
}

static ABI int32_t fiber_rename(Fiber *fiber,const char *name) {
    if (!fiber || !name) return FIBER_ERROR_NULL;
    if (!valid(fiber)) return FIBER_ERROR_INVALID;
    strncpy(fiber->name,name,31); return 0;
}

typedef struct {
    uint64_t size; void *entry; uint64_t arg_on_initialize; void *context; uint64_t context_size;
    char name[32]; uint64_t context_margin; uint8_t pad[48];
} FiberInfo;
static ABI int32_t fiber_get_info(Fiber *fiber,FiberInfo *info) {
    if (!fiber || !info) return FIBER_ERROR_NULL;
    if (info->size!=sizeof(*info) || !valid(fiber)) return FIBER_ERROR_INVALID;
    info->entry=(void *)(uintptr_t)fiber->entry; info->arg_on_initialize=fiber->arg_on_initialize;
    info->context=fiber->context; info->context_size=fiber->context_size;
    memcpy(info->name,fiber->name,32); info->context_margin=UINT64_MAX;
    return 0;
}

static ABI int32_t fiber_size_check(uint32_t flags) { return flags ? FIBER_ERROR_INVALID : 0; }
static ABI int32_t fiber_size_check_stop(void) { return 0; }

static const RuntimeExport exports[]={
    {"_sceFiberInitializeImpl",fiber_initialize}, {"sceFiberInitialize",fiber_initialize},
    {"sceFiberOptParamInitialize",fiber_opt_initialize}, {"sceFiberFinalize",fiber_finalize},
    {"sceFiberRun",fiber_run}, {"sceFiberSwitch",fiber_switch}, {"sceFiberGetSelf",fiber_get_self},
    {"sceFiberReturnToThread",fiber_return_to_thread}, {"sceFiberRename",fiber_rename},
    {"sceFiberGetInfo",fiber_get_info}, {"sceFiberStartContextSizeCheck",fiber_size_check},
    {"sceFiberStopContextSizeCheck",fiber_size_check_stop},
};

uintptr_t runtime_fiber_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }

void runtime_fiber_report(void) {
    printf("Runtime: fibers initialized=%llu, runs=%llu, switches=%llu\n",
           (unsigned long long)fibers_initialized,(unsigned long long)fiber_runs,(unsigned long long)fiber_switches);
}
