// acphal.dll: Audio Control Processor HAL. Scalable Hardware Audio Processing Engine
// (SHAPE) and APU (Audio Processing Unit) physical memory management for Xbox Series.
//
// On a PC there is no SHAPE/ACP hardware and APU physical memory does not exist.
// This shim creates the APU heap from process memory and manages allocations within it.
// The SHAPE context arrays are allocated on the process heap; the ACP interface
// accepts and acknowledges commands to allow audio bring-up to proceed cleanly.
// Enabled XMA contexts are pumped silently on a thread of ours (see "The XMA decoder, silent").
//
// Signatures from the public GDK reference (devdocs.xbox.com):
//   AcpHal, APU reference, SHAPE_CONTEXTS, APU_ADDRESS.
// Measured call-site shapes and findings from the retail executable (1.0.2.1):
//   - docs/retail_boot_chain.md, docs/retail_boot_frame_loop.md (Stop 28)
//   - PR #6885 (call sites and parameter analysis), PR #7009 (audio/HAL bring-up).

#include <windows.h>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <memory>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "shims.h"
#include "startup_trace.h"

namespace {

// ---- APU_ADDRESS (from the GDK) ----

// The console APU uses a 40-bit physical address space.
// On PC we translate to a 32-bit APU address offset.
struct APU_ADDRESS {
    uint64_t value;  // 40-bit physical address
};

// ---- SHAPE context structures (from the public GDK documentation at devdocs.xbox.com) ----
//
// These structures are defined here with their documented GDK field layouts so that
// sizeof() returns the correct per-context byte count. Without the real layouts the
// empty structs would be 1 byte each, and the title's writes (e.g. memset with the
// real stride) would overflow the allocation.

// UINT32 bitfields packed into 32-bit words
struct SHAPE_SRC_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t blocksToSkip : 2;
    uint32_t internalSaturate : 1;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t samplingIncrement : 21;
    uint32_t reserved0 : 1;
    uint32_t cmd : 2;
    uint32_t mixBufPeakMag2 : 4;
    uint32_t peakMag2 : 4;
    uint32_t sampleCount;
    uint32_t samplePointer : 25;
    uint32_t reserved1 : 7;
    uint32_t samplingIncrementTarget : 21;
    uint32_t reserved2 : 11;
    uint32_t reserved[3];
};
static_assert(sizeof(SHAPE_SRC_CONTEXT) == 32, "SHAPE_SRC_CONTEXT is 32 bytes per GDK");

// The EQ/Comp context: 88 UINT32s = 352 bytes per GDK definition.
#pragma pack(push, 1)
struct alignas(4) SHAPE_EQCOMP_CONTEXT {
    uint8_t data[352];
};
#pragma pack(pop)
static_assert(sizeof(SHAPE_EQCOMP_CONTEXT) == 352, "SHAPE_EQCOMP_CONTEXT is 352 bytes per GDK");

struct SHAPE_FILTVOL_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 2;
    uint32_t internalSaturate : 1;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t delay1 : 30;
    uint32_t headroom : 2;
    uint32_t delay2 : 30;
    uint32_t svfMode : 2;
    uint32_t gainTarget : 16;
    uint32_t gain : 16;
    uint32_t qRecip : 24;
    uint32_t reserved1 : 8;
    uint32_t qRecipTarget : 24;
    uint32_t reserved2 : 8;
    uint32_t fc : 24;
    uint32_t reserved3 : 8;
    uint32_t fcTarget : 24;
    uint32_t reserved4 : 8;
};
static_assert(sizeof(SHAPE_FILTVOL_CONTEXT) == 32, "SHAPE_FILTVOL_CONTEXT is 32 bytes per GDK");

struct SHAPE_DMA_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 3;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t readPointer : 5;
    uint32_t reserved1 : 3;
    uint32_t writePointer : 5;
    uint32_t reserved2 : 3;
    uint32_t full : 1;
    uint32_t reserved3 : 15;
    uint32_t reserved4 : 9;
    uint32_t address : 23;
    uint32_t numFrames : 5;
    uint32_t reserved5 : 3;
    uint32_t floatConvert : 1;
    uint32_t reserved6 : 7;
    uint32_t numChannels : 3;
    uint32_t reserved7 : 5;
    uint32_t channel : 3;
    uint32_t reserved8 : 5;
};
static_assert(sizeof(SHAPE_DMA_CONTEXT) == 16, "SHAPE_DMA_CONTEXT is 16 bytes per GDK");

struct SHAPE_XMA_CONTEXT {
    uint32_t sizeRead0 : 12;
    uint32_t numLoops : 8;
    uint32_t validBuffer : 2;
    uint32_t sizeWrite : 5;
    uint32_t offsetWrite : 5;
    uint32_t sizeRead1 : 12;
    uint32_t sequenceNumber : 12;
    uint32_t stopSampleAddress : 8;
    uint32_t subframeOffset : 4;
    uint32_t reserved0 : 1;
    uint32_t isLooping : 1;
    uint32_t subframeSkip : 2;
    uint32_t packetMetadata : 16;
    uint32_t currentByteOffset : 26;
    uint32_t reserved1 : 6;
    uint32_t loopStartOffset : 26;
    uint32_t loopEndOffset : 6;
    uint32_t reserved[11];  // the bitfields above take 5 units (packetMetadata ends a unit at 24 bits): 64 bytes total
};
static_assert(sizeof(SHAPE_XMA_CONTEXT) == 64, "SHAPE_XMA_CONTEXT is 64 bytes per GDK");

