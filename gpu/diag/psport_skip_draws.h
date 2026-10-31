// SPDX-License-Identifier: GPL-2.0-or-later
// psport: PS_SKIP_DRAWS=<vs>:<ps>[#<n>][,...] — graphics draws whose vertex and pixel shader hashes
// match an entry are not recorded, to find which draw paints something by making it disappear. A
// hash is hexadecimal (0x and leading zeros optional, the 16-digit form the frame capture prints)
// or `*` for any; `#<n>` (decimal) keeps only the n-th draw (from 0) matching that entry within each
// frame. Frames start at the first draw into a display buffer after draws into other targets, as
// the frame capture's do. Entries are parsed once; a malformed one is logged and ignored alone. The
// first skip of each pair is logged, and the totals per pair and per entry follow the call report.
// Off, each hook is one load of a constant flag.
#pragma once

#include <cstdint>
#include <cstdlib>

namespace PsSkipDraws {

inline const bool g_is_enabled = [] {
    const char* sz_env = std::getenv("PS_SKIP_DRAWS");
    return sz_env && sz_env[0];
}();

struct Verdict {
    bool is_skipped;
    /// The draw's index among this frame's draws of its (vs, ps) pair: what `#<n>` names for an
    /// entry without `*`.
    std::uint32_t i_pair_draw;
};

/// The recording thread, for every graphics draw about to record its draw command, while
/// PS_SKIP_DRAWS is set or the frame capture looks at draws (it prints `i_pair_draw`).
Verdict Check(std::uint64_t vs_hash, std::uint64_t ps_hash, bool is_display_target);

} // namespace PsSkipDraws
