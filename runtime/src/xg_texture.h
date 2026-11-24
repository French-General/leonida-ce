// Texture layouts of the console GPU (RDNA2, GFX10.3): a texture's size and alignment, where each mip and array
// slice starts, the address of each element, and copies between tiled memory and linear rows. Serves the xg_xs.dll
// texture computer (runtime/shims/xg_xs.cpp).
//
// Ported from Marvisak/VIctory, GPLv3 (crates/victory-formats/src/texture.rs), kept close to the original so fixes
// can be carried across.
//
// Units: an element is a texel, or a 4x4 block of a block-compressed format. Mips of one array slice (or one depth
// slab of a thick 3D texture) are stored together, smallest first: the mip tail (the small mips sharing one block)
// at offset 0, mip 0 last.
//
// The algorithm follows AMD's address library (addrlib), as shipped in Mesa under src/amd/addrlib: block dimensions,
// mip placement and mip tails from gfx10addrlib.cpp and addrlib2.cpp (ComputeSurfaceInfoMacroTiled,
// ComputeThinBlockDimension, GetMipTailDim, GetMaxNumMipsInTail, HwlComputeSurfaceInfoLinear,
// ComputeSurfaceAddrFromCoordMacroTiled), and the 256-byte micro tile patterns in kMicro from gfx10SwizzlePattern.h
// (the RB+ tables, as on RDNA2). addrlib carries this notice:
//
// Copyright (C) 2007-2026 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions: the above copyright notice and this permission
// notice shall be included in all copies or substantial portions of the
// Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace leonida::xg {

// A swizzle mode, by its hardware number (addrlib's ADDR_SW_* order).
struct Swizzle {
    uint32_t value = 0;

    static constexpr uint32_t LINEAR = 0;
    static constexpr uint32_t SW_256B_S = 1;
    static constexpr uint32_t SW_256B_D = 2;
    static constexpr uint32_t SW_4KB_S = 5;
    static constexpr uint32_t SW_4KB_D = 6;
    static constexpr uint32_t SW_64KB_S = 9;
    static constexpr uint32_t SW_64KB_D = 10;
    static constexpr uint32_t SW_64KB_S_T = 17;
    static constexpr uint32_t SW_64KB_D_T = 18;
    static constexpr uint32_t SW_4KB_S_X = 21;
    static constexpr uint32_t SW_4KB_D_X = 22;
    static constexpr uint32_t SW_4KB_R_X = 23;
    static constexpr uint32_t SW_64KB_Z_X = 24;
    static constexpr uint32_t SW_64KB_S_X = 25;
    static constexpr uint32_t SW_64KB_D_X = 26;
    static constexpr uint32_t SW_64KB_R_X = 27;

    // log2 of the block size in bytes (0 for linear); nullopt for modes GFX10 does not have.
    std::optional<uint32_t> block_log2() const {
        switch (value) {
            case 0: return 0u;
            case 1: case 2: return 8u;
            case 5: case 6: case 21: case 22: case 23: return 12u;
            case 9: case 10: case 17: case 18: case 24: case 25: case 26: case 27: return 16u;
            default: return std::nullopt;
        }
    }
    bool standard() const {
        return value == 1 || value == 5 || value == 9 || value == 17 || value == 21 || value == 25;
    }
    bool display() const {
        return value == 2 || value == 6 || value == 10 || value == 18 || value == 22 || value == 26;
    }
    bool z_order() const { return value == 24; }
    // Pipe and bank XOR modes (_T, _X): their addresses depend on the GPU's pipe configuration.
    bool xor_mode() const { return value >= 16; }
    bool operator==(const Swizzle&) const = default;
};

enum class Dimension { Tex1d, Tex2d, Tex3d };

enum class Error {
    Ok,
    // The description makes no texture (zero size, bad element size, a mode this dimension cannot use, too many mips).
    Invalid,
    // A valid layout whose addressing is not implemented here.
    Unsupported,
    // A buffer is smaller than the layout or the rows need.
    TooSmall,
};

