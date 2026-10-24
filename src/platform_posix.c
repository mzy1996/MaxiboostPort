/* SPDX-License-Identifier: GPL-2.0-or-later
 * The POSIX side of the host's services (src/platform.h): what the Windows build
 * (src/platform_win32.c) offers there.
 */
#include "platform.h"

#ifndef _WIN32

#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

int platform_mapping_create(PlatformMapping *mapping, size_t size, const char *name) {
    memset(mapping, 0, sizeof(*mapping));
    /* A memfd: the guest's view and the backing view are two mappings of the same file, so
     * the GPU can be given the backing one and the guest the other. */
    mapping->fd = memfd_create(name, MFD_CLOEXEC);
    if (mapping->fd < 0) return -1;
    if (ftruncate(mapping->fd, (off_t)size) < 0) {
        close(mapping->fd);
        mapping->fd = -1;
        return -1;
    }
    return 0;
}

void *platform_mapping_view(PlatformMapping *mapping, void *address, size_t size, uint64_t offset) {
    if (mapping->fd < 0) return MAP_FAILED;
    return mmap(address, size, PROT_READ | PROT_WRITE, MAP_SHARED, mapping->fd, (off_t)offset);
}

void platform_mapping_unmap(void *view, size_t size) {
    if (view && view != MAP_FAILED) munmap(view, size);
}

void *platform_mapping_backing(PlatformMapping *mapping) {
    if (mapping->fd < 0) return NULL;
    return mmap(NULL, 0, PROT_READ | PROT_WRITE, MAP_SHARED, mapping->fd, 0);
}

void platform_mapping_release(PlatformMapping *mapping) {
    if (mapping->fd >= 0) close(mapping->fd);
    mapping->fd = -1;
    mapping->view = NULL;
}

size_t platform_granularity(void) {
    long page = sysconf(_SC_PAGESIZE);
    return page > 0 ? (size_t)page : 4096;
}

