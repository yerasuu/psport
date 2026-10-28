// SPDX-License-Identifier: GPL-2.0-or-later
/* What common/host_gpu_memory.c asks of src/platform/<os>/host_gpu_memory.c: the native half of an exported
 * share, which only the OS knows how to view and close. Reached only for the export kinds
 * ps_host_gpu_memory_kinds names. */
#ifndef HOST_GPU_MEMORY_NATIVE_H
#define HOST_GPU_MEMORY_NATIVE_H
#include <stddef.h>
#include <stdint.h>

/* As ps_host_gpu_memory_map: the view's address, or NULL with the host's errno in errno. */
void *host_gpu_memory_map_native(uint64_t native, void *p_at, size_t cb_range, unsigned access, unsigned flags,
                                 uint64_t off_share);
/* 0, or the host's errno. */
int host_gpu_memory_close_native(uint64_t native);

#endif