// One plane of a texture.
struct Desc {
    Dimension dimension = Dimension::Tex2d;
    uint32_t width = 0;  // in texels
    uint32_t height = 0;
    uint32_t depth = 0;  // depth of a 3D texture, else the array size
    uint32_t mips = 0;
    uint32_t element_bytes = 0;  // 1, 2, 4, 8 or 16
    uint32_t block = 1;          // texels per element side: 1, or 4 for block compression
    uint32_t samples = 1;
    Swizzle swizzle{};
};

namespace detail {

inline uint32_t div_ceil(uint32_t a, uint32_t b) { return a / b + (a % b != 0); }
inline uint32_t next_multiple_of(uint32_t a, uint32_t m) { return div_ceil(a, m) * m; }
inline uint64_t next_multiple_of(uint64_t a, uint64_t m) { return (a / m + (a % m != 0)) * m; }
inline bool is_pow2(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

inline bool checked_mul(size_t a, size_t b, size_t& out) {
    if (a != 0 && b > SIZE_MAX / a) return false;
    out = a * b;
    return true;
}
inline bool checked_add(size_t a, size_t b, size_t& out) {
    if (b > SIZE_MAX - a) return false;
    out = a + b;
    return true;
}

// Elements along one side of mip `mip`, as data and copies see it: texels halve rounding down (to at least 1), then
// round up to whole elements.
inline uint32_t dim(uint32_t texels, uint32_t mip, uint32_t block) {
    return div_ceil(std::max(texels >> mip, 1u), block);
}

// Elements along one side of mip `mip` for placement (padding, tail, offsets): addrlib halves mip 0's elements
// rounding up, which can be one more than dim() for odd sizes.
inline uint32_t padded_dim(uint32_t texels, uint32_t mip, uint32_t block) {
    return div_ceil(div_ceil(std::max(texels, 1u), block), 1u << mip);
}

// Micro tile (256 bytes) patterns by log2 of the element size: for each address bit, the element coordinate bit it
// takes (x0, y1, z0...), "--" for the bits inside an element. Thin S, thin D, thick S (also the D 3D modes). From
// addrlib's GFX10_SW_256_S_RBPLUS, GFX10_SW_256_D_RBPLUS and GFX10_SW_4K_S3_RBPLUS patterns; the larger blocks add
// bits Y, X (thin) or Y, X, Z (thick) in turn.
inline constexpr const char* kMicro[3][5] = {
    {"x0x1x2x3y0y1y2y3", "--x0x1x2y0y1y2x3", "----x0x1y0y1y2x2", "------x0y0y1x1x2", "--------y0y1x0x1"},
    {"x0x1x2y1y0y2x3y3", "--x0x1x2y0y1y2x3", "----x0x1y0y1x2y2", "------x0y0x1x2y1", "--------x0y0x1y1"},
    {"x0x1z0y0z1y1x2z2", "--x0z0y0z1y1x1z2", "----x0y0z0y1x1z1", "------x0z0y0x1z1", "--------z0y0x0z1"},
};

// For each axis, the address bit each coordinate bit lands on.
inline std::array<std::vector<uint32_t>, 3> pattern(size_t kind, uint32_t element_log2, uint32_t block_log2) {
    std::array<std::vector<uint32_t>, 3> axes;
    const char* micro = kMicro[kind][element_log2];
    for (uint32_t bit = 0; bit < 8; ++bit) {
        char a = micro[2 * bit];
        size_t axis;
        if (a == 'x') axis = 0;
        else if (a == 'y') axis = 1;
        else if (a == 'z') axis = 2;
        else continue;
        size_t index = static_cast<size_t>(micro[2 * bit + 1] - '0');
        auto& to = axes[axis];
        if (to.size() <= index) to.resize(index + 1, 0);
        to[index] = bit;
    }
    static constexpr size_t kThin[] = {1, 0};
    static constexpr size_t kThick[] = {1, 0, 2};
    const size_t* order = kind == 2 ? kThick : kThin;
    size_t order_len = kind == 2 ? 3 : 2;
    size_t k = 0;
    for (uint32_t bit = 8; bit < block_log2; ++bit, ++k) axes[order[k % order_len]].push_back(bit);
    return axes;
}

// Byte offsets inside a block for coordinates 0..2^bits of one axis.
inline std::vector<uint32_t> spread(const std::vector<uint32_t>& bits) {
    std::vector<uint32_t> out(size_t{1} << bits.size());
    for (uint32_t c = 0; c < out.size(); ++c) {
        uint32_t sum = 0;
        for (size_t i = 0; i < bits.size(); ++i) sum += ((c >> i) & 1u) << bits[i];
        out[c] = sum;
    }
    return out;
}

inline constexpr uint32_t kMaxMips = 16;

// The largest width, height and depth (or array size) accepted, in texels. GFX10's image descriptors hold at most
// 16384 texels per side, so no texture the GPU can use is refused. The bound also keeps every size computed below
// within uint64_t (and its uint32_t steps), which unchecked values from a title would otherwise overflow.
inline constexpr uint32_t kMaxExtent = 1u << 14;

// 256-byte micro tile dimensions by log2 element size, 2D and 3D.
inline constexpr uint32_t kBlock256_2d[5][2] = {{16, 16}, {16, 8}, {8, 8}, {8, 4}, {4, 4}};
inline constexpr uint32_t kBlock256_3d[5][3] = {{8, 4, 8}, {4, 4, 8}, {4, 4, 4}, {4, 2, 4}, {2, 2, 4}};
inline constexpr uint32_t kBlock1k_3d[5][3] = {{16, 8, 8}, {8, 8, 8}, {8, 8, 4}, {8, 4, 4}, {4, 4, 4}};

}  // namespace detail

struct Tail {
    uint32_t offset = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    bool operator==(const Tail&) const = default;
};

// Where one mip lives inside a slice.
struct Mip {
    // Size in elements (depth 1 unless the texture is 3D).
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    // Padded size in elements: whole blocks, or the tail's share of one.
    uint32_t pitch = 0;
    uint32_t rows = 0;
    uint32_t depth_pitch = 0;
    // Byte offset of its first block in the slice (0 in the tail).
    uint64_t offset = 0;
    // In the mip tail: byte offset inside the tail block and the element position the mip starts at.
    std::optional<Tail> tail;
};

struct Layout {
    Desc desc{};
    // Block dimensions in elements (linear: the pitch alignment, 1, 1).
    std::array<uint32_t, 3> block{};
    // 3D blocks span block[2] depth slices (GFX10 3D S and D modes).
    bool thick = false;
    // Array slices, depth slabs of block[2] slices (thick), or depth slices (thin and linear 3D).
    uint32_t slices = 0;
    // Bytes of one slice: its whole mip chain.
    uint64_t slice_size = 0;
    uint64_t size = 0;
    uint64_t alignment = 0;
    // The first mip in the tail; mips.size() without one.
    uint32_t first_tail_mip = 0;
    std::vector<Mip> mips;
    // Byte offsets inside a block of each element coordinate bit, per axis; empty for linear and unsupported modes.
    std::array<std::vector<uint32_t>, 3> pattern;

