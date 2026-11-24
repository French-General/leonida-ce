# Boot hacks: getting `gta6rt --run` past the documented chain (Linux, Wine 9)

**Status: exploratory hacks, not pull-request quality.** Measured on one machine (Ubuntu 24.04, Wine 9.0,
3x RTX 6000 Ada, the Xbox Series 1.0.2.1 package) on 2026-11-23/24. Everything here is a stand-in to see the next
stop; most of it needs a real implementation. The scripts hard-code the paths of that machine (`/root/...`).

Some stand-ins use addresses inside the 1.0.2.1 executable (the watchdog flag, the exported allocation hooks
fallback). They only work with that exact version, which `make_game.py --check` enforces.

## Where it gets

About a minute into startup under Wine, past the first frame. The renderer is initialised: the device, queues, the 4K swap-chain resources,
the descriptor heaps at the title's addresses, all startup compute shaders but four, and every graphics
pipeline the title asks for. DirectStorage streams and inflates `shaders.rpf`. The title submits GPU work,
signals and polls its fences, and calls `PresentX` with its first frame. The current stop is a descriptor write
to a destination of a few hundred bytes (a descriptor-memory base that comes back as 0, likely from one of the
console-only command-list slots 70-75, 126, 139 or 142 that are not forwarded yet).

## Stops and what got past them

Continues `docs/retail_boot_chain.md` (stops 1-13). Slots are 0-based vtable indexes.

| Stop | Finding | Stand-in |
|---|---|---|
| GameInput | The title imports `gameinput.dll!#1` **by ordinal**; the shim registers it by name only. Without SDL3 the shim is silently left out of the build | `-DLEONIDA_SDL3_FETCH=ON`, `#1` registered too |
| DirectStorage | `DStorageGetFactory` with a console factory IID. Factory slot 4 = OpenFile, 5 = CreateStatusArray, 10 = queue creation. Queue slot 3 = EnqueueRequest, 4 = EnqueueStatus, 6 = Submit; status slot 3 = IsComplete. Request: +0 options (low byte 1 = zlib), +8 destination, +0x18 destination size, +0x20 file, +0x28 offset, +0x30 source size | `shims/dstorage_x.cpp`: synchronous reads, zlib through `shader_library.h`'s inflater, always complete |
| Device slot 26 | GetCustomHeapProperties with the struct-return buffer in RCX and the device in RDX | special case |
| Device slot 48 | CreateCommandQueueX with console flags 2 and 3 | flags masked to bit 0 |
| Device slot 73 | The title asks for a console graphics-list IID | created and wrapped as `ID3D12GraphicsCommandList` in upstream's console list proxy (`boot-hacks` first had its own list wrapper; dropped in the merge) |
| Physical pages | Wine 9's `MapViewOfFile3` rejects `PAGE_WRITECOMBINE` (error 87) | write-combine/no-cache bits dropped |
| Stop 13 (descriptor heap) | The heap description is 32 bytes: the PC 16, then `{address, size}` of memory the title reserved | upstream's console-addressed heap (issue #313) after its build fix: the title's memory is the heap's storage. (`boot-hacks` first used an address-translation stand-in; dropped in the merge) |
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
| Queue slots 16, 17 | **Signal(fence, value)** and **Wait**: this queue is two slots ahead of the PC one from Signal on (16-19 = PC 14-17). Forwarding 16 to PC 16 (GetTimestampFrequency) wrote 1,000,000,000 over the fence proxy's vtable pointer and never signalled the fence | routed to Signal/Wait with the fence unwrapped |
| Fence slot 12 | Returns a pointer to the fence's completed value, which the title polls in memory (the console GPU writes it) | a per-fence UINT64 kept current from GetCompletedValue by one thread |
| Queue slot 32 | **PresentX** `(planeCount, planes, params)`, the newer layout `xbox_present.h` lists | accepted and logged (no window yet); the title presents its first frame |
| Queue slot 35 | ExecuteCommandLists with two extra words | routed to ExecuteCommandLists, lists unwrapped as slot 10 |
| Console slots past the PC vtable | The child proxy refused slots past the PC table **before** the console handlers ran, so queue 32/35 and fence 12 never reached them | the console handlers run first |

