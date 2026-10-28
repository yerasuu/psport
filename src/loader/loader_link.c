// SPDX-License-Identifier: GPL-2.0-or-later
/* Links the eboot with the modules it needs into one image, as one table of imports: the game's
 * bundled modules (libc.prx and libSceFios2.prx always, the others of sce_module once something
 * linked imports their library), the user's system modules (--system-modules, never shipped)
 * likewise, and every module the game loads itself by path, linked unstarted. Each module starts
 * at the next 64 KiB after the previous one.
 * Exports are matched by (NID, library, module), versions included; one that matches only without
 * them is reported, and bound only with PS_LOOSE_IMPORT_VERSIONS=1. Imports of libSceLibcInternal are served by
 * libc.prx's same-NID exports: the internal library is not shipped with the game and exposes the
 * same functions. System modules' exports win over the host runtime's (binding kinds 3 and 4)
 * except for libraries named in --hle, so an own implementation can stand in for one and be
 * compared with it, and for those the port implements unless --lle names them. System modules that
 * are clients of system services are never linked (ipmi_client). Modules the game loads by path
 * win too (kinds 5 and 6), as the PS4 linker rebinds imports once one is loaded. */
#define _GNU_SOURCE
#include "loader.h"
#include "loader_traits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PT_TLS 7
#define PT_SCE_PROCPARAM 0x61000001
#define PT_SCE_RELRO 0x61000010
#define IMAGE_LIMIT (512u * 1024 * 1024)

/* Served by the game's libc.prx, whose functions the runtime partly replaces to track the game's
 * writes into GPU memory (memcpy, memset): a native copy of this one would bypass that. */
static const char *const s_never_system[] = {"libSceLibcInternal"};
/* The runtime's kernel, GPU, video, pad, audio output, clock and user-level threads; the ported
 * gpu/fiber, gpu/font, gpu/audiodec and gpu/ngs2 or gpu/ngs2_anyps5 (PS_NGS2). */
static const char *const s_port_implements[] = {"libkernel", "libkernel_unity", "libkernel_sys", "libSceGnmDriver",
                                              "libSceVideoOut", "libScePad", "libSceAudioOut", "libSceFiber",
                                              "libSceFont", "libSceFontFt", "libSceAudiodec", "libSceNgs2",
                                              "libSceUlt", "libSceRtc"};

typedef struct { char **pp_items; size_t c_items, c_capacity; } List;
typedef struct { uint64_t hash; uint32_t i_entry; } Slot;
typedef struct { Slot *p_slots; size_t c_capacity, c_entries; } Map;
typedef struct { LoaderIdentity id; uint64_t i_import; } Import;
typedef struct { LoaderIdentity id; uint64_t va_export, kind; const char *sz_file; int handle_only; } Export;
typedef struct { LoaderIdentity id; uint64_t va_export, kind; size_t c_exports; } NidExports;
typedef struct { uint32_t i_export; size_t c_exports; } VersionlessExports;
typedef struct {
    const char *sz_game;
    LoaderImage *p_out;
    size_t c_image_capacity, c_segment_capacity, c_reloc_capacity, c_name_capacity;
    LoaderModule main;
    uint64_t cb_eboot;
    Import *p_imports;
    size_t c_imports, c_import_capacity;
    int64_t i_none; /* the import index of names without an identity */
    Map import_map;
    Export *p_exports;
    size_t c_exports, c_export_capacity;
    Map export_map;
    NidExports *p_nids;
    size_t c_nids, c_nid_capacity;
    Map nid_map;
    VersionlessExports *p_versionless;
    size_t c_versionless, c_versionless_capacity;
    Map versionless_map;
    int is_versionless_indexed;
    List imported_libraries, module_names, deferred, from_system, system_stems, system_paths, bundled_stems, bundled_names;
} Linker;

static void *grow(void *p_array, size_t *p_capacity, size_t c_used, size_t cb_element) {
    if (c_used < *p_capacity) return p_array;
    *p_capacity = *p_capacity ? *p_capacity * 2 : 64;
    void *p_larger = realloc(p_array, *p_capacity * cb_element);
    if (!p_larger) loader_fail("allocation failed");
    return p_larger;
}
static void list_add(List *p_list, const char *sz_text) {
    p_list->pp_items = grow(p_list->pp_items, &p_list->c_capacity, p_list->c_items, sizeof(*p_list->pp_items));
    if (!(p_list->pp_items[p_list->c_items++] = strdup(sz_text))) loader_fail("allocation failed");
}
static void list_remove(List *p_list, size_t i_item) {
    free(p_list->pp_items[i_item]);
    memmove(p_list->pp_items + i_item, p_list->pp_items + i_item + 1, (--p_list->c_items - i_item) * sizeof(*p_list->pp_items));
}
static int list_has(const List *p_list, const char *sz_text) {
    for (size_t i = 0; i < p_list->c_items; ++i)
        if (!strcmp(p_list->pp_items[i], sz_text)) return 1;
    return 0;
}
static int listed(const char *const *pp_names, size_t c_names, const char *sz_name) {
    for (size_t i = 0; i < c_names; ++i)
        if (!strcmp(pp_names[i], sz_name)) return 1;
    return 0;
}
static void split_names(const char *sz_text, List *p_list) {
    for (const char *sz_cursor = sz_text ? sz_text : ""; *sz_cursor;) {
        size_t length = strcspn(sz_cursor, ",");
        const char *sz_from = sz_cursor, *sz_to = sz_cursor + length;
        while (sz_from < sz_to && strchr(" \t\n\r\v\f", *sz_from)) ++sz_from;
        while (sz_to > sz_from && strchr(" \t\n\r\v\f", sz_to[-1])) --sz_to;
        if (sz_to > sz_from) {
            char sz_name[256];
            snprintf(sz_name, sizeof(sz_name), "%.*s", (int)(sz_to - sz_from), sz_from);
            list_add(p_list, sz_name);
        }
        sz_cursor += length + (sz_cursor[length] == ',');
    }
}
/* Path.stem: the last component without its last suffix. */
static void stem(const char *sz_path, char *sz_out, size_t cb_out) {
    const char *sz_name = strrchr(sz_path, '/') ? strrchr(sz_path, '/') + 1 : sz_path, *sz_dot = strrchr(sz_name, '.');
    size_t length = sz_dot && sz_dot > sz_name && sz_dot[1] ? (size_t)(sz_dot - sz_name) : strlen(sz_name);
    snprintf(sz_out, cb_out, "%.*s", (int)length, sz_name);
}
static void join(char *sz_out, size_t cb_out, const char *sz_a, const char *sz_b) {
    if ((size_t)snprintf(sz_out, cb_out, "%s/%s", sz_a, sz_b) >= cb_out) loader_fail("path too long: %s/%s", sz_a, sz_b);
}