struct SHAPE_PCM_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 11;
    uint32_t loopCount : 8;
    uint32_t reserved1 : 8;
    uint32_t full : 1;
    uint32_t reserved2 : 15;
    uint32_t readPointer : 21;
    uint32_t reserved3 : 11;
    uint32_t loopStartWritePointer : 21;
    uint32_t reserved4 : 11;
    uint32_t loopEnd : 21;
    uint32_t reserved5 : 11;
    uint32_t mode : 1;
    uint32_t reserved6 : 15;
    uint32_t format : 2;
    uint32_t reserved7 : 14;
    uint32_t reserved[2];  // the bitfields above take 6 units: 32 bytes total
};
static_assert(sizeof(SHAPE_PCM_CONTEXT) == 32, "SHAPE_PCM_CONTEXT is 32 bytes per GDK");

// The SHAPE_CONTEXTS structure (from the GDK)
struct SHAPE_CONTEXTS {
    uint32_t numSrcContexts;
    uint32_t numEqCompContexts;
    uint32_t numFiltVolContexts;
    uint32_t numDmaContexts;
    uint32_t numXmaContexts;
    uint32_t numPcmContexts;
    SHAPE_SRC_CONTEXT* srcContextArray;
    SHAPE_EQCOMP_CONTEXT* eqCompContextArray;
    SHAPE_FILTVOL_CONTEXT* filtVolContextArray;
    SHAPE_DMA_CONTEXT* dmaContextArray;
    SHAPE_XMA_CONTEXT* xmaContextArray;
    SHAPE_PCM_CONTEXT* pcmContextArray;
    APU_ADDRESS apuSrcContextArray;
    APU_ADDRESS apuEqCompContextArray;
    APU_ADDRESS apuFiltVolContextArray;
    APU_ADDRESS apuDmaContextArray;
    APU_ADDRESS apuXmaContextArray;
    APU_ADDRESS apuPcmContextArray;
};

static_assert(sizeof(SHAPE_CONTEXTS) == 120, "SHAPE_CONTEXTS is 120 bytes per GDK");
static_assert(offsetof(SHAPE_CONTEXTS, xmaContextArray) == 0x38,
              "xmaContextArray is at offset +0x38 per retail binary measurement");

// ---- Startup trace helper ----

void trace_call(const char* api, const std::string& detail) {
    if (!rt::startup_trace::enabled()) return;
    static std::mutex mutex;
    static std::set<std::string> seen;
    {
        const std::lock_guard lock(mutex);
        if (!seen.insert(std::string(api) + detail).second) return;
    }
    rt::startup_trace::event("acphal",
                             std::string("\"api\":\"") + api + "\"" + detail);
}

// ---- Genuine APU Heap Implementation ----

constexpr uint32_t kApuBaseAddress = 0x40000000u;

struct ApuRegion {
    void* base = nullptr;
    uint64_t size = 0;
    std::map<uint32_t, uint32_t> blocks;  // offset -> size of active allocations

    bool contains(const void* ptr) const {
        if (!base || !ptr) return false;
        const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t b = reinterpret_cast<uintptr_t>(base);
        return p >= b && p < b + size;
    }

    bool is_valid_address(const void* ptr, uint32_t alignment) const {
        if (!base || !ptr) return false;
        const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t b = reinterpret_cast<uintptr_t>(base);
        if (p < b || p >= b + size) return false;
        if (alignment == 0) alignment = 1;
        return ((p - b) & (static_cast<uintptr_t>(alignment) - 1)) == 0;
    }

    uint32_t to_offset(const void* ptr) const {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr) -
                                     reinterpret_cast<uintptr_t>(base));
    }

    void* from_offset(uint32_t offset) const {
        if (offset >= size) return nullptr;
        return static_cast<uint8_t*>(base) + offset;
    }

    void* allocate(uint32_t alloc_size, uint32_t alignment, uint32_t* out_offset) {
        if (alloc_size == 0 || alignment == 0) return nullptr;
        uint64_t at = 0;
        for (auto it = blocks.begin();; ++it) {
            at = (at + alignment - 1) & ~static_cast<uint64_t>(alignment - 1);
            const uint64_t limit = (it == blocks.end()) ? size : it->first;
            if (at + alloc_size <= limit) break;
            if (it == blocks.end()) return nullptr;  // out of memory
            at = static_cast<uint64_t>(it->first) + it->second;
        }
        const uint32_t offset = static_cast<uint32_t>(at);
        blocks.emplace(offset, alloc_size);
        uint8_t* ptr = static_cast<uint8_t*>(base) + offset;
        std::memset(ptr, 0, alloc_size);
        if (out_offset) *out_offset = offset;
        return ptr;
    }

    bool free_block(void* ptr) {
        if (!contains(ptr)) return false;
        const uint32_t offset = to_offset(ptr);
        return blocks.erase(offset) > 0;
    }

    void release() {
        if (base) {
            VirtualFree(base, 0, MEM_RELEASE);
            base = nullptr;
        }
        size = 0;
        blocks.clear();
    }
};

std::mutex g_apu_mutex;
bool g_apu_heap_created = false;
ApuRegion g_cached_heap;
ApuRegion g_noncached_heap;

// Tracked SHAPE context arrays
std::vector<void*> g_active_shape_arrays;

// ---- IAcpHal Interface Implementation ----

