// SPDX-License-Identifier: GPL-2.0-or-later
/* Linux: guest memory shared with the GPU API (ps_host_gpu_memory.h): exported dma-buf descriptors mapped with
 * mmap and closed, ranges of the memfd's views imported in place. */
#define _GNU_SOURCE
#include "ps_host.h"
#include "common/host_gpu_memory_native.h"
#include <errno.h>
#include <limits.h>

unsigned ps_host_gpu_memory_kinds(void) { return PS_HOST_GPU_SHARE_EXPORT_FD | PS_HOST_GPU_SHARE_IMPORT_RANGE; }

const char *ps_host_gpu_memory_range_name(void) { return "memfd mapping"; }

void *host_gpu_memory_map_native(uint64_t native, void *p_at, size_t cb_range, unsigned access, unsigned flags,
                                 uint64_t off_share) {
    if (native > INT_MAX) {
        errno = EBADF;
        return NULL;
    }
    return ps_host_vm_map_shared(p_at, cb_range, access, flags, (int)native, off_share);
}

int host_gpu_memory_close_native(uint64_t native) { return native > INT_MAX ? EBADF : ps_host_fs_close((int)native); }
