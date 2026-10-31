"""Link the eboot with the game's bundled system modules into one probe image.

Successor of link_libc.py for several modules (default: libc.prx, then
libSceFios2.prx). Every module is placed after the previous one; imports of
all modules share one import table, so the C runtime can override any
function (host contracts first) and otherwise binds the native export.

Exports are matched by (NID, library identity, module identity). Imports of
libSceLibcInternal are served by libc.prx's same-NID exports: the internal
library is not shipped with the game and exposes the same functions.
Output: out/boot-linked.bin (format BBPROBE5) and out/link.json.
"""
import collections
import hashlib
import json
import os
from pathlib import Path
import struct
from prepare import parse_self, span, unpack
from link_libc import encode_id

DEFAULT_MODULES = ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx')
# libc exports the runtime calls itself (frame stats: malloc_stats), by NID: each libc.prx build
# has them at its own offsets (Bloodborne's and GODZILLA's +0x1f8c0, GUNDAM VERSUS' +0x55c50).
# One the game does not import gets an import entry of its own, bound to the export.
RUNTIME_EXPORTS = ('CC-BLMBu9-I',)  # malloc_stats
FS_LOAD = bytes.fromhex('64488b042500000000')  # mov rax, fs:[0]

# The game's own linker keeps a registry of loaded objects: register (0x15c9d08) hashes an
# address into an id, lookup (0x15c8678) maps an id back to the address, unregister
# (0x15c9ec8) removes it. A module entry stores its object's id, and when the object is
# unregistered elsewhere (a reload), the id goes stale: lookup returns NULL and the entry
# walker at 0x15cef28 dereferences it without a check (guest+0xb6ee00 in the shipped image,
# register r12=0, fault at 0x22 - every long run ends there, thread CApplicationBase).
# The sibling walker at 0x15ca068 does check for NULL, so skipping is the game's own
# defensive pattern. These patches (eboot image offsets; each verifies the bytes it
# replaces) route the NULL through the entry-skip path at 0x15cf28d instead:
#   0x15ceea9 (15 bytes of alignment nop): first entry jumps straight to the loop head as
#     before; a new late entry at +2 zeroes the loop counter (eax), tests the object and,
#     when it is NULL, jumps to the entry-skip path at 0x15cf28d;
#   0x15cef1f (the object load): keeps `mov r12,rax`, then jumps into that late entry.
_PATCH_WALKER = (
    # The entry walker's NULL check (the two patches above).
    (0xb6ed81, bytes.fromhex('6666666666662e0f1f840000000000'),
     bytes.fromhex('eb0d31c04d85e47576e9d603000090')),
    (0xb6edf7, bytes.fromhex('4989c431c00f1f4000'),
     bytes.fromhex('4989c4e984ffffff90')),
    # The walker's second phase (0xb6f210 loop) calls the same registry lookup
    # (0xb68550) per module entry and, unlike the sibling walker at 0x15ca068,
    # never checks the result: a stale or never-registered id dereferences NULL
    # at 0xb6f230. Rather than duplicating a check with no room for one (the
    # 5 bytes between the call and the loop head also hold the `xor r9d,r9d`
    # the element loop needs), the call is routed through a 14-byte wrapper in
    # alignment padding: on a NULL lookup it zeroes the child count (r14d), so
    # the existing `test r14d,r14d / je <next entry>` skips the entry - the
    # same defensive pattern the sibling walker uses.
    (0xb6f21d, bytes.fromhex('e82e93ffff'), bytes.fromhex('e89f82ffff')),
    (0xb674c1, bytes.fromhex('90' * 15),
     bytes.fromhex('e88a1000004885c075034531f6c3' + '90')),
)
# Third phase (0xb6f33e call): the array base lands in rbx and the loop at
# 0xb6f440 dereferences it. The child count is loaded from [r15+0x2a] right
# after the call and `test r15d,r15d / je` skips empty entries - so the
# helper simply zeroes the count when the lookup failed.
_PATCH_SITE3 = (
    (0xb6f34a, bytes.fromhex('450fb77f2a'), bytes.fromhex('e882d6ffff')),
    (0xb6c9d1, bytes.fromhex('90' * 15),
     bytes.fromhex('450fb77f2a4885c075034531ffc390')),
)
# Fourth phase (0xb6f5e7 call): same shape, count into ecx, skip at
# 0xb6faf4. The game's own count==0 path already tolerates r13==0 there.
_PATCH_SITE4 = (
    (0xb6f5ec, bytes.fromhex('410fb74f2a'), bytes.fromhex('e8f0d3ffff')),
    (0xb6c9e1, bytes.fromhex('90' * 15),
     bytes.fromhex('410fb74f2a4885c0750231c9c390' + '90')),
    # Fifth site (a sibling of the walker at 0xb6cb00): same unchecked array
    # base, the loop at 0xb6cb50 reads [rbx+0x12] of the first element. Here
    # the 13 bytes between the call and the loop head fit the whole check
    # inline: test rax,rax / je <next entry at 0xb6cba3> / then the original
    # `mov rbx,rax; add rbx,0x10`.
    (0xb6cb43, bytes.fromhex('4889c34883c310660f1f440000'),
     bytes.fromhex('4885c0745b4889c34883c31090')),
)
# Root fix: the walker's ids come from registry R1 (buckets 0x160b600 / 0x160b700,
# node {+0x0,+0x8}=links,+0x10=key,+0x18=value). Unregister (0xb69da0) unlinks the node
# and pushes it back onto the free list (0x160b688) WITHOUT zeroing the value, so the key
# 0x1001 keeps its node in memory but becomes invisible to lookup -> walker F dereferences
# NULL and crashes. Making unregister a no-op keeps every node (and its value) reachable
# forever. The node pool (0x19000 nodes) is far larger than a session registers, so never
# recycling it is safe; re-registering a live key still takes the collision-avoidance path
# (id++) so it does not clobber the original node. (Enlarging the pool 8x was tried and
# dropped: the game's own allocator cannot satisfy the 25MB request and init crashes at
# 0x66a859 with a NULL pool base.)
# The check the reader of the module's entry table does (0xb6952b) tests the address of
# the element (base+index*48) for zero instead of the base, so a lookup that found
# nothing slips through as soon as the index is not zero (guest+0xb69530). It is left
# alone: the self-healing lookup below makes the lookup find the object, so the check
# is not reached. (Patching it to test the base makes the game take the "not found"
# path and call through a null pointer a few minutes later - worse.)
# The root fix for the lookups that fail. The module's entry table holds, at entry+0x2c, the
# offset of the symbol inside the module; the linker converts it to an id with the register
# function (0xb69be0) and looks the id up in the registry (0xb68550). The conversion only
# happens when the entry is not converted yet (the flag is the sign bit of the word at
# entry+0x26), and the entries that carry the flag were never converted here: the registry
# stays empty (used=0) and every lookup of such an entry returns NULL. The walker then
# dereferences the NULL (guest+0xb6ee00) and the other reader of the same table does the
# same (guest+0xb69530).
# The lookup is therefore made self-healing: when it finds nothing, the entry is converted
# then (the register function is idempotent - an address that is already registered gets the
# id it has), the id is written back into the entry, and the lookup is done again. The
# helper lives in the alignment padding of libc.prx; the call reaches it from anywhere.
_PATCH_SELFHEAL = (
    (0x2764b4b, bytes.fromhex('90' * 53),
     bytes.fromhex('e8003a40fe4885c0751e53488b9d60ffffff8b7b2c4c01f7e8785040fe89432c89c75be8dd3940fec3')),
    (0xb6ede2, bytes.fromhex('e86997ffff'), bytes.fromhex('e8645dbf01')),
)
_PATCH_SELFHEAL2 = (
    (0x2765447, bytes.fromhex('90' * 57),
     bytes.fromhex('e8043140fe4885c07507488d05c0969bfec3')),
    (0xb69519, bytes.fromhex('e832f0ffff'), bytes.fromhex('e829bfbf01')),
)
_PATCH_DRET = (
    (0xb69da0, bytes.fromhex('554889e55350'), bytes.fromhex('c39090909090')),
)
# Pool enlargement is intentionally not used (see note above).
_PATCH_POOL8X = ()
_PATCH_GROUPS = {'walker': _PATCH_WALKER, 'dret': _PATCH_DRET, 'pool8x': _PATCH_POOL8X,
                 'site3': _PATCH_SITE3, 'site4': _PATCH_SITE4, 'selfheal': _PATCH_SELFHEAL,
                 'selfheal2': _PATCH_SELFHEAL2}