## Loops

- `leonida-try.sh <name> <seconds>`: repackage the runtime and run the title once under Wine (`/root/<name>.log`
  and the startup trace `/root/<name>.jsonl`).
- `leonida-shaders-pass.sh`: translate compute containers the runtime dumped to `/root/leonida-dump/`.
- `psomatch.py`: match code headers the runtime wanted (`/root/leonida-psowant.txt`) against the FSSM corpus and
  append them to `/root/leonida-psomap.txt` (not committed: it holds bytes of the title's code).
- `gdbat.sh <seconds> <gdb file>`: start the title and attach gdb after a delay. Hardware watchpoints do not work
  under Wine (it rewrites the debug registers); bisect with breakpoints instead.

## Next

1. The descriptor-memory base: which console list slot (70-75, 126, 139, 142) hands it out.
2. Device slot 86, another pipeline-creation variant the title calls thousands of times (refused so far).
3. Read back `PresentX` plane 0 and write frames to disk, so frames can be checked without a display.
3. Real implementations for the ignored sampler and view slots (61, 70) and the probe at 74.

## 2026-11-24: where loading stops now (after 2e85f84)

- At about 87 s the title's texture-object creation (game+0x34beb0, then +0x193de10) fails for small 160x90
  render textures (formats 24 and 26): the title's own GPU memory allocator (game+0x2474690, then +0x221ad20,
  allocator 2 via +0x4fc99c0, then +0x4fac3c0) returns null for a 0x4b0000-byte, 64 KiB-aligned request.
  The title then asserts (+0x2219650, +0x1647d03, +0x6eb888) and reads through the null object (+0x6eb759).
- Only about 450 MiB has been placed through device slot 133 by then, so it is one pool, not memory as a
  whole. Next: find allocator 2's backing region and why it fills (a leak, or a size the PC side gets wrong).
- The slot 127 charge to the title allocator (xs_title_d3d_alloc) is never freed; removing it did not move the
  stop and made a 2 MB budget soft assert (+0x247af2d) appear early, so it stays for now.
- Breakpoints under gdb: patch the title's soft-assert int3s to nop first (set {unsigned char}ADDR = 0x90),
  or they stop gdb before yours. The gdb breakpoint on a failure path of a long function, then on each
  return-false site, found this stop in three runs.
- Run settings that matter: LEONIDA_ARENA_MB=4095 (the 256 MiB default fills with compute programs),
  LEONIDA_SKIP_INT3=1 (step over soft asserts), LEONIDA_CE_STACKS=1 (thread stacks at presents 300, 20000).

## 2026-11-24 (later): after console footprints (9bd2a8d)

- The 2.2 GB GPU pool (title allocator 2, object at 0x1471c2808: base +0x20, size +0x30 = 0x89700000, used
  +0x58) no longer fills: allocation answers are console footprints now. The title runs to about 124 s and
  binds its UI pipelines (VertexCxformEAlpha, BatchVertexCxformEAlpha); frames are still all zero.
- Next stop at about 124 s: soft asserts in the title's free (game+0x1887b0; +0x187fb, +0x1881f, +0x1884a check
  the block header at -0x10/-0xc/-0x2), then a null read at +0x538d0e6 under a recursive teardown
  (+0x183aaf / +0x18435f). A block freed there is not from that heap, or its header was overwritten.
- The bad free (gdb, break at game+0x1400187fb with the other soft asserts patched to nop): the block's
  allocated bit (header word at -0x2, bit 10) is clear, so it was already freed (double free) or its header was
  overwritten; its payload is float data (1.0f). It is freed by a container's teardown (game+0x455aeb0: count at
  +0x32, entries at +0x38, each freed through the allocator at 0x14abcc310, vtable +0x60), under +0x515ad70.
  Teardown at this point is an error path: something earlier failed. The first anomaly in the run is a write
  past a stream buffer's capacity at about 98 s (soft assert +0x4dcbc9d, game+0x4dcbc55: remaining < size;
  the write is skipped and returns false). Next: what fills that buffer, and why it is too small here.
