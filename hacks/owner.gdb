set pagination off
python
import gdb, struct
lo, hi = 0x140001000, 0x147000000
inf = gdb.selected_inferior()
out = []
info = {}
slot = struct.unpack_from("<I", inf.read_memory(0x146e08b2c, 4), 0)[0]
out.append("game TLS slot %d" % slot)
for th in inf.threads():
    th.switch()
    try:
        gs = int(gdb.parse_and_eval("$gs_base"))
        teb = inf.read_memory(gs, 0x60)
        base, limit = struct.unpack_from("<QQ", teb, 8)
        tlsp = struct.unpack_from("<Q", teb, 0x58)[0]
        blk = struct.unpack_from("<Q", inf.read_memory(tlsp + 8 * slot, 8), 0)[0]
        idx = struct.unpack_from("<I", inf.read_memory(blk + 0x1454, 4), 0)[0]
        if not (limit < base and base - limit < (64 << 20)):
            continue
        mem = bytes(inf.read_memory(limit, base - limit))
    except Exception:
        continue
    info[th.ptid[1]] = (idx, limit, mem)
out.append("threads: %d" % len(info))
for needle in (struct.pack("<Q", 0x145594661), struct.pack("<Q", 0x145267740)):
  for lwp, (idx, limit, mem) in info.items():
    i = mem.find(needle)
    while i >= 0:
        if i % 8 == 0 and i >= 24:
            obj = struct.unpack_from("<Q", mem, i - 24)[0]
            try:
                word = struct.unpack_from("<Q", inf.read_memory(obj, 8), 0)[0]
            except Exception:
                word = 0
            owner = (word >> 32) & 0xffffffff
            out.append("audio lwp=%d idx=%d slot=%x obj=%x lockword=%x owner_idx=%d" % (lwp, idx, limit + i, obj, word, owner))
            for olwp, (oidx, olimit, omem) in info.items():
                if oidx == owner:
                    rets = []
                    for j in range(0, len(omem) - 8, 8):
                        v = struct.unpack_from("<Q", omem, j)[0]
                        if lo <= v < hi:
                            rets.append(hex(v - 0x140000000))
                            if len(rets) >= 40:
                                break
                    out.append("  owner lwp=%d frames(low->high): %s" % (olwp, " ".join(rets)))
        i = mem.find(needle, i + 1)
open("/root/owner.txt", "w").write("\n".join(out) + "\n")
end
detach
