#pragma once

// Where the title's memory lives in the GPU address arena (docs/d3d12_vulkan.md section 3.3).
//
// The arena stands in for the console's GPU address space, and the shader side computes an address as
// heap_base + VA. That works only while a VA is an offset inside the arena, and this title's are not: its
// reservations are tens of GiB apart (0x600000000 at a time, `title_address_range` keeping them below 2^40),
// so its VAs run from about 4 GiB to 55 GiB against an arena that cannot exceed 4 GiB.
//
// The fix is a rebase, not a wider arena: a VA inside a registered range becomes
// `va - base + arena_offset`, which is small and lands on the shadow copy the runtime keeps in the arena.
// A VA in no registered range passes through unchanged, so a run that registers nothing behaves exactly as
// it did before this existed.
//
// The title only touches part of what it reserves, and a 24 GiB reservation cannot be shadowed eagerly at
// all, so a range's shadow is allocated on first use: `arena_offset` means nothing until `backed` is set.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace leonida {

// One console VA range the title reserved, and the arena window that stands in for it.
struct TitleRange {
    std::uint64_t base = 0;          // first console VA of the range
    std::uint64_t size = 0;          // its length in bytes
    std::uint64_t arena_offset = 0;  // start of its shadow in the arena, once `backed`
    bool backed = false;
};

class RangeTable {
public:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    // Registers a range. Several ranges may be registered over time; they are kept sorted by base and may
    // not overlap or touch an end, so a VA belongs to at most one of them. An empty range or one that
    // overlaps is refused and leaves the table unchanged.
    bool add(std::uint64_t base, std::uint64_t size) {
        if (!size) return false;
        const std::uint64_t end = base + size;
        if (end < base) return false;  // the range wraps
        std::unique_lock lock(mutex_);
        const std::size_t at = lower_bound(base);
        if (at > 0 && ranges_[at - 1].base + ranges_[at - 1].size > base) return false;
        if (at < ranges_.size() && end > ranges_[at].base) return false;
        ranges_.insert(ranges_.begin() + static_cast<std::ptrdiff_t>(at), TitleRange{base, size, 0, false});
        return true;
    }

    // The index of the range holding `va`, or npos.
    std::size_t find(std::uint64_t va) const {
        std::shared_lock lock(mutex_);
        const std::size_t at = lower_bound(va);
        if (at == 0) return npos;
        const TitleRange& range = ranges_[at - 1];
        return va < range.base + range.size ? at - 1 : npos;
    }

    // A console VA as an arena offset: rebased inside a registered range whose shadow exists, unchanged
    // otherwise. A registered range with no shadow yet also passes through: the bytes are not in the arena,
    // and inventing an offset would point the shader at somebody else's data.
    std::uint64_t normalise(std::uint64_t va) const {
        std::shared_lock lock(mutex_);
        const std::size_t at = lower_bound(va);
        if (at == 0) return va;
        const TitleRange& range = ranges_[at - 1];
        if (va >= range.base + range.size || !range.backed) return va;
        return va - range.base + range.arena_offset;
    }

    // Records where the shadow of range `index` lives. The arena offset must be a multiple of nothing in
    // particular; the caller owns the allocation.
    bool set_arena_offset(std::size_t index, std::uint64_t arena_offset) {
        std::unique_lock lock(mutex_);
        if (index >= ranges_.size()) return false;
        ranges_[index].arena_offset = arena_offset;
        ranges_[index].backed = true;
        return true;
    }

    bool backed(std::size_t index) const {
        std::shared_lock lock(mutex_);
        return index < ranges_.size() && ranges_[index].backed;
    }

    std::uint64_t base(std::size_t index) const {
        std::shared_lock lock(mutex_);
        return index < ranges_.size() ? ranges_[index].base : 0;
    }

    std::uint64_t size(std::size_t index) const {
        std::shared_lock lock(mutex_);
        return index < ranges_.size() ? ranges_[index].size : 0;
    }

    std::size_t count() const {
        std::shared_lock lock(mutex_);
        return ranges_.size();
    }

private:
    // Index of the first range whose base is above `va`; the one that may hold `va` is the entry before it.
    std::size_t lower_bound(std::uint64_t va) const {
        std::size_t lo = 0, hi = ranges_.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (ranges_[mid].base <= va) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    mutable std::shared_mutex mutex_;
    std::vector<TitleRange> ranges_;  // sorted by base, non-overlapping
};

}  // namespace leonida
