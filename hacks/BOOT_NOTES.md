# Boot hacks: getting `gta6rt --run` past the documented chain (Linux, Wine 9)

**Status: exploratory hacks, not pull-request quality.** Measured on one machine (Ubuntu 24.04, Wine 9.0,
3x RTX 6000 Ada, the Xbox Series 1.0.2.1 package) on 2026-11-23/24. Everything here is a stand-in to see the next
stop; most of it needs a real implementation. The scripts hard-code the paths of that machine (`/root/...`).

Some stand-ins use addresses inside the 1.0.2.1 executable (the watchdog flag, the exported allocation hooks
fallback). They only work with that exact version, which `make_game.py --check` enforces.

## Where it gets

About 90 s into startup under Wine. The renderer is initialised: the device, queues, the 4K swap-chain resources,
the descriptor heaps at the title's addresses, all startup compute shaders but four, and every graphics
pipeline the title asks for. DirectStorage streams and inflates `shaders.rpf`. The current stop is a
fence wait (a token whose fence slot holds a pointer that is not a console fence) right before the first
`PresentX`.

## Stops and what got past them

Continues `docs/retail_boot_chain.md` (stops 1-13). Slots are 0-based vtable indexes.

| Stop | Finding | Stand-in |
|---|---|---|
| GameInput | The title imports `gameinput.dll!#1` **by ordinal**; the shim registers it by name only. Without SDL3 the shim is silently left out of the build | `-DLEONIDA_SDL3_FETCH=ON`, `#1` registered too |
| DirectStorage | `DStorageGetFactory` with a console factory IID. Factory slot 4 = OpenFile, 5 = CreateStatusArray, 10 = queue creation. Queue slot 3 = EnqueueRequest, 4 = EnqueueStatus, 6 = Submit; status slot 3 = IsComplete. Request: +0 options (low byte 1 = zlib), +8 destination, +0x18 destination size, +0x20 file, +0x28 offset, +0x30 source size | `shims/dstorage_x.cpp`: synchronous reads, zlib through `shader_library.h`'s inflater, always complete |
| Device slot 26 | GetCustomHeapProperties with the struct-return buffer in RCX and the device in RDX | special case |
| Device slot 48 | CreateCommandQueueX with console flags 2 and 3 | flags masked to bit 0 |
| Device slot 73 | The title asks for a console graphics-list IID | mapped to `ID3D12GraphicsCommandList`, plus a console-order list wrapper (slots 0-26 = PC, 31-63 = PC-4, 146 = GCL7 Barrier, the rest logged) |
| Physical pages | Wine 9's `MapViewOfFile3` rejects `PAGE_WRITECOMBINE` (error 87) | write-combine/no-cache bits dropped |
| Stop 13 (descriptor heap) | The heap description is 32 bytes: the PC 16, then `{address, size}` of memory the title reserved | the heap returns that address; arguments in such a range are moved to the PC heap before forwarded device and list calls |
| Device slots 25, 108 | GetResourceAllocationInfo / ...Info1 with swapped struct return; some console formats (A8_UNORM render targets) give UINT64_MAX on the PC | special cases plus a size estimate |
| Device slot 127 | CreateCommittedResource3 shape | routed to CreateCommittedResource |
| Resource memory | The console D3D runtime takes resource memory from the title's exported `__XMemAllocationHooks__` (allocator id 128 in bits 16-23, memory type 0x8000); the title checks its pools afterwards | slot 127 calls the title's hook with the resource size |
| Device slot 41 | Indirect argument type 105 (console only, 12-byte stride); Leonida has no DISPATCH_MESH signatures | both mapped to DISPATCH |
| Device slots 61, 70 | Sampler and buffer-view writes with console descriptions | ignored (logged) |
| Device slot 74 | `(va, page_count, &out)` after the title maps its ~6 GB physical-page GPU pool | a logging probe object |
| Device slot 57 | Not a PC description: a 0x160-byte console pipeline record (root signature, six `{ptr, size}` blobs, hashes). Blobs 2 and 4 are native code; their **first 24 bytes match exactly one corpus FSSM program** | `psomatch.py` maps those headers to corpus shaders; the pipeline is built from the corpus DXBC with an input layout from the shader's input signature |
| Runtime compute shaders | The title's compute pipelines carry FSPX programs from `shaders.rpf` that the offline stage never carved | dumped at run time, translated by `leonida-shaders-pass.sh` into `spirv/fspx_game/` (loaded as a third index folder); 38 of 42 translate |
| Hang watchdog | A title thread kills the process when something stalls 30 s; Vulkan pipeline creation is much slower than the console's precompiled pipelines | the title's own off switch is set at start |
| Queue slot 18 | **GetTimestampFrequency** (the console queue runs 2 slots ahead of the PC one past Wait). Forwarding it to PC GetDesc wrote 16 bytes into an 8-byte local and corrupted the caller's saved RBX: this was the "null render context" crash | queue 18/19 mapped to GetTimestampFrequency/GetClockCalibration |
| Queue slot 32 | **PresentX** `(planeCount, planes, params)`, the newer layout `xbox_present.h` lists | accepted and logged (no window yet) |
| Queue slot 35 | ExecuteCommandLists with two extra words | routed to ExecuteCommandLists |

## Loops

- `leonida-try.sh <name> <seconds>`: repackage the runtime and run the title once under Wine (`/root/<name>.log`
  and the startup trace `/root/<name>.jsonl`).
- `leonida-shaders-pass.sh`: translate compute containers the runtime dumped to `/root/leonida-dump/`.
- `psomatch.py`: match code headers the runtime wanted (`/root/leonida-psowant.txt`) against the FSSM corpus and
  append them to `/root/leonida-psomap.txt` (not committed: it holds bytes of the title's code).
- `gdbat.sh <seconds> <gdb file>`: start the title and attach gdb after a delay. Hardware watchpoints do not work
  under Wine (it rewrites the debug registers); bisect with breakpoints instead.

## Next

1. The fence-wait stop: the token's fence table entry and what queue/device call should have produced it.
2. Read back `PresentX` plane 0 and write frames to disk, so frames can be checked without a display.
3. Real implementations for the ignored sampler and view slots (61, 70) and the probe at 74.