    static Error make(const Desc& d, Layout& out) {
        using namespace detail;
        auto block_log2 = d.swizzle.block_log2();
        if (!block_log2) return Error::Invalid;
        auto in = [](uint32_t v, uint32_t lo, uint32_t hi) { return v >= lo && v <= hi; };
        uint32_t s = d.swizzle.value;
        bool valid = in(d.width, 1, kMaxExtent) && in(d.height, 1, kMaxExtent) && in(d.depth, 1, kMaxExtent) &&
                     in(d.mips, 1, kMaxMips) && is_pow2(d.element_bytes) && d.element_bytes <= 16 &&
                     (d.block == 1 || d.block == 4) && is_pow2(d.samples) && d.samples <= 8 &&
                     (d.samples == 1 || (d.dimension == Dimension::Tex2d && d.mips == 1)) &&
                     (d.dimension != Dimension::Tex1d || d.height == 1) &&
                     (d.dimension != Dimension::Tex3d || s == 0 || s == 5 || s == 9 || s == 17 || s == 21 ||
                      (s >= 24 && s <= 27));
        if (!valid) return Error::Invalid;
        out = *block_log2 == 0 ? linear(d) : tiled(d, *block_log2);
        return Error::Ok;
    }

    // HwlComputeSurfaceInfoLinear: rows aligned to 256 bytes, mips packed smallest first, a 3D texture's depth
    // slices each hold the chain.
    static Layout linear(const Desc& d) {
        using namespace detail;
        uint64_t bpe = d.element_bytes;
        uint32_t align = 256 / d.element_bytes;
        bool three_d = d.dimension == Dimension::Tex3d;
        std::vector<Mip> mips;
        uint64_t offset = 0;
        for (uint32_t i = d.mips; i-- > 0;) {
            uint32_t h = padded_dim(d.height, i, d.block);
            uint32_t pitch = next_multiple_of(padded_dim(d.width, i, d.block), align);
            Mip m;
            m.width = dim(d.width, i, d.block);
            m.height = dim(d.height, i, d.block);
            m.depth = three_d ? dim(d.depth, i, 1) : 1;
            m.pitch = pitch;
            m.rows = h;
            m.depth_pitch = three_d ? d.depth : 1;
            m.offset = offset;
            mips.push_back(m);
            offset += uint64_t{pitch} * h * bpe;
        }
        std::reverse(mips.begin(), mips.end());
        Layout l;
        l.desc = d;
        l.block = {align, 1, 1};
        l.thick = false;
        l.slices = d.depth;
        l.slice_size = offset;
        l.size = offset * d.depth;
        l.alignment = 256;
        l.first_tail_mip = d.mips;
        l.mips = std::move(mips);
        return l;
    }

