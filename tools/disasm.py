"""Disassemble a range of the linked image: tools/disasm.py 0xb69be0 0xb69d00.

The image is the BBPROBE5 file out/relink/boot-linked.bin; the header, the segment
table, the names and the relocations come before the image, so the image starts at
a fixed offset that is read out of the file itself.

The image's bytes are the guest's memory at their guest offsets, so an offset here
is the guest offset the fault reports use. The guest's own linker (the walker at
0xb6ec00), its registry (0xb69be0 registers, 0xb68550 looks up) and the R1
registry's initialization (0x685a80) are all read with this.

Run it from the repository root (the image path is relative to it).
"""
import struct
import sys

import capstone

IMAGE = 'out/relink/boot-linked.bin'


def image_bytes(path=IMAGE):
    data = open(path, 'rb').read()
    magic, size, entry, ns, nr, ni, flags = struct.unpack('<8s6Q', data[:56])
    pos = 56 + 8 + 32
    (table_count,) = struct.unpack('<Q', data[pos:pos + 8])
    pos += 8 + table_count * 7 * 8
    (bind_count,) = struct.unpack('<Q', data[pos:pos + 8])
    pos += 8 + bind_count * 3 * 8
    pos += ns * 3 * 8 + ni * 128 + nr * 32
    return data[pos:pos + size], size


def main():
    image, size = image_bytes()
    lo = int(sys.argv[1], 16)
    hi = int(sys.argv[2], 16) if len(sys.argv) > 2 else lo + 0x80
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for ins in md.disasm(image[lo:hi], lo):
        print('0x%x: %s %s' % (ins.address, ins.mnemonic, ins.op_str))


if __name__ == '__main__':
    main()
