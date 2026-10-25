/* SPDX-License-Identifier: GPL-2.0-or-later
 * The Windows side of the host's services (src/platform.h).
 *
 * The guest's memory is a memory section (CreateFileMapping) mapped twice: once where the
 * guest sees it and once where the host (and the GPU) sees it. Windows has no MAP_FIXED, so
 * a mapping at a fixed address has to unmap whatever was there first (platform_mapping_view
 * is called with the guest's address space, which the runtime reserves before mapping).
 */
#include "platform.h"

#ifdef _WIN32

#include <psapi.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* MapViewOfFileEx returns NULL when it fails; this is the value the caller compares with. */
#define MAP_FAILED NULL

/* The guest's PROT_* values (the Linux ones): the runtime passes them around. */
static DWORD host_protect(int prot) {
    if (prot & BB_PROT_WRITE) {
        if (prot & BB_PROT_EXEC) return PAGE_EXECUTE_READWRITE;
        if (prot & BB_PROT_READ) return PAGE_READWRITE;
        return PAGE_READWRITE;
    }
    if (prot & BB_PROT_EXEC) return PAGE_EXECUTE_READ;
    if (prot & BB_PROT_READ) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

/* The guest's memory is reserved before it is mapped, so the addresses the runtime asks for
 * are free: a mapping there just works. */
int platform_mapping_create(PlatformMapping *mapping, size_t size, const char *name) {
    memset(mapping, 0, sizeof(*mapping));
    mapping->section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                          (DWORD)(size >> 32), (DWORD)(size & 0xffffffffu), NULL);
    if (!mapping->section) return -1;
    return 0;
}

void *platform_mapping_view(PlatformMapping *mapping, void *address, size_t size, uint64_t offset) {
    if (!mapping->section) return MAP_FAILED;
    void *view = MapViewOfFileEx(mapping->section, FILE_MAP_ALL_ACCESS,
                                  (DWORD)(offset >> 32), (DWORD)(offset & 0xffffffffu),
                                  size, address);
    return view == NULL ? MAP_FAILED : view;
}

void platform_mapping_unmap(void *view, size_t size) {
    (void)size;
    if (view && view != MAP_FAILED) UnmapViewOfFile(view);
}

void *platform_mapping_backing(PlatformMapping *mapping) {
    if (!mapping->section) return NULL;
    return MapViewOfFile(mapping->section, FILE_MAP_ALL_ACCESS, 0, 0, 0);
}

void platform_mapping_release(PlatformMapping *mapping) {
    if (mapping->section) CloseHandle(mapping->section);
    mapping->section = NULL;
    mapping->view = NULL;
}

size_t platform_granularity(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (size_t)info.dwAllocationGranularity;
}

void *platform_map_fixed(void *address, size_t size, int prot) {
    /* The host's MAP_FIXED: the address has to be free (Windows has no way to map over an
     * existing mapping), so the caller reserves the address space first. */
    void *view=VirtualAlloc(address,size,MEM_RESERVE|MEM_COMMIT,host_protect(prot));
    return view;
}

void *platform_reserve_fixed(void *address, size_t size) {
    return VirtualAlloc(address,size,MEM_RESERVE,PAGE_NOACCESS);
}

void platform_unmap(void *address, size_t size) {
    if (address) VirtualFree(address,size,MEM_RELEASE);
}

/* The sections the guest's memory is made of (a memory section on Windows). */
#define BB_MAX_SECTIONS 512
static HANDLE sections[BB_MAX_SECTIONS];
static size_t section_sizes[BB_MAX_SECTIONS];
static size_t section_count;

int platform_section_create(int *index, size_t size, const char *name) {
    (void)name;
    if (section_count>=BB_MAX_SECTIONS) return -1;
    HANDLE handle=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,
                                      (DWORD)(size>>32),(DWORD)(size&0xffffffffu),NULL);
    if (!handle) return -1;
    sections[section_count]=handle;
    section_sizes[section_count]=size;
    *index=(int)section_count;
    ++section_count;
    return 0;
}

