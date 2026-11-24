"""Extract the first shader of a captured PSO from Starfield's shader archive.

Usage: py -3 -I tools/extract_shader.py <export_dir> <shaderspc_dx12_105.fxp> <pso_id> <out.dxbc> [stage]

`stage` picks a shader by its pipeline field name (VS, PS, CS...); default is the first one.

Uses the same alignment as pso_names.py to find the PSO's DXBC hash in resources.bin, then copies the
matching blob out of the .fxp so it can be disassembled with `dxc -dumpbin`.
"""
import mmap
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from pso_names import pso_reads  # noqa: E402


def fxp_blob_by_hash(path, want):
    data = open(path, "rb").read()
    q = data.find(b"DXBC")
    while q != -1:
        if data[q + 4:q + 20] == want:
            size = struct.unpack_from("<I", data, q + 24)[0]
            return data[q:q + size]
        q = data.find(b"DXBC", q + 4)
    return None


def main():
    root, fxp, pso, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    want_stage = sys.argv[5] if len(sys.argv) > 5 else None
    reads = pso_reads(root)
    cum = [0]
    for _, _, size in reads:
        cum.append(cum[-1] + size)
    with open(os.path.join(root, "resources.bin"), "rb") as fh:
        mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        base = None
        pos = mm.find(b"DXBC")
        while pos != -1:
            cand = pos - 4
            if all(mm[cand + cum[k] + 4:cand + cum[k] + 8] == b"DXBC" for k in range(min(12, len(reads)))):
                base = cand
                break
            pos = mm.find(b"DXBC", pos + 1)
        for k, (pid, stage, _) in enumerate(reads):
            if pid == pso and (want_stage is None or stage == want_stage):
                h = bytes(mm[base + cum[k] + 8:base + cum[k] + 24])
                blob = fxp_blob_by_hash(fxp, h)
                if not blob:
                    sys.exit("hash not found in fxp")
                open(out, "wb").write(blob)
                print(f"PSO {pso} {stage}: {len(blob)} bytes -> {out}")
                return
    sys.exit("PSO not found")


if __name__ == "__main__":
    main()