void *platform_map_fixed(void *address, size_t size, int prot) {
    /* The host's MAP_FIXED: the address has to be free (Windows has no way to map over an
     * existing mapping), so the caller reserves the address space first. */
    void *view=mmap(address,size,prot,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    return view==MAP_FAILED ? NULL : view;
}

void *platform_reserve_fixed(void *address, size_t size) {
    void *view=mmap(address,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    return view==MAP_FAILED ? NULL : view;
}

void platform_unmap(void *address, size_t size) {
    if (address) munmap(address,size);
}

/* The sections the guest's memory is made of (a memfd on Linux). */
#define BB_MAX_SECTIONS 512
static int sections[BB_MAX_SECTIONS];
static size_t section_count;

int platform_section_create(int *index, size_t size, const char *name) {
    if (section_count>=BB_MAX_SECTIONS) return -1;
    int fd=memfd_create(name ? name : "bb-guest-memory",MFD_CLOEXEC);
    if (fd<0) return -1;
    if (ftruncate(fd,(off_t)size)<0) { close(fd); return -1; }
    sections[section_count]=fd;
    *index=(int)section_count;
    ++section_count;
    return 0;
}

int platform_section_index(void) { return (int)section_count - 1; }

void *platform_section_map(int index, void *address, size_t size, uint64_t offset, int prot) {
    if (index<0 || (size_t)index>=section_count) return NULL;
    void *view=mmap(address,size,prot,MAP_SHARED|MAP_FIXED,sections[index],(off_t)offset);
    return view==MAP_FAILED ? NULL : view;
}

void *platform_section_backing(int index, size_t size) {
    if (index<0 || (size_t)index>=section_count) return NULL;
    void *view=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,sections[index],0);
    return view==MAP_FAILED ? NULL : view;
}

void platform_section_release(int index) {
    if (index<0 || (size_t)index>=section_count) return;
    if (sections[index]>=0) close(sections[index]);
    sections[index]=-1;
}

int platform_section_punch_hole(int index, uint64_t offset, size_t size) {
    /* The physical pages are given back to the host and read as zero again. */
    if (index<0 || (size_t)index>=section_count) return -1;
    return fallocate(sections[index],FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,
                     (off_t)offset,(off_t)size);
}

int platform_protect(void *address, size_t size, int prot) {
    if (!size) return 0;
    return mprotect(address, size, prot) < 0 ? -1 : 0;
}

int platform_query_protect(void *address, size_t size) {
    (void)size;
    /* The host does not tell what a range's permissions are; the guest's runtime keeps
     * track of them itself. */
    (void)address;
    return -1;
}

void *platform_reserve_low(size_t size, int prot) {
    return mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
}

void *platform_alloc_executable(size_t size) {
    /* The runtime's own code: it has to be executable and writable (the thunks are patched). */
    return mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

int platform_read_memory(void *address, void *buffer, size_t size) {
    if (!size) return 0;
    /* The host reads its own memory: the kernel interface is not needed, but it reads the
     * pages the runtime marked read-only, which a plain copy would fault on. */
    struct iovec local, remote;
    local.iov_base = buffer;
    local.iov_len = size;
    remote.iov_base = address;
    remote.iov_len = size;
    ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return got == (ssize_t)size ? 0 : -1;
}

uint64_t platform_thread_id(void) {
    return (uint64_t)syscall(SYS_gettid);
}

uint64_t platform_monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

uint64_t platform_realtime_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

void platform_sleep_ns(uint64_t ns) {
    struct timespec request;
    request.tv_sec = (time_t)(ns / 1000000000ull);
    request.tv_nsec = (long)(ns % 1000000000ull);
    struct timespec left;
    while (nanosleep(&request, &left) && errno == EINTR) request = left;
}

void platform_thread_name(const char *name) {
    pthread_setname_np(pthread_self(), name);
}

void platform_thread_name_get(char *buffer, size_t size) {
    if (!buffer || !size) return;
    buffer[0] = 0;
    pthread_getname_np(pthread_self(), buffer, size);
}

void platform_thread_stack(void **low, void **high) {
    pthread_attr_t attr;
    void *base = NULL;
    size_t size = 0;
    if (!pthread_getattr_np(pthread_self(), &attr)) {
        pthread_attr_getstack(&attr, &base, &size);
        pthread_attr_destroy(&attr);
    }
    if (low) *low = base;
    if (high) *high = (char *)base + size;
}

int64_t platform_utc_offset_seconds(void) {
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    return (int64_t)local.tm_gmtoff;
}

int platform_threads(uint64_t *ids, char names[][32], int capacity) {
    /* The process's threads, with their names (the profiler samples them). */
    DIR *dir = opendir("/proc/self/task");
    if (!dir) return 0;
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) && count < capacity) {
        if (entry->d_name[0] == '.') continue;
        char path[300], comm[32] = {0};
        snprintf(path, sizeof(path), "/proc/self/task/%s/comm", entry->d_name);
        FILE *file = fopen(path, "r");
        if (!file) continue;
        if (fgets(comm, sizeof(comm), file)) {
            snprintf(names[count], 32, "%s", comm);
            ids[count] = (uint64_t)strtoull(entry->d_name, NULL, 10);
            ++count;
        }
        fclose(file);
    }
    closedir(dir);
    return count;
}

int platform_symbol_of(void *address, char *module, size_t module_size, uint64_t *offset) {
    /* The module a host address belongs to and the offset in it (the profiler prints it). */
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (!dladdr(address, &info)) return 0;
    if (module && module_size) {
        const char *slash = info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;
        snprintf(module, module_size, "%s", slash ? slash + 1 : (info.dli_fname ? info.dli_fname : "?"));
    }
    if (offset) *offset = (uint64_t)((char *)address - (char *)info.dli_fbase);
    return 1;
}

uint64_t platform_file_mtime_ns(const char * path) {
    /* The file's modification time, in nanoseconds since the epoch. */
    struct stat info;
    if (stat(path, &info) != 0) return 0;
    return (uint64_t)info.st_mtime * 1000000000ull + (uint64_t)info.st_mtim.tv_nsec;
}

int platform_file_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

int platform_real_path(const char *path, char *buffer, size_t size) {
    return realpath(path, buffer) != NULL;
}

int64_t platform_pread(int handle, void *buffer, size_t size, int64_t offset) {
    return (int64_t)pread(handle, buffer, size, (off_t)offset);
}

int64_t platform_pwrite(int handle, const void *buffer, size_t size, int64_t offset) {
    return (int64_t)pwrite(handle, buffer, size, (off_t)offset);
}

char *platform_strcasestr(const char *haystack, const char *needle) {
    return strcasestr(haystack, needle);
}

