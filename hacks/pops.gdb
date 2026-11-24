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
seen = {}
out = open("/root/pops.txt", "w")
class Pop(gdb.Breakpoint):
    def stop(self):
        item = int(gdb.parse_and_eval("$rdx"))
        queue = int(gdb.parse_and_eval("$rsi"))
        if item:
            n = seen.get(item, 0) + 1
            seen[item] = n
            if n == 2 or len(seen) <= 5:
                out.write("pop item=%x queue=%x count=%d thread=%d total=%d\n" % (item, queue, n, gdb.selected_thread().num, len(seen)))
                out.flush()
        return False
Pop("*0x14503ce63")
end
continue
