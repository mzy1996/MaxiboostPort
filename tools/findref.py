"""Find the code that references an image offset: tools/findref.py 0x161d080 [0x160b688].

The image is the BBPROBE5 file out/relink/boot-linked.bin; the header, the segment
table, the names and the relocations come before the image, so the image starts at
a fixed offset that is read out of the file itself. The image's bytes are the guest's
memory at their guest offsets.

A RIP-relative reference to the offset T has the displacement d at some place p with
T = p + 4 + d (p is where the 4-byte displacement field is; the instruction's end is
p+4 whatever its length). So the scan is a pass over the four bytes at every place.

This is how the guest's own data variables are located: the R1 registry's control
block is at image offset 0x160b600, and this scan says which code reads and writes
it (0x685a80 initializes it, 0xb69be0 registers into it, 0xb68550 looks up in it).

Run it from the repository root (the image path is relative to it).
"""
import struct
import sys

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
    targets = {int(a, 16) for a in sys.argv[1:]}
    hits = []
    for p in range(0, size - 4):
        d = struct.unpack_from('<i', image, p)[0]
        at = p + 4 + d
        if at in targets:
            hits.append((p, at))
    for p, at in hits:
        lo = max(0, p - 8)
        print('displacement at image+%#x -> target %#x   bytes: %s'
              % (p, at, image[lo:p + 4].hex()))


if __name__ == '__main__':
    main()
