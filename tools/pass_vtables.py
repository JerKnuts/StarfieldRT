"""Finds render pass vtables and their Execute pointers (slot 7) for the pass hook table in src/main.cpp.

Usage: py -3 -I tools/pass_vtables.py <Starfield.exe> <versionlib-1-16-244-0.bin> <IDs_VTABLE.h> <name substring> [...]

- IDs_VTABLE.h (CommonLibSF) gives REL IDs for `CreationRendererPrivate__<Name>RenderPass` vtables.
- versionlib format 5 is a flat uint32 table: offset (RVA) = table[id], after a 96-byte header.
- The Execute pointer is read from the exe image at vtable + 7 * 8 (an absolute VA; minus the image base = RVA).
"""
import re
import struct
import sys


def pe_reader(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    image_base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
    secs = []
    o = pe + 24 + optsz
    for i in range(nsec):
        vsize, va, rsize, rptr = struct.unpack_from("<IIII", d, o + 8)
        secs.append((va, max(vsize, rsize), rptr))
        o += 40

    def read_u64(rva):
        for va, size, rptr in secs:
            if va <= rva < va + size:
                return struct.unpack_from("<Q", d, rptr + rva - va)[0]
        return None

    return image_base, read_u64


def main():
    exe, lib, ids_h = sys.argv[1:4]
    subs = [s.lower() for s in sys.argv[4:]]
    image_base, read_u64 = pe_reader(exe)
    lib_d = open(lib, "rb").read()
    count = struct.unpack_from("<i", lib_d, 92)[0]
    text = open(ids_h, encoding="utf-8", errors="replace").read()
    for m in re.finditer(r"(CreationRendererPrivate__(\w+)RenderPass)\s*\{\s*REL::ID\((\d+)\)", text):
        full, name, rid = m.group(1), m.group(2), int(m.group(3))
        if subs and not any(s in name.lower() for s in subs):
            continue
        if rid >= count:
            print(f"{name}: id {rid} out of range")
            continue
        vt = struct.unpack_from("<I", lib_d, 96 + rid * 4)[0]
        ex = read_u64(vt + 7 * 8)
        ex_rva = (ex - image_base) if ex else None
        print(f'{{ "{name}", {vt:#X}, {ex_rva:#X} }},' if ex_rva is not None else f"{name}: vtable {vt:#x}, slot 7 unreadable")


main()
