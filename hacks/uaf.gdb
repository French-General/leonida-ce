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
break *0x1413360a3
commands
silent
if ($rax & 0xffff800000000000) != 0 && ($rax & 0xffff800000000000) != 0xffff800000000000
set $n = $n + 1
if $n <= 1
printf "UAF obj=%lx vtable=%lx thread=%d\n", $rsi, $rax, $_thread
x/12gx $rsi-0x20
x/24gx $rsp
end
end
continue
end
break *0x1400187fb
commands
silent
printf "BADFREE ptr=%lx\n", $r14
continue
end
continue