int platform_fstatat(int dir_handle, const char *name, struct stat *entry) {
    /* The name may be a whole path (the Windows build has no dirfd, so the path is built
     * there); stat follows the symbolic links, like the fstatat without AT_SYMLINK_NOFOLLOW
     * does. */
    if (name && name[0] == '/') return stat(name, entry);
    return fstatat(dir_handle, name, entry, 0);
}

int platform_fsync(int handle) {
    return fsync(handle);
}

void platform_stat_times(const struct stat *entry, int64_t *atime, int64_t *mtime,
                         int64_t *ctime) {
    if (atime) *atime = (int64_t)entry->st_atim.tv_sec;
    if (mtime) *mtime = (int64_t)entry->st_mtim.tv_sec;
    if (ctime) *ctime = (int64_t)entry->st_ctim.tv_sec;
}

void platform_stat_blocks(const struct stat *entry, uint64_t *blocks, uint32_t *block_size) {
    if (blocks) *blocks = (uint64_t)entry->st_blocks;
    if (block_size) *block_size = (uint32_t)entry->st_blksize;
}

uint64_t platform_context_rip(const PlatformContext *context) {
    if (!context) return 0;
    const ucontext_t *uc = (const ucontext_t *)context->context;
    return (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
}
uint64_t platform_context_flags(const PlatformContext *context) {
    if (!context) return 0;
    const ucontext_t *uc = (const ucontext_t *)context->context;
    return (uint64_t)uc->uc_mcontext.gregs[REG_EFL];
}
uint64_t platform_context_register(const PlatformContext *context, int index) {
    if (!context) return 0;
    const ucontext_t *uc = (const ucontext_t *)context->context;
    /* The x86-64 numbering, as the guest's own code uses it. */
    static const int order[] = {
        REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
        REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15
    };
    if (index < 0 || index >= 16) return 0;
    return (uint64_t)uc->uc_mcontext.gregs[order[index]];
}
uint64_t platform_context_stack(const PlatformContext *context) {
    return platform_context_register(context, 4);
}
uint64_t platform_context_frame(const PlatformContext *context) {
    return platform_context_register(context, 5);
}
void platform_context_set_rip(PlatformContext *context, uint64_t value) {
    if (!context) return;
    ucontext_t *uc = (ucontext_t *)context->context;
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)value;
}
void platform_context_clear_trap(PlatformContext *context) {
    if (!context) return;
    ucontext_t *uc = (ucontext_t *)context->context;
    uc->uc_mcontext.gregs[REG_EFL] &= ~(greg_t)0x100;
}
void platform_context_set_trap(PlatformContext *context) {
    if (!context) return;
    ucontext_t *uc = (ucontext_t *)context->context;
    uc->uc_mcontext.gregs[REG_EFL] |= (greg_t)0x100;
}
uint64_t platform_context_thread_id(const PlatformContext *context) {
    (void)context;
    return (uint64_t)syscall(SYS_gettid);
}
uintptr_t platform_context_fault_address(const PlatformContext *context) {
    /* Linux gives the address in the signal information, which the caller has; the context
     * itself does not carry it. */
    (void)context;
    return 0;
}
int platform_context_is_write(const PlatformContext *context) {
    /* Linux tells a write fault by a bit in the error code of the context. */
    if (!context) return 0;
    const ucontext_t *uc = (const ucontext_t *)context->context;
    return (uc->uc_mcontext.gregs[REG_ERR] & 0x2) ? 1 : 0;
}
int platform_context_is_fetch(const PlatformContext *context) {
    /* And an instruction fetch by another bit of it. */
    if (!context) return 0;
    const ucontext_t *uc = (const ucontext_t *)context->context;
    return (uc->uc_mcontext.gregs[REG_ERR] & 0x10) ? 1 : 0;
}
void platform_write_stdout(const char *text, size_t size) {
    /* The runtime's log: the standard output, which the launcher reads. */
    fwrite(text, 1, size, stdout);
    fflush(stdout);
}

/* A shared library the runtime loads at run time (the online module). */
struct PlatformLibrary { void *handle; };

PlatformLibrary *platform_library_open(const char *path, char *error, size_t error_size) {
    /* RTLD_LOCAL: its C++ (network and NP libraries) stays out of the process's symbol scope. */
    void *handle=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if (!handle) {
        if (error && error_size) snprintf(error,error_size,"%s",dlerror());
        return NULL;
    }
    PlatformLibrary *library=malloc(sizeof(*library));
    if (!library) { dlclose(handle); return NULL; }
    library->handle=handle;
    return library;
}