void* __stdcall ApuMapApuAddress(uint32_t apu);

// ---- The XMA decoder, silent: ported from Marvisak/VIctory audio.rs (GPLv3) ----
//
// An XMA context (16 words in the shared context array) names up to two input buffers of 2 KiB packets, a ring
// of 256-byte output blocks the hardware fills with 16-bit PCM, and the read offset the title advances by command.
// Words and bit fields, as titles write them (VIctory's reading; the GDK-derived SHAPE_XMA_CONTEXT above differs
// from word 1 on and is not used here):
// 0: input 0 size in packets (0-11), loop count (12-19, 255 forever), valid inputs (20-21), output size in blocks
//    (22-26), write block (27-31, hardware);
// 1: input 1 size (0-11), loop end subframe (12-13), loop start subframes to skip (17-19), subframes to skip at the
//    start (24-26), sample rate (27-28), stereo (29), output writable (31);
// 2: read position in bits (0-25), error status (26-30), error (31);
// 3: loop start in bits (0-25); 4: loop end in bits (0-25), current input (31); 5, 6: input addresses; 7: output
//    address; 8: overlap buffer; 9: read block (0-4).
// Titles read back only the write block, "writable" and the read block, and take whatever lies between read and
// write. GTA VI uses only XMA contexts.
//
// shortcut (silent): no audio is decoded. The bitstream's framing (packet headers, frame length fields) is walked
// as VIctory's does, so the read position, the valid/consumed inputs, loops and the "input invalid" status change
// exactly as there; each frame counts as its 512 samples per channel, written to the ring as zeros, so the output
// advances at the rate a decoder's would.
namespace xma {

constexpr uint32_t kEnableXma = 5, kEnableXmas = 6, kDisableXma = 7, kDisableXmas = 8, kAdvanceXmaRead = 18;
constexpr size_t kPacketBytes = 2048, kPacketBits = kPacketBytes * 8, kHeaderBits = 32;
constexpr size_t kFrameSamples = 512, kSubframeSamples = 128;  // per channel
constexpr uint32_t kLengthBits = 15;
constexpr size_t kBlockBytes = 256, kSamplesPerBlock = kBlockBytes / 2;
constexpr uint32_t kInputInvalid = 4;  // error status: the input ran out
constexpr size_t kNone = SIZE_MAX;

// -- Framing (victory-formats xma.rs: packet, Cursor, read_frame, next_frame) --

struct Packet {
    uint32_t frames, first_frame_bit, skip;
};

bool packet(const uint8_t* data, size_t len, size_t index, Packet* out) {
    if (index > len / kPacketBytes) return false;
    const size_t start = index * kPacketBytes;
    if (start + 4 > len) return false;
    const uint32_t h = (uint32_t(data[start]) << 24) | (uint32_t(data[start + 1]) << 16) |
                       (uint32_t(data[start + 2]) << 8) | uint32_t(data[start + 3]);
    *out = {h >> 26, (h >> 11) & 0x7fff, h & 0xff};
    return true;
}

// Bit positions in a buffer of packets, skipping each packet's header and the packets of other streams.
struct Cursor {
    const uint8_t* data;
    size_t len;
    size_t pos;

    bool bit(uint32_t* out) {
        if (pos % kPacketBits == 0) {
            const size_t previous = pos / kPacketBits;
            if (previous > 0) {
                Packet p;
                if (!packet(data, len, previous - 1, &p)) return false;
                pos = (previous + p.skip) * kPacketBits;
            }
            pos += kHeaderBits;
        }
        if (pos / 8 >= len) return false;
        *out = (data[pos / 8] >> (7 - pos % 8)) & 1;
        ++pos;
        return true;
    }

    bool bits(uint32_t n, uint32_t* out) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t b;
            if (!bit(&b)) return false;
            v = v << 1 | b;
        }
        *out = v;
        return true;
    }
};

struct Frame {
    size_t start;  // bit offset where it starts
    size_t next;   // where the next frame starts, kNone when the buffer has no further frame
};

// Where the frame after the one at start..end begins: right after it, unless it was the last frame to begin in its
// packet and the rest of that packet is padding.
size_t next_frame(const uint8_t* data, size_t len, size_t start, size_t end) {
    const size_t index = start / kPacketBits;
    Packet header;
    if (!packet(data, len, index, &header)) return kNone;
    size_t at = index * kPacketBits + kHeaderBits + header.first_frame_bit;
    uint32_t number = 0;
    while (at < start) {
        Cursor cursor{data, len, at};
        uint32_t bits;
        if (!cursor.bits(kLengthBits, &bits) || bits <= kLengthBits) return kNone;
        at += bits;
        ++number;
    }
    const bool last = number + 1 >= header.frames;
    if (end % kPacketBits == 0 || (last && end / kPacketBits == index)) {
        // The next frame begins in the next packet of this stream.
        Packet p;
        if (!packet(data, len, (end - 1) / kPacketBits, &p)) return kNone;
        const size_t next = (end - 1) / kPacketBits + 1 + p.skip;
        if (!packet(data, len, next, &p)) return kNone;
        end = next * kPacketBits + kHeaderBits + p.first_frame_bit;
    }
    return end / 8 < len ? end : kNone;
}