    static Layout tiled(const Desc& d, uint32_t block_log2) {
        using namespace detail;
        uint32_t element_log2 = static_cast<uint32_t>(std::countr_zero(d.element_bytes));
        uint64_t bpe = d.element_bytes;
        uint64_t block_bytes = uint64_t{1} << block_log2;
        bool thick = d.dimension == Dimension::Tex3d && (d.swizzle.standard() || d.swizzle.display());
        std::array<uint32_t, 3> block;
        if (thick) {
            // ComputeThickBlockDimension
            uint32_t amp = (block_log2 - 10) / 3;
            uint32_t rest = (block_log2 - 10) % 3;
            const uint32_t* b = kBlock1k_3d[element_log2];
            block = {b[0] << amp, b[1] << (amp + rest / 2), b[2] << (amp + (rest != 0 ? 1u : 0u))};
        } else {
            // ComputeThinBlockDimension
            uint32_t samples_log2 = static_cast<uint32_t>(std::countr_zero(d.samples));
            uint32_t elements_log2 = block_log2 - element_log2 - samples_log2;
            bool wide = (samples_log2 & 1) == 0 || (block_log2 & 1) != 0;
            uint32_t w = (elements_log2 + (wide ? 1u : 0u)) / 2;
            block = {1u << w, 1u << (elements_log2 - w), 1};
        }
        uint32_t slices = next_multiple_of(d.depth, block[2]);
        bool three_d = d.dimension == Dimension::Tex3d;
        // Mip depths placed by a thick layout (thin ones store depth as slices).
        auto mip_depth = [&](uint32_t i) { return thick ? padded_dim(d.depth, i, 1) : 1u; };
        auto depth_of = [&](uint32_t i) { return three_d ? dim(d.depth, i, 1) : 1u; };
        auto width = [&](uint32_t i) { return dim(d.width, i, d.block); };
        auto height = [&](uint32_t i) { return dim(d.height, i, d.block); };
        std::vector<Mip> mips;
        mips.reserve(d.mips);
        uint32_t first_tail_mip = d.mips;
        uint64_t slice_size = 0;
        if (d.mips == 1) {
            Mip m;
            m.width = width(0);
            m.height = height(0);
            m.depth = depth_of(0);
            m.pitch = next_multiple_of(width(0), block[0]);
            m.rows = next_multiple_of(height(0), block[1]);
            m.depth_pitch = three_d ? slices : 1;
            m.offset = 0;
            mips.push_back(m);
            slice_size = uint64_t{m.pitch} * m.rows * bpe * d.samples;
        } else {
            // GetMipTailDim: the largest mip the tail takes.
            std::array<uint32_t, 3> tail_max = block;
            if (thick) {
                switch (block_log2 % 3) {
                    case 0: tail_max[1] >>= 1; break;
                    case 1: tail_max[0] >>= 1; break;
                    default: tail_max[2] >>= 1; break;
                }
            } else {
                tail_max[0] >>= 1;
            }
            // GetMaxNumMipsInTail; 256-byte blocks have no tail (ComputeSurfaceInfoMicroTiled).
            uint32_t effective = thick ? block_log2 - (block_log2 - 8) / 3 : block_log2;
            uint32_t max_in_tail;
            if (effective == 8) max_in_tail = 0;
            else if (effective >= 9 && effective <= 11) max_in_tail = 1 + (1u << (effective - 9));
            else max_in_tail = effective - 4;
            // dsMipmapHtileFix: 8- and 16-bit depth enter the tail as 32-bit.
            std::array<uint32_t, 3> fixed = tail_max;
            if (d.swizzle.z_order() && element_log2 <= 1) {
                const uint32_t* b = kBlock256_2d[element_log2];
                fixed[0] /= b[0] / kBlock256_2d[2][0];
                fixed[1] /= b[1] / kBlock256_2d[2][1];
            }
            std::vector<uint64_t> sizes;
            for (uint32_t i = 0; i < d.mips; ++i) {
                uint32_t w = padded_dim(d.width, i, d.block);
                uint32_t h = padded_dim(d.height, i, d.block);
                if (w <= fixed[0] && h <= fixed[1] && d.mips - i <= max_in_tail) {
                    first_tail_mip = i;
                    slice_size += block_bytes / block[2];
                    break;
                }
                uint32_t pitch = next_multiple_of(w, block[0]);
                uint32_t rows = next_multiple_of(h, block[1]);
                uint32_t depth = next_multiple_of(mip_depth(i), block[2]);
                uint64_t one = uint64_t{pitch} * rows * bpe;
                slice_size += one;
                sizes.push_back(one * block[2]);
                Mip m;
                m.width = width(i);
                m.height = height(i);
                m.depth = depth_of(i);
                m.pitch = pitch;
                m.rows = rows;
                m.depth_pitch = three_d ? depth : 1;
                m.offset = 0;
                mips.push_back(m);
            }
            // Standard mips from the smallest up, after the tail block.
            uint64_t offset = first_tail_mip < d.mips ? block_bytes : 0;
            for (size_t i = first_tail_mip; i-- > 0;) {
                mips[i].offset = offset;
                offset += sizes[i];
            }
            uint32_t micro[2];
            if (thick) {
                micro[0] = kBlock256_3d[element_log2][0];
                micro[1] = kBlock256_3d[element_log2][1];
            } else {
                micro[0] = kBlock256_2d[element_log2][0];
                micro[1] = kBlock256_2d[element_log2][1];
            }
            uint32_t pitch = tail_max[0], rows = tail_max[1];
            for (uint32_t i = first_tail_mip; i < d.mips; ++i) {
                uint32_t m = max_in_tail - 1 - (i - first_tail_mip);
                uint32_t toff = m > 6 ? 16u << m : m << 8;
                // Address bits 8.. of the offset, odd ones x, even ones y.
                auto bits = [&](uint32_t from) {
                    uint32_t sum = 0;
                    for (uint32_t b = 0; b < 6; ++b) sum += ((toff >> (from + 2 * b)) & 1u) << b;
                    return sum;
                };
                uint32_t x = bits(9), y = bits(8);
                if (block_log2 & 1) {
                    std::swap(x, y);
                    if (element_log2 & 1) {
                        y = (y << 1) | (x & 1);
                        x >>= 1;
                    }
                }
                Mip mip;
                mip.width = width(i);
                mip.height = height(i);
                mip.depth = depth_of(i);
                mip.pitch = pitch;
                mip.rows = rows;
                mip.depth_pitch = thick ? next_multiple_of(mip_depth(i), kBlock256_3d[element_log2][2]) : 1u;
                mip.offset = 0;
                mip.tail = Tail{toff, x * micro[0], y * micro[1]};
                mips.push_back(mip);
                pitch = std::max(pitch >> 1, micro[0]);
                rows = std::max(rows >> 1, micro[1]);
            }
        }
        // Thick layouts store each depth slab's chain together.
        uint64_t slab = thick ? block[2] : 1;
        slice_size *= slab;
        slices /= block[2];
        bool addressable = !d.swizzle.xor_mode() && d.samples == 1;
        Layout l;
        if (addressable) {
            size_t kind = thick ? 2 : (d.swizzle.display() ? 1 : 0);
            auto p = detail::pattern(kind, element_log2, block_log2);
            for (size_t a = 0; a < 3; ++a) l.pattern[a] = spread(p[a]);
        }
        l.desc = d;
        l.block = block;
        l.thick = thick;
        l.slices = slices;
        l.slice_size = slice_size;
        l.size = slice_size * slices;
        l.alignment = block_bytes;
        l.first_tail_mip = first_tail_mip;
        l.mips = std::move(mips);
        return l;
    }