void *platform_library_symbol(PlatformLibrary *library, const char *name) {
    if (!library || !library->handle) return NULL;
    return dlsym(library->handle,name);
}

void platform_library_close(PlatformLibrary *library) {
    if (library) { dlclose(library->handle); free(library); }
}

const char *platform_program_directory(char *buffer, size_t size) {
    /* The directory the running program is in (the online module is loaded from beside it). */
    char path[4096];
    if (!platform_executable_path(path,sizeof(path))) {
        if (buffer && size) buffer[0]=0;
        return buffer;
    }
    platform_directory_of(path,buffer,size);
    return buffer;
}

int platform_mapping_of(void *address, char *buffer, size_t size) {
    /* The mapping an address is in, as the host's own map shows it. */
    if (!buffer || !size) return 0;
    buffer[0]=0;
    FILE *maps=fopen("/proc/self/maps","r");
    if (!maps) return 0;
    char line[512];
    int found=0;
    while (fgets(line,sizeof(line),maps)) {
        unsigned long from,to;
        if (sscanf(line,"%lx-%lx",&from,&to)==2 && (uintptr_t)address>=from && (uintptr_t)address<to) {
            snprintf(buffer,size,"%s",line);
            found=1;
            break;
        }
    }
    fclose(maps);
    return found;
}

int platform_backtrace(void **frames, int capacity) {
    /* The host's call chain (the fault report prints it). */
    return backtrace(frames,capacity);
}

const char *platform_executable_path(char *buffer, size_t size) {
    ssize_t got = readlink("/proc/self/exe", buffer, size - 1);
    if (got < 0) return NULL;
    buffer[got] = 0;
    return buffer;
}

const char *platform_directory_of(const char *path, char *buffer, size_t size) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        if (size) buffer[0] = 0;
        return buffer;
    }
    size_t length = (size_t)(slash - path);
    if (length >= size) length = size - 1;
    memcpy(buffer, path, length);
    buffer[length] = 0;
    return buffer;
}

int platform_dir_open(PlatformDir *dir, const char *path) {
    memset(dir, 0, sizeof(*dir));
    dir->handle = opendir(path);
    if (!dir->handle) return -1;
    return 0;
}

int platform_dir_next(PlatformDir *dir) {
    if (!dir->handle) return -1;
    struct dirent *entry = readdir(dir->handle);
    if (!entry) return 0;
    snprintf(dir->name, sizeof(dir->name), "%s", entry->d_name);
    /* d_type is not always filled in (some file systems); then the entry is looked up. */
    switch (entry->d_type) {
    case DT_DIR: dir->is_dir = 1; dir->is_link = 0; break;
    case DT_LNK: dir->is_dir = 0; dir->is_link = 1; break;
    case DT_REG: dir->is_dir = 0; dir->is_link = 0; break;
    default: {
        struct stat info;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", (const char *)dir->handle, entry->d_name);
        dir->is_link = lstat(path, &info) == 0 && S_ISLNK(info.st_mode);
        dir->is_dir = !dir->is_link && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
        break;
    }
    }
    return 1;
}

void platform_dir_close(PlatformDir *dir) {
    if (dir->handle) closedir(dir->handle);
    dir->handle = NULL;
}

int platform_is_directory(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

int platform_is_link(const char *path) {
    struct stat info;
    return lstat(path, &info) == 0 && S_ISLNK(info.st_mode);
}

int platform_random(void *buffer, size_t size) {
    return getrandom(buffer, size, 0) == (ssize_t)size ? 0 : -1;
}

void platform_cpu_times(uint64_t *process_ns, uint64_t *thread_ns) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        if (process_ns)
            *process_ns = (uint64_t)(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000000ull +
                          (uint64_t)(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1000ull;
    } else if (process_ns) *process_ns = 0;
    if (getrusage(RUSAGE_THREAD, &usage) == 0) {
        if (thread_ns)
            *thread_ns = (uint64_t)(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000000ull +
                         (uint64_t)(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1000ull;
    } else if (thread_ns) *thread_ns = 0;
}

#endif /* !_WIN32 */
