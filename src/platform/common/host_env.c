// SPDX-License-Identifier: GPL-2.0-or-later
/* The environment's reader and the wipe of secrets: the same on every OS (ps_host.h); the setter
 * and the scrub differ (host_system.c). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <stdlib.h>

const char *ps_host_env_get(const char *sz_name) { return getenv(sz_name); }

void ps_host_wipe(void *p_bytes, size_t cb_bytes) {
    volatile unsigned char *p_byte = p_bytes;
    while (cb_bytes--) *p_byte++ = 0;
    __asm__ volatile("" : : "r"(p_bytes) : "memory");
}