    // Whether offset() and the copies work for this layout: linear and the modes without pipe or bank XOR,
    // single-sampled.
    //
    // shortcut: no addressing for the XOR modes (_X, _T) and MSAA; they need the GPU's pipe and bank configuration
    // (Series X and S differ). Titles use them for render targets and depth, which the host renders; add them when a
    // title copies into or out of one.
    bool addressable() const { return desc.swizzle.value == Swizzle::LINEAR || !pattern[0].empty(); }

    // The slice and the depth inside it for z (an array slice or a 3D depth slice).
    std::pair<uint32_t, uint32_t> split(uint32_t z) const {
        if (thick) return {z / block[2], z % block[2]};
        return {z, 0};
    }

    // Byte offset of element (x, y) of mip `mip` in array slice or depth slice z. Coordinates must lie inside the mip.
    Error offset(uint32_t mip, uint32_t x, uint32_t y, uint32_t z, uint64_t& out) const {
        if (mip >= mips.size()) return Error::Invalid;
        const Mip& m = mips[mip];
        uint32_t depth = desc.dimension == Dimension::Tex3d ? m.depth : slices;
        bool inside = x < m.width && y < m.height && z < depth;
        if (!inside) return Error::Invalid;
        if (!addressable()) return Error::Unsupported;
        auto [slice, inner] = split(z);
        uint64_t base = uint64_t{slice} * slice_size + m.offset;
        if (desc.swizzle.value == Swizzle::LINEAR) {
            uint64_t index = uint64_t{y} * m.pitch + x;
            out = base + index * desc.element_bytes;
            return Error::Ok;
        }
        out = base + row(m, y, inner) + column(m, x);
        return Error::Ok;
    }

