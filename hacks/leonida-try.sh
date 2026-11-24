#!/bin/sh
# Repackage the runtime and run the game once under Wine; $1 = log name, $2 = seconds
cd /root/Leonida && LLVM_MC=/usr/bin/llvm-mc-21 python3 -I make_game.py --game /root/hi --stages runtime --runtime-build runtime/build/windows --work /root/leonida-work --out /root/leonida-out --no-tui >/dev/null 2>&1 || echo "package failed"
export WINEPREFIX=/root/.local/share/leonida-wine WINEDEBUG=-all LEONIDA_D3D12=vulkan
O=/root/leonida-out; cd /root/hi; rm -f /root/$1.jsonl
timeout -k 10 ${2:-90} xvfb-run -a wine $O/gta6rt.exe --run ./game_scarlett_master.exe --overlay "Z:$O/overlay" --trace Z:/root/$1.jsonl > /root/$1.log 2>&1
echo "exit=$?"; wineserver -k 2>/dev/null
grep -E "^\[(rt|dstorage)\]|Unhandled|^0x[0-9a-f]+: " /root/$1.log | grep -v "TLS reads" | head -${3:-40}
