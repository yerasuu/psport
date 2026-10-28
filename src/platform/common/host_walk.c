// SPDX-License-Identifier: GPL-2.0-or-later
/* Walking and removing folder trees: the same on every OS (ps_host.h), over the folder listing and
 * link-aware stat each OS implements. */
#define _GNU_SOURCE
#include "ps_host.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned flags;
    int (*pfn_entry)(const PsHostWalkEntry *p_entry, void *p_context);
    void *p_context;
    char *sz_path;
    size_t cb_path;
} Walk;

static PsHostEntryKind kind_of(uint32_t mode) {
    if (PS_HOST_MODE_IS_DIRECTORY(mode)) return PS_HOST_ENTRY_DIRECTORY;
    if (PS_HOST_MODE_IS_LINK(mode)) return PS_HOST_ENTRY_LINK;
    return PS_HOST_MODE_IS_REGULAR(mode) ? PS_HOST_ENTRY_REGULAR : PS_HOST_ENTRY_OTHER;
}

/* sz_name after the c_path characters of the walk's path and a '/': 0, or ENOMEM. */
static int path_append(Walk *p_walk, size_t c_path, const char *sz_name) {
    const size_t cb_needed = c_path + 1 + strlen(sz_name) + 1;
    if (cb_needed > p_walk->cb_path) {
        const size_t cb_grown = cb_needed > p_walk->cb_path * 2 ? cb_needed : p_walk->cb_path * 2;
        char *sz_grown = realloc(p_walk->sz_path, cb_grown);
        if (!sz_grown) return ENOMEM;
        p_walk->sz_path = sz_grown;
        p_walk->cb_path = cb_grown;
    }
    p_walk->sz_path[c_path] = '/';
    memcpy(p_walk->sz_path + c_path + 1, sz_name, cb_needed - c_path - 1);
    return 0;
}

static int visit(Walk *p_walk, PsHostWalkEntry *p_entry) {
    p_entry->sz_path = p_walk->sz_path;
    return p_walk->pfn_entry(p_entry, p_walk->p_context);
}

/* The entry at the walk's path, c_path characters long, as nftw(3) with FTW_PHYS visits it. */
static int walk_at(Walk *p_walk, size_t c_path, int depth) {
    PsHostWalkEntry entry = {.depth = depth};
    const int error_stat = ps_host_fs_stat_link(p_walk->sz_path, &entry.stat);
    if (error_stat) {
        if (!depth || (error_stat != ENOENT && error_stat != EACCES)) return error_stat;
        entry.kind = PS_HOST_ENTRY_UNKNOWN;
        return visit(p_walk, &entry);
    }
    entry.kind = kind_of(entry.stat.mode);
    if (entry.kind != PS_HOST_ENTRY_DIRECTORY) return visit(p_walk, &entry);
    PsHostDir *p_dir = ps_host_dir_open(p_walk->sz_path);
    if (!p_dir) {
        const int error_open = errno;
        if (error_open != EACCES) return error_open;
        entry.is_unlisted = 1;
        return visit(p_walk, &entry);
    }
    const int is_post_order = (p_walk->flags & PS_HOST_WALK_POST_ORDER) != 0;
    int result = is_post_order ? 0 : visit(p_walk, &entry);
    for (PsHostDirEntry child; !result && ps_host_dir_next(p_dir, &child);) {
        if (!strcmp(child.sz_name, ".") || !strcmp(child.sz_name, "..")) continue;
        result = path_append(p_walk, c_path, child.sz_name);
        if (!result) result = walk_at(p_walk, c_path + 1 + strlen(child.sz_name), depth + 1);
        p_walk->sz_path[c_path] = 0;
    }
    ps_host_dir_close(p_dir);
    return !result && is_post_order ? visit(p_walk, &entry) : result;
}

int ps_host_fs_walk(const char *sz_root, unsigned flags, int (*pfn_entry)(const PsHostWalkEntry *p_entry, void *p_context),
                    void *p_context) {
    const size_t c_root = strlen(sz_root);
    Walk walk = {.flags = flags, .pfn_entry = pfn_entry, .p_context = p_context, .cb_path = c_root + 256};
    if (!(walk.sz_path = malloc(walk.cb_path))) return ENOMEM;
    memcpy(walk.sz_path, sz_root, c_root + 1);
    const int result = walk_at(&walk, c_root, 0);
    free(walk.sz_path);
    return result;
}

static int remove_entry(const PsHostWalkEntry *p_entry, void *p_context) {
    if (!p_entry->depth && *(const int *)p_context) return 0;
    return p_entry->kind == PS_HOST_ENTRY_DIRECTORY ? ps_host_fs_remove_dir(p_entry->sz_path)
                                                    : ps_host_fs_remove(p_entry->sz_path);
}

int ps_host_fs_remove_tree(const char *sz_path, int is_root_kept) {
    return ps_host_fs_walk(sz_path, PS_HOST_WALK_POST_ORDER, remove_entry, &is_root_kept);
}