    // The part of an address that depends on the row (and depth in a thick block).
    uint64_t row(const Mip& m, uint32_t y, uint32_t z) const {
        uint32_t bw = block[0], bh = block[1];
        int log2 = std::countr_zero(alignment);
        // In the tail the pattern of the shifted position gives the mip's offset inside the tail block.
        y += m.tail ? m.tail->y : 0;
        uint64_t blocks = uint64_t{y / bh} * (m.pitch / bw);
        return (blocks << log2) + (pattern[1][y % bh] | pattern[2][z]);
    }

    // The part of an address that depends on the column.
    uint64_t column(const Mip& m, uint32_t x) const {
        uint32_t bw = block[0];
        x += m.tail ? m.tail->x : 0;
        return (uint64_t{x / bw} << std::countr_zero(alignment)) + pattern[0][x % bw];
    }

    // Depth slices a subresource of mip `mip` covers: the mip's depth for 3D textures, else 1 (one array slice).
    uint32_t subresource_depth(uint32_t mip) const { return mip < mips.size() ? mips[mip].depth : 0; }

    // Copies one subresource out of tiled memory (the whole layout, `size` bytes) into linear rows: mip `mip` of array
    // slice `slice` (3D: every depth slice of the mip, slice 0). Rows are row_pitch bytes apart, depth slices
    // depth_pitch.
    Error detile(const uint8_t* tiled, size_t tiled_len, uint32_t mip, uint32_t slice, uint8_t* linear,
                 size_t linear_len, size_t row_pitch, size_t depth_pitch) const {
        return walk(tiled_len, mip, slice, linear_len, row_pitch, depth_pitch,
                    [&](size_t t, size_t l, size_t len) { std::memcpy(linear + l, tiled + t, len); });
    }

