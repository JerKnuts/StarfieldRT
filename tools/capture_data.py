"""Read initial resource contents out of a PIX C++ export (pixtool export-to-cpp).

resources.bin is one stream of XPRESS-compressed chunks consumed by g_resourceReader->Read(buf, size) in
execution order. This module replays the export's call tree from CreateAppResources_000/_001 to work out
each chunk's offset, then decompresses with Windows' own Compression API (cabinet.dll).

    from capture_data import Capture
    cap = Capture(r"analysis/cpp_export")
    data = cap.resource_data(11006)   # bytes uploaded into resource #11006 at capture start
"""
import ctypes
import glob
import mmap
import os
import re
from ctypes import wintypes

_FUNC = re.compile(r"^(?:static\s+)?void (\w+)\(\)\s*$", re.M)
_CALL = re.compile(r"^\s*(\w+)\(\);\s*$")
_READ = re.compile(r"g_resourceReader->Read\(\w+, (\d+)\)")
_RES_FN = re.compile(r"CreateAndInitResource_(\d+)$")
_COPY = re.compile(r"CopyBytesToMappableResource\(&uncompressedData\[(\d+)\], (\d+), GetResource\((\d+)\)\.Get\(\), (\d+)u?\)")
_COPY_STAGE = re.compile(r"CopyBytesToMappableResource\(&uncompressedData\[(\d+)\], (\d+), destAlloc\.GetResource\(\)")
_COPY_REGION = re.compile(r"CopyBufferRegion\(GetResource\((\d+)\)\.Get\(\), (\d+), destAlloc\.GetResource\(\), destAlloc\.GetOffset\(\), (\d+)\)")


class _Xpress:
    COMPRESS_ALGORITHM_XPRESS = 3

    def __init__(self):
        self.cab = ctypes.WinDLL("cabinet")
        self.handle = ctypes.c_void_p()
        if not self.cab.CreateDecompressor(self.COMPRESS_ALGORITHM_XPRESS, None, ctypes.byref(self.handle)):
            raise OSError("CreateDecompressor failed")
        self.cab.Decompress.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                        ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        self.cab.Decompress.restype = wintypes.BOOL

    def decompress(self, data):
        src = ctypes.create_string_buffer(data, len(data))
        need = ctypes.c_size_t(0)
        self.cab.Decompress(self.handle, src, len(data), None, 0, ctypes.byref(need))
        out = ctypes.create_string_buffer(need.value)
        got = ctypes.c_size_t(0)
        if not self.cab.Decompress(self.handle, src, len(data), out, need.value, ctypes.byref(got)):
            raise OSError("Decompress failed")
        return out.raw[:got.value]


class Capture:
    def __init__(self, root):
        self.root = root
        self.bodies = {}
        for f in glob.glob(os.path.join(root, "*.cpp")):
            text = open(f, encoding="utf-8", errors="replace").read()
            for m in _FUNC.finditer(text):
                start = text.find("{", m.end())
                end = text.find("\n}", start)
                self.bodies.setdefault(m.group(1), text[start + 1:end].splitlines())
        self.reads = []          # (function, offset, compressed size) in execution order
        self.by_function = {}    # function -> [index into reads]
        self._offset = 0         # chunks start at 0; each has a 24-byte Compression API header
        self._visiting = set()
        for top in ("CreateAppResources_000", "CreateAppResources_001"):
            self._walk(top)
        fh = open(os.path.join(root, "resources.bin"), "rb")
        self.mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        self.xp = _Xpress()

    def _walk(self, name):
        if name in self._visiting:
            return
        self._visiting.add(name)
        for line in self.bodies.get(name, []):
            r = _READ.search(line)
            if r:
                size = int(r.group(1))
                self.by_function.setdefault(name, []).append(len(self.reads))
                self.reads.append((name, self._offset, size))
                self._offset += size
                continue
            c = _CALL.match(line)
            if c and c.group(1) in self.bodies:
                self._walk(c.group(1))
        self._visiting.discard(name)

    def chunk(self, index):
        _, off, size = self.reads[index]
        return self.xp.decompress(bytes(self.mm[off:off + size]))

    def _segments(self, rid):
        """[(dst offset, chunk index, src offset, size)] describing how `rid` was initialized."""
        fn = f"CreateAndInitResource_{rid}"
        if fn not in self.by_function:
            return []
        segs, chunk_no, staged = [], -1, None
        reads = iter(self.by_function[fn])
        for line in self.bodies[fn]:
            if _READ.search(line):
                chunk_no = next(reads)
                continue
            m = _COPY_STAGE.search(line)
            if m:  # CopyBytesToMappableResource(&uncompressedData[src], n, destAlloc...) -> upload
                staged = (int(m.group(1)), int(m.group(2)))
                continue
            m = _COPY_REGION.search(line)
            if m and staged and int(m.group(1)) == rid:  # CopyBufferRegion(GetResource(rid), dst, destAlloc, ..., n)
                segs.append((int(m.group(2)), chunk_no, staged[0], int(m.group(3))))
                staged = None
                continue
            m = _COPY.search(line)
            if m and int(m.group(3)) == rid:  # direct copy into a mappable resource
                segs.append((int(m.group(4)), chunk_no, int(m.group(1)), int(m.group(2))))
        return segs

    def resource_view(self, rid):
        """Sparse random-access view of a resource's initial contents (use for huge resources)."""
        return SparseResource(self, self._segments(rid))

    def resource_data(self, rid):
        """Initial contents uploaded into resource `rid` (bytes, zero-filled gaps), or None."""
        segs = self._segments(rid)
        if not segs:
            fn = f"CreateAndInitResource_{rid}"
            idx = self.by_function.get(fn)
            return self.chunk(idx[0]) if idx else None
        out = bytearray(max(d + n for d, _, _, n in segs))
        cache = {}
        for dst, c, src, n in segs:
            if c not in cache:
                cache[c] = self.chunk(c)
            out[dst:dst + n] = cache[c][src:src + n]
        return bytes(out)


class SparseResource:
    """Random access into a resource assembled from upload segments, without materializing it."""

    def __init__(self, cap, segs):
        import bisect
        self._bisect = bisect
        self.cap = cap
        self.segs = sorted(segs)
        self.starts = [s[0] for s in self.segs]
        self._chunks = {}

    def _chunk(self, c):
        if c not in self._chunks:
            self._chunks[c] = self.cap.chunk(c)
        return self._chunks[c]

    def read(self, offset, size):
        out = bytearray(size)
        pos = offset
        while pos < offset + size:
            i = self._bisect.bisect_right(self.starts, pos) - 1
            if i < 0 or pos >= self.segs[i][0] + self.segs[i][3]:
                nxt = self.starts[i + 1] if i + 1 < len(self.starts) else offset + size
                pos = min(max(nxt, pos + 1), offset + size)  # unmapped / never uploaded: zeros
                continue
            dst, c, src, n = self.segs[i]
            take = min(dst + n, offset + size) - pos
            data = self._chunk(c)
            out[pos - offset:pos - offset + take] = data[src + (pos - dst):src + (pos - dst) + take]
            pos += take
        return bytes(out)

    def covered(self, offset, size):
        i = self._bisect.bisect_right(self.starts, offset) - 1
        return i >= 0 and offset + size <= self.segs[i][0] + self.segs[i][3]


if __name__ == "__main__":
    import sys
    cap = Capture(sys.argv[1])
    print(f"{len(cap.reads)} reads, stream ends at {cap._offset} of {len(cap.mm)} bytes")
    for rid in map(int, sys.argv[2:]):
        d = cap.resource_data(rid)
        print(rid, None if d is None else len(d))
