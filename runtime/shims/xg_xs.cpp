// xg_xs.dll: the console's texture layout library. The game executable (1.0.2.1) imports two functions from it.
//
// Measured from the call sites in the executable, read statically (shapes and checks only):
//   HRESULT XGSuggestSwizzleMode(const desc*, 0, uint32_t* mode)
//     2 call sites, both in the game's helper that fills a texture description before XGCreateTextureComputer. The
//     out pointer is a 32-bit field of that description, which the helper first sets to 0 and, when the mode is
//     known, writes itself as 0x100 | mode. The HRESULT is only passed back up; no caller branches on it.
//   HRESULT XGCreateTextureComputer(const desc*, computer** out)
//     4 call sites. Two test the HRESULT and take their failure path when it is negative; two ignore it and call
//     through *out at once. On the computer the callers use vtable slot 1 after they are done (3 sites, read as
//     Release; no site calls slot 0, read as AddRef, nor QueryInterface), slot 2 with one out pointer to a large
//     stack structure (2 sites), slot 8 with six arguments in a loop over subresources (1 site) and slot 10 with
//     seven arguments (1 site). No site reads the return value of slots 2, 8 or 10.
//
// The computer is ported from Marvisak/VIctory, GPLv3 (crates/victory-hle/src/modules/xg.rs), with the layout math
// in runtime/src/xg_texture.h. VIctory's byte offsets match the measured slots: 0x08 Release = slot 1, 0x10 layout =
// slot 2, 0x30 element offset = slot 6, 0x40 copy into = slot 8, 0x48 copy out of = slot 9, 0x50 64 KiB tiling =
// slot 10.
// - XGSuggestSwizzleMode writes 0x100 | the suggested mode (linear for 1D, the XOR modes for depth and render
//   targets, else 4 KiB standard).
// - XGCreateTextureComputer allocates a reference-counted computer per description. On a description with no layout
//   it returns E_INVALIDARG but still stores a static stub computer in *out: the two callers that do not check the
//   HRESULT would otherwise call through a null pointer. COM says *out is null on failure; this breaks that on
//   purpose. The stub counts no references and every slot but 0 and 1 returns E_NOTIMPL without writing anything.
// - Slots VIctory leaves out (3, 4, 5, 7, 11 and up to kSlots - 1) print their index and caller once and return
//   E_NOTIMPL.
#include <windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "../src/xg_texture.h"
#include "shims.h"

