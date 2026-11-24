// Checks the registered xg_xs.dll shim (runtime/shims/xg_xs.cpp) through rt::shims::find, so this is the table the
// runtime installs. Expected values from Marvisak/VIctory's xg.rs unit tests (GPLv3): the suggested modes, the
// layout of a 256x256 BC7 chain in 64 KiB D, copies in and out of every mip, the 64 KiB tiling, and the stub
// computer a failed creation still stores (two measured callers use *out without checking the HRESULT).
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "shims.h"

namespace {

using CreateFn = HRESULT(__stdcall*)(const void*, void**);
using SuggestFn = HRESULT(__stdcall*)(const void*, uint32_t, uint32_t*);
using ReleaseFn = ULONG(__stdcall*)(void*);
using Slot2Fn = HRESULT(__stdcall*)(void*, void*);
using Slot8Fn = HRESULT(__stdcall*)(void*, uint8_t*, uint32_t, uint32_t, const uint8_t*, uint32_t, uint32_t);
using Slot9Fn = HRESULT(__stdcall*)(void*, uint8_t*, uint32_t, uint32_t, uint32_t, const uint8_t*, uint32_t, uint32_t);
using Slot10Fn = HRESULT(__stdcall*)(void*, uint32_t*, void*, uint32_t*, uint32_t*, uint32_t, void*);
constexpr HRESULT kNotImpl = static_cast<HRESULT>(rt::shims::kENotImpl);

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

#pragma pack(push, 1)
struct Desc {  // D3D12_RESOURCE_DESC, 0x38 bytes
    uint32_t dimension, pad0;
    uint64_t alignment, width;
    uint32_t height;
    uint16_t depth_or_array, mips;
    uint32_t format, samples, quality, layout, flags, pad1;
};
#pragma pack(pop)
static_assert(sizeof(Desc) == 0x38);

Desc desc(uint64_t width, uint32_t height, uint16_t mips, uint32_t format, uint32_t layout) {
    Desc d{};
    d.dimension = 3;
    d.width = width;
    d.height = height;
    d.depth_or_array = 1;
    d.mips = mips;
    d.format = format;
    d.samples = 1;
    d.layout = layout;
    return d;
}

template <class T>
T read(const uint8_t* p, size_t at) {
    T v;
    std::memcpy(&v, p + at, sizeof(v));
    return v;
}

}  // namespace

