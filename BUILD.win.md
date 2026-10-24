# Building on Windows

The port runs the game's own x86-64 code on the CPU and translates its graphics to Vulkan. This
page describes the Windows build: what it is made of, how it is compiled, and what is still
missing.

## The game

The dump of MOBILE SUIT GUNDAM EXTREME VS. MAXIBOOST ON with update 1.15 merged in. Both
releases work: the North American one (`CUSA18758`) and the Hong Kong / Chinese deluxe
soundtrack edition (`CUSA15006`, 豪華原聲版). The Hong Kong dump comes with or without the
cheat trainer injected into the eboot; both are accepted (`scripts/game_check.py`).

The files go to the folder's root (`eboot.bin`, `sce_sys/`, `sce_module/`, `archives/`,
`raw/`), and the update is merged into that folder (the launcher asks for it).

## The toolchain: MinGW-w64, not MSVC

The guest's code speaks the **SysV ABI** and the runtime uses **pthreads**. Both decide the
toolchain:

| Requirement | Where | MSVC | MinGW-w64 |
|---|---|---|---|
| The SysV ABI | `src/runtime.h` (`ABI` = `__attribute__((sysv_abi))`), every `runtime_*.c` | no `sysv_abi` | yes |
| pthreads | `pthread_mutex`, `pthread_cond`, `pthread_rwlock` (29 files) | not available | yes (winpthreads) |
| Hand-written x86-64 assembly | `src/probe.c` (`enter_on_stack`), `src/runtime_thread.c`, `src/runtime_fiber.c` | no GNU inline asm | yes |

So the Windows build is made with **GCC from MSYS2's mingw64 repository**.

```
pacman -S --needed mingw-w64-x86_64-{vulkan-headers,vulkan-loader,sdl3,freetype,fmt,boost,
                                    vulkan-memory-allocator,xxhash,glslang,ffmpeg}
```

The header-only libraries (magic_enum, tsl-robin-map, miniz, Zydis, xbyak) are vendored in
`gpu/third_party`, so no package manager is needed for them. Boost is vendored too
(`gpu/third_party/boost`): the package manager's copy is incomplete (it has no `type_traits`,
no `system`, no `static_assert`). `tools/merge_boost.py` merges the per-library repositories
into the one directory the compiler reads.

## Building

```
bash tools/build_windows_all.sh
```

It builds the GPU library (`bbgpu.dll`) with CMake and the runtime (`bb-probe.exe`) with the
compiler directly. The runtime alone:

```
bash tools/build_windows_runtime.sh
```

The CMake build of the GPU library needs a **native Windows** CMake and ninja (the MSYS2 ones
drive the compiler through a shell that rewrites the Windows paths). `tools/mingw-w64.cmake` is
the toolchain file; it points CMake at the MinGW-w64 environment. The MSYS2 CMake is a POSIX
program and cannot drive a Windows compiler, so the tools come from the Python packages
(`pip install cmake ninja`) or from a native installation.

### The launcher: one executable, nothing to install

The player should not have to install Python, PySide6 or anything else, so the launcher
is packaged into a single executable (`tools/build_launcher_exe.py`, PyInstaller):

```
python tools/build_launcher_exe.py            # one file (46 MB)
python tools/build_launcher_exe.py --onedir   # a folder (it starts a little faster)
```

The executable carries the interpreter, the window's library and the game's preparation
scripts, so the player only double clicks `launcher/MAXIBOOST ON.exe`. The Qt modules the
launcher does not use (WebEngine, Quick, 3D, Charts) are left out, which is most of the
size.

Two paths matter in a packaged program and they are not the same: the modules and the
data are in the folder the executable was unpacked into (`sys._MEIPASS`), while the
runtime and the player's own files are in the folder the executable was put in
(`Path(sys.executable).parent`).

### The libraries the program ships with

The GPU library links the ffmpeg (the video decoder), the Vulkan loader, SDL3 (the window and
the input), fmt and xxhash. They are shared libraries, so the program needs their DLLs beside
it - and the DLLs they need themselves: the ffmpeg's own drag in the codecs' libraries (a
hundred of them).

`tools/collect_dlls.py` works the list out from the program's **import table** (the PE header
is read directly; `objdump` does not show the whole table of every DLL), so the package stays
right when a dependency changes. The DLLs that come with Windows are left out.

