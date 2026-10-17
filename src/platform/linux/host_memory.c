/* Linux: the process's mappings (ps_host.h). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