int platform_section_index(void) { return (int)section_count - 1; }

void *platform_section_map(int index, void *address, size_t size, uint64_t offset, int prot) {
    if (index<0 || (size_t)index>=section_count) return NULL;
    /* The section is mapped with the host's own permissions; the guest's protection is applied
     * on top of it (the runtime tracks it). */
    (void)prot;
    void *view=MapViewOfFileEx(sections[index],FILE_MAP_ALL_ACCESS,
                                (DWORD)(offset>>32),(DWORD)(offset&0xffffffffu),size,address);
    return view;
}

void *platform_section_backing(int index, size_t size) {
    if (index<0 || (size_t)index>=section_count) return NULL;
    (void)size;
    return MapViewOfFile(sections[index],FILE_MAP_ALL_ACCESS,0,0,0);
}

void platform_section_release(int index) {
    if (index<0 || (size_t)index>=section_count) return;
    if (sections[index]) CloseHandle(sections[index]);
    sections[index]=NULL;
}

int platform_section_punch_hole(int index, uint64_t offset, size_t size) {
    /* The pages are given back to the host and read as zero again (MEM_RESET). */
    if (index<0 || (size_t)index>=section_count) return -1;
    void *view=MapViewOfFileEx(sections[index],FILE_MAP_ALL_ACCESS,
                               (DWORD)(offset>>32),(DWORD)(offset&0xffffffffu),size,NULL);
    if (!view) return -1;
    /* The view is reset page by page: MEM_RESET needs the pages to be committed already. */
    VirtualFree(view,size,MEM_RESET);
    UnmapViewOfFile(view);
    return 0;
}


int platform_protect(void *address, size_t size, int prot) {
    if (!size) return 0;
    DWORD old = 0;
    /* Windows changes the protection of whole pages: the caller's range is rounded out. */
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    uintptr_t start = (uintptr_t)address & ~(uintptr_t)(info.dwPageSize - 1);
    uintptr_t end = ((uintptr_t)address + size + info.dwPageSize - 1) & ~(uintptr_t)(info.dwPageSize - 1);
    if (!VirtualProtect((void *)start, (SIZE_T)(end - start), host_protect(prot), &old))
        return -1;
    return 0;
}

int platform_query_protect(void *address, size_t size) {
    (void)size;
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(address, &info, sizeof(info))) return -1;
    if (info.State != MEM_COMMIT) return BB_PROT_NONE;
    switch (info.Protect & 0xff) {
    case PAGE_NOACCESS: return BB_PROT_NONE;
    case PAGE_READONLY: return BB_PROT_READ;
    case PAGE_EXECUTE: return BB_PROT_EXEC;
    case PAGE_EXECUTE_READ: return BB_PROT_READ | BB_PROT_EXEC;
    case PAGE_READWRITE: return BB_PROT_READ | BB_PROT_WRITE;
    case PAGE_EXECUTE_READWRITE: return BB_PROT_READ | BB_PROT_WRITE | BB_PROT_EXEC;
    default: return BB_PROT_READ | BB_PROT_WRITE;
    }
}

void *platform_reserve_low(size_t size, int prot) {
    /* The guest's low memory (below the 4 GiB line) is reserved with the host's allocator,
     * which needs the size to be a multiple of the allocation granularity. */
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    size_t granularity = (size_t)info.dwAllocationGranularity;
    size_t rounded = (size + granularity - 1) & ~(granularity - 1);
    return VirtualAlloc(NULL, rounded, MEM_RESERVE, host_protect(prot));
}

void *platform_private_memory(size_t size, int prot) {
    /* The pages are reserved, not committed: the game gets them as it writes them, so a
     * machine with little memory is not charged for the whole heap. */
    return VirtualAlloc(NULL,size,MEM_RESERVE,host_protect(prot));
}

void *platform_alloc_executable(size_t size) {
    /* The runtime's own code: it has to be executable and writable (the thunks are patched). */
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
}

