// SPDX-License-Identifier: GPL-2.0-or-later
// psport: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run in the
// host's idle class: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <thread>

#include "../../src/platform/ps_host.h"

namespace PsThreads {

/// Hardware threads available to the process: affinity mask capped by the cgroup CPU quota.
/// Read once, since callers poll it from hot paths and the limits do not change during a run.
inline unsigned Available() {
    static const unsigned c_cpus = [] {
        if (const unsigned c_host = ps_host_thread_cpu_count()) {
            return c_host;
        }
        return std::max(1u, std::thread::hardware_concurrency());
    }();
    return c_cpus;
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
    ps_host_thread_make_background();
}

} // namespace PsThreads
