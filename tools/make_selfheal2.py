"""The self-healing lookup for the module-entry reader (0xb694e0).

The reader looks the id at entry+0x2c up in the registry (0xb68550) and uses the result
as the base of an array of 48-byte elements. The id is not in the registry (the module
loader never converted the entries), so the lookup returns NULL and the reader reads
(0 + index*48 + 0xf) -> guest+0xb69530.

Instead of the address the helper returns a page of zeros: the reader then reads 0,
which is what it reads for an entry that is not there ("not found"), and the game takes
its own path for that. The zeroed page is a run of zeros in the image (the .bss), so it
is mapped and readable wherever the game is.
"""
import struct
import capstone
import subprocess
import sys

LOOKUP = 0xb68550
HOLE = 0x2765447
SITE = 0xb69519


def image_offset(data):
    magic, size, entry, ns, nr, ni, flags = struct.unpack('<8s6Q', data[:56])
    pos = 56 + 8 + 32
    (tc,) = struct.unpack('<Q', data[pos:pos + 8])
    pos += 8 + tc * 56
    (bc,) = struct.unpack('<Q', data[pos:pos + 8])
    pos += 8 + bc * 24 + ns * 24 + ni * 128 + nr * 32
    return pos, size


def find_zero_page(img, size, want=0x4000):
    """A run of zeros inside the image, big enough for the reader's index."""
    best = None
    i = 0
    while i < size - want:
        if img[i] == 0:
            j = i
            while j < size and img[j] == 0:
                j += 1
            if j - i >= want:
                # a run of zeros inside a data section: take it a little into the run
                best = i + 0x100
                break
            i = j
        else:
            i += 1
    return best


def rel32(after, target):
    v = (target - after) & 0xffffffff
    return v - 0x100000000 if v >= 0x80000000 else v


def main():
    data = open('out/relink/boot-linked.bin', 'rb').read()
    off, size = image_offset(data)
    img = data[off:off + size]
    page = find_zero_page(img, size)
    if page is None:
        print("no zeroed page found")
        return 1
    print(f"the zeroed page: guest {page:#x} ({size - page} bytes of zeros left)")

    body = bytearray()
    body += bytes([0xe8]) + struct.pack('<i', rel32(HOLE + len(body) + 5, LOOKUP))
    body += bytes.fromhex('4885c0')                    # test rax, rax
    jne_at = len(body)
    body += bytes([0x75, 0x00])                        # jne .done
    lea_at = len(body)
    body += bytes.fromhex('488d05') + struct.pack('<i', rel32(HOLE + lea_at + 7, page))
    body += bytes([0xc3])                              # ret
    body[jne_at + 1] = (HOLE + len(body) - 1) - (HOLE + jne_at + 2)

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    print(f"helper: {len(body)} bytes")
    for ins in md.disasm(bytes(body), HOLE):
        print(f"  0x{ins.address:x}: {ins.mnemonic} {ins.op_str}")
    site = bytes([0xe8]) + struct.pack('<i', rel32(SITE + 5, HOLE))
    print("HELPER =", body.hex())
    print("SITE   =", site.hex())

    p = 'scripts/link_modules.py'
    s = open(p).read()
    import re
    s = re.sub(r"_PATCH_SELFHEAL2 = \(.*?\n\)\n", "", s, flags=re.S)
    new = ("_PATCH_SELFHEAL2 = (\n"
           "    (0x2765447, bytes.fromhex('90' * 57),\n"
           "     bytes.fromhex('%s')),\n"
           "    (0xb69519, bytes.fromhex('e832f0ffff'), bytes.fromhex('%s')),\n"
           ")\n" % (body.hex(), site.hex()))
    s = s.replace("_PATCH_DRET = (", new + "_PATCH_DRET = (", 1)
    open(p, 'w').write(s)
    print("patch group written")
    return 0


if __name__ == '__main__':
    sys.exit(main())
