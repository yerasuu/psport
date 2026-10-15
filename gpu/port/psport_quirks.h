// psport: the quirk registry (src/runtime/runtime_quirks.h, docs/en/QUIRKS.md) for the GPU library.
#pragma once
#include "../../src/runtime/runtime_quirks.h"

namespace PsQuirk {
/// Read at setup (device or pipeline creation, configuration), never per draw: the bitset is
/// final once the Vulkan device is known.
inline bool Active(RuntimeQuirkId id) {
    return runtime_quirk_active(id) != 0;
}
} // namespace PsQuirk
