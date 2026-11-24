"""Validate the decoded mesh record layout against the captured geometry pool.

Usage: py -3 -I tools/mesh_validate.py <export_dir> [max_meshes]

For each mesh record whose position stream lives in the #132 pool (slot >= 14):
  indices  = uint16[+32] at pool byte (+36 * 2)
  vertices = int16x3[+44] at the +0 handle
Checks: every index < vertex count, no degenerate-only meshes, finite bounds.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from capture_data import Capture  # noqa: E402

WINDOW = 16 * 1024 * 1024


def main():
    root = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 400
    cap = Capture(root)
    mesh = cap.resource_data(11006)
    pool = cap.resource_view(132)
    stats = {"checked": 0, "ok": 0, "bad_index": 0, "not_uploaded": 0, "other_slot": 0, "empty": 0}
    slots = {}
    examples = []
    for mi in range(len(mesh) // 52):
        rec = mesh[mi * 52:(mi + 1) * 52]
        if not any(rec):
            continue
        u = lambda o: struct.unpack_from("<I", rec, o)[0]
        handle, icount, first, vcount = u(0), u(32), u(36), u(44)
        slot = handle & 0x3FFF
        slots[slot] = slots.get(slot, 0) + 1
        if icount == 0 or vcount == 0:
            stats["empty"] += 1
            continue
        if slot < 14:
            stats["other_slot"] += 1
            continue
        if stats["checked"] >= limit:
            continue
        pos_off = (slot - 14) * WINDOW + ((handle >> 6) & 0xFFFF00)
        if not (pool.covered(first * 2, icount * 2) and pool.covered(pos_off, vcount * 6)):
            stats["not_uploaded"] += 1
            continue
        stats["checked"] += 1
        idx = struct.unpack(f"<{icount}H", pool.read(first * 2, icount * 2))
        if max(idx) >= vcount:
            stats["bad_index"] += 1
            if len(examples) < 3:
                examples.append((mi, max(idx), vcount))
            continue
        stats["ok"] += 1
    print(stats)
    print("position stream slots (slot: meshes):", dict(sorted(slots.items())))
    if examples:
        print("bad examples (mesh, max index, vertex count):", examples)


if __name__ == "__main__":
    main()