The ffmpeg can also be linked statically (`-DBB_FFMPEG_STATIC=1`), which makes the package
smaller, but the static libraries need every codec's own library and the list of those keeps
growing (the codecs' dependencies reach into OpenSSL and the Windows certificate store).

## How the code is shared

Everything the two hosts have in common lives behind **`src/platform.h`** (the runtime) and
**`gpu/shim/host_services.h`** (the GPU library):

| Service | Linux | Windows |
|---|---|---|
| The guest's memory | a memfd mapped twice | a memory section mapped twice (`CreateFileMapping`) |
| The protection of a range | `mprotect` | `VirtualProtect` |
| The registers of an interrupted thread | `ucontext_t` | `CONTEXT` |
| The host's directories | `opendir`/`readdir` | `FindFirstFile`/`FindNextFile` |
| The host's shared libraries | `dlopen`/`dlsym` | `LoadLibrary`/`GetProcAddress` |
| The host's clock, the random bytes, the thread's name | `clock_gettime`, `getrandom` | `QueryPerformanceCounter`, `BCryptGenRandom`, `SetThreadDescription` |
| The host's own memory (the stubs beside the game) | `mmap` at an address | `VirtualAlloc` at an address |
| The page tracking's read of the guest's memory | `process_vm_readv` | `ReadProcessMemory` |
| The files at an offset | `pread`/`pwrite` | `ReadFile`/`WriteFile` with an `OVERLAPPED` |
| The type of a directory entry | the entry says (`d_type`) | the file system is asked (`bb_dirent_type`) |

`src/platform_posix.c` and `src/platform_win32.c` implement the runtime's half;
`gpu/shim/host_services_posix.cpp` and `gpu/shim/host_services_win32.cpp` the GPU library's.
The code above them (the runtime's PS4 library, the guest's memory model, the fault handler) is
the same on both hosts.

The POSIX headers the code includes but Windows does not have
(`sys/mman.h`, `sys/resource.h`, `sys/uio.h`, `dlfcn.h`, `execinfo.h`) have stand-ins in
`gpu/third_party/win_compat`.

### The two modules: how they share the runtime

The GPU library (`bbgpu.dll`) calls the runtime (`bb-probe.exe`) for the guest's memory, the
fault recovery and the like. Linux shares the symbols the usual way (the runtime is linked
with `-rdynamic`, the library leaves them undefined). Windows has no such thing: a DLL's
imports are resolved from another module's exports.

So the runtime exports the symbols the library needs and the library imports them:

- `src/runtime_exports.txt` lists the symbols (the runtime's own interface);
- `tools/build_windows_runtime.sh` writes them into a module definition file
  (`bb-probe.def`), which the linker turns into the runtime's export table;
- the GPU build (`gpu/CMakeLists.txt`) makes the import library from the same list with
  `dlltool` and links the library against it.

Adding a symbol that crosses the boundary means adding it to that list.

### The fault handler

Linux takes the faults as signals and reads the registers from the `ucontext`. Windows has no
signals, so the runtime installs a **vectored exception handler** (`src/probe.c`):
`EXCEPTION_ACCESS_VIOLATION` and the illegal instructions reach it, and the GPU's page tracking
and the single-step counter are asked whether they know the fault first.

The single-step counter (`src/runtime_count.c`) puts an `int3` in the game's code and counts the
hits. Linux tells a breakpoint from a single step by the signal's code; Windows does not, so the
thread's own state (the trap flag) says which one it was.

The GPU library's guest hooks (`gpu/shim/bbport_guest_hooks.cpp`,
`gpu/shim/bbport_free_check.cpp`) put breakpoints in the game's code too: on Linux they are
caught by a `SIGTRAP` handler, on Windows by a vectored exception handler of their own.

### The assembly

`src/asm.h` holds the few assembler directives that differ: the GNU assembler (Linux) wants
`.type` and `.size`, MinGW's (COFF) does not.

The embedded font (`gpu/shim/bbport_overlay.cpp`) is another such place: the ELF assembler
knows `.hidden` and `.previous`, the COFF one neither, so the two builds have their own
inline assembly.

## What is still missing

The Windows build compiles the runtime and the GPU library; the game does not run yet. These
are the parts that are left:

1. **The guest's TLS** (`src/runtime_thread.c`, `scripts/link_libc.py`,
   `scripts/link_modules.py`): the guest reads its thread's control block through `gs:[0]`,
   which on Linux the runtime sets with `arch_prctl`. Windows keeps the GS base for itself
   (the program cannot set it), so the guest's access has to go another way. The measurements:
   - `GS:[0x60]` is the TEB; `GS:[0x58]` is the thread's array of TLS slots (the first slot is
     the program's own, and the array's pointer is per thread).
   - The array's slots are writable (`TlsSetValue` writes them), so the guest's TCB pointer
     can live in one of them.
   - The loader's rewrite (the two `link_*.py` scripts turn the guest's `mov rax, fs:[0]` into
     `mov rax, gs:[0]`) has to become a load of that slot on Windows, which is one instruction
     longer than the one it replaces; the extra bytes need a cave in the guest's image.
   - The runtime's half is ready (`src/runtime_tls_win32.c`): the slot is reserved with
     `TlsAlloc` and the guest's TCB pointer is kept in it.
2. **The runtime's file system** (`src/runtime_file.c`): the code compiles on Windows (the
   MinGW-w64 environment has the POSIX headers it uses), but the mount points and the user's
   directory are still the Linux ones.
3. **The launcher**: the Linux one is GTK4 (PyGObject), which is hard to install on Windows.
   `launcher/win_launcher.py` is a new one (PySide6) that reuses the settings file, the
   translations (`launcher/bbport_i18n.py`) and the cheat table of the Linux one.
4. **`run.sh`**: it is a bash script (process substitution, `/dev/tcp`, `pgrep`).
   `run.py` is the Windows equivalent: it sets the same environment variables and starts the
   same program.

None of these are blocked on a decision; they are work that has not been done yet.
