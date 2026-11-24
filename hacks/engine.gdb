set pagination off
handle SIGUSR1 SIGUSR2 SIGILL SIGQUIT SIGSEGV nostop noprint pass
set {unsigned char}0x142219650 = 0x90
set {unsigned char}0x141647d03 = 0x90
set {unsigned char}0x1406eb888 = 0x90
set {unsigned char}0x1414a6390 = 0x90
set {unsigned char}0x144dcbc9d = 0x90
set $lock = 0
set $k = 0
break *0x1458109b0
commands
silent
if $lock == 0
set $lock = $rcx
printf "ENGINE-LOCK %lx thread=%d\n", $lock, $_thread
end
continue
end
break *0x140004bdf if $rsi == $lock
commands
silent
set $k = $k + 1
if $k <= 25
set $slot = *(unsigned int*)0x146e08b2c
set $blk = *(long*)(*(long*)($gs_base + 0x58) + 8 * $slot)
printf "ACQ k=%d thread=%d idx=%d word=%lx\n", $k, $_thread, *(int*)($blk + 0x1454), *(long*)$rsi
x/16gx $rsp
end
continue
end
continue
