"""Rebuild the captured scene from Starfield's GPU records (instances x meshes x transforms).

Usage: py -3 -I tools/scene_rebuild.py <export_dir> <out.obj> [center_instance] [radius] [max_tris]

Instance record (#144, 96 B): +16 quaternion xyzw, +32 translation xyz, +44 uniform scale, +4 transforms/bones.
LOD entries (#146, 20 B) at instance*4 + lod: +4 mesh index.
Mesh record (#11006, 52 B): +0 position handle, +32 index count, +36 first index, +44 vertex count, +48 scale.
World position = rotate(q, p * scale) + t, with p = int16x3 * meshScale.
Also writes a top-down depth-colored preview PNG next to the OBJ (needs numpy + Pillow).
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from capture_data import Capture  # noqa: E402

WINDOW = 16 * 1024 * 1024


def rotate(q, v):
    x, y, z, w = q
    # v' = v + 2w(q x v) + 2 q x (q x v)
    cx, cy, cz = y * v[2] - z * v[1], z * v[0] - x * v[2], x * v[1] - y * v[0]
    ccx, ccy, ccz = y * cz - z * cy, z * cx - x * cz, x * cy - y * cx
    return (v[0] + 2 * (w * cx + ccx), v[1] + 2 * (w * cy + ccy), v[2] + 2 * (w * cz + ccz))


def main():
    root, out = sys.argv[1], sys.argv[2]
    center_i = int(sys.argv[3]) if len(sys.argv) > 3 else 250
    radius = float(sys.argv[4]) if len(sys.argv) > 4 else 60.0
    max_tris = int(sys.argv[5]) if len(sys.argv) > 5 else 1_500_000
    cap = Capture(root)
    inst, lods, mesh = cap.resource_data(144), cap.resource_data(146), cap.resource_data(11006)
    pool = cap.resource_view(132)
    c = struct.unpack_from("<3f", inst, center_i * 96 + 32)
    mesh_cache = {}
    verts_out, faces_out, base = [], [], 1
    used = skipped = 0
    for i in range(len(inst) // 96):
        r = inst[i * 96:(i + 1) * 96]
        if not any(r):
            continue
        t = struct.unpack_from("<3f", r, 32)
        if math.dist(t, c) > radius:
            continue
        q = struct.unpack_from("<4f", r, 16)
        s = struct.unpack_from("<f", r, 44)[0]
        mi = struct.unpack_from("<I", lods, i * 4 * 20 + 4)[0]
        if mi * 52 + 52 > len(mesh):
            skipped += 1
            continue
        if mi not in mesh_cache:
            m = mesh[mi * 52:(mi + 1) * 52]
            u = lambda o: struct.unpack_from("<I", m, o)[0]
            h, icount, first, vcount = u(0), u(32), u(36), u(44)
            mscale = struct.unpack_from("<f", m, 48)[0]
            slot = h & 0x3FFF
            off = (slot - 14) * WINDOW + ((h >> 6) & 0xFFFF00)
            if slot < 14 or icount == 0 or not pool.covered(first * 2, icount * 2) or not pool.covered(off, vcount * 6):
                mesh_cache[mi] = None
            else:
                raw = pool.read(off, vcount * 6)
                pts = [tuple(c2 * mscale for c2 in struct.unpack_from("<3h", raw, k * 6)) for k in range(vcount)]
                idx = struct.unpack(f"<{icount}H", pool.read(first * 2, icount * 2))
                mesh_cache[mi] = (pts, idx)
        md = mesh_cache[mi]
        if md is None:
            skipped += 1
            continue
        pts, idx = md
        if len(faces_out) + len(idx) // 3 > max_tris:
            break
        for p in pts:
            w = rotate(q, (p[0] * s, p[1] * s, p[2] * s))
            verts_out.append((w[0] + t[0], w[1] + t[1], w[2] + t[2]))
        for k in range(0, len(idx) - 2, 3):
            faces_out.append((idx[k] + base, idx[k + 1] + base, idx[k + 2] + base))
        base += len(pts)
        used += 1
    with open(out, "w") as fh:
        for v in verts_out:
            fh.write(f"v {v[0]:.4f} {v[1]:.4f} {v[2]:.4f}\n")
        for f in faces_out:
            fh.write(f"f {f[0]} {f[1]} {f[2]}\n")
    print(f"center {tuple(round(x, 1) for x in c)} radius {radius}: {used} instances, {skipped} skipped, "
          f"{len(verts_out)} vertices, {len(faces_out)} triangles -> {out}")
    try:
        import numpy as np
        from PIL import Image
        v = np.array(verts_out)
        img = np.zeros((1200, 1200), np.float32)
        x = ((v[:, 0] - c[0]) / radius * 0.5 + 0.5) * 1199
        y = ((v[:, 1] - c[1]) / radius * 0.5 + 0.5) * 1199
        ok = (x >= 0) & (x < 1200) & (y >= 0) & (y < 1200)
        z = v[:, 2]
        zn = (z - np.percentile(z, 2)) / max(np.percentile(z, 98) - np.percentile(z, 2), 1e-3)
        np.maximum.at(img, (1199 - y[ok].astype(int), x[ok].astype(int)), np.clip(zn[ok], 0, 1) * 0.85 + 0.15)
        Image.fromarray((img * 255).astype(np.uint8)).save(os.path.splitext(out)[0] + "_top.png")
        print("preview:", os.path.splitext(out)[0] + "_top.png")
    except ImportError as e:
        print("no preview:", e)


if __name__ == "__main__":
    main()