def _selected_patches():
    spec = os.environ.get('BBPATCH')
    if not spec or spec == 'all':
        return (_PATCH_WALKER + _PATCH_DRET + _PATCH_POOL8X
                + _PATCH_SITE3 + _PATCH_SITE4 + _PATCH_SELFHEAL + _PATCH_SELFHEAL2)
    return tuple(patch for name in spec.split(',')
                 for patch in _PATCH_GROUPS[name.strip()])


LINKER_PATCHES = _selected_patches()


def module(path):
    source = path.read_bytes()
    elf, header, ph, _, missing = parse_self(source)
    dp = next(p for p in ph if p['type'] == 2)
    dynamic = []
    for pos in range(dp['offset'], dp['offset'] + dp['filesz'], 16):
        tag, value = unpack('<QQ', elf, pos)
        if not tag:
            break
        dynamic.append((tag, value))
    tags = dict(dynamic)
    lp = next(p for p in ph if p['type'] == 0x61000000)
    blob = span(elf, lp['offset'], lp['filesz'])
    strings = span(blob, tags[0x61000035], tags[0x61000037])

    def string(offset):
        span(strings, offset, 1)
        return strings[offset:strings.index(0, offset)].decode('ascii')
    libraries, modules = {}, {}
    for tag, value in dynamic:
        if tag in (0x61000013, 0x61000015, 0x6100000d, 0x6100000f):
            table = libraries if tag in (0x61000013, 0x61000015) else modules
            key = encode_id(value >> 48)
            identity = (string(value & 0xffffffff), (value >> 32) & 65535)
            if key in table and table[key] != identity:
                raise ValueError('conflicting module/library IDs')
            table[key] = identity

    def identity(name):
        parts = name.split('#')
        if len(parts) != 3:
            return None  # plain names such as module_start are not linkable
        n, lib, mod = parts
        return (n, libraries[lib], modules[mod])
    symbols = []
    table = span(blob, tags[0x61000039], tags[0x6100003f])
    for pos in range(0, len(table), 24):
        n, info, other, section, value, size = unpack('<IBBHQQ', table, pos)
        name = string(n)
        symbols.append(dict(name=name, type=info & 15, binding=info >> 4, section=section,
                            value=value, size=size, identity=identity(name) if name else None))
    relocations = []
    for ot, st in ((0x61000029, 0x6100002d), (0x6100002f, 0x61000031)):
        table = span(blob, tags[ot], tags[st])
        for pos in range(0, len(table), 24):
            target, info, addend = unpack('<QQq', table, pos)
            relocations.append((target, info & 0xffffffff, info >> 32, addend))
    return dict(elf=elf, header=header, ph=ph, tags=tags, symbols=symbols, relocs=relocations,
                identity=identity, libraries=libraries, modules=modules,
                sha256=hashlib.sha256(source).hexdigest(), missing=missing)


