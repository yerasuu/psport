// SPDX-License-Identifier: GPL-2.0-or-later
/* Guest memory shared with the GPU API (ps_host_gpu_memory.h): shares, ranges and their counts, one
 * definition for every OS; src/platform/<os>/host_gpu_memory.c maps and closes what the API exports. */
#define _GNU_SOURCE
#include "ps_host.h"
#include "host_gpu_memory_native.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct PsHostGpuShare {
    unsigned kind;
    uint64_t native, cb_range;
    void *p_range;
};
static PsHostGpuMemoryStats s_stats;

static PsHostGpuShareCounts *counts(unsigned kind) {
    return kind == PS_HOST_GPU_SHARE_IMPORT_RANGE ? &s_stats.import_range : &s_stats.export_fd;
}

static int failed(unsigned kind, const char *sz_operation, int error) {
    PsHostGpuShareCounts *p_counts = counts(kind);
    const uint64_t c_failures = __atomic_add_fetch(&p_counts->c_failures, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&p_counts->last_error, error, __ATOMIC_RELAXED);
    if (!(c_failures & (c_failures - 1)))
        fprintf(stderr, "Host GPU memory: %s an %s share failed: %s (%" PRIu64 " so far)\n", sz_operation,
                kind == PS_HOST_GPU_SHARE_IMPORT_RANGE ? "import_range" : "export_fd", strerror(error), c_failures);
    return error;
}

static void made(PsHostGpuShare *p_share, PsHostGpuShare **pp_out_share) {
    PsHostGpuShareCounts *p_counts = counts(p_share->kind);
    __atomic_add_fetch(&p_counts->c_shares, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&p_counts->cb_shared, p_share->cb_range, __ATOMIC_RELAXED);
    *pp_out_share = p_share;
}

size_t ps_host_gpu_memory_granularity(void) { return ps_host_vm_page_size(); }

int ps_host_gpu_memory_adopt(unsigned kind, uint64_t native, uint64_t cb_range, PsHostGpuShare **pp_out_share) {
    if (kind != PS_HOST_GPU_SHARE_EXPORT_FD || !(ps_host_gpu_memory_kinds() & kind))
        return failed(PS_HOST_GPU_SHARE_EXPORT_FD, "adopting", EOPNOTSUPP);
    PsHostGpuShare *p_share = cb_range ? malloc(sizeof(*p_share)) : NULL;
    if (!p_share) {
        host_gpu_memory_close_native(native);
        return failed(kind, "adopting", cb_range ? ENOMEM : EINVAL);
    }
    *p_share = (PsHostGpuShare){.kind = kind, .native = native, .cb_range = cb_range};
    made(p_share, pp_out_share);
    return 0;
}

int ps_host_gpu_memory_describe_range(void *p_range, size_t cb_range, PsHostGpuShare **pp_out_share) {
    const unsigned kind = PS_HOST_GPU_SHARE_IMPORT_RANGE;
    if (!(ps_host_gpu_memory_kinds() & kind)) return failed(kind, "describing", EOPNOTSUPP);
    const size_t cb_granule = ps_host_gpu_memory_granularity();
    if (!p_range || !cb_range || (uintptr_t)p_range % cb_granule || cb_range % cb_granule)
        return failed(kind, "describing", EINVAL);
    PsHostGpuShare *p_share = malloc(sizeof(*p_share));
    if (!p_share) return failed(kind, "describing", ENOMEM);
    *p_share = (PsHostGpuShare){.kind = kind, .native = UINT64_MAX, .cb_range = cb_range, .p_range = p_range};
    made(p_share, pp_out_share);
    return 0;
}

uint64_t ps_host_gpu_memory_native(const PsHostGpuShare *p_share) { return p_share->native; }

void *ps_host_gpu_memory_pointer(const PsHostGpuShare *p_share) { return p_share->p_range; }

void *ps_host_gpu_memory_map(PsHostGpuShare *p_share, void *p_at, size_t cb_range, unsigned access,
                             unsigned flags, uint64_t off_share) {
    int error = EOPNOTSUPP;
    if (p_share->kind == PS_HOST_GPU_SHARE_EXPORT_FD) {
        void *p_view = host_gpu_memory_map_native(p_share->native, p_at, cb_range, access, flags, off_share);
        if (p_view) return p_view;
        error = errno;
    }
    failed(p_share->kind, "mapping", error);
    errno = error;
    return NULL;
}

int ps_host_gpu_memory_release(PsHostGpuShare *p_share) {
    if (!p_share) return 0;
    const unsigned kind = p_share->kind;
    const int error = kind == PS_HOST_GPU_SHARE_EXPORT_FD ? host_gpu_memory_close_native(p_share->native) : 0;
    free(p_share);
    return error ? failed(kind, "releasing", error) : 0;
}

void ps_host_gpu_memory_stats(PsHostGpuMemoryStats *p_out_stats) {
    PsHostGpuShareCounts *const p_counts[] = {&s_stats.export_fd, &s_stats.import_range};
    PsHostGpuShareCounts *const p_out[] = {&p_out_stats->export_fd, &p_out_stats->import_range};
    for (size_t i = 0; i < sizeof(p_counts) / sizeof(*p_counts); ++i)
        *p_out[i] = (PsHostGpuShareCounts){
            .c_shares = __atomic_load_n(&p_counts[i]->c_shares, __ATOMIC_RELAXED),
            .cb_shared = __atomic_load_n(&p_counts[i]->cb_shared, __ATOMIC_RELAXED),
            .c_failures = __atomic_load_n(&p_counts[i]->c_failures, __ATOMIC_RELAXED),
            .last_error = __atomic_load_n(&p_counts[i]->last_error, __ATOMIC_RELAXED),
        };
}
