/* The Windows build's stand-in for <dlfcn.h>: the dynamic loader of the Windows API.
   The names are the POSIX ones, so the code above does not change. */
#pragma once

#include <windows.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* bb_dl_handle;

static inline bb_dl_handle dlopen(const char* file, int flags) {
    (void)flags;
    return (bb_dl_handle)LoadLibraryA(file);
}

static inline void* dlsym(bb_dl_handle handle, const char* name) {
    return handle ? (void*)GetProcAddress((HMODULE)handle, name) : NULL;
}

static inline int dlclose(bb_dl_handle handle) {
    if (handle) {
        FreeLibrary((HMODULE)handle);
    }
    return 0;
}

static inline const char* dlerror(void) {
    return NULL;
}

/* The address of a symbol in the running program (the profiler's symbols). */
typedef struct {
    const char* dli_fname;
    void* dli_fbase;
    const char* dli_sname;
    void* dli_saddr;
} Dl_info;

static inline int dladdr(const void* address, Dl_info* info) {
    if (!info) {
        return 0;
    }
    memset(info, 0, sizeof(*info));
    HMODULE handle = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)address, &handle)) {
        return 0;
    }
    char path[MAX_PATH];
    const DWORD length = GetModuleFileNameA(handle, path, MAX_PATH);
    if (length) {
        info->dli_fname = path;
    }
    info->dli_fbase = (void*)handle;
    return 1;
}

#ifdef __cplusplus
}
#endif