- The overflowing buffer (game+0x4dcbc55, about 100 s) is a shared command stream, not a DSP queue: records
  tagged 0x...04002821 carry float parameters (0.55, 0.2, 1000, 2000). Writers flush a local buffer into it
  (game+0x4e52a40: lock at owner+8, append via +0x4dcbc40) from +0x5ab13e8 through +0x4e529a0. The stream comes
  from the table at *0x149ae9950 (+0x4dcbc10): per slot an object with several 0x28-byte streams and a current
  index at +0xc8, so a consumer is meant to switch streams each frame and drain the old one (the switch is in the
  large function around +0x4dc3df2/+0x4dc3e11). No thread was in that code in the stack dumps, and the stream fills
  in about 100 s. A null ALSA device (ALSA_CONFIG_PATH with pcm.!default type null, run setting only) removed the
  ALSA open errors but did not change this. Next: who calls the switch, and what it waits on.
- gdb from 20 s to 280 s: the stream append (+0x4dcbc72) runs on several threads (gdb threads 27, 96), about
  1,000 times in that window; neither write of the stream index (+0x4dc3e11, +0x4dc2e23) ever runs. Either the
  consumer is elsewhere or its thread never runs. AcpHal now acknowledges commands (97ca179), but the title only
  registers for messages (command 2) before this stop, so the coprocessor is not what the consumer waits on.
  gdb note: a breakpoint must be on an instruction boundary (check game.asm), or the run breaks silently.
- The stream consumer, found: game+0x43a6b40 walks every stream-table entry (count at 0x149ae9958) and is called
  only from the manager update +0x6d8da0, which is reached from +0x43ae2f2 and from the job callback +0x537f810
  (CAS on a frame counter at obj+0x78, then the update, then clears obj+0x7c and decrements obj+0x80). Under gdb
  from 20 s the update ran once (gdb thread 17) and never again, so the per-frame job is not submitted again, or
  the scheduler holds it back on those counters. Next: who submits the +0x537f810 job each frame, and what it
  checks (obj+0x78/+0x7c/+0x80) before submitting.
- Why the consumer stops: the per-frame tick game+0x43ae230 (it runs the stream update inline or queues the
  +0x537f810 job; the job descriptor is static at 0x149ae9988, busy flag 0x149ae9a04, pending 0x149ae9a08) is
  called only from the audio thread's main loop at game+0x1e95xxx. That thread (seen in the stack dumps as
  ... G+a7c3f3 G+4bd6 G+5594661 G+1e95166) is stuck at the first instruction of +0x5594650: the title's own lock
  routine +0x4b40 on the audio object (not an imported CRITICAL_SECTION or SRW lock). So another thread holds
  that object's lock and never releases it, and the audio frame never comes. Next: break at +0x559465c to get
  the object, read its lock word after the stall, and find the owning thread's stack.
- The audio lock (gdb at 110 s, hacks: owner.gdb pattern): the title's lock word is one u64, owner thread index
  in the high half (the index is TLS slot 0 block +0x1454), recursion count in the low half, 0xFFFFFFFF00000000
  free. The audio object's lock reads 0x15000000003: thread 336, held three deep, and no live thread has index
  336. The owner exited while holding it. Next: which short-lived game thread takes the audio object's lock
  (index 336, so the title has made several hundred threads by then) and why it ends without unlocking (a thread
  the runtime ends early, or an exception path).
- The process has about 1,190 threads at 110 s: the background compute compiles make one thread each, and most
  wait for one of the 40 slots. To do: a fixed pool of compile workers.