    // The reverse of detile(): linear rows into tiled memory.
    Error tile(uint8_t* tiled, size_t tiled_len, uint32_t mip, uint32_t slice, const uint8_t* linear,
               size_t linear_len, size_t row_pitch, size_t depth_pitch) const {
        return walk(tiled_len, mip, slice, linear_len, row_pitch, depth_pitch,
                    [&](size_t t, size_t l, size_t len) { std::memcpy(tiled + t, linear + l, len); });
    }

    // Visits contiguous runs as (tiled byte offset, linear byte offset, length).
    template <class Copy>
    Error walk(size_t tiled_len, uint32_t mip, uint32_t slice, size_t linear_len, size_t row_pitch,
               size_t depth_pitch, Copy&& copy) const {
        using namespace detail;
        if (mip >= mips.size()) return Error::Invalid;
        const Mip& m = mips[mip];
        uint32_t depth = m.depth;
        uint32_t first;
        if (desc.dimension == Dimension::Tex3d) {
            if (slice != 0) return Error::Invalid;
            first = 0;
        } else {
            first = slice;
        }
        if (first >= slices) return Error::Invalid;
        if (!addressable()) return Error::Unsupported;
        size_t n = desc.element_bytes;
        size_t bytes = size_t{m.width} * n;
        size_t depth_offset, row_offset, need;
        if (!checked_mul(size_t{depth} - 1, depth_pitch, depth_offset) ||
            !checked_mul(size_t{m.height} - 1, row_pitch, row_offset) ||
            !checked_add(depth_offset, row_offset, need) || !checked_add(need, bytes, need))
            return Error::TooSmall;
        if (tiled_len < size || linear_len < need || row_pitch < bytes) return Error::TooSmall;
        bool is_linear = desc.swizzle.value == Swizzle::LINEAR;
        // The row and depth contribute one common base to every column, so the same contiguous runs repeat on every
        // row, including mip tails.
        struct Run {
            size_t tiled, linear, len;
        };
        std::vector<Run> runs;
        if (!is_linear) {
            size_t first_linear = 0;
            size_t first_tiled = static_cast<size_t>(column(m, 0));
            for (uint32_t x = 1; x < m.width; ++x) {
                size_t off = static_cast<size_t>(column(m, x));
                size_t at = size_t{x} * n;
                if (off != first_tiled + (at - first_linear)) {
                    runs.push_back({first_tiled, first_linear, at - first_linear});
                    first_tiled = off;
                    first_linear = at;
                }
            }
            runs.push_back({first_tiled, first_linear, bytes - first_linear});
        }
        for (uint32_t z = 0; z < depth; ++z) {
            auto [s, inner] = split(first + z);
            uint64_t base = uint64_t{s} * slice_size + m.offset;
            for (uint32_t y = 0; y < m.height; ++y) {
                size_t linear_row = size_t{z} * depth_pitch + size_t{y} * row_pitch;
                if (is_linear) {
                    uint64_t tiled_row = base + uint64_t{y} * m.pitch * n;
                    copy(static_cast<size_t>(tiled_row), linear_row, bytes);
                } else {
                    size_t tiled_row = static_cast<size_t>(base + row(m, y, inner));
                    for (const Run& r : runs) copy(tiled_row + r.tiled, linear_row + r.linear, r.len);
                }
            }
        }
        return Error::Ok;
    }
};

// What a DXGI format stores per element: bytes, texels per element side (4 for block compression), and whether its
// stencil is a second plane (the GPU keeps depth and stencil apart).
struct Format {
    uint32_t bytes = 0;
    uint32_t block = 1;
    bool stencil = false;
};

// By DXGI format number. nullopt for 96-bit, 1-bit, packed 4:2:2 and video formats.
//
// shortcut: those formats have no layout here; add them when a title uses one.
inline std::optional<Format> format(uint32_t dxgi) {
    auto r = [](uint32_t a, uint32_t b, uint32_t v) { return v >= a && v <= b; };
    if (r(1, 4, dxgi)) return Format{16, 1, false};
    if (r(9, 18, dxgi)) return Format{8, 1, false};
    if (r(19, 22, dxgi) || r(44, 47, dxgi)) return Format{4, 1, true};
    if (r(23, 43, dxgi) || dxgi == 67 || r(87, 93, dxgi) || dxgi == 116 || dxgi == 117) return Format{4, 1, false};
    if (r(48, 59, dxgi) || dxgi == 85 || dxgi == 86 || dxgi == 115) return Format{2, 1, false};
    if (r(118, 120, dxgi)) return Format{2, 1, true};
    if (r(60, 65, dxgi)) return Format{1, 1, false};
    if (r(70, 72, dxgi) || r(79, 81, dxgi)) return Format{8, 4, false};
    if (r(73, 78, dxgi) || r(82, 84, dxgi) || r(94, 99, dxgi)) return Format{16, 4, false};
    return std::nullopt;
}

// A texture as the console allocates it: a D3D12-style description with a swizzle mode.
struct TextureDesc {
    Dimension dimension = Dimension::Tex2d;
    uint32_t width = 0;  // in texels
    uint32_t height = 0;
    uint32_t depth_or_array = 0;  // depth of a 3D texture, else the array size
    uint32_t mips = 0;            // 0: the full chain
    uint32_t format = 0;          // DXGI format number
    uint32_t samples = 1;
    Swizzle swizzle{};
};

struct Plane {
    uint64_t offset = 0;  // from the start of the texture
    bool stencil = false;
    Layout layout;
};

// Every data plane of a texture (depth, then stencil), placed one after the other.
//
// shortcut: no compression metadata planes (HTILE, CMASK, FMASK, DCC); the console adds them to render targets and
// depth buffers unless the description denies them, so those are smaller here than on the console. The host GPU
// renders them, so nothing reads that memory; add the planes if a title sizes a pool from console sizes and runs out.
struct Texture {
    std::vector<Plane> planes;
    uint64_t size = 0;
    uint64_t alignment = 0;

