"""Dump a captured constant buffer as float4 rows (to decode Starfield's PerSceneConstants).

Usage: py -3 -I tools/cb_dump.py <export_dir> <resource_id> <offset> <size> [rows...]
With no rows given, prints every row that has non-zero data, flagging rows that look like
matrix rows, unit vectors or world-space positions.
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from capture_data import Capture  # noqa: E402


def main():
    root, rid, off, size = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    want = [int(r) for r in sys.argv[5:]]
    view = Capture(root).resource_view(rid)
    data = view.read(off, size)
    rows = size // 16
    for r in range(rows):
        if want and r not in want:
            continue
        v = struct.unpack_from("<4f", data, r * 16)
        u = struct.unpack_from("<4I", data, r * 16)
        if not want and not any(u):
            continue
        tags = []
        l3 = math.sqrt(sum(c * c for c in v[:3])) if all(math.isfinite(c) for c in v[:3]) else 0
        if 0.995 < l3 < 1.005:
            tags.append("unit xyz")
        if all(math.isfinite(c) for c in v) and any(abs(c) > 100 for c in v[:3]) and max(abs(c) for c in v[:3]) < 1e6:
            tags.append("large (position?)")
        vals = " ".join(f"{c:12.5g}" if math.isfinite(c) and (abs(c) < 1e9 and (abs(c) > 1e-9 or c == 0)) else f"   {u[i]:08x}" for i, c in enumerate(v))
        print(f"row {r:4d} (+{r * 16:5d}): {vals}  {' '.join(tags)}")


if __name__ == "__main__":
    main()
