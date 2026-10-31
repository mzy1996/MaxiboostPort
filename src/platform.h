/* SPDX-License-Identifier: GPL-2.0-or-later
 * The host's services the guest's runtime needs, in one place, so that the POSIX and the
 * Windows builds share the code above it.
 *
 * The guest speaks the SysV ABI (src/runtime.h's ABI macro) and uses pthreads, so the Windows
 * build is made with MinGW-w64 (which provides both), not with MSVC.
 */
#ifndef BB_PLATFORM_H
#define BB_PLATFORM_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
/* MinGW-w64's headers already have struct timespec (in sys/timeb.h); only the field names
 * differ from POSIX, which the code that uses it does not rely on. */
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The guest's memory map (src/runtime_memory.c). A mapping is a handle to a memory section:
 * the guest's view and the backing view are two views of the same section, so the GPU can be
 * given the backing one and the guest the other. */
typedef struct {
#ifdef _WIN32
    HANDLE section;     /* the memory section both views share */
    void *view;         /* this mapping's address */
#else
    int fd;             /* the memfd both views share */
    void *view;
#endif
} PlatformMapping;

/* A mapping of `size` bytes, backed by a section of its own (the guest's memory pool). */
int platform_mapping_create(PlatformMapping *mapping, size_t size, const char *name);
/* The same section, mapped again at a fixed address (NULL: any address). The address is the
 * guest's; it has to be aligned to the host's allocation granularity. */
void *platform_mapping_view(PlatformMapping *mapping, void *address, size_t size, uint64_t offset);
/* Releases one view (the section itself stays until platform_mapping_release). */
void platform_mapping_unmap(void *view, size_t size);
/* The section's backing address: where the host sees the guest's memory. */
void *platform_mapping_backing(PlatformMapping *mapping);
/* Releases the section and every view of it. */
void platform_mapping_release(PlatformMapping *mapping);
/* The host's allocation granularity: the guest's mappings are aligned to it. */
size_t platform_granularity(void);
/* Maps `size` bytes at a fixed address over whatever was there (the host's MAP_FIXED). The
 * address has to be free or the call fails. */
void *platform_map_fixed(void *address, size_t size, int prot);
/* Reserves `size` bytes at a fixed address without backing them (the host's MAP_FIXED with
 * PROT_NONE). */
void *platform_reserve_fixed(void *address, size_t size);
/* Unmaps a range. */
void platform_unmap(void *address, size_t size);
/* Releases the whole mapping that begins at address (VirtualFree(..., MEM_RELEASE) requires the
 * base address and a zero size; platform_unmap passes a non-zero size, which MEM_RELEASE rejects,
 * so a dedicated full-unmap is provided for callers that must release a known base). Returns 0 on
 * success, -1 on failure (the address was not a mapping base). */
int platform_unmap_full(void *address);
/* The host's last error code (GetLastError on Windows, errno on POSIX), for diagnostics. */
int platform_last_error(void);
/* A section of memory the guest's memory is made of: on Linux a memfd, on Windows a memory
 * section. The runtime keeps the sections in a table and holds their indices, so the type
 * does not leak into it. */
int platform_section_create(int *index, size_t size, const char *name);
/* The section's index, or -1. */
int platform_section_index(void);
/* Maps a section at a fixed address (the host's MAP_FIXED over the section's own file). */
void *platform_section_map(int index, void *address, size_t size, uint64_t offset, int prot);
/* The section's backing view (the host's own address of the memory). */
void *platform_section_backing(int index, size_t size);
/* Releases the section. */
void platform_section_release(int index);
/* Frees the physical pages of a section's range but keeps the range (the host's fallocate
 * with the hole-punching flag): the pages are read as zero again. */
int platform_section_punch_hole(int index, uint64_t offset, size_t size);

/* The guest's memory permissions (PROT_*): makes a range readable, writable or not. */
int platform_protect(void *address, size_t size, int prot);
/* The guest's PROT_* value of a range's current permissions, or -1. */
int platform_query_protect(void *address, size_t size);
/* Reserves address space for the guest below the 4 GiB line (the guest's low memory). */
void *platform_reserve_low(size_t size, int prot);
/* The host's own private memory for the guest (the flexible memory: the game's heap, which
 * it does not share with the GPU). It is reserved, not filled: the pages are given to the
 * game as it writes them. NULL when the host cannot give it. */
