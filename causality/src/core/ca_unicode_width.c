// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* ca_unicode_width.c — monospace cell width of Unicode codepoints. */

#include "causality.h"
#include "ca_unicode_width_table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Binary-search a sorted, non-overlapping inclusive range table.
 *
 * ranges  Table to search.
 * count   Number of entries in ranges.
 * cp      Codepoint to look up.
 * Returns true when cp lies inside one of the ranges.
 */
static bool ca_unicode_in_ranges(const Ca_UnicodeRange *ranges, size_t count,
                                 uint32_t cp)
{
    if (count == 0u || cp < ranges[0].first || cp > ranges[count - 1u].last)
        return false;
    size_t lo = 0u;
    size_t hi = count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2u;
        if (cp < ranges[mid].first)     hi = mid;
        else if (cp > ranges[mid].last) lo = mid + 1u;
        else                            return true;
    }
    return false;
}

CA_API int ca_codepoint_cell_width(uint32_t cp)
{
    if (cp >= 0x20u && cp < 0x7Fu) return 1;
    if (cp > 0x10FFFFu) return 1;
    if (ca_unicode_in_ranges(ca_unicode_zero_width,
                             sizeof(ca_unicode_zero_width) / sizeof(ca_unicode_zero_width[0]),
                             cp))
        return 0;
    if (ca_unicode_in_ranges(ca_unicode_double_width,
                             sizeof(ca_unicode_double_width) / sizeof(ca_unicode_double_width[0]),
                             cp))
        return 2;
    return 1;
}
