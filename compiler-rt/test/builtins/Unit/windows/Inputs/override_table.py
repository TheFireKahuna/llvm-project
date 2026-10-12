"""Writes the override table of an executable, as the link of a program that
replaces an overridable default writes it, from the anchor that the program
defines in its .ovra section.

Usage: override_table.py stub <output>
       override_table.py table <executable> [tampering]

"stub" writes a DOS stub with room for the table, for lld-link's -stub:.
"table" writes the table in the executable's header page, and then applies
the tampering, if one is named.
"""

import struct
import sys

MAGIC = 0x7B3AC2D5
TAG_BASE = 0xC5E1A7F3


def slot_offset(k):
    return 0x40 + 8 * k if k < 8 else 0x98 + 8 * (k - 8)


if sys.argv[1] == "stub":
    with open(sys.argv[2], "wb") as f:
        f.write(b"MZ" + bytes(0x120 - 2))
    sys.exit(0)

with open(sys.argv[2], "rb") as f:
    image = bytearray(f.read())

pe = struct.unpack_from("<I", image, 0x3C)[0]
sections, = struct.unpack_from("<H", image, pe + 6)
optional, = struct.unpack_from("<H", image, pe + 20)
image_base, = struct.unpack_from("<Q", image, pe + 24 + 24)
header = pe + 24 + optional
for i in range(sections):
    name = bytes(image[header : header + 8])
    rva, = struct.unpack_from("<I", image, header + 12)
    raw, = struct.unpack_from("<I", image, header + 20)
    header += 40
    if name != b".ovra\0\0\0":
        continue
    count, = struct.unpack_from("<H", image, raw + 6)
    for k in range(count):
        entry, = struct.unpack_from("<Q", image, raw + 16 + 8 * k)
        if entry:
            struct.pack_into("<II", image, slot_offset(k), TAG_BASE ^ k, entry - image_base)
    struct.pack_into("<IHHIIQ", image, 0x80, MAGIC, 1, count, rva, 0, 0)

tampering = sys.argv[3] if len(sys.argv) > 3 else None
if tampering == "erased":
    struct.pack_into("<I", image, 0x80, 0)
elif tampering == "version":
    struct.pack_into("<H", image, 0x84, 2)
elif tampering == "count":
    struct.pack_into("<H", image, 0x86, 8)
elif tampering == "dropped":
    struct.pack_into("<II", image, slot_offset(1), 0, 0)
elif tampering == "moved":
    tag, rva = struct.unpack_from("<II", image, slot_offset(0))
    struct.pack_into("<II", image, slot_offset(0), tag, rva + 16)
elif tampering == "stray":
    _, rva = struct.unpack_from("<II", image, slot_offset(0))
    struct.pack_into("<II", image, slot_offset(5), TAG_BASE ^ 5, rva)
elif tampering == "stub":
    image[0x40:0x120] = bytes(0x120 - 0x40)

with open(sys.argv[2], "wb") as f:
    f.write(image)
