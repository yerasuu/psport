// SPDX-License-Identifier: GPL-2.0-or-later
// psport: PS_SKIP_DRAWS=<vs>:<ps>[@<target>][#<n>][,...] — graphics draws whose vertex and pixel
// shader hashes match an entry are not recorded, to find which draw paints something by making it
// disappear. A hash is hexadecimal (0x and leading zeros optional, the 16-digit form the frame
// capture prints) or `*` for any. `@<target>` keeps only draws whose colour attachment 0 is
// `<W>x<H>` (decimal size) or `0x<address>` (its guest address), as the capture's draw lines print
// them; a draw without colour attachment 0 never matches it. `#<n>` (decimal) keeps only the n-th
// draw (from 0) matching the rest of that entry within each frame. Frames start at the first draw
// into a display buffer after draws into other targets, as the frame capture's do. Entries are
// parsed once; a malformed one is logged and ignored alone. The first skip of each entry is logged,
// and the totals per pair and per entry follow the call report. Off, each hook is one load of a
// constant flag.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace PsSkipDraws {

inline const bool g_is_enabled = [] {
    const char* sz_env = std::getenv("PS_SKIP_DRAWS");
    return sz_env && sz_env[0];
}();

/// A draw's colour attachment 0; `va_target` is 0 when it has none.
struct Target {
    std::uint64_t va_target;
    std::uint32_t width;
    std::uint32_t height;
};

struct Verdict {
    bool is_skipped;
    /// The draw's index among this frame's draws of its (vs, ps) pair: what `#<n>` names for an
    /// entry without `*` or `@`.
    std::uint32_t i_pair_draw;
};

/// One entry of the grammar above; the frame capture's PS_CAPTURE_DUMP_TEX takes one too.
struct Selector {
    std::uint64_t vs_hash;
    std::uint64_t ps_hash;
    bool is_any_vs;
    bool is_any_ps;
    bool has_target;
    Target target; ///< by address when `va_target` is not 0, else by size
    bool has_nth;
    std::uint32_t i_nth;
    std::string text; ///< the entry as parsed, for logs
};

/// Null on success, else why `entry` was rejected.
const char* ParseSelector(std::string_view entry, Selector& out_selector);

/// Whether the draw matches the selector's pair and target; `#<n>` is counted by the caller, among
/// the frame's draws for which this holds.
bool IsMatch(const Selector& selector, std::uint64_t vs_hash, std::uint64_t ps_hash, const Target& color0);

/// The recording thread, for every graphics draw about to record its draw command, while
/// PS_SKIP_DRAWS is set or the frame capture looks at draws (it prints `i_pair_draw`).
Verdict Check(std::uint64_t vs_hash, std::uint64_t ps_hash, const Target& color0, bool is_display_target);

} // namespace PsSkipDraws
