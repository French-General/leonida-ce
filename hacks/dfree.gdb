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
set $n = 0
set $cont = 0
break *0x14455aec0
commands
silent
set $cont = $rcx
continue
end
break *0x1400187fb
commands
silent
set $n = $n + 1
if $n <= 1
printf "BADFREE ptr=%lx container=%lx count=%d arr=%lx\n", $r14, $cont, *(unsigned short*)($cont+0x32), *(long*)($cont+0x38)
python
import gdb, struct
inf = gdb.selected_inferior()
cont = int(gdb.parse_and_eval("$cont"))
bad = int(gdb.parse_and_eval("$r14"))
cnt = struct.unpack("<H", inf.read_memory(cont + 0x32, 2))[0]
arr = struct.unpack("<Q", inf.read_memory(cont + 0x38, 8))[0]
print("vtable", hex(struct.unpack("<Q", inf.read_memory(cont, 8))[0]))
seen = {}
for i in range(min(cnt, 64)):
    try:
        ent = bytes(inf.read_memory(arr + i * 0x20, 0x20))
    except Exception as e:
        print("read fail", e); break
    p = struct.unpack_from("<Q", ent, 0x18)[0]
    mark = " <-- BAD" if p == bad else ""
    if p in seen: mark += " DUP of %d" % seen[p]
    seen.setdefault(p, i)
    print(" [%d] %s%s" % (i, ent.hex(), mark))
end
end
continue
end
continue