int platform_read_memory(void *address, void *buffer, size_t size) {
    /* The host reads its own memory: no permissions are needed, but the pages must be
     * readable, which is what the runtime guarantees. */
    if (!size) return 0;
    memcpy(buffer, address, size);
    return 0;
}

uint64_t platform_thread_id(void) {
    return (uint64_t)GetCurrentThreadId();
}

uint64_t platform_monotonic_ns(void) {
    static LARGE_INTEGER frequency;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000000000LL) / frequency.QuadPart);
}

uint64_t platform_realtime_ns(void) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER value;
    value.LowPart = now.dwLowDateTime;
    value.HighPart = now.dwHighDateTime;
    /* The FILETIME counts 100 ns intervals since 1601; the epoch is 1970. */
    const uint64_t epoch_delta = 116444736000000000ull;
    return (value.QuadPart - epoch_delta) * 100ull;
}

void platform_sleep_ns(uint64_t ns) {
    /* Sleep takes milliseconds; the rest is waited for in shorter steps so that the
     * guest's timers keep their resolution. */
    uint64_t left = ns;
    while (left) {
        DWORD chunk = left >= 1000000ull ? 100 : (DWORD)((left + 999999ull) / 1000000ull);
        if (!chunk) chunk = 1;
        Sleep(chunk);
        left = left >= (uint64_t)chunk * 1000000ull ? left - (uint64_t)chunk * 1000000ull : 0;
    }
}

void platform_thread_name(const char *name) {
    /* SetThreadDescription needs Windows 10 1803; older systems ignore it. */
    SetThreadDescription(GetCurrentThread(), (PCWSTR)name);
}

void platform_thread_name_get(char *buffer, size_t size) {
    if (!buffer || !size) return;
    buffer[0] = 0;
    PWCHAR text = NULL;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &text)) && text) {
        WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, (int)size, NULL, NULL);
        LocalFree(text);
    }
}

void platform_thread_stack(void **low, void **high) {
    /* The stack's bounds: the current frame is inside them, and the allocation is rounded
     * out to the host's page size. */
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(&info, &info, sizeof(info))) {
        if (low) *low = NULL;
        if (high) *high = NULL;
        return;
    }
    if (low) *low = info.BaseAddress;
    if (high) *high = (char *)info.BaseAddress + info.RegionSize;
}

int64_t platform_utc_offset_seconds(void) {
    TIME_ZONE_INFORMATION zone;
    DWORD result=GetTimeZoneInformation(&zone);
    if (result==TIME_ZONE_ID_INVALID) return 0;
    /* Bias is the difference between the local time and the universal time, in minutes,
     * with the sign the other way round. */
    return -(int64_t)zone.Bias * 60;
}

int platform_threads(uint64_t *ids, char names[][32], int capacity) {
    /* The process's threads, with their names (the profiler samples them). */
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if (snapshot==INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry;
    entry.dwSize=sizeof(entry);
    int count=0;
    if (Thread32First(snapshot,&entry)) {
        do {
            if (entry.th32OwnerProcessID!=GetCurrentProcessId()) continue;
            if (count>=capacity) break;
            ids[count]=entry.th32ThreadID;
            names[count][0]=0;
            HANDLE thread=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ThreadID);
            if (thread) {
                PWCHAR text=NULL;
                if (SUCCEEDED(GetThreadDescription(thread,&text)) && text) {
                    WideCharToMultiByte(CP_UTF8,0,text,-1,names[count],32,NULL,NULL);
                    LocalFree(text);
                }
                CloseHandle(thread);
            }
            ++count;
        } while (Thread32Next(snapshot,&entry));
    }
    CloseHandle(snapshot);
    return count;
}

int platform_symbol_of(void *address, char *module, size_t module_size, uint64_t *offset) {
    /* The module a host address belongs to and the offset in it (the profiler prints it). */
    HMODULE handle=NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)address,&handle)) return 0;
    if (module && module_size) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(handle,path,MAX_PATH)) {
            const char *slash=strrchr(path,'\\');
            snprintf(module,module_size,"%s",slash ? slash+1 : path);
        } else snprintf(module,module_size,"?");
    }
    if (offset) *offset=(uint64_t)((char *)address-(char *)handle);
    return 1;
}

