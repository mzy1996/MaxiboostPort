# SPDX-License-Identifier: GPL-2.0-or-later
"""Copies the DLLs the Windows build needs at run time into the package.

The GPU library links the ffmpeg libraries (the video decoder), the Vulkan loader, SDL3
(the window and the input), fmt and xxhash. Those are shared libraries, so they need their
own DLLs beside the program - and the DLLs they need themselves (the ffmpeg ones drag in
the codecs' libraries).

The list is worked out from the program's own import table, so it stays right when a
dependency changes. The DLLs that come with Windows itself are left out.

    python tools/collect_dlls.py PROGRAM OUTPUT_DIR
"""
import os
import struct
import sys

# The DLLs that come with Windows. The list is the "known DLLs" of a modern Windows plus
# the ones the C runtime and the compiler's own runtime bring.
SYSTEM = {
    'kernel32.dll', 'kernelbase.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll',
    'shell32.dll', 'ole32.dll', 'oleaut32.dll', 'combase.dll', 'comctl32.dll',
    'comdlg32.dll', 'ws2_32.dll', 'msvcrt.dll', 'ntdll.dll', 'bcrypt.dll', 'psapi.dll',
    'secur32.dll', 'imm32.dll', 'winmm.dll', 'version.dll', 'setupapi.dll', 'userenv.dll',
    'shlwapi.dll', 'crypt32.dll', 'iphlpapi.dll', 'dbghelp.dll', 'powrprof.dll', 'wldap32.dll',
    'mswsock.dll', 'normaliz.dll', 'nsi.dll', 'dnsapi.dll', 'winhttp.dll', 'wintrust.dll',
    'imagehlp.dll', 'cfgmgr32.dll', 'netapi32.dll', 'uxtheme.dll', 'dwmapi.dll', 'd3d11.dll',
    'dxgi.dll', 'd3d9.dll', 'opengl32.dll', 'glu32.dll', 'avrt.dll', 'mfplat.dll',
    'mf.dll', 'mfreadwrite.dll', 'dcomp.dll', 'dxva2.dll', 'dxva3.dll', 'd3d12.dll',
    'd3d12core.dll', 'gdiplus.dll', 'msimg32.dll', 'ncrypt.dll', 'usp10.dll', 'wsock32.dll',
    'dwrite.dll', 'bcryptprimitives.dll', 'winhttp.dll', 'wininet.dll', 'dnsapi.dll',
    'rpcrt4.dll', 'wtsapi32.dll', 'credui.dll', 'cryptnet.dll', 'msasn1.dll', 'sspicli.dll',
    'api-ms-win-core-synch-l1-2-0.dll', 'api-ms-win-core-synch-l1-1-0.dll',
}
# The API sets: the "api-ms-win-*.dll" files are the system's own.
for name in list(SYSTEM):
    if name.startswith('api-ms-win-') or name.startswith('ext-ms-'):
        SYSTEM.add(name)


def imports(path):
    """The DLLs the PE file imports."""
    with open(path, 'rb') as handle:
        data = handle.read()
    if data[:2] != b'MZ':
        return []
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        return []
    coff = pe + 4
    sections_count = struct.unpack_from('<H', data, coff + 2)[0]
    optional_size = struct.unpack_from('<H', data, coff + 16)[0]
    optional = coff + 20
    magic = struct.unpack_from('<H', data, optional)[0]
    directories = optional + (112 if magic == 0x20b else 96)
    import_rva = struct.unpack_from('<I', data, directories + 8)[0]
    table = optional + optional_size
    sections = []
    for index in range(sections_count):
        offset = table + index * 40
        virtual = struct.unpack_from('<I', data, offset + 12)[0]
        size = struct.unpack_from('<I', data, offset + 8)[0]
        raw = struct.unpack_from('<I', data, offset + 20)[0]
        raw_size = struct.unpack_from('<I', data, offset + 16)[0]
        sections.append((virtual, size, raw, raw_size))

    def to_offset(rva):
        for virtual, size, raw, raw_size in sections:
            if virtual <= rva < virtual + max(size, raw_size):
                return raw + (rva - virtual)
        return None

    names = []
    offset = to_offset(import_rva)
    if offset is None:
        return []
    while True:
        entry = data[offset:offset + 20]
        if len(entry) < 20 or entry == b'\0' * 20:
            break
        name_rva = struct.unpack_from('<I', entry, 12)[0]
        if name_rva == 0:
            break
        name_offset = to_offset(name_rva)
        if name_offset is None:
            break
        end = data.index(b'\0', name_offset)
        names.append(data[name_offset:end].decode('ascii', 'replace'))
        offset += 20
    return names


def collect(program, search_paths, ignore=()):
    """The DLLs the program needs, and the ones they need: the names, in the order they
    were found."""
    found = {}
    queue = []
    for name in imports(program):
        if name.lower() not in SYSTEM and name.lower() not in ignore:
            queue.append(name)
    while queue:
        name = queue.pop(0)
        key = name.lower()
        if key in found or key in ignore:
            continue
        for directory in search_paths:
            path = os.path.join(directory, name)
            if os.path.exists(path):
                found[key] = path
                for dependency in imports(path):
                    if dependency.lower() not in SYSTEM and dependency.lower() not in ignore:
                        queue.append(dependency)
                break
        else:
            found[key] = None  # not found: reported by the caller
    return found


def main(argv):
    if len(argv) < 3:
        print('Usage: collect_dlls.py PROGRAM OUTPUT_DIR', file=sys.stderr)
        return 2
    program = argv[1]
    output = argv[2]
    search = os.environ.get('BB_DLL_PATH', '').split(os.pathsep)
    search = [path for path in search if path]
    # The runtime is not a DLL: the GPU library imports its symbols, but the program
    # itself is the one that provides them.
    ignore = {name.lower() for name in os.environ.get('BB_DLL_IGNORE', '').split(os.pathsep)
              if name}
    found = collect(program, search, ignore)
    os.makedirs(output, exist_ok=True)
    copied = 0
    missing = []
    for key in sorted(found):
        path = found[key]
        if path is None:
            missing.append(key)
            continue
        with open(path, 'rb') as handle:
            data = handle.read()
        with open(os.path.join(output, os.path.basename(path)), 'wb') as handle:
            handle.write(data)
        copied += 1
    for name in missing:
        print(f'collect_dlls.py: {name} was not found', file=sys.stderr)
    print(f'collect_dlls.py: {copied} DLLs copied into {output}')
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