void *platform_private_memory(size_t size, int prot);
/* The guest's PROT_* values, the same numbers the guest uses. */
#define BB_PROT_NONE 0
#define BB_PROT_READ 1
#define BB_PROT_WRITE 2
#define BB_PROT_EXEC 4
/* Executable memory for the runtime's own code (the import thunks). */
void *platform_alloc_executable(size_t size);

/* Reading the host's own memory (a stack trace, a profiler): the guest's own address space. */
int platform_read_memory(void *address, void *buffer, size_t size);
/* The same, but the address is checked first (a wrong one gives an error instead of
 * taking the program down). */
int platform_read_memory_checked(void *address, void *buffer, size_t size);

/* The thread's own id, as the guest's sceKernelGetTid would report it. */
uint64_t platform_thread_id(void);
/* The thread's own stack limit (where its stack ends). The C library's stack probe
 * (which the host's own functions use for their big frames) walks down from it, so a
 * thread that runs on a stack of the guest's - which is somewhere else entirely - has
 * to put it out of the way (nothing is probed then). Zero means "no probing". */
uint64_t platform_thread_stack_limit(void);
void platform_set_thread_stack_limit(uint64_t limit);
/* The host's clock in nanoseconds, monotonic. */
uint64_t platform_monotonic_ns(void);
/* The host's wall clock in nanoseconds since the epoch (the guest's timed waits use it). */
uint64_t platform_realtime_ns(void);
/* Sleeps for the given nanoseconds, restarting when a signal interrupts it. */
void platform_sleep_ns(uint64_t ns);
/* The host's thread's name (the profiler prints it). */
void platform_thread_name(const char *name);
/* The current thread's name, read back (the wait profile prints it). */
void platform_thread_name_get(char *buffer, size_t size);
/* The current thread's stack: its lowest and highest address (the profiler walks it). */
void platform_thread_stack(void **low, void **high);
/* The host's offset from the universal time, in seconds (the guest's timezone). */
int64_t platform_utc_offset_seconds(void);
/* The process's threads: their ids and names (the profiler samples them). The names are the
 * host's own (the guest's are set through platform_thread_name). */
int platform_threads(uint64_t *ids, char names[][32], int capacity);
/* The symbol a host address belongs to (the profiler prints it): the module and the offset. */
int platform_symbol_of(void *address, char *module, size_t module_size, uint64_t *offset);
/* A file's modification time, in nanoseconds since the epoch (0 when it cannot be read). */
uint64_t platform_file_mtime_ns(const char *path);
/* Whether a path exists. */
int platform_file_exists(const char *path);
/* The real path of a path (the symbolic links resolved); the buffer gets it when it can be
 * read, otherwise it is left alone and 0 is returned. */
int platform_real_path(const char *path, char *buffer, size_t size);
/* Reads from a file at an offset, and writes to it at one (the POSIX pread and pwrite). The
 * offset is the file's own, not the descriptor's. */
int64_t platform_pread(int handle, void *buffer, size_t size, int64_t offset);
int64_t platform_pwrite(int handle, const void *buffer, size_t size, int64_t offset);
/* Finds a string in another one, ignoring the case (the POSIX strcasestr). */
char *platform_strcasestr(const char *haystack, const char *needle);
/* The stat of a file in a directory (the POSIX fstatat with the flags 0): the directory's
 * handle is the one opendir returned (the POSIX dirfd). */
int platform_fstatat(int dir_handle, const char *name, struct stat *entry);
/* Writes a file's data to the disk (the POSIX fsync). */
int platform_fsync(int handle);
/* The three times of a stat entry (the access, the modification and the change): the
 * seconds and the nanoseconds. MinGW's stat has no nanoseconds, so they are zero there. */
void platform_stat_times(const struct stat *entry, int64_t *atime, int64_t *mtime,
                         int64_t *ctime);
/* The size a file occupies on the disk (the blocks and the block size of a stat entry).
 * MinGW's stat has neither, so the size itself is reported there. */
void platform_stat_blocks(const struct stat *entry, uint64_t *blocks, uint32_t *block_size);

/* The registers of an interrupted thread: the fault handler and the single-step counter
 * read them (Linux gives a ucontext, Windows a CONTEXT). */
