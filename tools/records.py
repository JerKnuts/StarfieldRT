"""Dump Starfield scene record buffers from a PIX C++ export to help decode their layouts.

Usage: py -3 -I tools/records.py <export_dir> <resource_id> <stride> [count] [start]
Prints `count` non-zero records from `start`, each dword as hex / int / float.
"""
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(__file__))
from capture_data import Capture  # noqa: E402


def fmt_dword(raw):
    u = struct.unpack("<I", raw)[0]
    f = struct.unpack("<f", raw)[0]
    fs = f"{f:.4g}" if 1e-6 < abs(f) < 1e7 else "-"
    return f"{u:08x}|{u:>10}|{fs:>9}"


def main():
    root, rid, stride = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    count = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    start = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    data = Capture(root).resource_data(rid)
    n = len(data) // stride
    nonzero = sum(1 for i in range(n) if any(data[i * stride:(i + 1) * stride]))
    print(f"resource #{rid}: {len(data)} bytes, {n} records of {stride} B, {nonzero} non-zero")
    shown = 0
    for i in range(start, n):
        rec = data[i * stride:(i + 1) * stride]
        if not any(rec):
            continue
        print(f"[{i}]")
        for off in range(0, stride, 4):
            print(f"   +{off:2d}: {fmt_dword(rec[off:off + 4])}")
        shown += 1
        if shown >= count:
            break


if __name__ == "__main__":
    main()