static uint64_t fnv(uint64_t hash, const void *p_data, size_t length) {
    for (size_t i = 0; i < length; ++i) hash = (hash ^ ((const unsigned char *)p_data)[i]) * 0x100000001b3u;
    return hash;
}
static uint64_t identity_hash(const LoaderIdentity *p_identity) {
    uint64_t hash = fnv(0xcbf29ce484222325u, p_identity->sz_nid, p_identity->nid_length);
    hash = fnv(hash, p_identity->p_library->sz_name, strlen(p_identity->p_library->sz_name) + 1);
    hash = fnv(hash, &p_identity->p_library->version, sizeof(p_identity->p_library->version));
    hash = fnv(hash, p_identity->p_module->sz_name, strlen(p_identity->p_module->sz_name) + 1);
    return fnv(hash, &p_identity->p_module->version, sizeof(p_identity->p_module->version));
}
static int same_identity(const LoaderIdentity *p_a, const LoaderIdentity *p_b) {
    return p_a->nid_length == p_b->nid_length && !memcmp(p_a->sz_nid, p_b->sz_nid, p_a->nid_length) &&
           p_a->p_library->version == p_b->p_library->version && !strcmp(p_a->p_library->sz_name, p_b->p_library->sz_name) &&
           p_a->p_module->version == p_b->p_module->version && !strcmp(p_a->p_module->sz_name, p_b->p_module->sz_name);
}
/* Open addressing over indices into the caller's array; pfn_equal() compares an entry with the key. */
typedef int (*MapEqual)(const void *p_array, uint32_t i_entry, const void *p_key);
static uint32_t map_find(const Map *p_map, uint64_t hash, MapEqual pfn_equal, const void *p_array, const void *p_key) {
    if (!p_map->c_capacity) return UINT32_MAX;
    for (size_t i = hash & (p_map->c_capacity - 1);; i = (i + 1) & (p_map->c_capacity - 1)) {
        const Slot *p_slot = &p_map->p_slots[i];
        if (p_slot->i_entry == UINT32_MAX) return UINT32_MAX;
        if (p_slot->hash == hash && pfn_equal(p_array, p_slot->i_entry, p_key)) return p_slot->i_entry;
    }
}
static void map_insert(Map *p_map, uint64_t hash, uint32_t i_entry) {
    if ((p_map->c_entries + 1) * 2 > p_map->c_capacity) {
        Map larger = {calloc(p_map->c_capacity ? p_map->c_capacity * 2 : 1024, sizeof(Slot)), p_map->c_capacity ? p_map->c_capacity * 2 : 1024, 0};
        if (!larger.p_slots) loader_fail("allocation failed");
        memset(larger.p_slots, 0xff, larger.c_capacity * sizeof(Slot));
        for (size_t i = 0; i < p_map->c_capacity; ++i)
            if (p_map->p_slots[i].i_entry != UINT32_MAX) map_insert(&larger, p_map->p_slots[i].hash, p_map->p_slots[i].i_entry);
        free(p_map->p_slots);
        *p_map = larger;
    }
    size_t i = hash & (p_map->c_capacity - 1);
    while (p_map->p_slots[i].i_entry != UINT32_MAX) i = (i + 1) & (p_map->c_capacity - 1);
    p_map->p_slots[i].hash = hash;
    p_map->p_slots[i].i_entry = i_entry;
    ++p_map->c_entries;
}
static int import_equal(const void *p_array, uint32_t i_entry, const void *p_key) {
    return same_identity(&((const Import *)p_array)[i_entry].id, p_key);
}
static int export_equal(const void *p_array, uint32_t i_entry, const void *p_key) {
    return same_identity(&((const Export *)p_array)[i_entry].id, p_key);
}
static uint64_t nid_hash(const LoaderIdentity *p_identity) {
    return fnv(fnv(0xcbf29ce484222325u, p_identity->sz_nid, p_identity->nid_length), p_identity->p_library->sz_name, strlen(p_identity->p_library->sz_name) + 1);
}
static int nid_equal(const void *p_array, uint32_t i_entry, const void *p_key) {
    const LoaderIdentity *p_a = &((const NidExports *)p_array)[i_entry].id, *p_b = p_key;
    return p_a->nid_length == p_b->nid_length && !memcmp(p_a->sz_nid, p_b->sz_nid, p_a->nid_length) && !strcmp(p_a->p_library->sz_name, p_b->p_library->sz_name);
}
/* (NID, library name, module name): the identity without its versions. */
static uint64_t versionless_hash(const LoaderIdentity *p_identity) {
    uint64_t hash = fnv(0xcbf29ce484222325u, p_identity->sz_nid, p_identity->nid_length);
    hash = fnv(hash, p_identity->p_library->sz_name, strlen(p_identity->p_library->sz_name) + 1);
    return fnv(hash, p_identity->p_module->sz_name, strlen(p_identity->p_module->sz_name) + 1);
}
typedef struct { const VersionlessExports *p_entries; const Export *p_exports; } VersionlessView;
static int versionless_equal(const void *p_array, uint32_t i_entry, const void *p_key) {
    const VersionlessView *p_view = p_array;
    const LoaderIdentity *p_a = &p_view->p_exports[p_view->p_entries[i_entry].i_export].id, *p_b = p_key;
    return p_a->nid_length == p_b->nid_length && !memcmp(p_a->sz_nid, p_b->sz_nid, p_a->nid_length) &&
           !strcmp(p_a->p_library->sz_name, p_b->p_library->sz_name) && !strcmp(p_a->p_module->sz_name, p_b->p_module->sz_name);
}

static void ensure_image(Linker *p_linker, uint64_t cb_image) {
    LoaderImage *p_out = p_linker->p_out;
    if (cb_image > p_linker->c_image_capacity) {
        size_t c_capacity = p_linker->c_image_capacity ? p_linker->c_image_capacity : 64u << 20;
        while (c_capacity < cb_image) c_capacity *= 2;
        if (!(p_out->p_image = realloc(p_out->p_image, c_capacity))) loader_fail("allocation failed");
        p_linker->c_image_capacity = c_capacity;
    }
    if (cb_image > p_out->cb_image) memset(p_out->p_image + p_out->cb_image, 0, cb_image - p_out->cb_image), p_out->cb_image = cb_image;
}
/* Only the permission bits: some system modules set high bits in p_flags (libSceAvPlayer: 0x08000006),
 * which are not protections. */
