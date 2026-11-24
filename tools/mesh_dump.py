"""Rebuild a Starfield mesh from captured GPU data and write it as OBJ (to validate the record layouts).

Usage: py -3 -I tools/mesh_dump.py <export_dir> <mesh_index> [out.obj]

Layout being validated (see RT_FINDINGS "Scene geometry layout" / "Record layouts"):
  mesh record (#11006, 52 B): +0 position handle, +36 first index (uint16 units), +48 position scale
  meshlet record (#131, 32 B): +12 low 16 bits = index count, +28 first index
  handle: slot = v & 0x3FFF (heap 32768+slot), offset = (v >> 6) & 0xFFFF00; slots 14+k = #132 window k (16 MB each)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from capture_data import Capture  # noqa: E402

POOL_FIRST_SLOT = 14
WINDOW = 16 * 1024 * 1024


def pool_offset(handle):
    slot = handle & 0x3FFF
    if slot < POOL_FIRST_SLOT:
        raise ValueError(f"handle slot {slot} is not a #132 pool window")
    return (slot - POOL_FIRST_SLOT) * WINDOW + ((handle >> 6) & 0xFFFF00)


def main():
    root, mi = sys.argv[1], int(sys.argv[2])
    out = sys.argv[3] if len(sys.argv) > 3 else None
    cap = Capture(root)
    mesh = cap.resource_data(11006)
    mlet = cap.resource_data(131)
    clus = cap.resource_data(146)
    pool = cap.resource_view(132)  # 4 GB reserved resource: sparse view
    rec = mesh[mi * 52:(mi + 1) * 52]
    u = lambda o: struct.unpack_from("<I", rec, o)[0]
    pos_off = pool_offset(u(0))
    first_index = u(36)
    scale = struct.unpack_from("<f", rec, 48)[0]
    # Meshlets used by this mesh: cluster records pointing at it give the first meshlet.
    first_meshlets = sorted({struct.unpack_from("<I", clus, i * 20 + 16)[0]
                             for i in range(len(clus) // 20) if struct.unpack_from("<I", clus, i * 20 + 4)[0] == mi})
    index_count = 0
    k = first_meshlets[0] if first_meshlets else None
    expected = first_index
    while k is not None and k < len(mlet) // 32:
        fi = struct.unpack_from("<I", mlet, k * 32 + 28)[0]
        cnt = struct.unpack_from("<I", mlet, k * 32 + 12)[0] & 0xFFFF
        if fi != expected or cnt == 0:
            break
        index_count += cnt
        expected += cnt
        k += 1
    idx = struct.unpack(f"<{index_count}H", pool.read(first_index * 2, index_count * 2))
    vcount = max(idx) + 1 if idx else 0
    raw = pool.read(pos_off, vcount * 6)
    verts = [struct.unpack_from("<3h", raw, v * 6) for v in range(vcount)]
    pts = [(x * scale, y * scale, z * scale) for x, y, z in verts]
    lo = [min(p[i] for p in pts) for i in range(3)]
    hi = [max(p[i] for p in pts) for i in range(3)]
    print(f"mesh {mi}: positions @pool+{pos_off}, first index {first_index} (byte {first_index * 2}), "
          f"meshlets from {first_meshlets[:1]} -> {index_count} indices ({index_count // 3} tris), "
          f"{vcount} vertices, scale {scale:g}")
    print(f"  bounds min {[round(v, 3) for v in lo]} max {[round(v, 3) for v in hi]}")
    print(f"  record +32={u(32)} +40={u(40)} +44={u(44)} +8={u(8)} +12={u(12)}")
    if out:
        with open(out, "w") as fh:
            for p in pts:
                fh.write(f"v {p[0]} {p[1]} {p[2]}\n")
            for t in range(0, index_count - 2, 3):
                fh.write(f"f {idx[t] + 1} {idx[t + 1] + 1} {idx[t + 2] + 1}\n")
        print(f"  wrote {out}")


if __name__ == "__main__":
    main()