namespace {

namespace xg = leonida::xg;

constexpr HRESULT kNotImpl = static_cast<HRESULT>(rt::shims::kENotImpl);
constexpr size_t kSlots = 32;

// Prints once per entry point; `slot` is the vtable index for a computer method, or -1 for an export.
void warn_once(std::atomic<bool>& warned, const char* what, int slot, const void* caller) {
    if (warned.exchange(true, std::memory_order_relaxed)) return;
    char name[64];
    if (slot < 0) std::snprintf(name, sizeof(name), "%s", what);
    else std::snprintf(name, sizeof(name), "%s%d", what, slot);
    std::fprintf(stderr, "[rt] xg_xs: %s: not implemented; E_NOTIMPL (caller %s, logged once)\n", name,
                 rt::shims::module_offset(reinterpret_cast<uint64_t>(caller)).c_str());
}

// --- the stub computer handed out when creation fails ---

ULONG __stdcall stub_add_ref(void*) { return 1; }
ULONG __stdcall stub_release(void*) { return 1; }

template <size_t Slot>
HRESULT __stdcall stub_slot(void*) {
    static std::atomic<bool> warned{false};
    warn_once(warned, "stub texture computer (creation failed) slot ", static_cast<int>(Slot), _ReturnAddress());
    return kNotImpl;
}

template <size_t... Slot>
constexpr auto make_stub_vtable(std::index_sequence<Slot...>) {
    return std::array<void*, sizeof...(Slot)>{
        (Slot == 0   ? reinterpret_cast<void*>(&stub_add_ref)
         : Slot == 1 ? reinterpret_cast<void*>(&stub_release)
                     : reinterpret_cast<void*>(&stub_slot<Slot>))...};
}

const auto kStubVtable = make_stub_vtable(std::make_index_sequence<kSlots>{});
struct StubComputer {
    const void* const* vtable;
};
StubComputer g_stub{kStubVtable.data()};

// magic code, don't wanna deal with hours of plumbing with what these conjured devils of the souls of gpt 6 astra did.. jeez
// --- the computer: ported from Marvisak/VIctory, GPLv3 (crates/victory-hle/src/modules/xg.rs) ---

// D3D12_RESOURCE_DESC, whose layout field carries 0x100 | swizzle mode on the console (1 row major, 0 "pick one").
struct ResourceDesc {
    uint32_t dimension;
    uint64_t alignment;
    uint64_t width;
    uint32_t height;
    uint16_t depth_or_array;
    uint16_t mips;
    uint32_t format;
    uint32_t samples;
    uint32_t quality;
    uint32_t layout;
    uint32_t flags;
};
static_assert(sizeof(ResourceDesc) == 0x38);

constexpr uint32_t kSwizzled = 0x100;
constexpr uint32_t kRowMajor = 1;
constexpr uint32_t kAllowRenderTarget = 1;
constexpr uint32_t kAllowDepthStencil = 2;
constexpr uint32_t kTexture1d = 2;
constexpr uint32_t kTexture2d = 3;
constexpr uint32_t kTexture3d = 4;
constexpr size_t kMips = 15;  // the layout structure has room for 15 mips
constexpr uint64_t kTile = 64 << 10;

// The mode the console's library picks for a description: linear for 1D, the XOR render and depth modes for
// targets, else 4 KiB standard (.ttpf textures without a mode of their own use it).
//
// shortcut (VIctory): the rule of the library's older optimal-mode function (any size, 2D, 3D and cubes alike);
// XGSuggestSwizzleMode itself is newer than every copy VIctory could check. Revisit if streamed textures come out
// scrambled.
xg::Swizzle suggested(const ResourceDesc& desc) {
    if (desc.dimension == kTexture1d) return {xg::Swizzle::LINEAR};
    if (desc.flags & kAllowDepthStencil) return {xg::Swizzle::SW_64KB_Z_X};
    if (desc.flags & kAllowRenderTarget) return {xg::Swizzle::SW_64KB_R_X};
    return {xg::Swizzle::SW_4KB_S};
}

HRESULT __stdcall XGSuggestSwizzleMode(const ResourceDesc* desc, uint32_t /*flags*/, uint32_t* out) {
    if (!desc || !out) return E_INVALIDARG;
    *out = kSwizzled | suggested(*desc).value;
    return S_OK;
}

// One mip in the layout the title reads (offsets 0x00, 0x08, 0x10 and 0x1c are read by the game; the rest follow
// the library's older public shape).
struct MipOut {
    uint64_t size;
    uint64_t offset;
    uint64_t slice_size;
    uint32_t pitch_texels;
    uint32_t pitch_bytes;
    uint32_t alignment;
    uint32_t padded[3];  // width, height in elements, depth or array size
    uint32_t elements[2];
    uint32_t texels[2];
    uint32_t depth_or_array;
    uint32_t samples;
    uint32_t swizzle;
    uint64_t bank_rotation[2];
    uint32_t slice_depth;
    uint32_t in_tail;
    uint32_t tail_position;  // where a tail mip starts in elements: x bits 0-10, y 11-21
};

struct PlaneOut {
    uint32_t usage;  // 1 data, 7 depth, 8 stencil (metadata planes are not made)
    uint64_t size;
    uint64_t slice_size;
    uint64_t offset;
    uint64_t alignment;
    uint32_t element_bytes;
    uint32_t standard_mips;
    uint32_t tail_mips;
    MipOut mips[kMips];
};

struct LayoutOut {
    uint64_t size;
    uint64_t alignment;
    uint32_t mips;
    uint32_t planes;
    PlaneOut plane[4];
    uint32_t dimension;
};
static_assert(sizeof(MipOut) == 0x70);
static_assert(sizeof(PlaneOut) == 0x6c8);
static_assert(sizeof(LayoutOut) == 0x1b40);

struct PackedMips {
    uint8_t standard;
    uint8_t packed;
    uint32_t tiles;
    uint32_t start;
};
struct SubresourceTiling {
    uint32_t width;
    uint16_t height;
    uint16_t depth;
    uint32_t start;
};
static_assert(sizeof(PackedMips) == 12 && sizeof(SubresourceTiling) == 12);

struct Computer {
    const void* const* vtable;
    std::atomic<uint32_t> refs;
    ResourceDesc desc;
    uint32_t mips;
    xg::Texture texture;
};

bool make_texture(const ResourceDesc& desc, xg::Texture& out) {
    xg::Dimension dimension;
    switch (desc.dimension) {
        case kTexture1d: dimension = xg::Dimension::Tex1d; break;
        case kTexture2d: dimension = xg::Dimension::Tex2d; break;
        case kTexture3d: dimension = xg::Dimension::Tex3d; break;
        default: return false;
    }
    xg::Swizzle swizzle;
    if (desc.layout == 0) swizzle = suggested(desc);
    else if (desc.layout == kRowMajor) swizzle = {xg::Swizzle::LINEAR};
    else if ((desc.layout & ~0xffu) == kSwizzled) swizzle = {desc.layout & 0xff};
    else return false;
    if (desc.width > UINT32_MAX) return false;
    xg::TextureDesc td;
    td.dimension = dimension;
    td.width = static_cast<uint32_t>(desc.width);
    td.height = desc.height;
    td.depth_or_array = desc.depth_or_array;
    td.mips = desc.mips;
    td.format = desc.format;
    td.samples = desc.samples ? desc.samples : 1;
    td.swizzle = swizzle;
    if (xg::Texture::make(td, out) != xg::Error::Ok) return false;
    return out.planes[0].layout.mips.size() <= kMips;
}

// The mode a tail mip is reported in: the 256-byte one of its kind (3 for the render kind, which GFX10 lacks), the
// depth mode itself.
uint32_t tail_swizzle(xg::Swizzle s) {
    switch (s.value) {
        case 2: case 6: case 10: case 18: case 22: case 26: return xg::Swizzle::SW_256B_D;
        case 23: case 27: return 3;
        case 24: return s.value;
        default: return xg::Swizzle::SW_256B_S;
    }
}

MipOut mip_out(const ResourceDesc& desc, const xg::Layout& l, size_t i) {
    const xg::Mip& m = l.mips[i];
    uint64_t bpe = l.desc.element_bytes;
    uint32_t block = l.desc.block;
    bool three_d = desc.dimension == kTexture3d;
    uint32_t array = desc.depth_or_array ? desc.depth_or_array : 1u;
    MipOut out{};
    out.pitch_texels = m.pitch * block;
    out.pitch_bytes = m.pitch * l.desc.element_bytes;
    out.padded[0] = m.pitch;
    out.padded[1] = m.rows;
    out.padded[2] = three_d ? m.depth_pitch : array;
    out.elements[0] = m.width;
    out.elements[1] = m.height;
    out.texels[0] = std::max(static_cast<uint32_t>(desc.width >> i), 1u);
    out.texels[1] = std::max(desc.height >> i, 1u);
    out.depth_or_array = three_d ? m.depth : array;
    out.samples = l.desc.samples;
    out.slice_depth = l.block[2];
    uint64_t slice = uint64_t{m.pitch} * m.rows * bpe;
    uint64_t depth = three_d ? m.depth_pitch : 1;
    if (!m.tail) {
        out.offset = m.offset;
        out.slice_size = slice * l.desc.samples;
        out.size = out.slice_size * depth;
        out.alignment = static_cast<uint32_t>(l.alignment);
        out.swizzle = l.desc.swizzle.value;
    } else {
        out.in_tail = 1;
        out.alignment = 256;
        out.swizzle = tail_swizzle(l.desc.swizzle);
        if (l.thick) {
            // A 3D tail mip is placed by position, not by offset.
            out.slice_size = l.alignment / l.block[2];
            out.tail_position = m.tail->x | (m.tail->y << 11);
        } else {
            out.offset = m.tail->offset;
            out.slice_size = slice;
        }
        out.size = out.slice_size * depth;
    }
    return out;
}

ULONG __stdcall computer_add_ref(Computer* c) { return c->refs.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG __stdcall computer_release(Computer* c) {
    uint32_t left = c->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) delete c;
    return left;
}

// Slot 2: fills the layout structure (sizes, planes, every mip's placement).
HRESULT __stdcall computer_layout(Computer* c, LayoutOut* out) {
    if (!out) return E_INVALIDARG;
    std::memset(out, 0, sizeof(*out));
    out->size = c->texture.size;
    out->alignment = c->texture.alignment;
    out->mips = c->mips;
    out->planes = static_cast<uint32_t>(c->texture.planes.size());
    out->dimension = c->desc.dimension;
    // The library calls a depth plane "depth" only beside stencil or without mips.
    bool depth = (c->desc.flags & kAllowDepthStencil) && (c->texture.planes.size() > 1 || c->mips == 1);
    for (size_t pi = 0; pi < c->texture.planes.size() && pi < 4; ++pi) {
        const xg::Plane& p = c->texture.planes[pi];
        const xg::Layout& l = p.layout;
        PlaneOut& plane = out->plane[pi];
        plane.usage = p.stencil ? 8 : depth ? 7 : 1;
        plane.size = l.size;
        plane.slice_size = l.slice_size;
        plane.offset = p.offset;
        plane.alignment = l.alignment;
        plane.element_bytes = l.desc.element_bytes;
        plane.standard_mips = l.first_tail_mip;
        plane.tail_mips = c->mips - l.first_tail_mip;
        for (size_t i = 0; i < l.mips.size() && i < kMips; ++i) plane.mips[i] = mip_out(c->desc, l, i);
    }
    return S_OK;
}

// The plane, its mip and array slice for a subresource index, and the linear bytes the copy touches.
struct CopyArgs {
    const xg::Plane* plane;
    uint32_t mip;
    uint32_t slice;
    size_t linear;
};

bool copy_args(const Computer* c, uint32_t plane, uint32_t subresource, uint32_t row, uint32_t depth,
               CopyArgs& a) {
    if (plane >= c->texture.planes.size()) return false;
    const xg::Plane& p = c->texture.planes[plane];
    const xg::Layout& l = p.layout;
    uint32_t mip = subresource % c->mips, slice = subresource / c->mips;
    if (mip >= l.mips.size()) return false;
    const xg::Mip& m = l.mips[mip];
    size_t bytes = size_t{m.width} * l.desc.element_bytes;
    size_t depths = l.subresource_depth(mip);
    // Pitches are 32-bit, so this stays within 64 bits.
    a.linear = (depths - 1) * depth + (size_t{m.height} - 1) * row + bytes;
    a.plane = &p;
    a.mip = mip;
    a.slice = slice;
    return true;
}

HRESULT result(xg::Error e) {
    switch (e) {
        case xg::Error::Ok: return S_OK;
        case xg::Error::Unsupported: return kNotImpl;
        default: return E_INVALIDARG;
    }
}

void warn_copy(const char* what, const Computer* c, uint32_t subresource, xg::Error e) {
    static std::atomic<uint32_t> printed{0};
    if (printed.fetch_add(1, std::memory_order_relaxed) >= 16) return;
    std::fprintf(stderr,
                 "[rt] xg_xs: copy %s a texture failed (error %d): %llux%u dim %u format %u layout %#x subresource %u "
                 "(first 16 logged)\n",
                 what, static_cast<int>(e), static_cast<unsigned long long>(c->desc.width), c->desc.height,
                 c->desc.dimension, c->desc.format, c->desc.layout, subresource);
}

// Slot 8: (tiled base, plane, subresource, linear, row pitch, depth pitch). Linear rows into tiled memory.
HRESULT __stdcall computer_copy_into(Computer* c, uint8_t* tiled, uint32_t plane, uint32_t subresource,
                                     const uint8_t* linear, uint32_t row, uint32_t depth) {
    CopyArgs a;
    if (!copy_args(c, plane, subresource, row, depth, a)) return E_INVALIDARG;
    if (!tiled || !linear) return E_INVALIDARG;
    const xg::Layout& l = a.plane->layout;
    xg::Error e = l.tile(tiled + a.plane->offset, static_cast<size_t>(l.size), a.mip, a.slice, linear, a.linear, row,
                         depth);
    if (e != xg::Error::Ok) warn_copy("into", c, subresource, e);
    return result(e);
}

// Slot 9: (linear, plane, subresource, sample, tiled base, row pitch, depth pitch). Tiled memory into linear rows.
HRESULT __stdcall computer_copy_from(Computer* c, uint8_t* linear, uint32_t plane, uint32_t subresource,
                                     uint32_t /*sample*/, const uint8_t* tiled, uint32_t row, uint32_t depth) {
    CopyArgs a;
    if (!copy_args(c, plane, subresource, row, depth, a)) return E_INVALIDARG;
    if (!tiled || !linear) return E_INVALIDARG;
    const xg::Layout& l = a.plane->layout;
    xg::Error e = l.detile(tiled + a.plane->offset, static_cast<size_t>(l.size), a.mip, a.slice, linear, a.linear, row,
                           depth);
    if (e != xg::Error::Ok) warn_copy("out of", c, subresource, e);
    return result(e);
}

// Slot 10: (total tiles, packed mips, tile shape in texels, in: room / out: count, first, subresource tilings), in
// 64 KiB tiles of plane 0, for 2D 64 KiB modes only (others fail, as on the console). The mip tail is the packed
// part, tile 0 of each slice; tilings list the standard mips of every slice in turn. With a tail the console's
// library lists two slices at most and zeroes all but the first; so does this (the title reads slice 0 only).
HRESULT __stdcall computer_tiling(Computer* c, uint32_t* tiles, PackedMips* packed, uint32_t* shape, uint32_t* count,
                                  uint32_t first, SubresourceTiling* out) {
    const xg::Layout& l = c->texture.planes[0].layout;
    if (l.alignment != kTile) return E_FAIL;
    if (l.thick) return kNotImpl;
    uint32_t standard = l.first_tail_mip;
    bool tail = standard < c->mips;
    uint64_t tail_tiles = standard == 0 ? l.slice_size / kTile
                          : tail        ? l.mips[standard - 1].offset / kTile
                                        : 0;
    uint32_t block = l.desc.block;
    if (tiles) *tiles = static_cast<uint32_t>(l.size / kTile);
    if (packed) {
        packed->standard = static_cast<uint8_t>(standard);
        packed->packed = static_cast<uint8_t>(c->mips - standard);
        packed->tiles = static_cast<uint32_t>(tail_tiles);
        packed->start = 0;
    }
    if (shape) {
        shape[0] = l.block[0] * block;
        shape[1] = l.block[1] * block;
        shape[2] = l.block[2];
    }
    if (count) {
        uint32_t slices = tail ? std::min(l.slices, 2u) : l.slices;
        uint32_t total = standard * slices;
        uint32_t wanted = std::min(total > first ? total - first : 0u, *count);
        for (uint32_t i = 0; i < wanted; ++i) {
            uint32_t slice = (first + i) / standard, mip = (first + i) % standard;
            const xg::Mip& m = l.mips[mip];
            SubresourceTiling entry{};
            if (!(tail && slice > 0)) {
                entry.width = m.pitch / l.block[0];
                entry.height = static_cast<uint16_t>(m.rows / l.block[1]);
                entry.depth = 1;
                entry.start = static_cast<uint32_t>((uint64_t{slice} * l.slice_size + m.offset) / kTile);
            }
            out[i] = entry;
        }
        *count = wanted;
    }
    return S_OK;
}

// Slot 6: (plane, mip, x, y, z or slice, sample, bit position): an element's byte offset from the texture's start.
// VIctory saw the game read texels of a 128x4 4KB_S texture on the CPU through it; an error returned as an offset
// made it read wild memory, so this returns 0 when the element is outside.
uint64_t __stdcall computer_element_offset(Computer* c, uint32_t plane, uint32_t mip, uint64_t x, uint32_t y,
                                           uint32_t z, uint32_t /*sample*/, uint32_t* bit) {
    if (bit) *bit = 0;
    if (plane >= c->texture.planes.size()) return 0;
    const xg::Plane& p = c->texture.planes[plane];
    uint32_t x32 = x > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(x);
    uint64_t at = 0;
    if (p.layout.offset(mip, x32, y, z, at) != xg::Error::Ok) {
        static std::atomic<uint32_t> printed{0};
        if (printed.fetch_add(1, std::memory_order_relaxed) < 16)
            std::fprintf(stderr, "[rt] xg_xs: texel outside its texture: mip %u x %u y %u z %u (first 16 logged)\n",
                         mip, x32, y, z);
        return 0;
    }
    return p.offset + at;
}

// Slots VIctory does not implement either: size, alignment, mip offset, element coordinate, view descriptor,
// tiled-resource query and anything past them.
template <size_t Slot>
HRESULT __stdcall computer_unused(void*) {
    static std::atomic<bool> warned{false};
    warn_once(warned, "texture computer slot ", static_cast<int>(Slot), _ReturnAddress());
    return kNotImpl;
}

template <size_t Slot>
void* computer_slot() {
    if constexpr (Slot == 0) return reinterpret_cast<void*>(&computer_add_ref);
    else if constexpr (Slot == 1) return reinterpret_cast<void*>(&computer_release);
    else if constexpr (Slot == 2) return reinterpret_cast<void*>(&computer_layout);
    else if constexpr (Slot == 6) return reinterpret_cast<void*>(&computer_element_offset);
    else if constexpr (Slot == 8) return reinterpret_cast<void*>(&computer_copy_into);
    else if constexpr (Slot == 9) return reinterpret_cast<void*>(&computer_copy_from);
    else if constexpr (Slot == 10) return reinterpret_cast<void*>(&computer_tiling);
    else return reinterpret_cast<void*>(&computer_unused<Slot>);
}

template <size_t... Slot>
auto make_vtable(std::index_sequence<Slot...>) {
    return std::array<void*, sizeof...(Slot)>{computer_slot<Slot>()...};
}

const auto kVtable = make_vtable(std::make_index_sequence<kSlots>{});

HRESULT __stdcall XGCreateTextureComputer(const ResourceDesc* desc, void** out) {
    if (!out) return E_POINTER;
    // See the top of the file: two measured callers use *out without checking the HRESULT, so a failure still
    // stores the stub.
    *out = &g_stub;
    if (!desc) return E_INVALIDARG;
    xg::Texture texture;
    if (!make_texture(*desc, texture)) {
        static std::atomic<uint32_t> printed{0};
        if (printed.fetch_add(1, std::memory_order_relaxed) < 16)
            std::fprintf(stderr,
                         "[rt] xg_xs: XGCreateTextureComputer: no texture layout for %llux%ux%u dim %u mips %u format "
                         "%u layout %#x flags %#x; E_INVALIDARG with a stub computer (first 16 logged)\n",
                         static_cast<unsigned long long>(desc->width), desc->height, desc->depth_or_array,
                         desc->dimension, desc->mips, desc->format, desc->layout, desc->flags);
        return E_INVALIDARG;
    }
    auto* c = new (std::nothrow) Computer{kVtable.data(), {1}, *desc, 0, {}};
    if (!c) return E_OUTOFMEMORY;
    c->mips = static_cast<uint32_t>(texture.planes[0].layout.mips.size());
    c->texture = std::move(texture);
    *out = c;
    return S_OK;
}

}  // namespace

SHIM_TABLE(xg_xs,
    {"xg_xs.dll", "XGCreateTextureComputer", reinterpret_cast<void*>(&XGCreateTextureComputer)},
    {"xg_xs.dll", "XGSuggestSwizzleMode", reinterpret_cast<void*>(&XGSuggestSwizzleMode)},
);