- Correction: the audio lock was never taken. Under gdb from 1 s, no thread with index 336 ever acquires a lock
  (break at game+0x4bdf if the new owner is 336), and the audio object (heap, e.g. 0x7bf358747a40) already reads
  0x0000015000000002 when the audio thread first locks it (its first frame, about 61 s). The word was overwritten
  with data (2 and 0x150), not locked: memory corruption of the audio object. Suspect first: the runtime's
  allocations made at audio start (trace: XMemAlloc attributes 0xf0814000, 393216 bytes at 61.79 s; ApuCreateHeap
  and SHAPE contexts at 61.78 s) overlapping or under-sizing; then the silent xapu/chad status and result writes.
  Next: log the object's address against those allocations, and break on writes to it (software: single-step is
  too slow; check each runtime writer's target range instead).
- Not the runtime's audio-time allocations: in one run the corrupted audio object was 0x7b0ecd569140, the APU
  cached heap 0x7b08778f0000+0x1c700000, the non-cached one 0x7b0c39c60000+0x226e00, and the two XMemAlloc blocks
  (0xf0814000) 0x7b0c26548440+0x60000 and 0x7b08981a0080+0x1e00000; none contains it. The silent xapu result
  record matches VIctory's 0x50-byte layout, so dequeues do not overrun either. Next: which allocator made the
  audio object (the title's own heaps), and a check of other runtime writers into title memory (bottom-of-pipe
  writes 71/72, the console texture refresh, CBV snapshots).
- Shell note: never `pkill -f` a pattern that also appears in the command running it (it kills its own shell,
  exit 144); use `pkill -x gdb`.
- Not the bottom-of-pipe writes (list slots 71/72): with every write logged that carries 0x150 in its high half
  (none) and the first 40 targets (0x795cb29000xx and 0x795e1259xxxx, values 0 or counters), none lands on the
  audio object (0x795e17541ec0 in that run). Left to check: the console texture refresh and CBV snapshots (both
  read title memory, they do not write it), the title's own heaps (which allocator the object came from and its
  neighbours), and whether the object was ever constructed (a lock word that was never initialised would read
  as whatever the allocator left there).
- The "audio object" lock is a member: the audio thread locks engine+0x3ced0 (rsi = the audio engine, the same
  object the manager update reads at +0x3cdf8 and +0x3dd80). Its word differs between runs (0x15000000001,
  ...02, ...03), so it is being locked by a path other than +0x4bdf (the recursive path, or an inlined lock),
  not a one-off overwrite. Forcing it free under gdb at the audio thread's first lock (hacks: unlock.gdb pattern)
  did not help: the stream update still ran once and the run still ended at +0x538d0e6 (117 s). So this lock is
  not the only thing holding the audio thread. Next: the audio thread's stack after a forced unlock, and the
  inlined lock sites that write engine+0x3ced0.
- Correction (TLS): the title's thread index is in its own TLS slot, not slot 0. The exe's TLS directory says
  AddressOfIndex 0x146e08b2c (slot 2 in these runs); the earlier "owner 336 does not exist" read slot 0.
  With the right slot the audio engine's lock (engine+0x3ced0, word 0x15000000003) belongs to thread index 336,
  a job worker that is idle at its entry wait (G+221c3c2) by present 300, holding it three deep. No exception
  on any thread before about 119 s, so it was not skipped by an unwind: the engine is locked across jobs on
  purpose, and the job that unlocks never runs. Prime suspect: the XMA decoder contexts the title polls (VIctory's
  audio.rs decodes XMA contexts on a thread and advances their read/write blocks; here nothing advances them),
  so the first audio frame never completes. Next: port VIctory's XMA context pump, silent (consume input packets,
  write zero output blocks, advance the pointers), with credit.
- Stack dumps now print each thread's title index ("tid N idx M"), read through the title's TLS slot.
- Not XMA either: with the silent XMA pump in place (acphal, after VIctory) the title still sends only
  coprocessor command 2 before the stop, so no context is ever enabled; the audio thread stays on the same lock.