static uint64_t protection(uint32_t flags) { return flags & 7; }
static void add_segment(Linker *p_linker, uint64_t va_segment, uint64_t cb_segment, uint32_t flags) {
    LoaderImage *p_out = p_linker->p_out;
    p_out->p_segments = grow(p_out->p_segments, &p_linker->c_segment_capacity, p_out->c_segments, sizeof(*p_out->p_segments));
    p_out->p_segments[p_out->c_segments++] = (LoaderSegment){va_segment, cb_segment, protection(flags)};
}
static uint64_t signed64(__int128 value) {
    if (value < INT64_MIN || value > INT64_MAX) loader_fail("relocation value out of range");
    return (uint64_t)(int64_t)value;
}
static void store64(Linker *p_linker, uint64_t target, __int128 value) {
    if (value < 0 || value > (__int128)UINT64_MAX) loader_fail("relocation value out of range");
    uint64_t v = (uint64_t)value;
    memcpy(p_linker->p_out->p_image + target, &v, 8);
}
static void add_reloc(Linker *p_linker, uint64_t target, uint64_t kind, uint64_t value, int64_t addend) {
    LoaderImage *p_out = p_linker->p_out;
    p_out->p_relocs = grow(p_out->p_relocs, &p_linker->c_reloc_capacity, p_out->c_relocs, sizeof(*p_out->p_relocs));
    p_out->p_relocs[p_out->c_relocs++] = (LoaderReloc){target, kind, value, (uint64_t)addend};
}
static uint64_t add_name(Linker *p_linker, const char *sz_name) {
    LoaderImage *p_out = p_linker->p_out;
    if (strlen(sz_name) >= 128) loader_fail("import name too long: %s", sz_name);
    p_out->p_names = grow(p_out->p_names, &p_linker->c_name_capacity, p_out->c_names, sizeof(*p_out->p_names));
    memset(p_out->p_names[p_out->c_names], 0, 128);
    strcpy(p_out->p_names[p_out->c_names], sz_name);
    return p_out->c_names++;
}
static int is_load(const LoaderPh *p_ph) { return p_ph->type == 1 || p_ph->type == PT_SCE_RELRO; }
static int in_loads(const LoaderModule *p_module, uint64_t va_range, uint64_t length) {
    for (unsigned i = 0; i < p_module->phnum; ++i) {
        const LoaderPh *p_ph = &p_module->p_ph[i];
        if (is_load(p_ph) && va_range >= p_ph->vaddr && va_range - p_ph->vaddr <= p_ph->memsz && length <= p_ph->memsz - (va_range - p_ph->vaddr))
            return 1;
    }
    return 0;
}
static uint64_t load_size(const LoaderModule *p_module) {
    uint64_t cb_load = 0;
    int any = 0;
    for (unsigned i = 0; i < p_module->phnum; ++i)
        if (is_load(&p_module->p_ph[i])) {
            any = 1;
            uint64_t end = p_module->p_ph[i].vaddr > UINT64_MAX - p_module->p_ph[i].memsz ? UINT64_MAX : p_module->p_ph[i].vaddr + p_module->p_ph[i].memsz;
            if (end > cb_load) cb_load = end;
        }
    if (!any) loader_fail("no loadable segment");
    return cb_load;
}
static void module_segments(const LoaderModule *p_module, uint64_t base, RuntimeModule *p_out) {
    p_out->c_segments = 0;
    for (unsigned i = 0; i < p_module->phnum && p_out->c_segments < 4; ++i)
        if (is_load(&p_module->p_ph[i]))
            p_out->segments[p_out->c_segments++] = (RuntimeModuleSegment){base + p_module->p_ph[i].vaddr, p_module->p_ph[i].memsz, protection(p_module->p_ph[i].flags)};
}
static int exported(const LoaderSymbol *p_symbol) {
    return p_symbol->section && (p_symbol->binding == 1 || p_symbol->binding == 2) && p_symbol->identity.is_valid && (p_symbol->type == 1 || p_symbol->type == 2);
}
static void add_export_entry(RuntimeModule *p_module, size_t *p_capacity, const LoaderSymbol *p_symbol, uint64_t va_export) {
    p_module->p_exports = grow(p_module->p_exports, p_capacity, p_module->c_exports, sizeof(*p_module->p_exports));
    RuntimeModuleExport *p_module_export = &p_module->p_exports[p_module->c_exports++];
    memset(p_module_export->sz_nid, 0, sizeof(p_module_export->sz_nid));
    memcpy(p_module_export->sz_nid, p_symbol->identity.sz_nid, p_symbol->identity.nid_length < 16 ? p_symbol->identity.nid_length : 16);
    p_module_export->va_export = va_export;
}

#define FS_LOAD_SIZE 9
/* `mov r64, fs:[0]`: FS prefix, REX.W with or without REX.R, 8B, ModRM with mod 00 and r/m 100 (any
 * register), SIB 25 (no base, no index) and a zero disp32. Games load the thread pointer into any
 * register, not only rax. */
static int is_fs_load(const unsigned char *p) {
    return p[0] == 0x64 && (p[1] == 0x48 || p[1] == 0x4c) && p[2] == 0x8b && (p[3] & 0xc7) == 0x04 && p[4] == 0x25 &&
           !p[5] && !p[6] && !p[7] && !p[8];
}

/* Rewrites initial-exec `mov r64, fs:[0]` to GS, since the host's own TLS keeps FS, reading the TCB
 * pointer at the platform layer's displacement from GS's base. */
static uint64_t patch_fs_loads(unsigned char *p_image, const LoaderModule *p_module, uint64_t base) {
    const int32_t off_tcb = ps_host_tls_tcb_offset();
    uint64_t patched = 0;
    for (unsigned i = 0; i < p_module->phnum; ++i) {
        const LoaderPh *p_ph = &p_module->p_ph[i];
        if (p_ph->type != 1 || !(p_ph->flags & 1) || p_ph->filesz < FS_LOAD_SIZE) continue;
        unsigned char *p_cursor = p_image + base + p_ph->vaddr, *p_last = p_cursor + p_ph->filesz - FS_LOAD_SIZE;
        while (p_cursor <= p_last && (p_cursor = memchr(p_cursor, 0x64, (size_t)(p_last - p_cursor) + 1))) {
            if (!is_fs_load(p_cursor)) {
                ++p_cursor;
                continue;
            }
            p_cursor[0] = 0x65;
            memcpy(p_cursor + 5, &off_tcb, sizeof(off_tcb));
            ++patched;
            p_cursor += FS_LOAD_SIZE;
        }
    }
    return patched;
}

static void note_import(Linker *p_linker, const LoaderIdentity *p_identity, uint64_t i_import) {
    if (!p_identity->is_valid) {
        p_linker->i_none = (int64_t)i_import;
        return;
    }
    uint64_t hash = identity_hash(p_identity);
    uint32_t found = map_find(&p_linker->import_map, hash, import_equal, p_linker->p_imports, p_identity);
    if (found != UINT32_MAX) {
        p_linker->p_imports[found].i_import = i_import;
        return;
    }
    p_linker->p_imports = grow(p_linker->p_imports, &p_linker->c_import_capacity, p_linker->c_imports, sizeof(*p_linker->p_imports));
    p_linker->p_imports[p_linker->c_imports] = (Import){*p_identity, i_import};
    map_insert(&p_linker->import_map, hash, (uint32_t)p_linker->c_imports++);
    if (!list_has(&p_linker->imported_libraries, p_identity->p_library->sz_name)) list_add(&p_linker->imported_libraries, p_identity->p_library->sz_name);
}
/* A module's import joins the table under the eboot's spelling when the eboot names its library
 * and module (the runtime's tables use it), else as NID#library. */