// The frame starting at bit `start`; false when there is none (the input is used up).
bool read_frame(const uint8_t* data, size_t len, size_t start, Frame* out) {
    Cursor cursor{data, len, start};
    uint32_t bits;
    if (!cursor.bits(kLengthBits, &bits) || bits <= kLengthBits || bits == 0x7fff) return false;
    cursor = Cursor{data, len, start};
    for (uint32_t i = 0; i < bits; ++i) {
        uint32_t b;
        if (!cursor.bit(&b)) return false;
    }
    *out = {start, next_frame(data, len, start, cursor.pos)};
    return true;
}

// -- Contexts and voices --

// A context's words in title memory, which the title reads and writes concurrently, like the hardware's.
struct Context {
    volatile uint32_t* w;

    uint32_t get(int word) const { return w[word]; }
    void set(int word, uint32_t value) const { w[word] = value; }
    uint32_t field(int word, uint32_t shift, uint32_t bits) const {
        return (get(word) >> shift) & ((1u << bits) - 1);
    }
    void set_field(int word, uint32_t shift, uint32_t bits, uint32_t value) const {
        const uint32_t mask = ((1u << bits) - 1) << shift;
        set(word, (get(word) & ~mask) | ((value << shift) & mask));
    }
    size_t channels() const { return 1 + field(1, 29, 1); }
};

struct Voice {
    bool enabled = false;
    size_t pcm = 0;   // silent samples (all channels) decoded but not yet written to the ring
    size_t skip = 0;  // samples per channel still to drop (start or loop subframe skips)
    bool ended = false;
};

// Written (under the unique lock) only when the context arrays are allocated or released. Each voice has its own
// lock, so a command for one voice never waits for others (the title sends them from its mixer thread).
// Lock order: this, then g_apu_mutex (taken by ApuMapApuAddress).
struct State {
    std::shared_mutex mu;
    uint8_t* array = nullptr;
    uint32_t count = 0;
    std::unique_ptr<std::mutex[]> locks;
    std::unique_ptr<Voice[]> voices;
};

State& state() {
    static State s;
    return s;
}

bool context(State& s, uint32_t index, Context* out) {
    if (index >= s.count || !s.array) return false;
    *out = Context{reinterpret_cast<volatile uint32_t*>(s.array + size_t(index) * 64)};
    return true;
}

// `len` bytes of title memory at an APU address, if they are all inside one APU heap (VIctory's heap_bytes).
uint8_t* apu_bytes(uint32_t apu, size_t len) {
    if (len == 0 || uint64_t(apu) + len - 1 > UINT32_MAX) return nullptr;
    auto* first = static_cast<uint8_t*>(ApuMapApuAddress(apu));
    auto* last = static_cast<uint8_t*>(ApuMapApuAddress(apu + uint32_t(len - 1)));
    return first && last == first + (len - 1) ? first : nullptr;
}

// Decodes (silently) the next frame of the current input into voice.pcm; false when there is nothing left.
bool decode(Context c, Voice& v) {
    if (v.ended) return false;
    const size_t channels = c.channels();
    uint32_t current = c.field(4, 31, 1);
    const uint32_t valid = c.field(0, 20, 2);
    if (!(valid & (1u << current))) {
        if (!(valid & (1u << (1 - current)))) {
            // Out of input: (VIctory flushes the decoder's overlap tail here; silence has none), then nothing.
            v.ended = true;
            c.set_field(2, 26, 5, kInputInvalid);
            c.set_field(2, 31, 1, 1);
            return v.pcm != 0;
        }
        current = 1 - current;
        c.set_field(4, 31, 1, current);
        c.set_field(2, 0, 26, 0);
    }
    const uint32_t address = current == 0 ? c.get(5) : c.get(6);
    const uint32_t packets = current == 0 ? c.field(0, 0, 12) : c.field(1, 0, 12);
    const size_t len = size_t(packets) * kPacketBytes;
    const uint8_t* data = apu_bytes(address & ~0x7ffu, len);
    if (!data) {
        c.set_field(0, 20 + current, 1, 0);
        return true;
    }
    size_t at = c.field(2, 0, 26);
    if (at < 32) {
        Packet p;
        at = 32 + (packet(data, len, 0, &p) ? p.first_frame_bit : 0);
    }
    Frame frame;
    if (!read_frame(data, len, at, &frame)) {
        // This input is used up.
        c.set_field(0, 20 + current, 1, 0);
        return true;
    }
    size_t pcm = kFrameSamples * channels;
    size_t next = frame.next == kNone ? 0 : frame.next;
    const uint32_t loops = c.field(0, 12, 8);
    const size_t loop_end = c.field(4, 0, 26);
    const bool ends_loop =
        loop_end != 0 && (frame.start == loop_end ||
                          (frame.start < loop_end && (frame.next == kNone || loop_end < frame.next)));
    if (loops > 0 && ends_loop) {
        const size_t keep = (size_t(c.field(1, 12, 2)) + 1) * kSubframeSamples * channels;
        if (pcm > keep) pcm = keep;
        next = c.field(3, 0, 26);
        v.skip = size_t(c.field(1, 17, 3)) * kSubframeSamples;
        if (loops != 255) c.set_field(0, 12, 8, loops - 1);
    }
    size_t skip = v.skip * channels;
    if (skip > pcm) skip = pcm;
    v.skip -= skip / channels;
    v.pcm += pcm - skip;
    if (next == 0) c.set_field(0, 20 + current, 1, 0);
    c.set_field(2, 0, 26, static_cast<uint32_t>(next));
    return true;
}

