"""Builds a pass-level map of a PIX C++ export (pixtool export-to-cpp).

Usage: py -3 -I tools/frame_map.py <export_dir> [out.txt] [pso_names.tsv]

For every Dispatch / Draw / ExecuteIndirect it records the pipeline state, the bound
render targets / depth target, and resources transitioned into UNORDERED_ACCESS since
the previous piece of work (compute outputs), then groups consecutive work with the same
pipeline state on the same command list into "passes".
"""
import glob
import os
import re
import sys
from collections import defaultdict

FMT = {  # DXGI format names come straight from the export text
}


def load_resources(root):
    res = {}
    pat_tex = re.compile(r"void CreateAndInitResource_(\d+)\(\)")
    pat_desc = re.compile(r"D3D12_RESOURCE_DESC resourceDesc = \{ D3D12_RESOURCE_DIMENSION_(\w+), \d+, (\d+), (\d+), (\d+), (\d+), (DXGI_FORMAT_\w+),[^}]*\}, \w+, ([\w| ]+) \}")
    for f in glob.glob(os.path.join(root, "CreateAndInitResources_*.cpp")):
        cur = None
        with open(f, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = pat_tex.search(line)
                if m:
                    cur = int(m.group(1))
                    continue
                if cur is not None:
                    d = pat_desc.search(line)
                    if d:
                        dim, w, h, depth, mips, fmt, flags = d.groups()
                        res[cur] = (dim, int(w), int(h), int(depth), int(mips), fmt.replace("DXGI_FORMAT_", ""), flags.strip())
                        cur = None
    return res


def describe(res, rid):
    r = res.get(rid)
    if not r:
        return f"#{rid}"
    dim, w, h, depth, mips, fmt, flags = r
    if dim == "BUFFER":
        return f"#{rid} buffer {w}B"
    extra = f"x{depth}" if depth > 1 else ""
    return f"#{rid} {dim.replace('TEXTURE', 'tex')} {w}x{h}{extra} {fmt}" + (f" mips{mips}" if mips > 1 else "")


def load_uav_descriptors(root):
    """(heap, index) -> resource id for UAV descriptors."""
    m = {}
    pat = re.compile(r"CreateUnorderedAccessView_\w+\(GetResource\((\d+)\)\.Get\(\), [^,]+, GetCpuDescriptor\((\w+)\.Get\(\), (\d+)\)")
    for f in glob.glob(os.path.join(root, "Descriptors_*.cpp")) + glob.glob(os.path.join(root, "ModifyDescriptors_*.cpp")):
        with open(f, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                x = pat.search(line)
                if x:
                    m[(x.group(2), int(x.group(3)))] = int(x.group(1))
    return m


RE_LIST = re.compile(r"GetCommandList\((\d+)\)->(\w+)\((.*)\);")
RE_GID = re.compile(r"// GlobalId\s+= (\d+)")
RE_RTV = re.compile(r"CreateRenderTargetView\(GetResource\((\d+)\)")
RE_DSV = re.compile(r"CreateDepthStencilView\(GetResource\((\d+)\)")
RE_TRANS = re.compile(r"Transition\(GetResource\((\d+)\)\.Get\(\), ([\w| ]+), ([\w| ]+), ")
RE_PSO = re.compile(r"GetPipelineState\((\d+)\)")
RE_TABLE = re.compile(r"SetComputeRootDescriptorTable\((\d+), GetGpuDescriptor\((\w+)\.Get\(\), (\d+)\)\)")


def parse_lists(root):
    work = []
    state = defaultdict(lambda: {"pso": None, "rtv": [], "dsv": None, "pending_rtv": [], "pending_dsv": None,
                                 "uav_writes": [], "tables": {}})
    files = sorted(glob.glob(os.path.join(root, "CommandLists_*.cpp")))
    gid = None
    pending_rtv, pending_dsv, pending_trans = [], None, []
    for f in files:
        with open(f, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                g = RE_GID.search(line)
                if g:
                    gid = int(g.group(1))
                    continue
                r = RE_RTV.search(line)
                if r:
                    pending_rtv.append(int(r.group(1)))
                    continue
                d = RE_DSV.search(line)
                if d:
                    pending_dsv = int(d.group(1))
                    continue
                for t in RE_TRANS.finditer(line):
                    pending_trans.append((int(t.group(1)), t.group(2), t.group(3)))
                m = RE_LIST.search(line)
                if not m:
                    continue
                lst, method, args = int(m.group(1)), m.group(2), m.group(3)
                s = state[lst]
                if method == "ResourceBarrier":
                    for rid, before, after in pending_trans:
                        if "UNORDERED_ACCESS" in after and "UNORDERED_ACCESS" not in before:
                            s["uav_writes"].append(rid)
                    pending_trans = []
                elif method == "OMSetRenderTargets":
                    s["rtv"] = pending_rtv
                    s["dsv"] = pending_dsv if "dsvHandle" in args else None
                    pending_rtv, pending_dsv = [], None
                elif method == "SetPipelineState":
                    p = RE_PSO.search(args)
                    s["pso"] = int(p.group(1)) if p else None
                    s["tables"] = {}
                elif method == "SetComputeRootDescriptorTable":
                    t = RE_TABLE.search(line)
                    if t:
                        s["tables"][int(t.group(1))] = (t.group(2), int(t.group(3)))
                elif method in ("Dispatch", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect"):
                    work.append({"gid": gid, "list": lst, "kind": method, "args": args, "pso": s["pso"],
                                 "rtv": list(s["rtv"]), "dsv": s["dsv"], "uav": list(s["uav_writes"]),
                                 "tables": dict(s["tables"]), "file": os.path.basename(f)})
                    s["uav_writes"] = []
    return work


def group_passes(work):
    passes = []
    for w in work:
        p = passes[-1] if passes else None
        if p and p["list"] == w["list"] and p["pso"] == w["pso"] and p["rtv"] == w["rtv"]:
            p["items"].append(w)
            p["uav"].extend(x for x in w["uav"] if x not in p["uav"])
        else:
            passes.append({"list": w["list"], "pso": w["pso"], "rtv": w["rtv"], "dsv": w["dsv"], "items": [w],
                           "uav": list(w["uav"])})
    return passes


def load_pso_names(path):
    names = {}
    if path and os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            parts = line.rstrip("\n").split("\t")
            if len(parts) == 3:
                names.setdefault(int(parts[0]), parts[2])
    return names


def main():
    root = sys.argv[1]
    out = open(sys.argv[2], "w", encoding="utf-8") if len(sys.argv) > 2 else sys.stdout
    psoNames = load_pso_names(sys.argv[3] if len(sys.argv) > 3 else None)
    res = load_resources(root)
    uavd = load_uav_descriptors(root)
    work = parse_lists(root)
    passes = group_passes(work)
    print(f"resources {len(res)}, uav descriptors {len(uavd)}, work items {len(work)}, passes {len(passes)}", file=out)
    for i, p in enumerate(passes):
        items = p["items"]
        kinds = defaultdict(int)
        for w in items:
            kinds[w["kind"]] += 1
        first = items[0]
        line = (f"P{i:04d} gid {first['gid']}-{items[-1]['gid']} list {p['list']} pso {p['pso']} {psoNames.get(p['pso'], '')} | "
                + ", ".join(f"{k} x{v}" for k, v in kinds.items()) + f" | first {first['kind']}({first['args'][:60]})")
        print(line, file=out)
        if p["rtv"]:
            print("      RT : " + "; ".join(describe(res, r) for r in p["rtv"]), file=out)
        if p["dsv"]:
            print("      DS : " + describe(res, p["dsv"]), file=out)
        if p["uav"]:
            print("      UAV: " + "; ".join(describe(res, r) for r in p["uav"][:8]) + (" ..." if len(p["uav"]) > 8 else ""), file=out)
        if first["kind"] == "Dispatch" and first["tables"]:
            bound = []
            for slot, (heap, idx) in sorted(first["tables"].items()):
                for k in range(8):
                    rid = uavd.get((heap, idx + k))
                    if rid is not None and rid not in bound:
                        bound.append(rid)
            tex = [describe(res, r) for r in bound if res.get(r, ("BUFFER",))[0] != "BUFFER"]
            if tex:
                print("      tbl-UAV: " + "; ".join(tex[:6]), file=out)


if __name__ == "__main__":
    main()