static uint64_t imported(Linker *p_linker, const LoaderSymbol *p_symbol) {
    const LoaderIdentity *p_identity = &p_symbol->identity;
    if (!p_identity->is_valid) {
        if (p_linker->i_none < 0) loader_fail("import %s names no library and module", p_symbol->sz_name);
        return (uint64_t)p_linker->i_none;
    }
    uint32_t found = map_find(&p_linker->import_map, identity_hash(p_identity), import_equal, p_linker->p_imports, p_identity);
    if (found != UINT32_MAX) return p_linker->p_imports[found].i_import;
    const LoaderId *p_library = NULL, *p_module = NULL;
    for (size_t i = 0; i < p_linker->main.c_libraries; ++i)
        if (p_linker->main.p_libraries[i].version == p_identity->p_library->version && !strcmp(p_linker->main.p_libraries[i].sz_name, p_identity->p_library->sz_name))
            p_library = &p_linker->main.p_libraries[i];
    for (size_t i = 0; i < p_linker->main.c_modules; ++i)
        if (p_linker->main.p_modules[i].version == p_identity->p_module->version && !strcmp(p_linker->main.p_modules[i].sz_name, p_identity->p_module->sz_name))
            p_module = &p_linker->main.p_modules[i];
    char sz_name[512];
    if (p_library && p_module) snprintf(sz_name, sizeof(sz_name), "%.*s#%s#%s", (int)p_identity->nid_length, p_identity->sz_nid, p_library->sz_key, p_module->sz_key);
    else snprintf(sz_name, sizeof(sz_name), "%.*s#%s", (int)p_identity->nid_length, p_identity->sz_nid, p_identity->p_library->sz_name);
    uint64_t i_name = add_name(p_linker, sz_name);
    note_import(p_linker, p_identity, i_name);
    return i_name;
}

/* The eboot alone: its relocations, its imports in the order they first appear. */
static void prepare(Linker *p_linker) {
    LoaderModule *p_module = &p_linker->main;
    LoaderImage *p_out = p_linker->p_out;
    uint64_t cb_load = load_size(p_module);
    if (cb_load > IMAGE_LIMIT || !in_loads(p_module, p_module->entry, 1)) loader_fail("invalid memory image");
    ensure_image(p_linker, cb_load);
    p_linker->cb_eboot = cb_load;
    p_out->entry = p_module->entry;
    for (unsigned i = 0; i < p_module->phnum; ++i) {
        const LoaderPh *p_ph = &p_module->p_ph[i];
        if (!is_load(p_ph)) continue;
        if (p_ph->filesz > p_ph->memsz) loader_fail("segment file size exceeds memory size");
        memcpy(p_out->p_image + p_ph->vaddr, loader_span(p_module->p_elf, p_module->cb_elf, p_ph->offset, p_ph->filesz), p_ph->filesz);
        add_segment(p_linker, p_ph->vaddr, p_ph->memsz, p_ph->flags);
        if (p_ph->type == 1) {
            p_out->p_eboot_loads = realloc(p_out->p_eboot_loads, (p_out->c_eboot_loads + 1) * sizeof(*p_out->p_eboot_loads));
            if (!p_out->p_eboot_loads) loader_fail("allocation failed");
            p_out->p_eboot_loads[p_out->c_eboot_loads++] = (LoaderSegment){p_ph->vaddr, p_ph->memsz, protection(p_ph->flags)};
        }
    }
    int64_t *p_import_of = malloc((p_module->c_symbols ? p_module->c_symbols : 1) * sizeof(*p_import_of));
    if (!p_import_of) loader_fail("allocation failed");
    for (size_t i = 0; i < p_module->c_symbols; ++i) p_import_of[i] = -1;
    for (size_t i = 0; i < p_module->c_relas; ++i) {
        const LoaderRela *p_rela = &p_module->p_relas[i];
        if (!in_loads(p_module, p_rela->target, 8)) loader_fail("relocation outside mapped segments: %#llx", (unsigned long long)p_rela->target);
        if (p_rela->kind == 8) {
            add_reloc(p_linker, p_rela->target, 0, (uint64_t)p_rela->addend, 0);
            continue;
        }
        if (p_rela->kind != 1 && p_rela->kind != 6 && p_rela->kind != 7) loader_fail("unsupported relocation %u", p_rela->kind);
        if (p_rela->symbol >= p_module->c_symbols) loader_fail("relocation symbol out of range");
        const LoaderSymbol *p_symbol = &p_module->p_symbols[p_rela->symbol];
        if (p_symbol->section == 0xfff1) store64(p_linker, p_rela->target, (__int128)p_symbol->value + p_rela->addend);
        else if (p_symbol->section) add_reloc(p_linker, p_rela->target, 0, signed64((__int128)p_symbol->value + p_rela->addend), 0);
        else {
            if (p_import_of[p_rela->symbol] < 0) p_import_of[p_rela->symbol] = (int64_t)add_name(p_linker, p_symbol->sz_name);
            uint64_t kind = p_symbol->type == 1 ? 2 : 1;
            if ((kind == 1 && p_rela->addend) || p_rela->addend < 0 || p_rela->addend >= 4096) loader_fail("unsupported import addend");
            add_reloc(p_linker, p_rela->target, kind, (uint64_t)p_import_of[p_rela->symbol], p_rela->addend);
        }
    }
    /* The eboot's imports are keyed by identity: an identity named twice keeps its first
     * place in the table but binds the later index. */
    uint64_t *p_symbol_of = malloc((p_out->c_names ? p_out->c_names : 1) * sizeof(*p_symbol_of));
    if (!p_symbol_of) loader_fail("allocation failed");
    for (size_t i = 0; i < p_module->c_symbols; ++i)
        if (p_import_of[i] >= 0) p_symbol_of[p_import_of[i]] = i;
    for (uint64_t i_name = 0; i_name < p_out->c_names; ++i_name) note_import(p_linker, &p_module->p_symbols[p_symbol_of[i_name]].identity, i_name);
    free(p_symbol_of);
    free(p_import_of);
}

static int compare_names(const void *p_a, const void *p_b) { return strcmp(*(char *const *)p_a, *(char *const *)p_b); }
/* Path order: component by component, so "a/b" sorts before "a-c". */
static int compare_paths(const void *p_a, const void *p_b) {
    const char *sz_x = *(char *const *)p_a, *sz_y = *(char *const *)p_b;
    for (;;) {
        size_t lx = strcspn(sz_x, "/"), ly = strcspn(sz_y, "/");
        int c = memcmp(sz_x, sz_y, lx < ly ? lx : ly);
        if (c) return c;
        if (lx != ly) return lx < ly ? -1 : 1;
        if (!sz_x[lx] || !sz_y[ly]) return !sz_x[lx] ? (sz_y[ly] ? -1 : 0) : 1;
        sz_x += lx + 1;
        sz_y += ly + 1;
    }
}
static int has_suffix(const char *sz_name, const char *sz_suffix) {
    size_t cb_name = strlen(sz_name), cb_suffix = strlen(sz_suffix);
    return cb_name >= cb_suffix && !strcmp(sz_name + cb_name - cb_suffix, sz_suffix);
}
/* The directory's entries ending in suffix, by name. */
static void glob_directory(const char *sz_directory, const char *sz_suffix, List *p_names) {
    PsHostDir *p_dir = ps_host_dir_open(sz_directory);
    if (!p_dir) return;
    for (PsHostDirEntry entry; ps_host_dir_next(p_dir, &entry);)
        if (strcmp(entry.sz_name, ".") && strcmp(entry.sz_name, "..") && has_suffix(entry.sz_name, sz_suffix)) list_add(p_names, entry.sz_name);
    ps_host_dir_close(p_dir);
    qsort(p_names->pp_items, p_names->c_items, sizeof(*p_names->pp_items), compare_names);
}
/* Modules the game loads by path (any *.prx or *.sprx outside sce_module and sce_sys, such as
 * Unity's Media/Modules and Media/Plugins), relative to /app0. Linked directories are entered too:
 * loader_mods.c builds a modded game's folder out of links. depth bounds a link loop. */
