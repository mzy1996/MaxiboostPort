"""The self-healing lookup for the entry walker (0xb6ec00).

The walker's first phase reads the module's entry table: the id at entry+0x2c is looked up
in the registry (0xb68550) and the object it gives is what the walker works on. The ids
are handed out by the register function (0xb69be0) and the entries that were converted
carry them. The entries that were not converted carry the offset of the symbol in the
module instead, and the lookup of that finds nothing: the walker then dereferences NULL
(guest+0xb6ee00).

The helper makes the lookup self-healing: when it finds nothing, the entry is converted
(the address - the module base plus the offset - is registered, which gives the entry its
id), the id is written back into the entry and the lookup is done again with the new id.
The register function is idempotent - an address that is already registered gets the id it
already has - so an entry that is converted is left alone.

The helper lives in the alignment padding of libc.prx; the call reaches it from anywhere.
"""
import struct
import capstone

LOOKUP = 0xb68550
REGISTER = 0xb69be0
HOLE = 0x2764b4b
SITE = 0xb6ede2


def rel32(after, target):
    v = (target - after) & 0xffffffff
    return v - 0x100000000 if v >= 0x80000000 else v


def main():
    body = bytearray()
    body += bytes([0xe8]) + struct.pack('<i', rel32(HOLE + len(body) + 5, LOOKUP))
    body += bytes.fromhex('4885c0')                       # test rax, rax
    jne_at = len(body)
    body += bytes([0x75, 0x00])                           # jne .done
    body += bytes([0x53])                                 # push rbx
    body += bytes.fromhex('488b9d') + struct.pack('<i', -0xa0)   # mov rbx, [rbp-0xa0]
    body += bytes.fromhex('8b7b2c')                       # mov edi, [rbx + 0x2c]
    body += bytes.fromhex('4c01f7')                       # add rdi, r14 (the game's own form)
    body += bytes([0xe8]) + struct.pack('<i', rel32(HOLE + len(body) + 5, REGISTER))
    body += bytes.fromhex('89432c')                       # mov [rbx + 0x2c], eax
    body += bytes.fromhex('89c7')                         # mov edi, eax  (the new id!)
    body += bytes([0x5b])                                 # pop rbx
    body += bytes([0xe8]) + struct.pack('<i', rel32(HOLE + len(body) + 5, LOOKUP))
    body += bytes([0xc3])                                 # ret
    body[jne_at + 1] = (HOLE + len(body) - 1) - (HOLE + jne_at + 2)

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    print(f"helper: {len(body)} bytes (the hole is 53)")
    for ins in md.disasm(bytes(body), HOLE):
        print(f"  0x{ins.address:x}: {ins.mnemonic} {ins.op_str}")
    site = bytes([0xe8]) + struct.pack('<i', rel32(SITE + 5, HOLE))
    print("HELPER =", body.hex())
    print("SITE   =", site.hex())

    p = 'scripts/link_modules.py'
    s = open(p).read()
    import re
    s = re.sub(r"_PATCH_SELFHEAL = \(.*?\n\)\n", "", s, flags=re.S)
    new = ("_PATCH_SELFHEAL = (\n"
           "    (0x2764b4b, bytes.fromhex('90' * 53),\n"
           "     bytes.fromhex('%s')),\n"
           "    (0xb6ede2, bytes.fromhex('e86997ffff'), bytes.fromhex('%s')),\n"
           ")\n" % (body.hex(), site.hex()))
    s = s.replace("_PATCH_SELFHEAL2 = (", new + "_PATCH_SELFHEAL2 = (", 1)
    open(p, 'w').write(s)
    print("patch group written")


if __name__ == '__main__':
    main()
