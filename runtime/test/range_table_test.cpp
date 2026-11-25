// The title's VA ranges and their arena shadows (src/range_table.h) without a GPU: pass-through for an
// unregistered VA, rebasing inside a registered range, no aliasing between two ranges, refusal of overlaps,
// and the lazy shadow. No game data.
#include "../src/range_table.h"

#include <cstdio>

#define CHECK(condition) do {                                        \
    if (!(condition)) {                                              \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1;                                                    \
    }                                                                \
} while (0)

using namespace leonida;

namespace {

constexpr std::uint64_t GiB = 1024ull * 1024 * 1024;

// Nothing registered: every VA is its own offset, which is the behaviour before the table existed.
int empty_table_passes_through() {
    const RangeTable table;
    CHECK(table.count() == 0);
    CHECK(table.normalise(0) == 0);
    CHECK(table.normalise(0x100200000) == 0x100200000);
    CHECK(table.normalise(0xDC9601B00) == 0xDC9601B00);
    CHECK(table.find(0x100200000) == RangeTable::npos);
    return 0;
}

// A registered range with a shadow rebases to the shadow; without one it passes through, because the bytes
// are not in the arena and a made-up offset would read somebody else's.
int registered_range_rebases() {
    RangeTable table;
    CHECK(table.add(0x100200000, 0x23400000));
    CHECK(table.count() == 1);
    CHECK(table.find(0x100200000) == 0);
    CHECK(table.find(0x100200000 + 0x233FFFFF) == 0);  // last byte is inside
    CHECK(table.find(0x100200000 + 0x23400000) == RangeTable::npos);
    CHECK(table.find(0xFFFFFFFF) == RangeTable::npos);

    // Lazy: no shadow yet, so the VA is unchanged.
    CHECK(table.normalise(0x100200000) == 0x100200000);
    CHECK(!table.backed(0));

    CHECK(table.set_arena_offset(0, 0x00400000));
    CHECK(table.backed(0));
    CHECK(table.normalise(0x100200000) == 0x00400000);
    CHECK(table.normalise(0x100200000 + 0x10) == 0x00400010);
    // Outside the range still passes through.
    CHECK(table.normalise(0x200000000) == 0x200000000);
    return 0;
}

// Two ranges at different bases must not alias: the same in-range offset in each lands on its own shadow.
int two_ranges_do_not_alias() {
    RangeTable table;
    CHECK(table.add(0x100200000, 0x1000));
    CHECK(table.add(0xDC9601B00, 0x1000));
    CHECK(table.count() == 2);
    CHECK(table.find(0x100200000) == 0);
    CHECK(table.find(0xDC9601B00) == 1);
    CHECK(table.set_arena_offset(0, 0x1000));
    CHECK(table.set_arena_offset(1, 0x2000));
    CHECK(table.normalise(0x100200000) == 0x1000);
    CHECK(table.normalise(0xDC9601B00) == 0x2000);
    CHECK(table.normalise(0x100200000 + 0x800) == 0x1800);
    CHECK(table.normalise(0xDC9601B00 + 0x800) == 0x2800);
    return 0;
}

// Ranges are kept sorted by base whatever order they are added in, and an overlap is refused without
// disturbing the table.
int overlaps_are_refused() {
    RangeTable table;
    CHECK(table.add(0x2000, 0x1000));
    CHECK(table.add(0x1000, 0x1000));  // out of order, but adjacent: allowed
    CHECK(table.count() == 2);
    CHECK(table.base(0) == 0x1000);
    CHECK(table.base(1) == 0x2000);

    CHECK(!table.add(0x1800, 0x100));   // overlaps both
    CHECK(!table.add(0x2000, 0x100));   // same base
    CHECK(!table.add(0x0FFF, 0x2));     // overlaps the start of the first
    CHECK(!table.add(0x3000, 0x0));     // empty
    CHECK(!table.add(0xFFFFFFFFFFFFFFFFull, 0x10));  // wraps
    CHECK(table.count() == 2);          // unchanged by every refusal
    return 0;
}

// A shadow may be registered for any index in range, and an out-of-range one is refused rather than
// corrupting the table.
int set_arena_offset_bounds() {
    RangeTable table;
    CHECK(table.add(0x1000, 0x1000));
    CHECK(!table.set_arena_offset(1, 0x8000));
    CHECK(!table.set_arena_offset(RangeTable::npos, 0x8000));
    CHECK(!table.backed(0));
    CHECK(table.set_arena_offset(0, 0));
    CHECK(table.backed(0));
    CHECK(table.normalise(0x1000) == 0);  // a shadow at offset 0 is legitimate
    return 0;
}

}  // namespace

int main() {
    if (const int rc = empty_table_passes_through()) return rc;
    if (const int rc = registered_range_rebases()) return rc;
    if (const int rc = two_ranges_do_not_alias()) return rc;
    if (const int rc = overlaps_are_refused()) return rc;
    if (const int rc = set_arena_offset_bounds()) return rc;
    std::printf("range_table: all checks passed\n");
    return 0;
}