static void find_loadable(const char *sz_root, const char *sz_relative, List *p_found, int depth) {
    if (depth > 32) return;
    char sz_path[4096];
    if (sz_relative[0]) join(sz_path, sizeof(sz_path), sz_root, sz_relative);
    else snprintf(sz_path, sizeof(sz_path), "%s", sz_root);
    PsHostDir *p_dir = ps_host_dir_open(sz_path);
    if (!p_dir) return;
    for (PsHostDirEntry entry; ps_host_dir_next(p_dir, &entry);) {
        if (!strcmp(entry.sz_name, ".") || !strcmp(entry.sz_name, "..")) continue;
        if (!sz_relative[0] && (!strcmp(entry.sz_name, "sce_module") || !strcmp(entry.sz_name, "sce_sys"))) continue;
        char sz_child[4096], sz_full[4096];
        if (sz_relative[0]) join(sz_child, sizeof(sz_child), sz_relative, entry.sz_name);
        else snprintf(sz_child, sizeof(sz_child), "%s", entry.sz_name);
        join(sz_full, sizeof(sz_full), sz_root, sz_child);
        PsHostStat st;
        if (ps_host_fs_stat(sz_full, &st)) continue;
        if (PS_HOST_MODE_IS_DIRECTORY(st.mode)) {
            find_loadable(sz_root, sz_child, p_found, depth + 1);
            continue;
        }
        char sz_extension[8] = "";
        const char *sz_dot = strrchr(entry.sz_name, '.');
        if (!PS_HOST_MODE_IS_REGULAR(st.mode) || !sz_dot || sz_dot == entry.sz_name || !sz_dot[1] || strlen(sz_dot) >= sizeof(sz_extension)) continue;
        for (size_t i = 0; sz_dot[i]; ++i) sz_extension[i] = (char)(sz_dot[i] >= 'A' && sz_dot[i] <= 'Z' ? sz_dot[i] + 32 : sz_dot[i]);
        if (!strcmp(sz_extension, ".prx") || !strcmp(sz_extension, ".sprx")) list_add(p_found, sz_child);
    }
    ps_host_dir_close(p_dir);
}

static int linked_stem(const Linker *p_linker, const char *sz_library) {
    char sz_name[256];
    for (size_t i = 0; i < p_linker->module_names.c_items; ++i) {
        stem(p_linker->module_names.pp_items[i], sz_name, sizeof(sz_name));
        if (!strcmp(sz_name, sz_library)) return 1;
    }
    return 0;
}
/* The console's dynamic linker loads libSceNpManager for such a module, and its module_start runs
 * libSceNet's init before the module's own (kb #869). */
static int needs_np_manager(const LoaderModule *p_module) {
    for (size_t i = 0; i < p_module->c_modules; ++i)
        if (!strcmp(p_module->p_modules[i].sz_name, "libSceNpManager")) return 1;
    return 0;
}
static void free_module(LoaderModule *p_module) {
    free(p_module->p_elf); free(p_module->p_ph); free(p_module->p_dynamic); free(p_module->p_libraries); free(p_module->p_modules); free(p_module->p_symbols); free(p_module->p_relas);
}
/* On the console these libraries are clients of system services, which they reach over IPMI
 * (libSceIpmi's IPMI::Client, the kernel's ipmimgr_call, libSceNpCommon's ServiceIpmiClient,
 * libSceAudioOut's IPMI session...): with no service process behind them their initialization
 * fails and they fault on it (libSceAppContent) or stop the game (libSceNpTrophy), so the port's
 * own library serves them instead. The first such import is written to why. */
static int ipmi_client(const char *sz_path, char *sz_why, size_t cb_why) {
    LoaderModule m = {0};
    g_sz_loader_file = sz_path;
    loader_module(sz_path, &m);
    int is_found = 0;
    for (size_t i = 0; i < m.c_symbols && !is_found; ++i) {
        const LoaderIdentity *p_identity = &m.p_symbols[i].identity;
        if (m.p_symbols[i].section || !p_identity->is_valid) continue;
        const char *sz_symbol = runtime_symbol(p_identity->sz_nid);
        if ((sz_symbol && strcasestr(sz_symbol, "ipmi")) || !strcmp(p_identity->p_library->sz_name, "libSceIpmi")) {
            snprintf(sz_why, cb_why, "%s from %s", sz_symbol ? sz_symbol : "an export", p_identity->p_library->sz_name);
            is_found = 1;
        }
    }
    free_module(&m);
    g_sz_loader_file = NULL;
    return is_found;
}
/* Bundled and system modules whose library something linked imports and nothing linked provides:
 * the system ones in their order, then the bundled ones no system module shares a name with. */
static void link_wanted(Linker *p_linker) {
    size_t candidates = p_linker->system_stems.c_items + p_linker->bundled_stems.c_items;
    for (size_t c = 0; c < candidates; ++c) {
        const char *sz_library = c < p_linker->system_stems.c_items ? p_linker->system_stems.pp_items[c] : p_linker->bundled_stems.pp_items[c - p_linker->system_stems.c_items];
        if (c >= p_linker->system_stems.c_items && list_has(&p_linker->system_stems, sz_library)) continue;
        if (!list_has(&p_linker->imported_libraries, sz_library) || linked_stem(p_linker, sz_library)) continue;
        size_t b = 0;
        while (b < p_linker->bundled_stems.c_items && strcmp(p_linker->bundled_stems.pp_items[b], sz_library)) ++b;
        if (b < p_linker->bundled_stems.c_items) {
            list_add(&p_linker->module_names, p_linker->bundled_names.pp_items[b]);
            continue;
        }
        size_t s = 0;
        while (strcmp(p_linker->system_stems.pp_items[s], sz_library)) ++s;
        char sz_why[256];
        if (ipmi_client(p_linker->system_paths.pp_items[s], sz_why, sizeof(sz_why))) {
            printf("Loader: system module %s not linked, the port serves %s: it is a client of a system service (imports %s)\n",
                   strrchr(p_linker->system_paths.pp_items[s], '/') + 1, sz_library, sz_why);
            list_remove(&p_linker->system_stems, s);
            list_remove(&p_linker->system_paths, s);
            --c;
            --candidates;
            continue;
        }
        const char *sz_file = strrchr(p_linker->system_paths.pp_items[s], '/') + 1;
        list_add(&p_linker->module_names, sz_file);
        list_add(&p_linker->from_system, sz_file);
    }
}

