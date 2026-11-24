"""Match wanted 24-byte code headers (leonida-psowant.txt) against the FSSM corpus; append to leonida-psomap.txt.

A full want list is 8,000-10,000 headers against 26,503 corpus files, and the straightforward loop is
O(files x headers): it runs for tens of minutes and, on one machine, did not finish at all. With numpy
present the same work takes about six minutes: each file's 8-byte windows are packed into integers and
intersected with the wanted prefixes, and the full 24-byte header is verified only where a prefix
matched. Without numpy it falls back to the loop, so the tool keeps working with the standard library
alone.
"""
import json
import os
import sys

try:
    import numpy as np
except ImportError:  # standard-library fallback below
    np = None

WORK = "/root/leonida-work/shaders"
want_path, map_path = "/root/leonida-psowant.txt", "/root/leonida-psomap.txt"

HEADER = 24
PREFIX = 8

have = set()
if os.path.exists(map_path):
    have = {line.split(" ", 1)[0] for line in open(map_path, encoding="utf-8-sig") if line.strip()}
wanted = [h.strip() for h in open(want_path, encoding="utf-8-sig")] if os.path.exists(want_path) else []
wanted = sorted({h for h in wanted if h and h not in have})
print(f"wanted: {len(wanted)} new headers")
if not wanted:
    sys.exit(0)

index = json.load(open(os.path.join(WORK, "spirv", "index.json")))
pats = {bytes.fromhex(h): h for h in wanted}
found = {}
fdir = os.path.join(WORK, "dxbc", "fssm")


def packed_prefixes(data):
    """Every PREFIX-byte window of `data`, packed into one integer, as a numpy array."""
    windows = np.lib.stride_tricks.sliding_window_view(data, PREFIX)[: data.size - HEADER + 1]
    keys = windows.astype(np.uint64)
    packed = np.zeros(keys.shape[0], dtype=np.uint64)
    for b in range(PREFIX):
        packed = (packed << np.uint64(8)) | keys[:, b]
    return packed


# The loop is better for a short want list: it stops as soon as every header is found, which is usually a
# few files in. The numpy path pays a fixed per-file cost, and only wins once the list is long enough that a
# full scan is unavoidable. Measured on 26,503 corpus files: 120 headers take 33 s in the loop and 112 s with
# numpy; 8,747 headers did not finish the loop in 20 minutes and take 370 s with numpy.
use_numpy = np is not None and len(wanted) > 500

if not use_numpy:
    for fn in os.listdir(fdir):
        if not fn.endswith(".fssm"):
            continue
        data = open(os.path.join(fdir, fn), "rb").read()
        for p, h in pats.items():
            if h not in found and p in data:
                name = fn[:-5]
                found[h] = (index.get(name, {}).get("stage", "?"), name)
        if len(found) == len(pats):
            break
else:
    by_prefix = {}
    for p, h in pats.items():
        packed = 0
        for b in p[:PREFIX]:
            packed = (packed << 8) | b
        by_prefix.setdefault(packed, []).append((p, h))
    for fn in os.listdir(fdir):
        if not fn.endswith(".fssm"):
            continue
        data = np.fromfile(os.path.join(fdir, fn), dtype=np.uint8)
        if data.size < HEADER:
            continue
        candidates = set(packed_prefixes(data).tolist()).intersection(by_prefix)
        if not candidates:
            continue
        blob = data.tobytes()
        for packed in candidates:
            for p, h in by_prefix[packed]:
                if h not in found and p in blob:
                    name = fn[:-5]
                    found[h] = (index.get(name, {}).get("stage", "?"), name)
        if len(found) == len(pats):
            break

with open(map_path, "a") as out:
    for h, (stage, name) in sorted(found.items()):
        sig = ";".join(f"{e['semantic']}:{e['semantic_index']}:{e['mask']}:{e['component_type']}"
                       for e in index.get(name, {}).get("input_signature", []) if e.get("system_value", 0) == 0)
        out.write(f"{h} {stage} {name}\t{sig}\n")
print(f"matched {len(found)}/{len(wanted)}; stages: " +
      ", ".join(f"{s}={sum(1 for v in found.values() if v[0] == s)}" for s in sorted({v[0] for v in found.values()})))
