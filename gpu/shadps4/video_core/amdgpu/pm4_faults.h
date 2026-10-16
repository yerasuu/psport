// SPDX-License-Identifier: GPL-2.0-or-later
// psport: PM4 packets Liverpool cannot run as written, where shadPS4 asserted (a game's corrupted
// buffer, or a field it never met). A malformed or unknown packet is skipped by its declared size;
// one whose size cannot be trusted ends its buffer; an unsupported field of a known packet takes
// the nearest safe behaviour. Each is counted by opcode, logged the first time and whenever its
// count doubles, and summed up in the exit report.
#pragma once

#include "common/types.h"

namespace AmdGpu::Pm4Faults {

enum class Kind : u8 {
    Skipped,     ///< the packet was not run; decoding went on after it
    Ended,       ///< the rest of its buffer was not run
    Unsupported, ///< run with a fallback for one of its fields
};

/// `opcode`: the header's opcode bits, also for a packet that is not type 3. True when this
/// occurrence was logged, so the caller may add detail (ReportBadPacket's dump) only then.
bool Note(Kind kind, u32 opcode, const char* sz_format, ...) __attribute__((format(printf, 3, 4)));

} // namespace AmdGpu::Pm4Faults