// Fills a context's output ring as far as it has room.
void pump(Context c, Voice& v) {
    const uint32_t size = c.field(0, 22, 5);
    if (size == 0) return;
    while (c.field(1, 31, 1) == 1) {
        if (v.pcm < kSamplesPerBlock && !decode(c, v)) return;
        const uint32_t write = c.field(0, 27, 5);
        uint8_t* out = apu_bytes(c.get(7) + write * uint32_t(kBlockBytes), kBlockBytes);
        if (!out) return;
        std::memset(out, 0, kBlockBytes);
        v.pcm -= v.pcm < kSamplesPerBlock ? v.pcm : kSamplesPerBlock;
        const uint32_t next = (write + 1) % size;
        c.set_field(0, 27, 5, next);
        if (next == c.field(9, 0, 5)) {
            // Full: nothing more until the title advances its read block.
            c.set_field(1, 31, 1, 0);
        }
    }
}

void pump_all() {
    State& s = state();
    std::shared_lock lock(s.mu);
    for (uint32_t i = 0; i < s.count; ++i) {
        Context c;
        if (!context(s, i, &c)) continue;
        std::lock_guard voice_lock(s.locks[i]);
        if (s.voices[i].enabled) pump(c, s.voices[i]);
    }
}

std::mutex g_wake_mu;
std::condition_variable g_wake_cv;
bool g_wake = false;

// Starts the pump thread on the first enabled voice, and has it fill rings now: a voice just enabled or read from.
// Otherwise it pumps every 2 ms (a 256-byte output block lasts 2.7 ms at 48 kHz mono), as VIctory's does.
void wake_decoder() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            for (;;) {
                pump_all();
                std::unique_lock lock(g_wake_mu);
                g_wake_cv.wait_for(lock, std::chrono::milliseconds(2), [] { return g_wake; });
                g_wake = false;
            }
        }).detach();
    });
    {
        std::lock_guard lock(g_wake_mu);
        g_wake = true;
    }
    g_wake_cv.notify_one();
}

void enable(State& s, uint32_t index) {
    Context c;
    if (!context(s, index, &c)) return;
    {
        std::lock_guard lock(s.locks[index]);
        c.set_field(2, 26, 6, 0);
        s.voices[index] = Voice{true, 0, size_t(c.field(1, 24, 3)) * kSubframeSamples, false};
    }
    wake_decoder();
}

void disable(State& s, uint32_t index) {
    Context c;
    if (!context(s, index, &c)) return;
    std::lock_guard lock(s.locks[index]);
    s.voices[index] = Voice{};
}

// Moves the title's read block on, which frees output space.
void advance_read(State& s, uint32_t index, uint32_t blocks) {
    Context c;
    if (!context(s, index, &c)) return;
    {
        // The pump writes the same words.
        std::lock_guard lock(s.locks[index]);
        uint32_t size = c.field(0, 22, 5);
        if (size == 0) size = 1;
        c.set_field(9, 0, 5, (c.field(9, 0, 5) + blocks) % size);
        if (blocks > 0) c.set_field(1, 31, 1, 1);
    }
    wake_decoder();
}

// Runs an XMA command; false for one this does not know.
bool run(uint32_t command, const void* data) {
    auto word = [&](size_t at, uint32_t fallback) -> uint32_t {
        if (!data) return fallback;
        uint32_t v;
        std::memcpy(&v, static_cast<const uint8_t*>(data) + at * 4, 4);
        return v;
    };
    State& s = state();
    std::shared_lock lock(s.mu);
    switch (command) {
    case kEnableXma: enable(s, word(0, UINT32_MAX)); break;
    case kDisableXma: disable(s, word(0, UINT32_MAX)); break;
    case kEnableXmas:
    case kDisableXmas:
        for (uint32_t i = 0; i < s.count; ++i) {
            if (word(i / 32, 0) & (1u << (i % 32))) {
                if (command == kEnableXmas) enable(s, i);
                else disable(s, i);
            }
        }
        break;
    case kAdvanceXmaRead: advance_read(s, word(0, UINT32_MAX), word(1, 0)); break;
    default: return false;
    }
    return true;
}

}  // namespace xma

// The audio coprocessor's command and message queues, after Marvisak/VIctory's audio.rs (GPLv3): every command
// completes at once and is acknowledged as the hardware would. A command with an id posts COMMAND_COMPLETED
// (command, id, audio frame), a flowgraph load posts FLOWGRAPH_COMPLETED, message registrations take effect, and a
// command's notification word (an APU address) is set to 1. The title's audio engine waits on these: dropping every
// command, as before, left its per-frame command streams undrained until one overflowed (about 100 s in).
// XMA context commands (5-8, 18) are carried out by xma::run, as VIctory's xma_command.
// shortcut: context updates and the other SHAPE units' commands are acknowledged but not carried out.
#pragma pack(push, 1)
struct AcpMessage {
    uint32_t kind;
    uint32_t dropped;
    uint32_t usec;
    uint8_t data[16];
};
#pragma pack(pop)
static_assert(sizeof(AcpMessage) == 28, "kind, dropped, usec, 16 bytes of data");

struct AcpHalImpl {
    static constexpr uint32_t kLoadFlowgraph = 1, kRegisterMessages = 2, kUnregisterMessages = 3;
    static constexpr uint32_t kFlowgraphCompleted = 0x02, kCommandCompleted = 0x10, kDisconnected = 0x80;
    static constexpr uint64_t kFramesPerSecond = 375;  // 128-sample audio frames at 48 kHz