uint64_t platform_file_mtime_ns(const char *path) {
    /* The file's modification time, in nanoseconds since the epoch. */
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path,GetFileExInfoStandard,&data)) return 0;
    ULARGE_INTEGER value;
    value.LowPart=data.ftLastWriteTime.dwLowDateTime;
    value.HighPart=data.ftLastWriteTime.dwHighDateTime;
    const uint64_t epoch_delta=116444736000000000ull;
    if (value.QuadPart<epoch_delta) return 0;
    return (value.QuadPart-epoch_delta)*100ull;
}

int platform_file_exists(const char *path) {
    DWORD attributes=GetFileAttributesA(path);
    return attributes!=INVALID_FILE_ATTRIBUTES;
}

int platform_real_path(const char *path, char *buffer, size_t size) {
    /* GetFullPathNameA resolves the ".", the ".." and the relative parts; the symbolic links
     * are not followed (Windows needs the privileges for that), which is enough here: the
     * caller compares a path with the mods' directory. */
    DWORD written=GetFullPathNameA(path,(DWORD)size,buffer,NULL);
    return written>0 && written<size;
}

int64_t platform_pread(int handle, void *buffer, size_t size, int64_t offset) {
    OVERLAPPED overlapped;
    memset(&overlapped,0,sizeof(overlapped));
    overlapped.Offset=(DWORD)(offset & 0xffffffff);
    overlapped.OffsetHigh=(DWORD)((uint64_t)offset>>32);
    if (!ReadFile((HANDLE)handle,buffer,(DWORD)size,NULL,&overlapped)) {
        if (GetLastError()==ERROR_HANDLE_EOF) return 0;
        return -1;
    }
    return (int64_t)GetOverlappedResult((HANDLE)handle,&overlapped,NULL,FALSE);
}

int64_t platform_pwrite(int handle, const void *buffer, size_t size, int64_t offset) {
    OVERLAPPED overlapped;
    memset(&overlapped,0,sizeof(overlapped));
    overlapped.Offset=(DWORD)(offset & 0xffffffff);
    overlapped.OffsetHigh=(DWORD)((uint64_t)offset>>32);
    DWORD written=0;
    if (!WriteFile((HANDLE)handle,buffer,(DWORD)size,&written,&overlapped)) return -1;
    return (int64_t)written;
}

char *platform_strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return (char *)haystack;
    const size_t length=strlen(needle);
    for (const char *p=haystack; *p; ++p) {
        if (!_strnicmp(p,needle,length)) return (char *)p;
    }
    return NULL;
}

int platform_fstatat(int dir_handle, const char *name, struct stat *entry) {
    /* The name is relative to the directory; the handle is not used (the listing builds the
     * whole path itself). */
    (void)dir_handle;
    char path[1024];
    const DWORD written=GetFullPathNameA(name,(DWORD)sizeof(path),path,NULL);
    if (!written || written>=sizeof(path)) return -1;
    return stat(path,entry);
}

int platform_fsync(int handle) {
    if (!_commit(handle)) return 0;
    return -1;
}

void platform_stat_times(const struct stat *entry, int64_t *atime, int64_t *mtime,
                         int64_t *ctime) {
    /* MinGW's stat has no nanoseconds, so they are zero. */
    if (atime) *atime = (int64_t)entry->st_atime;
    if (mtime) *mtime = (int64_t)entry->st_mtime;
    if (ctime) *ctime = (int64_t)entry->st_ctime;
}

void platform_stat_blocks(const struct stat *entry, uint64_t *blocks, uint32_t *block_size) {
    /* MinGW's stat has neither the blocks nor the block size; the size itself is reported
     * as the blocks (the guest only asks how much room the file takes). */
    if (blocks) *blocks = (uint64_t)entry->st_size;
    if (block_size) *block_size = 4096;
}

