set pagination off
handle SIGUSR1 SIGUSR2 SIGILL SIGQUIT SIGSEGV nostop noprint pass
set {unsigned char}0x142219650 = 0x90
set {unsigned char}0x141647d03 = 0x90
set {unsigned char}0x1406eb888 = 0x90
set {unsigned char}0x1414a6390 = 0x90
set {unsigned char}0x144dcbc9d = 0x90
set {unsigned char}0x14001881f = 0x90
set {unsigned char}0x14001884a = 0x90
set {unsigned char}0x1400187fb = 0x90
python
import gdb
popped = {}
freed = {}
out = open("/root/popfree.txt", "w")
class Pop(gdb.Breakpoint):
    def stop(self):
        item = int(gdb.parse_and_eval("$rdx"))
        if item:
            popped[item] = popped.get(item, 0) + 1
            if item in freed:
                out.write("POP-AFTER-FREE item=%x (popped %d, freed %d) thread=%d\n" % (item, popped[item], freed[item], gdb.selected_thread().num))
                out.flush()
        return False
class Free(gdb.Breakpoint):
    def stop(self):
        p = int(gdb.parse_and_eval("$rdx"))
        if p in popped:
            freed[p] = freed.get(p, 0) + 1
            rets = []
            try:
                import struct
                sp = int(gdb.parse_and_eval("$sp"))
                mem = bytes(gdb.selected_inferior().read_memory(sp, 0x300))
                for i in range(0, 0x300, 8):
                    v = struct.unpack_from("<Q", mem, i)[0]
                    if 0x140001000 <= v < 0x146000000:
                        rets.append(hex(v - 0x140000000))
                    if len(rets) >= 6:
                        break
            except Exception:
                pass
            out.write("FREE of popped item %x (popped %d, freed %d) thread=%d callers %s\n" % (p, popped[p], freed[p], gdb.selected_thread().num, " ".join(rets)))
            out.flush()
        return False
Pop("*0x14503ce63")
f = Free("*0x1400187b0")
f.condition = "$rdx >= 0x14e000000 && $rdx < 0x150000000"
end
continue