static void link_module(Linker *p_linker, const char *sz_filename, LoaderModule *p_module, RuntimeModule *p_runtime_module, uint64_t base, uint64_t *p_tls_module,
                        uint64_t *p_fs_patched) {
    int deferred = list_has(&p_linker->deferred, sz_filename), from_system = list_has(&p_linker->from_system, sz_filename);
    char sz_path[4096], sz_name[256];
    PsHostStat st;
    if (deferred) join(sz_path, sizeof(sz_path), p_linker->sz_game, sz_filename);
    else {
        char sz_directory[4096];
        join(sz_directory, sizeof(sz_directory), p_linker->sz_game, "sce_module");
        join(sz_path, sizeof(sz_path), sz_directory, sz_filename);
    }
    if (ps_host_fs_stat(sz_path, &st) && from_system) {
        stem(sz_filename, sz_name, sizeof(sz_name));
        size_t s = 0;
        while (strcmp(p_linker->system_stems.pp_items[s], sz_name)) ++s;
        snprintf(sz_path, sizeof(sz_path), "%s", p_linker->system_paths.pp_items[s]);
    }
    g_sz_loader_file = sz_path;
    loader_module(sz_path, p_module);
    uint64_t cb_load = load_size(p_module);
    if (cb_load > IMAGE_LIMIT || base + cb_load > IMAGE_LIMIT) loader_fail("linked image exceeds probe limit");
    ensure_image(p_linker, base + cb_load);
    unsigned char *p_image = p_linker->p_out->p_image;
    for (unsigned i = 0; i < p_module->phnum; ++i) {
        const LoaderPh *p_ph = &p_module->p_ph[i];
        if (!is_load(p_ph)) continue;
        if (p_ph->filesz > p_ph->memsz) loader_fail("invalid segment size");
        memcpy(p_image + base + p_ph->vaddr, loader_span(p_module->p_elf, p_module->cb_elf, p_ph->offset, p_ph->filesz), p_ph->filesz);
        add_segment(p_linker, base + p_ph->vaddr, p_ph->memsz, p_ph->flags);
    }
    const LoaderPh *p_tls = loader_find_ph(p_module, PT_TLS);
    uint64_t module_id = 0;
    /* A PT_TLS of no size needs no module id: ids are what the runtime keeps a block per thread
     * for, and titles with many modules (Atelier Ryza, 11) mostly have empty ones. */
    if (p_tls && p_tls->memsz) {
        if (p_tls->filesz > p_tls->memsz || !in_loads(p_module, p_tls->vaddr, p_tls->memsz)) loader_fail("unsupported TLS layout");
        module_id = (*p_tls_module)++;
    }
    for (size_t i = 0; i < p_module->c_relas; ++i) {
        const LoaderRela *p_rela = &p_module->p_relas[i];
        if (!in_loads(p_module, p_rela->target, 8)) loader_fail("relocation outside image %#llx", (unsigned long long)p_rela->target);
        uint64_t target = base + p_rela->target;
        if (p_rela->kind == 8) {
            add_reloc(p_linker, target, 0, signed64((__int128)base + p_rela->addend), 0);
            continue;
        }
        if (p_rela->kind != 16 && p_rela->kind != 1 && p_rela->kind != 6 && p_rela->kind != 7) loader_fail("unsupported relocation type %u", p_rela->kind);
        if (p_rela->symbol >= p_module->c_symbols) loader_fail("relocation symbol out of range");
        const LoaderSymbol *p_symbol = &p_module->p_symbols[p_rela->symbol];
        if (p_rela->kind == 16) {
            int own = p_symbol->sz_name[0] ? p_symbol->type == 6 && p_symbol->section : p_symbol->type == 3;
            if (!own || p_rela->addend || !module_id) loader_fail("unsupported external TLS relocation");
            store64(p_linker, target, module_id);
        } else if (p_symbol->section == 0xfff1) store64(p_linker, target, (__int128)p_symbol->value + p_rela->addend);
        else if (p_symbol->section) add_reloc(p_linker, target, 0, signed64((__int128)base + p_symbol->value + p_rela->addend), 0);
        else if (!p_symbol->identity.is_valid && p_symbol->binding == 2) store64(p_linker, target, 0); /* weak undefined (module_start/stop) */
        else {
            uint64_t kind = p_symbol->type == 1 ? 2 : 1;
            if ((kind == 1 && p_rela->addend) || p_rela->addend < 0 || p_rela->addend >= 4096) loader_fail("unsupported import addend");
            add_reloc(p_linker, target, kind, imported(p_linker, p_symbol), p_rela->addend);
        }
    }
    size_t c_export_capacity = 0;
    for (size_t i = 0; i < p_module->c_symbols; ++i) {
        const LoaderSymbol *p_symbol = &p_module->p_symbols[i];
        if (!exported(p_symbol)) continue;
        if (!in_loads(p_module, p_symbol->value, p_symbol->cb_symbol ? p_symbol->cb_symbol : 1)) loader_fail("export outside load segments");
        uint64_t hash = identity_hash(&p_symbol->identity), kind = (p_symbol->type == 1 ? 2 : 1) + (deferred ? 4 : from_system ? 2 : 0);
        uint32_t found = map_find(&p_linker->export_map, hash, export_equal, p_linker->p_exports, &p_symbol->identity);
        /* Modules the game loads by path (a PS2 emulator's per-disc AOT modules) may export the same
         * names: the game reaches them through sceKernelDlsym on its own handle, so none of them
         * binds imports at link time. */
        if (found != UINT32_MAX) {
            if (!deferred || !list_has(&p_linker->deferred, p_linker->p_exports[found].sz_file)) loader_fail("ambiguous export %s", p_symbol->sz_name);
            p_linker->p_exports[found].handle_only = 1;
        } else {
            p_linker->p_exports = grow(p_linker->p_exports, &p_linker->c_export_capacity, p_linker->c_exports, sizeof(*p_linker->p_exports));
            p_linker->p_exports[p_linker->c_exports] = (Export){p_symbol->identity, 0, 0, NULL, 0};
            map_insert(&p_linker->export_map, hash, found = (uint32_t)p_linker->c_exports++);
        }
        p_linker->p_exports[found].va_export = base + p_symbol->value;
        p_linker->p_exports[found].kind = kind;
        p_linker->p_exports[found].sz_file = sz_filename;
        uint64_t nid = nid_hash(&p_symbol->identity);
        uint32_t same = map_find(&p_linker->nid_map, nid, nid_equal, p_linker->p_nids, &p_symbol->identity);
        if (same != UINT32_MAX) ++p_linker->p_nids[same].c_exports;
        else {
            p_linker->p_nids = grow(p_linker->p_nids, &p_linker->c_nid_capacity, p_linker->c_nids, sizeof(*p_linker->p_nids));
            p_linker->p_nids[p_linker->c_nids] = (NidExports){p_symbol->identity, base + p_symbol->value, kind, 1};
            map_insert(&p_linker->nid_map, nid, (uint32_t)p_linker->c_nids++);
        }
        add_export_entry(p_runtime_module, &c_export_capacity, p_symbol, base + p_symbol->value);
    }
    *p_fs_patched += patch_fs_loads(p_image, p_module, base);
    if (strlen(sz_filename) >= 128) loader_fail("path too long");
    strcpy(p_runtime_module->sz_name, sz_filename);
    uint64_t init = 0;
    loader_tag(p_module, 12, &init);
    p_runtime_module->base = base; p_runtime_module->cb_module = cb_load; p_runtime_module->init = base + init;
    p_runtime_module->va_tls = p_tls ? base + p_tls->vaddr : 0;
    p_runtime_module->tls_memsz = p_tls ? p_tls->memsz : 0;
    p_runtime_module->tls_filesz = p_tls ? p_tls->filesz : 0;
    p_runtime_module->tls_module = module_id;
    p_runtime_module->deferred = (uint64_t)deferred;
    p_runtime_module->has_np_manager_dependency = (uint64_t)needs_np_manager(p_module);
    loader_eh_frame(p_module, base, p_runtime_module);
    module_segments(p_module, base, p_runtime_module);
}