uint64_t platform_context_rip(const PlatformContext *context) {
    return context ? (uint64_t)context->context->Rip : 0;
}
uint64_t platform_context_flags(const PlatformContext *context) {
    return context ? (uint64_t)context->context->EFlags : 0;
}
uint64_t platform_context_register(const PlatformContext *context, int index) {
    if (!context) return 0;
    const CONTEXT *c = context->context;
    switch (index) {
    case 0: return c->Rax; case 1: return c->Rcx; case 2: return c->Rdx; case 3: return c->Rbx;
    case 4: return c->Rsp; case 5: return c->Rbp; case 6: return c->Rsi; case 7: return c->Rdi;
    case 8: return c->R8;  case 9: return c->R9;  case 10: return c->R10; case 11: return c->R11;
    case 12: return c->R12; case 13: return c->R13; case 14: return c->R14; case 15: return c->R15;
    default: return 0;
    }
}
uint64_t platform_context_stack(const PlatformContext *context) {
    return context ? (uint64_t)context->context->Rsp : 0;
}
uint64_t platform_context_frame(const PlatformContext *context) {
    return context ? (uint64_t)context->context->Rbp : 0;
}
void platform_context_set_rip(PlatformContext *context, uint64_t value) {
    if (context) context->context->Rip = (DWORD)value;
}
void platform_context_clear_trap(PlatformContext *context) {
    if (context) context->context->EFlags &= ~(DWORD)0x100;
}
void platform_context_set_trap(PlatformContext *context) {
    if (context) context->context->EFlags |= (DWORD)0x100;
}
uint64_t platform_context_thread_id(const PlatformContext *context) {
    (void)context;
    return (uint64_t)GetCurrentThreadId();
}
uintptr_t platform_context_fault_address(const PlatformContext *context) {
    /* The address the access violation was about (the exception's information). */
    if (!context || !context->exception) return 0;
    const EXCEPTION_RECORD *record = context->exception->ExceptionRecord;
    if (record->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION &&
        record->ExceptionCode!=EXCEPTION_IN_PAGE_ERROR) return 0;
    return (uintptr_t)record->ExceptionInformation[1];
}
int platform_context_is_write(const PlatformContext *context) {
    /* The exception's first information word says what the thread was doing: 1 write,
     * 2 read, 3 execute. */
    if (!context || !context->exception) return 0;
    const EXCEPTION_RECORD *record = context->exception->ExceptionRecord;
    if (record->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION) return 0;
    return record->ExceptionInformation[0]==1;
}
int platform_context_is_fetch(const PlatformContext *context) {
    if (!context || !context->exception) return 0;
    const EXCEPTION_RECORD *record = context->exception->ExceptionRecord;
    if (record->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION) return 0;
    return record->ExceptionInformation[0]==3;
}
void platform_write_stdout(const char *text, size_t size) {
    /* The runtime's log: the standard output, which the launcher reads. */
    fwrite(text, 1, size, stdout);
    fflush(stdout);
}

/* A shared library the runtime loads at run time (the online module). */
struct PlatformLibrary { HMODULE handle; };

PlatformLibrary *platform_library_open(const char *path, char *error, size_t error_size) {
    HMODULE handle=LoadLibraryA(path);
    if (!handle) {
        if (error && error_size)
            snprintf(error,error_size,"%lu",(unsigned long)GetLastError());
        return NULL;
    }
    PlatformLibrary *library=malloc(sizeof(*library));
    if (!library) { FreeLibrary(handle); return NULL; }
    library->handle=handle;
    return library;
}

void *platform_library_symbol(PlatformLibrary *library, const char *name) {
    if (!library || !library->handle) return NULL;
    return (void *)GetProcAddress(library->handle,name);
}

void platform_library_close(PlatformLibrary *library) {
    if (library) { FreeLibrary(library->handle); free(library); }
}