    std::mutex mu;
    bool connected = false;
    uint32_t mask = 0;
    std::vector<AcpMessage> messages;  // front at index 0; small queues
    size_t capacity = 0;
    uint32_t dropped_count = 0;
    LARGE_INTEGER since{}, freq{};

    AcpHalImpl() {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&since);
    }
    uint64_t usec() const {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return static_cast<uint64_t>((now.QuadPart - since.QuadPart) * 1000000 / (freq.QuadPart ? freq.QuadPart : 1));
    }
    void post(uint32_t kind, const void* data, size_t len) {  // with mu held
        if (!(mask & kind)) return;
        if (messages.size() >= capacity) {
            ++dropped_count;
            return;
        }
        AcpMessage m{};
        m.kind = kind;
        m.dropped = dropped_count;
        dropped_count = 0;
        m.usec = static_cast<uint32_t>(usec());
        if (data && len) std::memcpy(m.data, data, len < sizeof(m.data) ? len : sizeof(m.data));
        messages.push_back(m);
    }

    // Slot 0: Connect(command queue size, message queue size)
    virtual HRESULT __stdcall connect(uint32_t commands, uint32_t message_capacity) {
        trace_call("AcpHal::Connect", "");
        if (!commands) return E_INVALIDARG;
        std::lock_guard lock(mu);
        connected = true;
        capacity = message_capacity;
        return S_OK;
    }

    // Slot 1: Disconnect
    virtual HRESULT __stdcall disconnect() {
        trace_call("AcpHal::Disconnect", "");
        std::lock_guard lock(mu);
        if (!connected) return E_FAIL;
        post(kDisconnected, nullptr, 0);
        connected = false;
        return S_OK;
    }

    // Slot 2: SubmitCommand(command, id, frame, data, notification). The title's first is (2, 0xf0f0f0f0, -1,
    // config, 0); a negative result triggers a fatal int3 (+0x43ad94e).
    virtual HRESULT __stdcall submit_command(uint32_t command, uint64_t id, uint32_t /*frame*/, const void* data,
                                            uint32_t notification) {
        static std::atomic<uint64_t> seen{0};
        if (command < 64 && !(seen.fetch_or(1ull << command) & (1ull << command))) {
            std::fprintf(stderr, "[rt] acphal: first coprocessor command %u\n", command);
            std::fflush(stderr);
        }
        trace_call("AcpHal::SubmitCommand", "");
        auto word = [&](size_t at) -> uint32_t {
            if (!data) return 0;
            uint32_t v;
            std::memcpy(&v, static_cast<const uint8_t*>(data) + at * 4, 4);
            return v;
        };
        {
            std::lock_guard lock(mu);
            if (command == kRegisterMessages) mask |= word(0);
            else if (command == kUnregisterMessages) mask &= ~word(0);
            else if (command == kLoadFlowgraph) {
                const uint32_t flowgraph = word(1);
                post(kFlowgraphCompleted, &flowgraph, 4);
            } else {
                xma::run(command, data);
            }
            if (id != 0) {
                uint8_t done[16];
                const uint32_t frame_now = static_cast<uint32_t>(usec() * kFramesPerSecond / 1000000);
                std::memcpy(done, &command, 4);
                std::memcpy(done + 4, &id, 8);
                std::memcpy(done + 12, &frame_now, 4);
                post(kCommandCompleted, done, sizeof(done));
            }
        }
        if (notification) {
            if (void* w = ApuMapApuAddress(notification))
                *static_cast<volatile uint32_t*>(w) = 1;
        }
        return S_OK;
    }

    // Slot 3: PopMessage(message*) -> whether one was taken
    virtual bool __stdcall pop_message(AcpMessage* out) {
        std::lock_guard lock(mu);
        if (messages.empty()) return false;
        if (out) std::memcpy(out, &messages.front(), sizeof(AcpMessage));
        messages.erase(messages.begin());
        return true;
    }

    // Slot 4: GetNumMessages
    virtual uint32_t __stdcall get_num_messages() {
        std::lock_guard lock(mu);
        return static_cast<uint32_t>(messages.size());
    }

    // Slot 5: Release
    virtual HRESULT __stdcall release() {
        trace_call("AcpHal::Release", "");
        delete this;
        return S_OK;
    }
};

// ---- AcpHalCreate ----

HRESULT __stdcall AcpHalCreate(void** ppAcpHal) {
    if (!ppAcpHal) return E_POINTER;
    *ppAcpHal = new AcpHalImpl();
    trace_call("AcpHalCreate", "");
    return S_OK;
}

// ---- AcpHalAllocateShapeContexts ----