/* The console's kernel binds imports and nothing shows whether it compares library and module
 * versions (kb #881): every import that only an export of other versions would serve is logged
 * and counted, and bound only with PS_LOOSE_IMPORT_VERSIONS=1, since the console may refuse it. */
static size_t s_c_versionless_matches, s_c_versionless_bound, s_c_versionless_ambiguous;
static void versionless_summary(char *sz_out, size_t cb_out) {
    snprintf(sz_out, cb_out, "Loader: summary: %zu imports match an export only ignoring versions: %zu bound, %zu left to the host runtime (%zu ambiguous)\n",
             s_c_versionless_matches, s_c_versionless_bound, s_c_versionless_matches - s_c_versionless_bound, s_c_versionless_ambiguous);
}
static void index_versionless(Linker *p_linker) {
    p_linker->is_versionless_indexed = 1;
    for (size_t i = 0; i < p_linker->c_exports; ++i) {
        const Export *p_export = &p_linker->p_exports[i];
        if (p_export->handle_only) continue;
        uint64_t hash = versionless_hash(&p_export->id);
        VersionlessView view = {p_linker->p_versionless, p_linker->p_exports};
        uint32_t same = map_find(&p_linker->versionless_map, hash, versionless_equal, &view, &p_export->id);
        if (same != UINT32_MAX) {
            ++p_linker->p_versionless[same].c_exports;
            continue;
        }
        p_linker->p_versionless = grow(p_linker->p_versionless, &p_linker->c_versionless_capacity, p_linker->c_versionless, sizeof(*p_linker->p_versionless));
        p_linker->p_versionless[p_linker->c_versionless] = (VersionlessExports){(uint32_t)i, 1};
        map_insert(&p_linker->versionless_map, hash, (uint32_t)p_linker->c_versionless++);
    }
}
static void match_ignoring_versions(Linker *p_linker, const LoaderIdentity *p_identity, int should_bind, uint64_t *p_out_va, uint64_t *p_out_kind) {
    if (!p_linker->is_versionless_indexed) index_versionless(p_linker);
    VersionlessView view = {p_linker->p_versionless, p_linker->p_exports};
    uint32_t found = map_find(&p_linker->versionless_map, versionless_hash(p_identity), versionless_equal, &view, p_identity);
    if (found == UINT32_MAX) return;
    const VersionlessExports *p_match = &p_linker->p_versionless[found];
    const Export *p_export = &p_linker->p_exports[p_match->i_export];
    const char *sz_symbol = runtime_symbol(p_identity->sz_nid);
    if (!s_c_versionless_matches++) runtime_calls_add_summary(versionless_summary);
    if (p_match->c_exports > 1) {
        ++s_c_versionless_ambiguous;
        printf("Loader: import %.*s (%s) of %s version %#x, module %s version %#x, matches %zu exports of other versions "
               "only ignoring versions: left to the host runtime, ambiguous\n",
               (int)p_identity->nid_length, p_identity->sz_nid, sz_symbol ? sz_symbol : "unnamed", p_identity->p_library->sz_name,
               p_identity->p_library->version, p_identity->p_module->sz_name, p_identity->p_module->version, p_match->c_exports);
        return;
    }
    printf("Loader: import %.*s (%s) of %s version %#x, module %s version %#x, matches the export in %s "
           "(library version %#x, module version %#x) only ignoring versions: %s\n",
           (int)p_identity->nid_length, p_identity->sz_nid, sz_symbol ? sz_symbol : "unnamed", p_identity->p_library->sz_name,
           p_identity->p_library->version, p_identity->p_module->sz_name, p_identity->p_module->version,
           p_export->sz_file, p_export->id.p_library->version, p_export->id.p_module->version,
           should_bind ? "bound (PS_LOOSE_IMPORT_VERSIONS=1)" : "left to the host runtime (PS_LOOSE_IMPORT_VERSIONS=1 binds it)");
    if (!should_bind) return;
    ++s_c_versionless_bound;
    *p_out_va = p_export->va_export;
    *p_out_kind = p_export->kind;
}

