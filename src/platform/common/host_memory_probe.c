// SPDX-License-Identifier: GPL-2.0-or-later
/* The host memory state's uses, one definition for every OS (ps_host.h): the guest's budget at
 * launch and the pressure level; src/platform/<os>/host_memory_probe.c reads the host. */
#define _GNU_SOURCE
#include "ps_host.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define MIB(cb) ((cb) >> 20)

uint64_t ps_host_memory_headroom(const PsHostMemory *p_memory) {
    const uint64_t cb_left =
        p_memory->cb_limit > p_memory->cb_limit_used ? p_memory->cb_limit - p_memory->cb_limit_used : 0;
    return cb_left < p_memory->cb_available ? cb_left : p_memory->cb_available;
}

static void describe(const PsHostMemory *p_memory, char *sz_out, size_t cb_out) {
    char sz_limit[256], sz_pressure[32];
    if (p_memory->cb_limit == UINT64_MAX)
        snprintf(sz_limit, sizeof sz_limit, "none");
    else
        snprintf(sz_limit, sizeof sz_limit, "%" PRIu64 " MiB, %" PRIu64 " MiB used (%s)", MIB(p_memory->cb_limit),
                 MIB(p_memory->cb_limit_used), p_memory->sz_limit_source);
    if (p_memory->pressure_permyriad < 0)
        snprintf(sz_pressure, sizeof sz_pressure, "none");
    else
        snprintf(sz_pressure, sizeof sz_pressure, "%d.%02d%%", p_memory->pressure_permyriad / 100,
                 p_memory->pressure_permyriad % 100);
    snprintf(sz_out, cb_out, "total %" PRIu64 " MiB, available %" PRIu64 " MiB, limit %s, PSI some avg10 %s",
             MIB(p_memory->cb_total), MIB(p_memory->cb_available), sz_limit, sz_pressure);
}

uint64_t ps_host_memory_guest_budget(void) {
    static uint64_t s_cb_budget;
    static int s_is_read;
    if (__atomic_load_n(&s_is_read, __ATOMIC_ACQUIRE)) return s_cb_budget;
    PsHostMemory memory;
    const int error = ps_host_memory_read(&memory);
    uint64_t cb_budget = UINT64_MAX;
    if (error) {
        fprintf(stderr, "WARNING: Host memory: unreadable (%s): the guest's memory is not checked against the host\n",
                strerror(error));
    } else {
        const uint64_t cb_headroom = ps_host_memory_headroom(&memory);
        cb_budget = cb_headroom / 8 * 7;
        char sz_state[512];
        describe(&memory, sz_state, sizeof sz_state);
        printf("Host memory: %s; headroom %" PRIu64 " MiB, guest budget %" PRIu64 " MiB (7/8 of it)\n", sz_state,
               MIB(cb_headroom), MIB(cb_budget));
    }
    /* Launch decisions run on one thread before the game's; a second reader would only log twice. */
    s_cb_budget = cb_budget;
    __atomic_store_n(&s_is_read, 1, __ATOMIC_RELEASE);
    return cb_budget;
}

static const char *const s_sz_levels[] = {"none", "moderate", "high", "critical"};

static PsHostMemoryPressure level_of(const PsHostMemory *p_memory) {
    const uint64_t cb_reference = p_memory->cb_limit != UINT64_MAX ? p_memory->cb_limit : p_memory->cb_total;
    const uint64_t cb_headroom = ps_host_memory_headroom(p_memory);
    const int32_t psi = p_memory->pressure_permyriad;
    if (cb_headroom < cb_reference / 20 || psi >= 1000) return PS_HOST_MEMORY_PRESSURE_CRITICAL;
    if (cb_headroom < cb_reference / 10 || psi >= 300) return PS_HOST_MEMORY_PRESSURE_HIGH;
    if (cb_headroom < cb_reference / 5 || psi >= 50) return PS_HOST_MEMORY_PRESSURE_MODERATE;
    return PS_HOST_MEMORY_PRESSURE_NONE;
}

PsHostMemoryPressure ps_host_memory_pressure(void) {
    static PsHostMemoryPressure s_level = PS_HOST_MEMORY_PRESSURE_NONE;
    static uint64_t s_c_entered[4], s_c_failures;
    PsHostMemory memory;
    const int error = ps_host_memory_read(&memory);
    if (error) {
        const uint64_t c_failures = __atomic_add_fetch(&s_c_failures, 1, __ATOMIC_RELAXED);
        if (!(c_failures & (c_failures - 1)))
            fprintf(stderr, "Host memory: pressure unreadable (%s), taken as none (%" PRIu64 " so far)\n",
                    strerror(error), c_failures);
        return PS_HOST_MEMORY_PRESSURE_NONE;
    }
    const PsHostMemoryPressure level = level_of(&memory);
    const PsHostMemoryPressure previous = __atomic_exchange_n(&s_level, level, __ATOMIC_ACQ_REL);
    if (level != previous) {
        const uint64_t c_entered = __atomic_add_fetch(&s_c_entered[level], 1, __ATOMIC_RELAXED);
        char sz_state[512];
        describe(&memory, sz_state, sizeof sz_state);
        printf("Host memory: pressure %s -> %s (%s entered %" PRIu64 " times): %s; headroom %" PRIu64 " MiB\n",
               s_sz_levels[previous], s_sz_levels[level], s_sz_levels[level], c_entered, sz_state,
               MIB(ps_host_memory_headroom(&memory)));
    }
    return level;
}