HRESULT __stdcall AcpHalAllocateShapeContexts(SHAPE_CONTEXTS* ctx) {
    if (!ctx) return E_POINTER;

    xma::State& xs = xma::state();
    std::unique_lock xma_lock(xs.mu);  // before g_apu_mutex (see xma::State)
    std::lock_guard lock(g_apu_mutex);

    auto alloc_array = [](uint32_t count, size_t elem_size) -> void* {
        if (count == 0) return nullptr;
        void* ptr = std::calloc(count, elem_size);
        if (ptr) g_active_shape_arrays.push_back(ptr);
        return ptr;
    };

    ctx->srcContextArray = static_cast<SHAPE_SRC_CONTEXT*>(
        alloc_array(ctx->numSrcContexts, sizeof(SHAPE_SRC_CONTEXT)));
    ctx->eqCompContextArray = static_cast<SHAPE_EQCOMP_CONTEXT*>(
        alloc_array(ctx->numEqCompContexts, sizeof(SHAPE_EQCOMP_CONTEXT)));
    ctx->filtVolContextArray = static_cast<SHAPE_FILTVOL_CONTEXT*>(
        alloc_array(ctx->numFiltVolContexts, sizeof(SHAPE_FILTVOL_CONTEXT)));
    ctx->dmaContextArray = static_cast<SHAPE_DMA_CONTEXT*>(
        alloc_array(ctx->numDmaContexts, sizeof(SHAPE_DMA_CONTEXT)));
    ctx->xmaContextArray = static_cast<SHAPE_XMA_CONTEXT*>(
        alloc_array(ctx->numXmaContexts, sizeof(SHAPE_XMA_CONTEXT)));
    ctx->pcmContextArray = static_cast<SHAPE_PCM_CONTEXT*>(
        alloc_array(ctx->numPcmContexts, sizeof(SHAPE_PCM_CONTEXT)));

    // On PC there is no physical APU address translation for host context arrays;
    // assign unique synthetic addresses from the APU base.
    ctx->apuSrcContextArray = {ctx->srcContextArray ? kApuBaseAddress : 0};
    ctx->apuEqCompContextArray = {ctx->eqCompContextArray ? kApuBaseAddress + 0x1000 : 0};
    ctx->apuFiltVolContextArray = {ctx->filtVolContextArray ? kApuBaseAddress + 0x2000 : 0};
    ctx->apuDmaContextArray = {ctx->dmaContextArray ? kApuBaseAddress + 0x3000 : 0};
    ctx->apuXmaContextArray = {ctx->xmaContextArray ? kApuBaseAddress + 0x4000 : 0};
    ctx->apuPcmContextArray = {ctx->pcmContextArray ? kApuBaseAddress + 0x5000 : 0};

    // The XMA pump's view of the context array (VIctory sets it here too).
    xs.array = reinterpret_cast<uint8_t*>(ctx->xmaContextArray);
    xs.count = ctx->xmaContextArray ? ctx->numXmaContexts : 0;
    xs.locks = std::make_unique<std::mutex[]>(xs.count);
    xs.voices = std::make_unique<xma::Voice[]>(xs.count);

    trace_call("AcpHalAllocateShapeContexts",
               ",\"src\":" + std::to_string(ctx->numSrcContexts) +
               ",\"xma\":" + std::to_string(ctx->numXmaContexts) +
               ",\"pcm\":" + std::to_string(ctx->numPcmContexts));
    return S_OK;
}

// ---- AcpHalReleaseShapeContexts ----

HRESULT __stdcall AcpHalReleaseShapeContexts(SHAPE_CONTEXTS* ctx = nullptr) {
    xma::State& xs = xma::state();
    std::unique_lock xma_lock(xs.mu);  // before g_apu_mutex (see xma::State)
    std::lock_guard lock(g_apu_mutex);
    xs.array = nullptr;
    xs.count = 0;
    xs.voices.reset();
    xs.locks.reset();

    // Free all recorded context allocations
    for (void* p : g_active_shape_arrays) {
        if (p) std::free(p);
    }
    g_active_shape_arrays.clear();

    if (ctx) {
        ctx->srcContextArray = nullptr;
        ctx->eqCompContextArray = nullptr;
        ctx->filtVolContextArray = nullptr;
        ctx->dmaContextArray = nullptr;
        ctx->xmaContextArray = nullptr;
        ctx->pcmContextArray = nullptr;
    }

    trace_call("AcpHalReleaseShapeContexts", "");
    return S_OK;
}

// ---- ApuCreateHeap ----

HRESULT __stdcall ApuCreateHeap(uint32_t cachedSizeInBytes,
                                uint32_t nonCachedSizeInBytes) {
    std::lock_guard lock(g_apu_mutex);
    if (g_apu_heap_created) return E_FAIL;

    if (cachedSizeInBytes > 0) {
        g_cached_heap.base = VirtualAlloc(
            nullptr, cachedSizeInBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_cached_heap.base) return E_OUTOFMEMORY;
        g_cached_heap.size = cachedSizeInBytes;
    }

    if (nonCachedSizeInBytes > 0) {
        g_noncached_heap.base = VirtualAlloc(
            nullptr, nonCachedSizeInBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_noncached_heap.base) {
            g_cached_heap.release();
            return E_OUTOFMEMORY;
        }
        g_noncached_heap.size = nonCachedSizeInBytes;
    }

    g_apu_heap_created = true;
    trace_call("ApuCreateHeap",
               ",\"cachedSize\":" + std::to_string(cachedSizeInBytes) +
               ",\"nonCachedSize\":" + std::to_string(nonCachedSizeInBytes));
    return S_OK;
}

// ---- ApuAlloc ----

