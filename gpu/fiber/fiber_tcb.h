// SPDX-License-Identifier: GPL-2.0-or-later
// shadPS4's core/tls.h as far as fiber.cpp uses it. bbport keeps the game's TCB in GS (link_modules
// rewrites its fs: loads), laid out as the console's; +0x30 is free for the fiber context.
#pragma once
#include <cstddef>

#include "common/types.h"

namespace Libraries::Fiber {
struct OrbisFiberContext;
}

namespace Core {

struct Tcb {
    Tcb* tcb_self;
    void* tcb_dtv;
    void* tcb_thread;
    void* tcb_spare[2];
    u64 tcb_canary;
    ::Libraries::Fiber::OrbisFiberContext* tcb_fiber;
};
static_assert(offsetof(Tcb, tcb_fiber) == 0x30);

inline Tcb* GetTcbBase() {
    Tcb* tcb;
    asm volatile("mov %%gs:0, %0" : "=r"(tcb));
    return tcb;
}

} // namespace Core