def patch_fs_loads(image, ph, base):
    """Rewrite initial-exec `mov rax, fs:[0]` to GS: glibc owns FS on Linux."""
    patched = 0
    for p in ph:
        if p['type'] != 1 or not p['flags'] & 1:
            continue
        start, end = base + p['vaddr'], base + p['vaddr'] + p['filesz']
        at = image.find(FS_LOAD, start, end)
        while at >= 0:
            image[at] = 0x65
            patched += 1
            at = image.find(FS_LOAD, at + len(FS_LOAD), end)
    return patched


def unwind_entry(image, ph, base, name):
    """(code start, code size, eh_frame_hdr, eh_frame, eh_frame size, name) of a module in the
    image, for the runtime's sceKernelGetModuleInfoForUnwind (C++ exceptions); None without one."""
    hdr = next((p for p in ph if p['type'] == 0x6474e550), None)
    code = next((p for p in ph if p['type'] == 1), None)
    if not hdr or not code:
        return None
    at = base + hdr['vaddr']
    version, ptr_enc = image[at], image[at + 1]
    if version != 1 or ptr_enc not in (0x1b, 0x03):  # pcrel|sdata4, or udata4
        return None
    value = struct.unpack_from('<i' if ptr_enc == 0x1b else '<I', image, at + 4)[0]
    eh_frame = at + 4 + value if ptr_enc == 0x1b else base + value
    end = at if eh_frame < at else base + code['vaddr'] + code['memsz']  # up to the header or the segment's end
    return (base + code['vaddr'], code['memsz'], at, eh_frame, max(0, end - eh_frame), name)