    static Error make(const TextureDesc& desc, Texture& out) {
        auto f = format(desc.format);
        if (!f) return Error::Invalid;
        uint32_t depth3d = desc.dimension == Dimension::Tex3d ? desc.depth_or_array : 1;
        uint32_t mips = desc.mips;
        if (mips == 0) {
            uint32_t biggest = std::max({desc.width, desc.height, depth3d, 1u});
            mips = 32 - static_cast<uint32_t>(std::countl_zero(biggest));
        }
        auto plane = [&](uint32_t bytes) {
            Desc d;
            d.dimension = desc.dimension;
            d.width = desc.width;
            d.height = desc.height;
            d.depth = desc.depth_or_array;
            d.mips = mips;
            d.element_bytes = bytes;
            d.block = f->block;
            d.samples = std::max(desc.samples, 1u);
            d.swizzle = desc.swizzle;
            return d;
        };
        Texture t;
        Plane p0;
        if (Error e = Layout::make(plane(f->bytes), p0.layout); e != Error::Ok) return e;
        t.planes.push_back(std::move(p0));
        if (f->stencil) {
            Plane p1;
            if (Error e = Layout::make(plane(1), p1.layout); e != Error::Ok) return e;
            p1.offset = detail::next_multiple_of(t.planes[0].layout.size, p1.layout.alignment);
            p1.stencil = true;
            t.planes.push_back(std::move(p1));
        }
        const Plane& last = t.planes.back();
        t.size = last.offset + last.layout.size;
        t.alignment = 1;
        for (const Plane& p : t.planes) t.alignment = std::max(t.alignment, p.layout.alignment);
        out = std::move(t);
        return Error::Ok;
    }
};

}  // namespace leonida::xg
