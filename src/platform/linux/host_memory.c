/* Linux: mappings, sections and the process's list of mappings (ps_host.h). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

_Static_assert(PS_HOST_VM_READ == PROT_READ && PS_HOST_VM_WRITE == PROT_WRITE && PS_HOST_VM_EXECUTE == PROT_EXEC,
               "access bits are passed to mmap and mprotect as they are");

size_t ps_host_vm_page_size(void) { return (size_t)sysconf(_SC_PAGESIZE); }

static int placement(unsigned flags) {
    return (flags & PS_HOST_VM_REPLACE ? MAP_FIXED : 0) | (flags & PS_HOST_VM_EXACT ? MAP_FIXED_NOREPLACE : 0) |
           (flags & PS_HOST_VM_SPARSE ? MAP_NORESERVE : 0);
}

static void *placed(void *p_mapping, void *p_at, size_t cb_range, unsigned flags) {
    if (p_mapping == MAP_FAILED) return NULL;
    /* A kernel older than MAP_FIXED_NOREPLACE (4.17) takes the address as a hint only. */
    if ((flags & PS_HOST_VM_EXACT) && p_mapping != p_at) {
        munmap(p_mapping, cb_range);
        errno = EEXIST;
        return NULL;
    }
    return p_mapping;
}

void *ps_host_vm_map(void *p_at, size_t cb_range, unsigned access, unsigned flags) {
    return placed(mmap(p_at, cb_range, (int)access, MAP_PRIVATE | MAP_ANONYMOUS | placement(flags), -1, 0), p_at,
                  cb_range, flags);
}

void *ps_host_vm_map_shared(void *p_at, size_t cb_range, unsigned access, unsigned flags, int h_file,
                            uint64_t off_file) {
    return placed(mmap(p_at, cb_range, (int)access, MAP_SHARED | placement(flags), h_file, (off_t)off_file), p_at,
                  cb_range, flags);
}

int ps_host_vm_unmap(void *p_range, size_t cb_range) { return munmap(p_range, cb_range) ? errno : 0; }

int ps_host_vm_protect(void *p_range, size_t cb_range, unsigned access) {
    return mprotect(p_range, cb_range, (int)access) ? errno : 0;
}

int ps_host_vm_flush(void *p_range, size_t cb_range, unsigned flags) {
    const int host_flags = (flags & PS_HOST_VM_FLUSH_ASYNC ? MS_ASYNC : MS_SYNC) |
                           (flags & PS_HOST_VM_FLUSH_INVALIDATE ? MS_INVALIDATE : 0);
    return msync(p_range, cb_range, host_flags) ? errno : 0;
}

int ps_host_vm_section_create(const char *sz_name, uint64_t cb_section, int *p_out_h_section) {
    const int h_section = memfd_create(sz_name, MFD_CLOEXEC);
    if (h_section < 0) return errno;
    if (ftruncate(h_section, (off_t)cb_section)) {
        const int error = errno;
        close(h_section);
        return error;
    }
    *p_out_h_section = h_section;
    return 0;
}

int ps_host_vm_section_discard(int h_section, uint64_t off_section, uint64_t cb_range) {
    return fallocate(h_section, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)off_section, (off_t)cb_range)
               ? errno
               : 0;
}

/* psport is linked -no-pie, so that the brk heap follows the executable, far below 1 TiB: one arena
 * keeps every thread's allocations there, and only blocks above 32 MiB go to mmap's high
 * addresses. */
void ps_host_vm_keep_heap_low(void) {
    mallopt(M_ARENA_MAX, 1);
    mallopt(M_MMAP_THRESHOLD, 32 * 1024 * 1024);
}

static int parse_mapping(char *sz_line, PsHostMapping *p_out) {
    char *sz_end;
    p_out->va_begin = (uintptr_t)strtoull(sz_line, &sz_end, 16);
    if (*sz_end != '-') return 0;
    p_out->va_end = (uintptr_t)strtoull(sz_end + 1, &sz_end, 16);
    if (*sz_end != ' ' || strlen(sz_end) < 4) return 0;
    p_out->is_readable = sz_end[1] == 'r';
    p_out->is_writable = sz_end[2] == 'w';
    p_out->is_executable = sz_end[3] == 'x';
    p_out->sz_description = sz_line;
    return 1;
}

/* Read in pieces with read(2) instead of stdio, which allocates. */
int ps_host_list_mappings(int (*pfn_mapping)(const PsHostMapping *p_mapping, void *p_context), void *p_context) {
    const int h_maps = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (h_maps < 0) return errno;
    char buffer[4096], sz_line[4096];
    size_t cb_line = 0;
    int is_stopped = 0, error = 0;
    for (ssize_t cb_read; !is_stopped && (cb_read = read(h_maps, buffer, sizeof(buffer))) != 0;) {
        if (cb_read < 0) {
            if (errno == EINTR) continue;
            error = errno;
            break;
        }
        for (ssize_t i = 0; i < cb_read && !is_stopped; ++i) {
            if (buffer[i] != '\n') {
                if (cb_line < sizeof(sz_line) - 1) sz_line[cb_line++] = buffer[i];
                continue;
            }
            sz_line[cb_line] = 0;
            cb_line = 0;
            PsHostMapping mapping;
            if (parse_mapping(sz_line, &mapping)) is_stopped = pfn_mapping(&mapping, p_context);
        }
    }
    close(h_maps);
    return error;
}