- The engine lock's word has 2 or 3 in its low half, but the lock routine +0x4b40 always writes 1 there (its
  recursion count lives at +0x10), so another routine (a recursive or inlined variant) takes this lock; that is
  why the breakpoint at +0x4bdf never saw owner 336. Next: find the lock routines that add to the low half.
- FIXED (audio): the lock leak is the title's own function +0x5267720, which takes the engine lock and skips the
  unlock when its object's count at +0x52c is 0 (branch +0x5267747). Patched at device setup to go to the
  unlocking tail (+0x5267c7f). The audio thread now runs and the command stream no longer overflows. What +0x52c
  counts (and why it is 0 here) is still open; the patch only makes the zero case unlock like its sibling.
- The run still ends at about 121 s: the bad free (+0x187fb, +0x1881f, +0x1884a) then +0x538d0e6. That is a
  separate fault, not the overflow's error path as guessed before.
- Not the XMemAlloc hooks: routing XMemAlloc/XMemFree through __XMemAllocationHooks__ (as VIctory does) is in,
  and the bad free still comes at about 123 s. Not command allocator slot 9 either (an "allocation callback"
  VIctory ignores; the caller at +0x188501 ignores its result).
- The stop at about 120-130 s has two forms that alternate between runs: (a) the bad free (+0x187fb..) then a
  null read at +0x538d0e6, (b) a read of address -1 at +0x13360a3 (stack +0x186424, +0x18573b, +0x184f42,
  +0x1847d8, +0x1837a2). Both run through the +0x18xxxx code next to the title's free (+0x187b0), so they likely
  share a cause. hacks: dfree.gdb dumps the freeing container (+0x455aeb0: count +0x32, entries of 0x20 at +0x38,
  pointer at +0x18) to test for a double entry, when the run takes form (a).
- The bad-freed block sits at a fixed offset: 0x72e67db40590 in one run, 0x7b582db40590 in another (same low
  28 bits, ...db40590), so it lives at a fixed place inside one large reservation, not at a random heap address.
  The -1 read form (+0x13360a3) is a virtual call through the object at rsi (call *0xb0(vtable)) whose vtable is
  not a canonical address: a freed or overwritten object, consistent with the double free. hacks: uaf.gdb.
  Next: which reservation holds ...db40590 (the title's XMemVirtualAlloc regions and pools vs the runtime's
  VirtualAllocs, e.g. console texture memory), from /proc/<pid>/maps against the trace.
- Not physical-page aliasing in xmem.cpp: a code review and a full-scale Wine test (111,136 pages in the 6.5 GB
  and 710 MB windows, offsets past 4 GiB, a 24 GiB placeholder) found pages distinct and shared mappings correct.
- Queue slot 31 is CopyPageMappingsBatchX (VIctory slot 29 + 2) and is now implemented; its calls succeed. The
  bad free still comes (at 131 s in that run). Open, from the same review: VIctory keeps XMemVirtualAlloc
  reservations without an address field below 2^40 (the console keeps title memory there); CE places them near
  0x7xxx_xxxx_xxxx. If the title packs pointers into fewer bits, that would corrupt memory. Next candidate.
- Not the address range either: with title reservations below 2^40 (as on the console) the bad free still comes
  at about 125 s. Ruled out so far for this stop: audio lock (fixed), XMemAlloc hooks, xmem aliasing, page
  mapping batches (implemented), the address range, the runtime's audio allocations and list writes.
- Both forms of the 120-130 s stop are the same corruption: float data overwrites live objects. The bad-freed
  block held 1.0f floats; in form (b) the object at rsi (0x14f341440 in one run) is rows of 0x3f800000 (1.0f,
  patterns like 1,1,1,0), its "vtable" two floats. That address is not in the exe (image ends 0x14b06d000): it
  is inside the title's 0x183 pool, reserved at 0x14b200000 (0x25400000 bytes) since title reservations stay
  below 2^40, which also makes pool addresses repeat between runs. Next: if the object's address repeats,
  write-protect its page under gdb before 120 s (call mprotect in the inferior) and stop on the fault to find
  the writer of the floats.