int main() {
    auto create = reinterpret_cast<CreateFn>(rt::shims::find("xg_xs.dll", "XGCreateTextureComputer"));
    auto suggest = reinterpret_cast<SuggestFn>(rt::shims::find("xg_xs.dll", "XGSuggestSwizzleMode"));
    check(create != nullptr, "XGCreateTextureComputer is registered");
    check(suggest != nullptr, "XGSuggestSwizzleMode is registered");
    check(rt::shims::owns_dll("xg_xs.dll"), "the registry owns xg_xs.dll");
    if (!create || !suggest) return 1;

    Desc d = desc(256, 256, 1, 98, 0);
    uint32_t mode = 0;
    check(suggest(&d, 0, &mode) == S_OK && mode == 0x105, "XGSuggestSwizzleMode: 4KB_S for a plain texture");
    d.flags = 1;
    check(suggest(&d, 0, &mode) == S_OK && mode == 0x11b, "XGSuggestSwizzleMode: 64KB_R_X for a render target");
    d.flags = 2;
    check(suggest(&d, 0, &mode) == S_OK && mode == 0x118, "XGSuggestSwizzleMode: 64KB_Z_X for depth");

    check(create(&d, nullptr) == E_POINTER, "XGCreateTextureComputer(desc, nullptr) is E_POINTER");

    // A failed creation still stores a stub computer whose slots write nothing.
    Desc bad = desc(256, 256, 1, 0, 0x10a);
    void* stub = nullptr;
    check(create(&bad, &stub) == E_INVALIDARG && stub != nullptr, "a bad description fails but stores a stub");
    if (!stub) return 1;
    void* const* svt = *static_cast<void* const* const*>(stub);
    uint8_t junk[64];
    std::memset(junk, 0xcd, sizeof(junk));
    check(reinterpret_cast<Slot2Fn>(svt[2])(stub, junk) == kNotImpl && junk[0] == 0xcd, "stub slot 2 is E_NOTIMPL");
    reinterpret_cast<ReleaseFn>(svt[1])(stub);
    reinterpret_cast<ReleaseFn>(svt[1])(stub);
    check(reinterpret_cast<Slot2Fn>(svt[31])(stub, junk) == kNotImpl, "the stub survives extra Releases");

    // 256x256 BC7, full chain, 64 KiB display mode.
    d = desc(256, 256, 0, 98, 0x10a);
    void* c = nullptr;
    check(create(&d, &c) == S_OK && c != nullptr, "XGCreateTextureComputer makes a computer");
    if (!c) return 1;
    void* const* vt = *static_cast<void* const* const*>(c);
    std::vector<uint8_t> layout(0x1b40, 0xcd);
    check(reinterpret_cast<Slot2Fn>(vt[2])(c, layout.data()) == S_OK, "slot 2 fills the layout");
    const uint8_t* l = layout.data();
    check(read<uint64_t>(l, 0) == 131072 && read<uint64_t>(l, 8) == 65536 && read<uint32_t>(l, 0x10) == 9 &&
              read<uint32_t>(l, 0x14) == 1,
          "layout: 128 KiB, 64 KiB aligned, 9 mips, 1 plane");
    const uint8_t* p = l + 0x18;
    check(read<uint32_t>(p, 0x2c) == 1 && read<uint32_t>(p, 0x30) == 8 && read<uint32_t>(p, 0x28) == 16,
          "plane: 1 standard mip, 8 tail mips, 16-byte elements");
    check(read<uint64_t>(p, 0x38 + 8) == 65536 && read<uint32_t>(p, 0x38 + 0x1c) == 1024,
          "mip 0 at 64 KiB, 1024-byte pitch");
    check(read<uint64_t>(p, 0x38 + 0x70 + 8) == 32768 && read<uint32_t>(p, 0x38 + 0x70 + 0x64) == 1,
          "mip 1 in the tail at 32 KiB");

    std::vector<uint8_t> tiled(131072);
    bool round_trip = true;
    for (uint32_t mip = 0; mip < 9; ++mip) {
        const uint8_t* m = p + 0x38 + 0x70 * mip;
        uint32_t row = read<uint32_t>(m, 0x30) * 16;
        std::vector<uint8_t> rows(size_t{row} * read<uint32_t>(m, 0x34));
        for (size_t i = 0; i < rows.size(); ++i) rows[i] = static_cast<uint8_t>(i ^ mip);
        std::vector<uint8_t> back(rows.size());
        round_trip &= reinterpret_cast<Slot8Fn>(vt[8])(c, tiled.data(), 0, mip, rows.data(), row, 0) == S_OK;
        round_trip &= reinterpret_cast<Slot9Fn>(vt[9])(c, back.data(), 0, mip, 0, tiled.data(), row, 0) == S_OK;
        round_trip &= back == rows;
    }
    check(round_trip, "slots 8 and 9 copy every mip in and back out");

    uint32_t tiles = 0, shape[3] = {}, n = 4;
    uint8_t packed[12] = {}, subs[48] = {};
    check(reinterpret_cast<Slot10Fn>(vt[10])(c, &tiles, packed, shape, &n, 0, subs) == S_OK, "slot 10 succeeds");
    check(tiles == 2 && packed[0] == 1 && packed[1] == 8 && read<uint32_t>(packed, 4) == 1,
          "slot 10: 2 tiles, 1 standard mip, 8 packed in 1 tile");
    check(shape[0] == 256 && shape[1] == 256 && shape[2] == 1 && n == 1 && read<uint32_t>(subs, 8) == 1,
          "slot 10: 256x256 tiles, mip 0 starts at tile 1");
    check(reinterpret_cast<ReleaseFn>(vt[1])(c) == 0, "Release drops the last reference");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