#ifdef _WIN32
typedef struct PlatformContext {
    EXCEPTION_POINTERS *exception;
    CONTEXT *context;
} PlatformContext;
#else
typedef struct PlatformContext {
    void *context;
} PlatformContext;
#endif
/* The instruction pointer of the interrupted thread. */
uint64_t platform_context_rip(const PlatformContext *context);
/* The flags of the interrupted thread (the trap flag, for the single step). */
uint64_t platform_context_flags(const PlatformContext *context);
/* The value of a general register: the index is the x86-64 numbering (0 rax, 1 rcx, ...). */
uint64_t platform_context_register(const PlatformContext *context, int index);
/* The stack pointer of the interrupted thread. */
uint64_t platform_context_stack(const PlatformContext *context);
/* The frame pointer of the interrupted thread (the game keeps frame pointers). */
uint64_t platform_context_frame(const PlatformContext *context);
/* Sets the instruction pointer of the interrupted thread. */
void platform_context_set_rip(PlatformContext *context, uint64_t value);
/* Clears the trap flag of the interrupted thread (the single step is over). */
void platform_context_clear_trap(PlatformContext *context);
/* Sets the trap flag of the interrupted thread (the next instruction is single stepped). */
void platform_context_set_trap(PlatformContext *context);
/* The host's thread id of the interrupted thread. */
uint64_t platform_context_thread_id(const PlatformContext *context);
/* The faulting address of an access violation (Windows only; 0 elsewhere). */
uintptr_t platform_context_fault_address(const PlatformContext *context);
/* Whether the interrupted thread was writing (the GPU's page tracking needs to know). */
int platform_context_is_write(const PlatformContext *context);
/* Whether the interrupted thread was fetching an instruction (never a tracking fault). */
int platform_context_is_fetch(const PlatformContext *context);
/* Writes a message to the standard output (the runtime's log). */
void platform_write_stdout(const char *text, size_t size);
/* A shared library the runtime loads at run time (the online module). */
typedef struct PlatformLibrary PlatformLibrary;
/* Opens the library; NULL when it cannot be opened, and the reason is written to `error`. */
PlatformLibrary *platform_library_open(const char *path, char *error, size_t error_size);
/* A symbol in the library; NULL when it has none. */
void *platform_library_symbol(PlatformLibrary *library, const char *name);
/* Closes the library. */
void platform_library_close(PlatformLibrary *library);
/* The directory the running program is in (the online module is loaded from beside it). */
const char *platform_program_directory(char *buffer, size_t size);
/* The mapping an address is in, written into the buffer as the host's map does
 * ("start-end perms"); empty when the address is not mapped. */
int platform_mapping_of(void *address, char *buffer, size_t size);
/* The host's call chain (the fault report prints it). */
int platform_backtrace(void **frames, int capacity);
/* The symbol a host address belongs to: the module and the offset in it. */
int platform_symbol_of(void *address, char *module, size_t module_size, uint64_t *offset);

/* The host's own executable's path (the online module is loaded from beside it). */
const char *platform_executable_path(char *buffer, size_t size);
/* The directory the given path is in, written into `buffer`; the path itself is returned. */
const char *platform_directory_of(const char *path, char *buffer, size_t size);

/* A directory's entries: the guest's file system walks the host's directories. */
typedef struct {
#ifdef _WIN32
    HANDLE handle;
    WIN32_FIND_DATAA entry;
    int first;
#else
    void *handle;
    struct dirent *entry;
#endif
    char name[512];
    int is_dir;
    int is_link;
} PlatformDir;

/* Opens a directory for reading its entries; 0 on success. */
int platform_dir_open(PlatformDir *dir, const char *path);
/* The next entry; 1 when one was read, 0 at the end, -1 on error. */
int platform_dir_next(PlatformDir *dir);
/* Closes the directory. */
void platform_dir_close(PlatformDir *dir);
/* Whether a path is a directory. */
int platform_is_directory(const char *path);
/* Whether a path is a symbolic link. */
int platform_is_link(const char *path);

/* The host's random bytes. */
int platform_random(void *buffer, size_t size);
/* The host's CPU time used by this process and this thread, in nanoseconds. */
void platform_cpu_times(uint64_t *process_ns, uint64_t *thread_ns);

/* bbport: find the places in this process's memory that hold this 8-byte value (little endian).
 * The guest's memory is the host's memory (the guest's address space is the host's), so a value
 * that leaked from the host into the guest is found here. The number of hits is returned; their
 * addresses are in hits[] (up to max_hits of them). The bytes looked at are in scanned.
 * The search is bounded (a fixed budget of memory, the huge regions are skipped), so it is a
 * diagnostic, not an exhaustive search. */
int platform_scan_memory(uint64_t value, uintptr_t *hits, int max_hits, uint64_t *scanned);

/* The path separator the host uses ('/' or '\\'). */
#define platform_sep(path) ((path)[0] == '/' ? '/' : '\\')

#ifdef __cplusplus
}
#endif

#endif