def link(game, out, module_names=DEFAULT_MODULES):
    main = module(game / 'eboot.bin')
    raw = (out / 'boot.bin').read_bytes()
    magic, size, entry, ns, nr, ni, flags = unpack('<8s6Q', raw, 0)
    if magic != b'BBPROBE2':
        raise ValueError('linker expects freshly prepared BBPROBE2')
    pos = 56
    segments = [unpack('<3Q', raw, pos + i * 24) for i in range(ns)]
    pos += ns * 24
    names = [span(raw, pos + i * 128, 128).split(b'\0', 1)[0].decode() for i in range(ni)]
    pos += ni * 128
    relocs = [unpack('<QQqq', raw, pos + i * 32) for i in range(nr)]
    pos += nr * 32
    image = bytearray(span(raw, pos, size))
    if pos + size != len(raw):
        raise ValueError('unexpected boot trailer')

    identities = {main['identity'](name): i for i, name in enumerate(names)}
    main_libraries = {identity: key for key, identity in main['libraries'].items()}
    main_modules = {identity: key for key, identity in main['modules'].items()}

    def imported(symbol):
        key = symbol['identity']
        if key not in identities:
            n, lib, mod = key
            # Main-program spelling when the identity matches (existing host
            # tables use it); otherwise NID#library for new libraries.
            name = (f'{n}#{main_libraries[lib]}#{main_modules[mod]}'
                    if lib in main_libraries and mod in main_modules else f'{n}#{lib[0]}')
            identities[key] = len(names)
            names.append(name)
        return identities[key]

    exports, by_nid = {}, collections.defaultdict(list)
    # The layout of the modules first, then their exports: the main program is linked
    # before the libraries, and it imports some of the libraries' symbols, which are
    # their exports (the same identity, another spelling). Without knowing them, the
    # import would be left to the runtime, which does not have it, and the game would
    # call nothing.
    layout = {}
    base = (size + 65535) & ~65535
    for filename in module_names:
        m = module(game / 'sce_module' / filename)
        loads = [p for p in m['ph'] if p['type'] in (1, 0x61000010)]
        modsize = max(p['vaddr'] + p['memsz'] for p in loads)
        if base + modsize > 512 * 1024 * 1024:
            raise ValueError('linked image exceeds probe limit')
        layout[filename] = (m, base, modsize, loads)
        base = (base + modsize + 65535) & ~65535
    for filename, (m, module_base, modsize, loads) in layout.items():
        for s in m['symbols']:
            if s['section'] and s['binding'] in (1, 2) and s['identity'] and s['type'] in (1, 2):
                if s['identity'] in exports:
                    raise ValueError(f'{filename}: ambiguous export')
                entry_value = (module_base + s['value'], 2 if s['type'] == 1 else 1, filename)
                exports[s['identity']] = entry_value
                by_nid[(s['identity'][0], s['identity'][1][0])].append(entry_value)
    table = []
    base = (size + 65535) & ~65535
    fs_patched = patch_fs_loads(image, main['ph'], 0)
    tls_module = 2
    module_phs = []
    for filename in module_names:
        m, base, modsize, loads = layout[filename]
        module_phs.append(m['ph'])
        image.extend(bytes(base + modsize - len(image)))

        def mapped(address, length=8, loads=loads):
            return any(p['vaddr'] <= address and address + length <= p['vaddr'] + p['memsz'] for p in loads)
        for p in loads:
            if p['filesz'] > p['memsz']:
                raise ValueError(f'{filename}: invalid segment size')
            image[base + p['vaddr']:base + p['vaddr'] + p['filesz']] = span(m['elf'], p['offset'], p['filesz'])
            segments.append((base + p['vaddr'], p['memsz'], p['flags']))
        tls = next((p for p in m['ph'] if p['type'] == 7), None)
        module_id = 0
        if tls:
            if tls['filesz'] > tls['memsz'] or not mapped(tls['vaddr'], tls['memsz']):
                raise ValueError(f'{filename}: unsupported TLS layout')
            module_id = tls_module
            tls_module += 1
        tls_relocations = 0
        for target, kind, symid, addend in m['relocs']:
            if not mapped(target):
                raise ValueError(f'{filename}: relocation outside image {target:#x}')
            target += base
            if kind == 8:
                relocs.append((target, 0, base + addend, 0))
            elif kind == 16:
                symbol = m['symbols'][symid]
                if symbol['name'] or symbol['type'] != 3 or addend or not module_id:
                    raise ValueError(f'{filename}: unsupported external TLS relocation')
                struct.pack_into('<Q', image, target, module_id)
                tls_relocations += 1
            elif kind in (1, 6, 7):
                symbol = m['symbols'][symid]
                if symbol['section']:
                    if symbol['section'] == 0xfff1:
                        struct.pack_into('<Q', image, target, symbol['value'] + addend)
                    else:
                        relocs.append((target, 0, base + symbol['value'] + addend, 0))
                elif not symbol['identity'] and symbol['binding'] == 2:
                    struct.pack_into('<Q', image, target, 0)  # weak undefined (module_start/stop)
                else:
                    found = exports.get(symbol['identity'])
                    if found is not None and found[2] != filename:
                        # The symbol is another module's export: the call goes there.
                        struct.pack_into('<Q', image, target, found[0])
                    else:
                        rk = 2 if symbol['type'] == 1 else 1
                        if (rk == 1 and addend) or not 0 <= addend < 4096:
                            raise ValueError(f'{filename}: unsupported import addend')
                        relocs.append((target, rk, imported(symbol), addend))
            else:
                raise ValueError(f'{filename}: unsupported relocation type {kind}')
        count = 0
        for s in m['symbols']:
            if s['section'] and s['binding'] in (1, 2) and s['identity'] and s['type'] in (1, 2):
                if not mapped(s['value'], max(1, s['size'])):
                    raise ValueError(f'{filename}: export outside load segments')
                entry_value = (base + s['value'], 2 if s['type'] == 1 else 1, filename)
                exports.setdefault(s['identity'], entry_value)
                by_nid[(s['identity'][0], s['identity'][1][0])].append(entry_value)
                count += 1
        fs_patched += patch_fs_loads(image, m['ph'], base)
        table.append(dict(file=filename, base=base, size=modsize, init=base + m['tags'].get(12, 0),
                          tls_address=base + tls['vaddr'] if tls else 0, tls_memsz=tls['memsz'] if tls else 0,
                          tls_filesz=tls['filesz'] if tls else 0, tls_module=module_id,
                          tls_relocations=tls_relocations, exports=count, sha256=m['sha256']))
        base = (base + modsize + 65535) & ~65535

    for nid in RUNTIME_EXPORTS:
        candidates = [i for i, e in exports.items() if i[0] == nid and i[1][0] == 'libc']
        if len(candidates) == 1 and candidates[0] not in identities:
            identities[candidates[0]] = len(names)
            names.append(f'{nid}#libc')

    bindings, unresolved = [], []
    for identity, index in identities.items():
        found = exports.get(identity)
        if not found and identity and identity[1][0] == 'libSceLibcInternal':
            candidates = by_nid.get((identity[0], 'libc'), [])
            found = candidates[0] if len(candidates) == 1 else None
        if found:
            bindings.append((index, found[0], found[1]))
        else:
            unresolved.append(names[index])

    main_tls = next((p for p in main['ph'] if p['type'] == 7), None)
    main_tls_values = (main_tls['vaddr'], main_tls['filesz'], main_tls['memsz'], main_tls['align']) if main_tls else (0, 0, 0, 0)
    if main_tls and (main_tls['filesz'] > main_tls['memsz'] or main_tls['memsz'] > 1024 * 1024
                     or main_tls['vaddr'] + main_tls['filesz'] > size):
        raise ValueError('unsupported eboot TLS layout')
    procparam = next(p for p in main['ph'] if p['type'] == 0x61000001)
    # Where each module's unwind tables are (out/unwind.txt, read by the runtime).
    unwind = [unwind_entry(image, main['ph'], 0, 'eboot.bin')]
    for t, ph in zip(table, module_phs):
        unwind.append(unwind_entry(image, ph, t['base'], t['file']))
    for offset, original, replacement in LINKER_PATCHES:
        if bytes(image[offset:offset + len(original)]) != original:
            raise ValueError(f'eboot.bin changed: linker patch site {offset:#x} does not match '
                             f'(found {bytes(image[offset:offset + len(original)]).hex()})')
        image[offset:offset + len(replacement)] = replacement
    (out / 'unwind.txt').write_text(''.join(
        f'{a:#x} {b:#x} {c:#x} {d:#x} {e:#x} {n}\n' for a, b, c, d, e, n in filter(None, unwind)))
    with (out / 'boot-linked.bin').open('wb') as f:
        f.write(struct.pack('<8s6Q', b'BBPROBE5', len(image), entry, len(segments), len(relocs), len(names), flags))
        f.write(struct.pack('<Q', procparam['vaddr']))
        f.write(struct.pack('<4Q', *main_tls_values))
        f.write(struct.pack('<Q', len(table)))
        for t in table:
            f.write(struct.pack('<7Q', t['base'], t['size'], t['init'], t['tls_address'], t['tls_memsz'],
                                t['tls_filesz'], t['tls_module']))
        f.write(struct.pack('<Q', len(bindings)))
        for binding in bindings:
            f.write(struct.pack('<3Q', *binding))
        for segment in segments:
            f.write(struct.pack('<3Q', *segment))
        for name in names:
            if len(name.encode()) >= 128:
                raise ValueError('name too long')
            f.write(name.encode().ljust(128, b'\0'))
        for relocation in relocs:
            f.write(struct.pack('<QQqq', *relocation))
        f.write(image)
    report = dict(modules=[{k: (hex(v) if k in ('base', 'init', 'tls_address') else v) for k, v in t.items()} for t in table],
                  bindings=len(bindings), imports=len(names), fs_loads_patched=fs_patched,
                  main_tls=dict(zip(('vaddr', 'filesz', 'memsz', 'align'), main_tls_values)),
                  unresolved_imports=unresolved)
    (out / 'link.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    summary = ', '.join(f"{t['file']}@{t['base']:#x}" for t in table)
    print(f'Linked modules: {summary}; {len(bindings)} native bindings, {len(unresolved)} imports left to the host runtime, '
          f'fs->gs patched={fs_patched}')


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path)
    p.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'out')
    p.add_argument('--modules', nargs='*', default=list(DEFAULT_MODULES))
    a = p.parse_args()
    link(a.game, a.out, a.modules)