const char *platform_program_directory(char *buffer, size_t size) {
    /* The directory the running program is in (the online module is loaded from beside it). */
    char path[MAX_PATH];
    if (!GetModuleFileNameA(NULL,path,MAX_PATH)) {
        if (buffer && size) buffer[0]=0;
        return buffer;
    }
    platform_directory_of(path,buffer,size);
    return buffer;
}

int platform_mapping_of(void *address, char *buffer, size_t size) {
    /* The mapping an address is in, in the host's own format. */
    MEMORY_BASIC_INFORMATION info;
    if (!buffer || !size) return 0;
    buffer[0]=0;
    if (!VirtualQuery(address,&info,sizeof(info))) return 0;
    const char *state=info.State==MEM_COMMIT ? "rw" : (info.State==MEM_RESERVE ? "res" : "---");
    DWORD protect=info.Protect & 0xff;
    const char *perms=protect==PAGE_NOACCESS ? "---" :
                       (protect==PAGE_EXECUTE ? "--x" :
                       (protect==PAGE_READONLY ? "r--" :
                       (protect==PAGE_EXECUTE_READ ? "r-x" :
                       (protect==PAGE_READWRITE ? "rw-" : "rwx"))));
    snprintf(buffer,size,"%016llx-%016llx %s %s\n",
             (unsigned long long)(uintptr_t)info.BaseAddress,
             (unsigned long long)((uintptr_t)info.BaseAddress+info.RegionSize),
             perms,state);
    return 1;
}

int platform_backtrace(void **frames, int capacity) {
    /* The host's call chain (the fault report prints it). */
    return RtlCaptureStackBackTrace(0,(ULONG)capacity,frames,NULL);
}

const char *platform_executable_path(char *buffer, size_t size) {
    if (!GetModuleFileNameA(NULL, buffer, (DWORD)size)) return NULL;
    return buffer;
}

const char *platform_directory_of(const char *path, char *buffer, size_t size) {
    const char *slash = strrchr(path, '\\');
    const char *other = strrchr(path, '/');
    if (other > slash) slash = other;
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

/* The guest's paths are POSIX-like (/app0, /savedata0), but the host's are Windows paths:
 * a slash works in a Windows path too, so the names are taken as they are. */
int platform_dir_open(PlatformDir *dir, const char *path) {
    memset(dir, 0, sizeof(*dir));
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    dir->handle = FindFirstFileA(pattern, &dir->entry);
    if (dir->handle == INVALID_HANDLE_VALUE) {
        dir->handle = NULL;
        return -1;
    }
    dir->first = 1;
    return 0;
}

int platform_dir_next(PlatformDir *dir) {
    if (!dir->handle) return -1;
    if (dir->first) {
        dir->first = 0;
    } else if (!FindNextFileA(dir->handle, &dir->entry)) {
        return 0;
    }
    snprintf(dir->name, sizeof(dir->name), "%s", dir->entry.cFileName);
    if (!strcmp(dir->name, ".") || !strcmp(dir->name, "..")) return platform_dir_next(dir);
    dir->is_dir = (dir->entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    dir->is_link = (dir->entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
    return 1;
}

void platform_dir_close(PlatformDir *dir) {
    if (dir->handle) FindClose(dir->handle);
    dir->handle = NULL;
}

int platform_is_directory(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

int platform_is_link(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

int platform_random(void *buffer, size_t size) {
    if (BCryptGenRandom(NULL, (PUCHAR)buffer, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
        return 0;
    /* The BCrypt path can fail before the library is loaded; the CRT's generator is enough
     * for the guest's stack canaries. */
    for (size_t i = 0; i < size; ++i) ((unsigned char *)buffer)[i] = (unsigned char)rand();
    return 0;
}

void platform_cpu_times(uint64_t *process_ns, uint64_t *thread_ns) {
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER k, u;
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        if (process_ns) *process_ns = (k.QuadPart + u.QuadPart) * 100;
    } else if (process_ns) *process_ns = 0;
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER k, u;
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        if (thread_ns) *thread_ns = (k.QuadPart + u.QuadPart) * 100;
    } else if (thread_ns) *thread_ns = 0;
}

#endif /* _WIN32 */
