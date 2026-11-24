"""Match wanted 24-byte code headers (leonida-psowant.txt) against the FSSM corpus; append to leonida-psomap.txt."""
import json
import os
import sys

WORK = "/root/leonida-work/shaders"
want_path, map_path = "/root/leonida-psowant.txt", "/root/leonida-psomap.txt"

have = set()
if os.path.exists(map_path):
    have = {line.split(" ", 1)[0] for line in open(map_path) if line.strip()}
wanted = [h.strip() for h in open(want_path)] if os.path.exists(want_path) else []
wanted = sorted({h for h in wanted if h and h not in have})
print(f"wanted: {len(wanted)} new headers")
if not wanted:
    sys.exit(0)

index = json.load(open(os.path.join(WORK, "spirv", "index.json")))
pats = {bytes.fromhex(h): h for h in wanted}
found = {}
fdir = os.path.join(WORK, "dxbc", "fssm")
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

with open(map_path, "a") as out:
    for h, (stage, name) in sorted(found.items()):
        sig = ";".join(f"{e['semantic']}:{e['semantic_index']}:{e['mask']}:{e['component_type']}"
                       for e in index.get(name, {}).get("input_signature", []) if e.get("system_value", 0) == 0)
        out.write(f"{h} {stage} {name}\t{sig}\n")
print(f"matched {len(found)}/{len(wanted)}; stages: " +
      ", ".join(f"{s}={sum(1 for v in found.values() if v[0] == s)}" for s in sorted({v[0] for v in found.values()})))
