// SPDX-License-Identifier: GPL-2.0-or-later
/* Windows: guest memory shared with the GPU API (ps_host_gpu_memory.h): ranges of the direct memory section's
 * views imported in place. No exported allocation is taken (adopt answers EOPNOTSUPP), since whether a
 * Win32 handle the API exports maps on the CPU is not measured on a Windows driver yet. */
#include "ps_host.h"
#include "common/host_gpu_memory_native.h"
#include <errno.h>

unsigned ps_host_gpu_memory_kinds(void) { return PS_HOST_GPU_SHARE_IMPORT_RANGE; }

const char *ps_host_gpu_memory_range_name(void) { return "view of a section"; }

void *host_gpu_memory_map_native(uint64_t native, void *p_at, size_t cb_range, unsigned access, unsigned flags,
                                 uint64_t off_share) {
    (void)native, (void)p_at, (void)cb_range, (void)access, (void)flags, (void)off_share;
    errno = EOPNOTSUPP;
    return NULL;
}

int host_gpu_memory_close_native(uint64_t native) {
    (void)native;
    return EOPNOTSUPP;
}
