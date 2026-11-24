#!/bin/bash
# gdbat.sh <secs> <gdb file>: start the game, attach gdb after <secs>
export WINEPREFIX=/root/.local/share/leonida-wine WINEDEBUG=-all LEONIDA_D3D12=vulkan
O=/root/leonida-out; cd /root/hi; rm -f /root/g.jsonl
export DISPLAY=:78; Xvfb :78 -screen 0 1920x1080x24 >/dev/null 2>&1 & XP=$!
sleep 1
wine $O/gta6rt.exe --run ./game_scarlett_master.exe --overlay "Z:$O/overlay" --trace Z:/root/g.jsonl > /root/g.log 2>&1 &
sleep $1
pid=$(for p in $(pgrep -f gta6rt.exe); do grep -q "^140000000-" /proc/$p/maps 2>/dev/null && echo $p; done | head -1)
echo "pid=$pid"
timeout 200 gdb -p $pid -batch -nx -x "$2" > /root/gdbat.out 2>&1
sleep 1; wineserver -k 2>/dev/null; kill $XP 2>/dev/null; true
