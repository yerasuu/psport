// SPDX-License-Identifier: GPL-2.0-or-later
// psport: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run in the
// host's idle class: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <thread>

#include "../../src/platform/ps_host.h"

namespace PsThreads {

/// Hardware threads available to the process.
inline unsigned Available() {
    if (const unsigned c_cpus = ps_host_thread_cpu_count()) {
        return c_cpus;
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
    ps_host_thread_make_background();
}

} // namespace PsThreads
