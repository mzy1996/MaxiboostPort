#ifndef BB_RUNTIME_H
#define BB_RUNTIME_H
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#ifndef _WIN32
#include <setjmp.h>
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread sigjmp_buf *runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
#else
#include <setjmp.h>
/* The recovery point for speculative guest memory reads on this thread (probe.c fault handler).
 * Windows has no signals, so the plain jump/long jump pair is used (it does not restore a
 * signal mask, which Windows does not have). The GPU library shares this one (the type is the
 * same there), and both sides jump on a host stack, where the C library's jump is fine. */
typedef jmp_buf runtime_jmp_buf;
extern __thread runtime_jmp_buf *runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
#endif
/* A thread's own jump, for leaving a guest thread. It does not unwind: the C library's long
 * jump walks the frames between the two points, and the guest's frames have no unwind
 * information for it (the walk goes off into nonsense and the process dies). This one only
 * puts the registers and the stack pointer back, which is what leaving a thread needs - and
 * it has to work when the two points are on different stacks (a guest thread may run on a
 * stack of the game's own, and a fiber on a stack of the guest's memory).
 *
 * Every register is put back, not only the ones the convention keeps: the host's compiler
 * keeps values that are live across the jump in the registers the convention does not keep
 * (on Windows the first argument is in %rcx), and a jump that leaves those alone brings the
 * thread back with nonsense in hand. */
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip;    /* where to continue there */
    uint64_t rflags; /* the flags of that point */
    uint64_t fp;     /* the FPU state: the control word and the MXCSR */
} runtime_thread_jump;
/* The hand-written code takes its arguments in %rdi (the System V way), so the declarations say
 * so: the host's compiler has to put them there (its own convention puts them in %rcx). */
int runtime_setjmp(runtime_thread_jump *buffer) __attribute__((sysv_abi));
void runtime_longjmp(runtime_thread_jump *buffer, int value) __attribute__((sysv_abi));
/* The GPU library reads these (the guest memory's caching and the fault recovery point). */
extern uint64_t runtime_disabled_optimizations;
extern uint64_t runtime_experiment_bits;
extern volatile int runtime_restarting;
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
void runtime_finalize(void *dso);
uintptr_t runtime_printf_resolve(const char *name);
/* The guest's threads and their numbers (the watchdog looks at them). */
size_t runtime_thread_guest_threads(uint64_t *ids,char *names,size_t name_size,size_t max);
uintptr_t runtime_stdio_resolve(const char *name);
void *runtime_stdio_stream(int which);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_describe(uintptr_t address, char *buffer, size_t size);
/* bbport: the guest's own memory regions (their start, end, physical address and kind) for the
 * crash diagnostics: where a value that should not be there lives. The number of regions is
 * returned; the arrays are filled up to max of them. The regions are the guest's address space,
 * which is the host's, so their contents can be read directly. */
int runtime_memory_regions(uintptr_t *starts, uintptr_t *ends, uintptr_t *phys, int *kinds, int max);
/* bbport (diagnostics): dumps the ring of recent mapping operations (place/unmap/release/
 * protect/write-backing) with guest call sites, so the pool watch can see what remapped the
 * pool's physical range even after the fact. Returns characters written. */
int runtime_memory_op_dump(char *buffer, size_t size);
/* Puts the guest's own protection back on a page the GPU tracking left write-protected,
 * so the write that faulted goes on. The thread's context is not touched. */
int runtime_memory_restore_access(void *context, uintptr_t address, int repeated);
int runtime_memory_guest_chunk_gpu(uint64_t phys, uint64_t size);
void runtime_memory_set_guest_chunk_lazy(int lazy);
/* The CPU is about to write the range outside guest code (a file read): tells the GPU side. */
void runtime_memory_note_write(uintptr_t address, uint64_t size);
void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size);
/* The copies the HLE'd C library makes (memset/memcpy/memmove) into the watched window are named
 * by the probe (diagnostics): the game's code that asked for the copy is printed. */
void bb_cpu_copy_note(const char *what, const void *destination, uint64_t size, const void *caller);
void runtime_memory_prepare_cpu_write(uintptr_t address, uint64_t size);
/* One window of the guest's memory is watched (diagnostics): every mapping operation that lands
 * on it is said. */
void runtime_memory_watch_range(uintptr_t address, uint64_t size);
/* The runtime says where a mapping was placed (diagnostics): the probe watches the pool's page
 * from the instant it exists. The hook runs with the runtime's write lock held. */
void runtime_memory_set_place_hook(void (*hook)(uintptr_t address, uint64_t size, uint64_t phys));
/* The guest ranges that show the same direct memory as [address, address+size) (diagnostics). */
int runtime_memory_aliases(uintptr_t address, uint64_t size, uintptr_t *starts, uintptr_t *ends,
                           int max);
/* Where direct memory at a guest address lives (diagnostics): its offset into the host's view and
 * the end of the mapping. 0 for anything that is not direct memory. */
int runtime_memory_direct_phys(uintptr_t address, uint64_t *phys, uintptr_t *end);
/* The host's own view of the same bytes: a second name for the direct memory at phys. Writes
 * through it are stores the guest's page protection and the breakpoints do not see. */
