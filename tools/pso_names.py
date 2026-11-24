"""Names the pipeline states in a PIX C++ export using Starfield's own shader archive.

Usage: py -3 -I tools/pso_names.py <export_dir> <shaderspc_dx12_105.fxp> [out.tsv]

How it works:
- CreatePSOs.cpp reads each shader blob from resources.bin with g_resourceReader->Read(data, <compressed size>).
  The PSOs are created contiguously (FrameResources_000.cpp), so the blobs sit back to back in resources.bin.
- Each compressed chunk (Windows XPRESS LZ77) starts with a 4-byte flag word followed by the literal bytes
  "DXBC" + the 16-byte container hash, so the hash can be read without decompressing.
- The game's .fxp archive groups DXBC blobs by technique name. Matching hashes names each PSO.
"""
import mmap
import os
import re
import struct
import sys


def fxp_hash_map(path):
    data = open(path, "rb").read()
    count = struct.unpack_from("<I", data, 0x2C)[0]
    p = 0x30
    names = {}
    for _ in range(count):
        n = struct.unpack_from("<I", data, p)[0]
        name = data[p + 4:p + 4 + n].rstrip(b"\0").decode("ascii", "replace")
        tid, gsize = struct.unpack_from("<II", data, p + 4 + n)
        start = p + 4 + n + 8
        end = start + gsize
        q, variant = data.find(b"DXBC", start), 0
        while q != -1 and q < end:
            names.setdefault(data[q + 4:q + 20], f"{name}#{variant}")
            size = struct.unpack_from("<I", data, q + 24)[0]
            variant += 1
            q = data.find(b"DXBC", q + max(size, 4))
        p = end
    return names


def pso_reads(root):
    """[(pso_id, stage, compressed_size)] in creation order."""
    order = re.findall(r"CreatePipelineState_(\d+)\(\);", open(os.path.join(root, "FrameResources_000.cpp"), encoding="utf-8", errors="replace").read())
    text = open(os.path.join(root, "CreatePSOs.cpp"), encoding="utf-8", errors="replace").read()
    bodies = {}
    for m in re.finditer(r"void CreatePipelineState_(\d+)\(\)\s*\{(.*?)\n\}", text, re.S):
        reads = []
        lines = m.group(2).splitlines()
        for i, line in enumerate(lines):
            r = re.search(r"g_resourceReader->Read\(data, (\d+)\)", line)
            if r:
                stage = "?"
                for nxt in lines[i + 1:i + 4]:
                    s = re.search(r"pssDesc\.(\w+) = \{", nxt)
                    if s:
                        stage = s.group(1)
                        break
                reads.append((stage, int(r.group(1))))
        bodies[m.group(1)] = reads
    out = []
    for pid in order:
        for stage, size in bodies.get(pid, []):
            out.append((int(pid), stage, size))
    return out


def main():
    root, fxp = sys.argv[1], sys.argv[2]
    out = open(sys.argv[3], "w", encoding="utf-8") if len(sys.argv) > 3 else sys.stdout
    names = fxp_hash_map(fxp)
    reads = pso_reads(root)
    print(f"fxp blobs {len(names)}, PSO shader reads {len(reads)}", file=sys.stderr)
    with open(os.path.join(root, "resources.bin"), "rb") as fh:
        mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        # Find the base offset: a "DXBC" at +4 for the first several chunks in sequence.
        cum = [0]
        for _, _, size in reads:
            cum.append(cum[-1] + size)
        base = None
        pos = mm.find(b"DXBC")
        while pos != -1:
            cand = pos - 4
            if all(mm[cand + cum[k] + 4:cand + cum[k] + 8] == b"DXBC" for k in range(min(12, len(reads)))):
                base = cand
                break
            pos = mm.find(b"DXBC", pos + 1)
        if base is None:
            sys.exit("could not align PSO shader chunks in resources.bin")
        print(f"PSO shader section starts at resources.bin offset {base}", file=sys.stderr)
        matched = 0
        for k, (pid, stage, size) in enumerate(reads):
            off = base + cum[k]
            ok = mm[off + 4:off + 8] == b"DXBC"
            h = bytes(mm[off + 8:off + 24]) if ok else b""
            name = names.get(h, "?" if ok else "<misaligned>")
            matched += name not in ("?", "<misaligned>")
            print(f"{pid}\t{stage}\t{name}", file=out)
        print(f"named {matched}/{len(reads)} shader blobs", file=sys.stderr)


if __name__ == "__main__":
    main()
