#!/bin/bash
# Translate every dumped runtime shader container into <work>/shaders/spirv/fspx_game
set -e
W=/root/leonida-work/shaders; L=/root/Leonida/tools
n=$(ls /root/leonida-dump/*.dxbc 2>/dev/null | wc -l); echo "dumped containers: $n"; [ "$n" = 0 ] && exit 0
rm -rf $W/fspx_game; mkdir -p $W/spirv/fspx_game
LLVM_MC=/usr/bin/llvm-mc-21 python3 -I $L/fspx_unpack.py /root/leonida-dump/*.dxbc --out $W/fspx_game --disasm 2>&1 | tail -3
LLVM_MC=/usr/bin/llvm-mc-21 python3 -I $L/rdna2spirv.py $W/fspx_game/disasm --out $W/spirv/fspx_game --no-asm --wave64-emulate 2>&1 | tail -3