void loader_load_game(const LoaderOptions *p_options, LoaderImage *p_out) {
    static const char *const s_default_modules[] = {"libc.prx", "libSceFios2.prx"};
    Linker l = {.sz_game = p_options->sz_game, .p_out = p_out, .i_none = -1};
    char sz_path[4096];
    memset(p_out, 0, sizeof(*p_out));
    if (p_options->sz_executable) snprintf(sz_path, sizeof(sz_path), "%s", p_options->sz_executable);
    else join(sz_path, sizeof(sz_path), l.sz_game, "eboot.bin");
    g_sz_loader_file = sz_path;
    loader_module(sz_path, &l.main);
    prepare(&l);
    join(sz_path, sizeof(sz_path), l.sz_game, "sce_module/libc.prx");
    p_out->capabilities = (uint64_t)loader_init_env_is_ret(sz_path);
    g_sz_loader_file = NULL;

    uint64_t base = (p_out->cb_image + 65535) & ~(uint64_t)65535, tls_module = 2;
    uint64_t fs_patched = patch_fs_loads(p_out->p_image, &l.main, 0);
    for (size_t i = 0; i < 2; ++i) list_add(&l.module_names, s_default_modules[i]);
    if (p_options->sz_system_modules && p_options->sz_system_modules[0]) {
        List hle = {0}, lle = {0}, files = {0};
        split_names(p_options->sz_hle, &hle);
        split_names(p_options->sz_lle, &lle);
        glob_directory(p_options->sz_system_modules, ".sprx", &files);
        for (size_t i = 0; i < files.c_items; ++i) {
            char sz_name[256];
            stem(files.pp_items[i], sz_name, sizeof(sz_name));
            if (listed(s_never_system, sizeof(s_never_system) / sizeof(*s_never_system), sz_name) || list_has(&hle, sz_name) ||
                (listed(s_port_implements, sizeof(s_port_implements) / sizeof(*s_port_implements), sz_name) && !list_has(&lle, sz_name)))
                continue;
            join(sz_path, sizeof(sz_path), p_options->sz_system_modules, files.pp_items[i]);
            list_add(&l.system_stems, sz_name);
            list_add(&l.system_paths, sz_path);
        }
    }
    List bundled = {0};
    join(sz_path, sizeof(sz_path), l.sz_game, "sce_module");
    glob_directory(sz_path, ".prx", &bundled);
    for (size_t i = 0; i < bundled.c_items; ++i) {
        char sz_name[256];
        stem(bundled.pp_items[i], sz_name, sizeof(sz_name));
        list_add(&l.bundled_stems, sz_name);
        list_add(&l.bundled_names, bundled.pp_items[i]);
    }
    link_wanted(&l);
    find_loadable(l.sz_game, "", &l.deferred, 0);
    qsort(l.deferred.pp_items, l.deferred.c_items, sizeof(*l.deferred.pp_items), compare_paths);
    for (size_t i = 0; i < l.deferred.c_items; ++i) list_add(&l.module_names, l.deferred.pp_items[i]);

    LoaderModule *p_linked = NULL;
    RuntimeModule *p_table = NULL;
    size_t c_linked_capacity = 0, c_table_capacity = 0, c_linked = 0;
    for (size_t i_name = 0; i_name < l.module_names.c_items; ++i_name) {
        p_linked = grow(p_linked, &c_linked_capacity, c_linked, sizeof(*p_linked));
        p_table = grow(p_table, &c_table_capacity, c_linked, sizeof(*p_table));
        memset(&p_table[c_linked], 0, sizeof(*p_table));
        link_module(&l, l.module_names.pp_items[i_name], &p_linked[c_linked], &p_table[c_linked], base, &tls_module, &fs_patched);
        base = (base + p_table[c_linked].cb_module + 65535) & ~(uint64_t)65535;
        ++c_linked;
        /* Linking a module can import a library only another bundled or system module provides. */
        link_wanted(&l);
    }
    g_sz_loader_file = NULL;

    size_t c_binding_capacity = 0, unresolved = l.i_none >= 0;
    const int should_bind_versionless = runtime_env_flag("PS_LOOSE_IMPORT_VERSIONS", 0);
    for (size_t i = 0; i < l.c_imports; ++i) {
        const Import *p_import = &l.p_imports[i];
        uint32_t found = map_find(&l.export_map, identity_hash(&p_import->id), export_equal, l.p_exports, &p_import->id);
        uint64_t va_export = 0, kind = 0;
        if (found != UINT32_MAX && !l.p_exports[found].handle_only) va_export = l.p_exports[found].va_export, kind = l.p_exports[found].kind;
        else if (!strcmp(p_import->id.p_library->sz_name, "libSceLibcInternal")) {
            LoaderIdentity libc = p_import->id;
            LoaderId library = {"", "libc", 0};
            libc.p_library = &library;
            uint32_t same = map_find(&l.nid_map, nid_hash(&libc), nid_equal, l.p_nids, &libc);
            if (same != UINT32_MAX && l.p_nids[same].c_exports == 1) va_export = l.p_nids[same].va_export, kind = l.p_nids[same].kind;
        }
        if (!kind && found == UINT32_MAX) match_ignoring_versions(&l, &p_import->id, should_bind_versionless, &va_export, &kind);
        if (!kind) {
            ++unresolved;
            continue;
        }
        p_out->p_bindings = grow(p_out->p_bindings, &c_binding_capacity, p_out->c_bindings, sizeof(*p_out->p_bindings));
        p_out->p_bindings[p_out->c_bindings++] = (LoaderBinding){p_import->i_import, va_export, kind};
    }

    const LoaderPh *p_tls = loader_find_ph(&l.main, PT_TLS);
    if (p_tls) {
        if (p_tls->filesz > p_tls->memsz || p_tls->memsz > 1024 * 1024 || p_tls->vaddr > l.cb_eboot || p_tls->filesz > l.cb_eboot - p_tls->vaddr)
            loader_fail("unsupported eboot TLS layout");
        p_out->main_tls[0] = p_tls->vaddr; p_out->main_tls[1] = p_tls->filesz; p_out->main_tls[2] = p_tls->memsz; p_out->main_tls[3] = p_tls->align;
    }
    const LoaderPh *p_procparam = loader_find_ph(&l.main, PT_SCE_PROCPARAM);
    if (!p_procparam) loader_fail("eboot.bin has no PT_SCE_PROCPARAM");
    p_out->procparam = p_procparam->vaddr;
    if (!(p_out->p_all_modules = calloc(c_linked + 1, sizeof(*p_out->p_all_modules)))) loader_fail("allocation failed");
    RuntimeModule *p_eboot = &p_out->p_all_modules[0];
    strcpy(p_eboot->sz_name, "eboot.bin");
    /* A linked system libSceNpManager runs its own module_start. */
    const int is_np_manager_linked = linked_stem(&l, "libSceNpManager");
    p_eboot->has_np_manager_dependency = (uint64_t)(!is_np_manager_linked && needs_np_manager(&l.main));
    for (size_t i = 0; i < c_linked && is_np_manager_linked; ++i) p_table[i].has_np_manager_dependency = 0;
    loader_eh_frame(&l.main, 0, p_eboot);
    module_segments(&l.main, 0, p_eboot);
    size_t c_export_capacity = 0;
    /* Unity's IL2CPP code asks the eboot (sceKernelDlsym handle 0) for the player's functions. */
    for (size_t i = 0; i < l.main.c_symbols; ++i)
        if (exported(&l.main.p_symbols[i])) add_export_entry(p_eboot, &c_export_capacity, &l.main.p_symbols[i], l.main.p_symbols[i].value);
    if (c_linked) memcpy(p_out->p_all_modules + 1, p_table, c_linked * sizeof(*p_table));
    p_out->c_modules = c_linked;

    printf("Linked modules:");
    for (size_t i = 0; i < c_linked; ++i)
        printf("%s %s@%#llx%s", i ? "," : "", p_table[i].sz_name, (unsigned long long)p_table[i].base,
               p_table[i].deferred ? " (loaded by the game)" : "");
    printf("; %llu native bindings, %zu imports left to the host runtime, fs->gs patched=%llu\n",
           (unsigned long long)p_out->c_bindings, unresolved, (unsigned long long)fs_patched);
    loader_record_game_traits(l.sz_game, &l.main);
    for (size_t i = 0; i < c_linked; ++i) free_module(&p_linked[i]);
    free_module(&l.main);
    free(p_linked); free(p_table); free(l.p_imports); free(l.p_exports); free(l.p_nids);
    free(l.import_map.p_slots); free(l.export_map.p_slots); free(l.nid_map.p_slots);
    free(l.p_versionless); free(l.versionless_map.p_slots);
}
