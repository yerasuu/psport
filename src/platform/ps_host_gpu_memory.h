/* Guest memory shared with the GPU API (decision #1009, phase 2c, #1017): only how the host makes a range
 * of the guest's direct memory and a GPU allocation alias. Which memory type and placement the GPU library
 * picks, and whether the device is unified memory, come from the API's own properties, the same on every OS,
 * and stay in gpu/memory/.
 *
 * A share is either an allocation the API exported, which the host maps at the guest's addresses
 * (EXPORT_FD), or a range of the host's own view of guest memory, which the API imports in place
 * (IMPORT_RANGE). The GPU library adopts or describes shares; the runtime maps and releases the ones it is
 * handed. ps_host_gpu_memory_kinds says what this OS can do, not what a driver does: the probes of
 * gpu/memory/ answer that. Errors are the host's errno values (EOPNOTSUPP: no such kind on this host); every
 * failure is counted per kind (ps_host_gpu_memory_stats) and logged at its 1st, 2nd, 4th, 8th... time. */
#ifndef PS_HOST_GPU_MEMORY_H
#define PS_HOST_GPU_MEMORY_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsHostGpuShare PsHostGpuShare;
enum {
    /* The API exports a file descriptor the host maps (a dma-buf). */
    PS_HOST_GPU_SHARE_EXPORT_FD = 1,
    /* The API imports a host pointer into a view of guest memory the host owns. */
    PS_HOST_GPU_SHARE_IMPORT_RANGE = 2,
};

/* The kinds this host can share, a bit set of PS_HOST_GPU_SHARE_*. */
unsigned ps_host_gpu_memory_kinds(void);
/* What the host's own view of guest memory, which the GPU imports in place, is called on this host, for
 * logs ("memfd mapping"). */
const char *ps_host_gpu_memory_range_name(void);
/* The least alignment of a shared range's address and size on this host; the API may ask for more
 * (VK_EXT_external_memory_host's minImportedHostPointerAlignment). */
size_t ps_host_gpu_memory_granularity(void);
/* Takes ownership of native, which the API exported as cb_range bytes of the export kind: release closes it,
 * and so does this call when it fails on a kind the host has. 0 with the share in *pp_out_share, or the
 * host's errno. */
int ps_host_gpu_memory_adopt(unsigned kind, uint64_t native, uint64_t cb_range, PsHostGpuShare **pp_out_share);
/* An IMPORT_RANGE share of [p_range, p_range + cb_range), which must lie in a view the caller keeps mapped
 * while the API uses it: 0 with the share in *pp_out_share, or EINVAL when the range is empty or not
 * aligned to ps_host_gpu_memory_granularity. */
int ps_host_gpu_memory_describe_range(void *p_range, size_t cb_range, PsHostGpuShare **pp_out_share);
/* The descriptor an export share holds, for the API's own calls; UINT64_MAX for a range. */
uint64_t ps_host_gpu_memory_native(const PsHostGpuShare *p_share);
/* Where an IMPORT_RANGE share starts, the pointer the API imports; NULL for an export. */
void *ps_host_gpu_memory_pointer(const PsHostGpuShare *p_share);
/* A view of an export share from off_share, placed and protected as ps_host_vm_map_shared does (its
 * PS_HOST_VM_* access and flags): its address, or NULL with the host's errno in errno (EOPNOTSUPP for a
 * range, whose bytes are already where the guest sees them). */
void *ps_host_gpu_memory_map(PsHostGpuShare *p_share, void *p_at, size_t cb_range, unsigned access,
                             unsigned flags, uint64_t off_share);
/* Closes what adopt took and frees the share (NULL: nothing); views mapped from it stay. 0, or the host's
 * errno (the share is freed anyway). */
int ps_host_gpu_memory_release(PsHostGpuShare *p_share);

typedef struct {
    /* Shares adopted or described, and their bytes. */
    uint64_t c_shares, cb_shared;
    /* Failed calls of any kind on these shares, and the errno of the last one. */
    uint64_t c_failures;
    int last_error;
} PsHostGpuShareCounts;
typedef struct {
    PsHostGpuShareCounts export_fd, import_range;
} PsHostGpuMemoryStats;
/* The counts since launch, for the exit summary. */
void ps_host_gpu_memory_stats(PsHostGpuMemoryStats *p_out_stats);

#ifdef __cplusplus
}
#endif
#endif
