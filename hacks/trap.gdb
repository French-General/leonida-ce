set pagination off
handle SIGUSR1 SIGUSR2 SIGILL SIGQUIT nostop noprint pass
handle SIGSEGV stop noprint nopass
set {unsigned char}0x142219650 = 0x90
set {unsigned char}0x141647d03 = 0x90
set {unsigned char}0x1406eb888 = 0x90
set {unsigned char}0x1414a6390 = 0x90
set {unsigned char}0x144dcbc9d = 0x90
set {unsigned char}0x14001881f = 0x90
set {unsigned char}0x14001884a = 0x90
set {unsigned char}0x1400187fb = 0x90
python
import gdb, struct
PAGE = 0x14f341000
inf = gdb.selected_inferior()
log = open("/root/trap.txt", "w")
seen = {}

def protect(prot):
    gdb.execute("call (int)mprotect(0x%x, 0x1000, %d)" % (PAGE, prot), to_string=True)

def floats():
    return bytes(inf.read_memory(PAGE, 0x1000)).count(struct.pack("<I", 0x3f800000))

def callers():
    out = []
    try:
        mem = bytes(inf.read_memory(int(gdb.parse_and_eval("$sp")), 0x600))
        for i in range(0, 0x600, 8):
            v = struct.unpack_from("<Q", mem, i)[0]
            if 0x140001000 <= v < 0x146000000:
                out.append(hex(v - 0x140000000))
            if len(out) >= 6:
                break
    except Exception:
        pass
    return " ".join(out)

log.write("floats in page at attach: %d\n" % floats())
log.flush()
protect(1)
resume = "continue"
handled = 0
while True:
    try:
        gdb.execute(resume, to_string=True)
    except Exception as e:
        log.write("stopped: %s\n" % e)
        break
    resume = "continue"
    try:
        sig = str(gdb.parse_and_eval("$_siginfo.si_signo"))
        addr = int(gdb.parse_and_eval("$_siginfo._sifields._sigfault.si_addr"))
    except Exception:
        continue
    if sig != "11" or not (PAGE <= addr < PAGE + 0x1000):
        resume = "signal SIGSEGV" if sig == "11" else "continue"
        continue
    pc = int(gdb.parse_and_eval("$pc"))
    before = floats()
    protect(3)
    gdb.execute("stepi", to_string=True)
    after = floats()
    handled += 1
    if pc not in seen:
        seen[pc] = 0
        log.write("writer pc=%x addr=%x floats %d->%d callers %s\n" % (pc, addr, before, after, callers()))
    seen[pc] += 1
    if after > before:
        log.write("  FLOATS by pc=%x addr=%x %d->%d callers %s\n" % (pc, addr, before, after, callers()))
    log.flush()
    if handled < 5000:
        protect(1)
log.write("handled %d writes, %d distinct writers\n" % (handled, len(seen)))
log.close()
end