HRESULT __stdcall ApuAlloc(void** virtualAddress, uint32_t* physicalAddress,
                            uint32_t sizeInBytes, uint32_t alignmentInBytes,
                            uint32_t flags) {
    if (!virtualAddress) return E_POINTER;
    *virtualAddress = nullptr;
    if (physicalAddress) *physicalAddress = 0;

    if (sizeInBytes == 0) return E_INVALIDARG;
    if (alignmentInBytes == 0) alignmentInBytes = 1;
    if ((alignmentInBytes & (alignmentInBytes - 1)) != 0) return E_INVALIDARG;

    std::lock_guard lock(g_apu_mutex);

    // Routing: flags=2 routes to non-cached heap; flags=0 or 1 routes to cached heap.
    // If non-cached heap wasn't created or has no room, fall back to cached heap
    // because PC memory is unified and cache-coherent.
    const bool prefer_noncached = (flags == 2);
    ApuRegion* target = prefer_noncached ? &g_noncached_heap : &g_cached_heap;

    if (!target->base) {
        if (!prefer_noncached) {
            // Lazy default cached heap if ApuCreateHeap wasn't called yet
            constexpr uint32_t kDefaultHeapSize = 64 * 1024 * 1024;
            target->base = VirtualAlloc(nullptr, kDefaultHeapSize,
                                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!target->base) return E_OUTOFMEMORY;
            target->size = kDefaultHeapSize;
            g_apu_heap_created = true;
        } else {
            // Fall back to cached heap
            target = &g_cached_heap;
            if (!target->base) return E_OUTOFMEMORY;
        }
    }

    uint32_t offset = 0;
    void* ptr = target->allocate(sizeInBytes, alignmentInBytes, &offset);
    if (!ptr && prefer_noncached && g_cached_heap.base) {
        // Fall back to cached heap if non-cached heap is full
        target = &g_cached_heap;
        ptr = target->allocate(sizeInBytes, alignmentInBytes, &offset);
    }

    if (!ptr) return E_OUTOFMEMORY;

    *virtualAddress = ptr;
    if (physicalAddress) {
        // Disjoint APU address ranges:
        //   cached:     0x40000000 .. 0x40000000 + cached_size - 1
        //   non-cached: 0x40000000 + cached_size .. 0x40000000 + cached_size + noncached_size - 1
        uint32_t apu = kApuBaseAddress + offset;
        if (target == &g_noncached_heap) {
            apu += static_cast<uint32_t>(g_cached_heap.size);
        }
        *physicalAddress = apu;
    }

    return S_OK;
}

// ---- ApuFree ----

void __stdcall ApuFree(void* virtualAddress) {
    if (!virtualAddress) return;
    std::lock_guard lock(g_apu_mutex);
    if (!g_cached_heap.free_block(virtualAddress)) {
        g_noncached_heap.free_block(virtualAddress);
    }
    trace_call("ApuFree", "");
}

// ---- ApuIsVirtualAddressValid ----

BOOL __stdcall ApuIsVirtualAddressValid(void* virtualAddress, uint32_t alignment) {
    if (!virtualAddress) return FALSE;
    if (alignment == 0) alignment = 1;
    std::lock_guard lock(g_apu_mutex);
    if (g_cached_heap.is_valid_address(virtualAddress, alignment)) return TRUE;
    if (g_noncached_heap.is_valid_address(virtualAddress, alignment)) return TRUE;
    return FALSE;
}

// ---- ApuMapApuAddress ----

void* __stdcall ApuMapApuAddress(uint32_t apu) {
    if (apu == 0 || apu < kApuBaseAddress) return nullptr;
    std::lock_guard lock(g_apu_mutex);
    const uint32_t offset = apu - kApuBaseAddress;
    if (g_cached_heap.base && offset < g_cached_heap.size) {
        return g_cached_heap.from_offset(offset);
    }
    if (g_noncached_heap.base) {
        const uint32_t cached_span = static_cast<uint32_t>(g_cached_heap.size);
        if (offset >= cached_span && offset - cached_span < g_noncached_heap.size) {
            return g_noncached_heap.from_offset(offset - cached_span);
        }
    }
    return nullptr;
}

// ---- ApuMapVirtualAddress ----

uint32_t __stdcall ApuMapVirtualAddress(void* virtualAddress) {
    if (!virtualAddress) return 0;
    std::lock_guard lock(g_apu_mutex);
    if (g_cached_heap.contains(virtualAddress)) {
        return kApuBaseAddress + g_cached_heap.to_offset(virtualAddress);
    }
    if (g_noncached_heap.contains(virtualAddress)) {
        return kApuBaseAddress + static_cast<uint32_t>(g_cached_heap.size) +
               g_noncached_heap.to_offset(virtualAddress);
    }
    return 0;
}

}  // namespace

SHIM_TABLE(acphal,
    {"acphal.dll", "AcpHalAllocateShapeContexts", reinterpret_cast<void*>(&AcpHalAllocateShapeContexts)},
    {"acphal.dll", "AcpHalCreate", reinterpret_cast<void*>(&AcpHalCreate)},
    {"acphal.dll", "AcpHalReleaseShapeContexts", reinterpret_cast<void*>(&AcpHalReleaseShapeContexts)},
    {"acphal.dll", "ApuAlloc", reinterpret_cast<void*>(&ApuAlloc)},
    {"acphal.dll", "ApuCreateHeap", reinterpret_cast<void*>(&ApuCreateHeap)},
    {"acphal.dll", "ApuFree", reinterpret_cast<void*>(&ApuFree)},
    {"acphal.dll", "ApuIsVirtualAddressValid", reinterpret_cast<void*>(&ApuIsVirtualAddressValid)},
    {"acphal.dll", "ApuMapApuAddress", reinterpret_cast<void*>(&ApuMapApuAddress)},
    {"acphal.dll", "ApuMapVirtualAddress", reinterpret_cast<void*>(&ApuMapVirtualAddress)},
);
