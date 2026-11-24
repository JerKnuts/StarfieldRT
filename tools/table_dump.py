"""Dump what a descriptor table points at in a PIX C++ export.

Usage: py -3 -I tools/table_dump.py <export_dir> <heapVar> <startIndex> <count>
"""
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
from frame_map import load_resources, describe  # noqa: E402

root, heap, start, count = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
res = load_resources(root)
pat = re.compile(r"(Create\w+)\((?:GetResource\((\d+)\)\.Get\(\)|nullptr)?[^;]*?GetCpuDescriptor\((\w+)\.Get\(\), (\d+)\)")
found = {}
for f in glob.glob(os.path.join(root, "Descriptors_*.cpp")) + glob.glob(os.path.join(root, "ModifyDescriptors_*.cpp")):
    for line in open(f, encoding="utf-8", errors="replace"):
        m = pat.search(line)
        if m and m.group(3) == heap:
            idx = int(m.group(4))
            if start <= idx < start + count:
                found.setdefault(idx, []).append((m.group(1), int(m.group(2)) if m.group(2) else None))
for idx in range(start, start + count):
    entries = found.get(idx, [])
    text = "; ".join(f"{kind.replace('Create', '')} {describe(res, rid) if rid else ''}" for kind, rid in entries) or "(not written in capture)"
    print(f"  [{idx - start:2d}] {idx}: {text}")