void *runtime_memory_backing_pointer(uint64_t phys);
/* bbport: move the R1 registry node pool (guest direct memory at phys..phys+size) into its own
 * dedicated section so that neither the GPU's in-place import of direct-memory chunks (which
 * aliases the shared pool's physical pages) nor any discard/zero of that shared section can reach
 * the live pool. Called once the game publishes the pool's base address. Idempotent. */
int runtime_memory_protect_r1_pool(uint64_t phys, uint64_t size);
/* bbport (frame stats): a guest thread was blocked `ns` in the runtime (0 cond, 1 mutex, 2 sema, 3 sleep). */
void runtime_wait_note(int kind, uint64_t ns);
/* What a thread waits for (BB_WAIT_TRACE=1 says it): the name is remembered per thread
 * and the watchdog reads it. */
void runtime_wait_mark(const char *what);
const char *runtime_wait_marked(void);
const char *runtime_wait_marked_of(uint64_t tid);
void runtime_wait_report(double frames);
/* What the guest asked the direct memory for, by size (the game's pools, the library's heap). */
void runtime_memory_buckets_report(void);
void runtime_guest_call_sites(uint64_t out[3]);
/* bbport: where the guest's image is mapped (it is the guest's own memory, but not one of the
 * regions the runtime manages, so the diagnostics have to know about it separately). */
extern uintptr_t runtime_image_base;
extern uint64_t runtime_image_bytes;
/* bbport: times the game's heap asked for more memory (posix_mmap): a sign it leaks. */
uint64_t runtime_heap_growths(void);
static inline uint64_t runtime_wait_clock(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec; }
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
/* gzport: where the linker put libc.prx (its load address differs per game; NULL without it). */
void runtime_set_libc_module(const unsigned char *base, uint64_t size);
extern const unsigned char *runtime_libc_module;
extern uint64_t runtime_libc_module_size;
/* libc.prx's malloc_stats (link_modules.py binds it by NID; NULL without it). */
void runtime_set_libc_malloc_stats(const unsigned char *function);
extern const unsigned char *runtime_libc_malloc_stats;
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
void runtime_sema_report(void);
uintptr_t runtime_eventflag_resolve(const char *name);
uintptr_t runtime_fiber_resolve(const char *name);
uintptr_t runtime_process_resolve(const char *name);
uintptr_t runtime_usbd_resolve(const char *name);
int runtime_neo_mode(void);
/* BB_IMPORT_TRACE (runtime_trace.c): a logging thunk for a traced import, else target. */
uintptr_t runtime_trace_wrap(const char *symbol, uintptr_t target);
void runtime_trace_set_image(const unsigned char *image);
/* BB_GUEST_COUNT: how often given game instructions run (runtime_count.c). */
void runtime_count_install(unsigned char *image);
/* Couch co-op (runtime_pad.c): users logged in (ids[4], -1 after the last) and new logins. */
int runtime_users_logged_in(int32_t ids[4]);
int runtime_users_next_login(void);
uintptr_t runtime_process_resolve_data(const char *name);
void runtime_process_load_unwind(const char *path, const unsigned char *image);
void runtime_guest_backtrace(FILE *out);
void runtime_guest_where(FILE *out, uint64_t address);
void runtime_report_maps(FILE *out);
int runtime_guest_address(uint64_t address, uint64_t *offset, const char **module);
void runtime_fiber_report(void);
uintptr_t runtime_ngs2_resolve(const char *name);
uintptr_t runtime_font_resolve(const char *name);
void runtime_font_report(void);
void runtime_ngs2_report(void);
void runtime_eventflag_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
/* Whether a scoped import ("NID#lib#module") is the one named by scoped: the NIDs are compared,
 * the library and module letters differ from one executable to another. */
static inline int runtime_nid_is(const char *name, const char *scoped) {
    size_t length=strcspn(scoped,"#");
    return !strncmp(name,scoped,length) && (name[length]=='#' || !name[length]);
}
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
/* The system's C library: the time functions and the character set conversions that
 * the library of the game asks it for. */
uintptr_t runtime_ces_resolve(const char *name);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
uintptr_t runtime_json_resolve(const char *name);
/* Online play (runtime_net.c, BB_ONLINE=1): the online module answers the network and PSN imports. */
void runtime_net_configure(const char *app0, const char *serial, const char *title, const char *version);
int runtime_net_enabled(void);
int runtime_net_symbol(const char *symbol);
uintptr_t runtime_net_resolve(const char *name);
/* Guest threads for host code (the online module's NP threads); 0 or a positive errno value. */
int32_t runtime_thread_spawn(void **thread, void *(ABI *entry)(void *), void *argument, uint64_t stack, const char *name);
int32_t runtime_thread_join_spawned(void *thread, void **result);
/* The guest start PC of the calling thread (the CApplicationBase entry, &c.): said at a
 * fault so a null-pointer jump can be placed inside the thread's code. 0 if unknown. */
uintptr_t runtime_thread_current_entry(void);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
void runtime_thread_attach_host(const char *name);
#endif
