/* Links the eboot with the modules it needs into one image, as one table of imports: the game's
 * bundled modules (libc.prx and libSceFios2.prx always, the others of sce_module once something
 * linked imports their library), the user's system modules (--system-modules, never shipped)
 * likewise, and every module the game loads itself by path, linked unstarted. Each module starts
 * at the next 64 KiB after the previous one.
 * Exports are matched by (NID, library, module). Imports of libSceLibcInternal are served by
 * libc.prx's same-NID exports: the internal library is not shipped with the game and exposes the
 * same functions. System modules' exports win over the host runtime's (binding kinds 3 and 4)
 * except for libraries named in --hle, so an own implementation can stand in for one and be
 * compared with it, and for those the port implements unless --lle names them. System modules that
 * are clients of system services are never linked (ipmi_client). Modules the game loads by path
 * win too (kinds 5 and 6), as the PS4 linker rebinds imports once one is loaded. */
#define _GNU_SOURCE
#include "loader.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PT_TLS 7
#define PT_SCE_PROCPARAM 0x61000001
#define PT_SCE_RELRO 0x61000010
#define IMAGE_LIMIT (512u * 1024 * 1024)

/* Served by the game's libc.prx, whose functions the runtime partly replaces to track the game's
 * writes into GPU memory (memcpy, memset): a native copy of this one would bypass that. */
static const char *const never_system[] = {"libSceLibcInternal"};
/* The runtime's kernel, GPU, video, pad, audio output, clock and user-level threads; the ported
 * gpu/fiber, gpu/font, gpu/audiodec and gpu/ngs2. */
static const char *const port_implements[] = {"libkernel", "libkernel_unity", "libkernel_sys", "libSceGnmDriver",
                                              "libSceVideoOut", "libScePad", "libSceAudioOut", "libSceFiber",
                                              "libSceFont", "libSceFontFt", "libSceAudiodec", "libSceNgs2",
                                              "libSceUlt", "libSceRtc"};
static const unsigned char fs_load[9] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0}; /* mov rax, fs:[0] */

typedef struct { char **items; size_t count, capacity; } List;
typedef struct { uint64_t hash; uint32_t index; } Slot;
typedef struct { Slot *slots; size_t capacity, count; } Map;
typedef struct { LoaderIdentity id; uint64_t index; } Import;
typedef struct { LoaderIdentity id; uint64_t address, kind; const char *file; int handle_only; } Export;
typedef struct { LoaderIdentity id; uint64_t address, kind; size_t count; } NidExports;
typedef struct {
    const char *game;
    LoaderImage *out;
    size_t image_capacity, segment_capacity, reloc_capacity, name_capacity;
    LoaderModule main;
    uint64_t eboot_size;
    Import *imports;
    size_t import_count, import_capacity;
    int64_t none_index; /* the import index of names without an identity */
    Map import_map;
    Export *exports;
    size_t export_count, export_capacity;
    Map export_map;
    NidExports *nids;
    size_t nid_count, nid_capacity;
    Map nid_map;
    List imported_libraries, module_names, deferred, from_system, system_stems, system_paths, bundled_stems, bundled_names;
} Linker;

static void *grow(void *array, size_t *capacity, size_t count, size_t element) {
    if (count < *capacity) return array;
    *capacity = *capacity ? *capacity * 2 : 64;
    void *larger = realloc(array, *capacity * element);
    if (!larger) loader_fail("allocation failed");
    return larger;
}
static void list_add(List *list, const char *text) {
    list->items = grow(list->items, &list->capacity, list->count, sizeof(*list->items));
    if (!(list->items[list->count++] = strdup(text))) loader_fail("allocation failed");
}
static void list_remove(List *list, size_t index) {
    free(list->items[index]);
    memmove(list->items + index, list->items + index + 1, (--list->count - index) * sizeof(*list->items));
}
static int list_has(const List *list, const char *text) {
    for (size_t i = 0; i < list->count; ++i)
        if (!strcmp(list->items[i], text)) return 1;
    return 0;
}
static int listed(const char *const *names, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(names[i], name)) return 1;
    return 0;
}
static void split_names(const char *text, List *list) {
    for (const char *p = text ? text : ""; *p;) {
        size_t length = strcspn(p, ",");
        const char *from = p, *to = p + length;
        while (from < to && strchr(" \t\n\r\v\f", *from)) ++from;
        while (to > from && strchr(" \t\n\r\v\f", to[-1])) --to;
        if (to > from) {
            char name[256];
            snprintf(name, sizeof(name), "%.*s", (int)(to - from), from);
            list_add(list, name);
        }
        p += length + (p[length] == ',');
    }
}
/* Path.stem: the last component without its last suffix. */
static void stem(const char *path, char *out, size_t size) {
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path, *dot = strrchr(name, '.');
    size_t length = dot && dot > name && dot[1] ? (size_t)(dot - name) : strlen(name);
    snprintf(out, size, "%.*s", (int)length, name);
}
static void join(char *out, size_t size, const char *a, const char *b) {
    if ((size_t)snprintf(out, size, "%s/%s", a, b) >= size) loader_fail("path too long: %s/%s", a, b);
}

static uint64_t fnv(uint64_t hash, const void *data, size_t length) {
    for (size_t i = 0; i < length; ++i) hash = (hash ^ ((const unsigned char *)data)[i]) * 0x100000001b3u;
    return hash;
}
static uint64_t identity_hash(const LoaderIdentity *id) {
    uint64_t hash = fnv(0xcbf29ce484222325u, id->nid, id->nid_length);
    hash = fnv(hash, id->library->name, strlen(id->library->name) + 1);
    hash = fnv(hash, &id->library->version, sizeof(id->library->version));
    hash = fnv(hash, id->module->name, strlen(id->module->name) + 1);
    return fnv(hash, &id->module->version, sizeof(id->module->version));
}
static int same_identity(const LoaderIdentity *a, const LoaderIdentity *b) {
    return a->nid_length == b->nid_length && !memcmp(a->nid, b->nid, a->nid_length) &&
           a->library->version == b->library->version && !strcmp(a->library->name, b->library->name) &&
           a->module->version == b->module->version && !strcmp(a->module->name, b->module->name);
}
/* Open addressing over indices into the caller's array; equal() compares an entry with the key. */
typedef int (*MapEqual)(const void *array, uint32_t index, const void *key);
static uint32_t map_find(const Map *map, uint64_t hash, MapEqual equal, const void *array, const void *key) {
    if (!map->capacity) return UINT32_MAX;
    for (size_t i = hash & (map->capacity - 1);; i = (i + 1) & (map->capacity - 1)) {
        const Slot *slot = &map->slots[i];
        if (slot->index == UINT32_MAX) return UINT32_MAX;
        if (slot->hash == hash && equal(array, slot->index, key)) return slot->index;
    }
}
static void map_insert(Map *map, uint64_t hash, uint32_t index) {
    if ((map->count + 1) * 2 > map->capacity) {
        Map larger = {calloc(map->capacity ? map->capacity * 2 : 1024, sizeof(Slot)), map->capacity ? map->capacity * 2 : 1024, 0};
        if (!larger.slots) loader_fail("allocation failed");
        memset(larger.slots, 0xff, larger.capacity * sizeof(Slot));
        for (size_t i = 0; i < map->capacity; ++i)
            if (map->slots[i].index != UINT32_MAX) map_insert(&larger, map->slots[i].hash, map->slots[i].index);
        free(map->slots);
        *map = larger;
    }
    size_t i = hash & (map->capacity - 1);
    while (map->slots[i].index != UINT32_MAX) i = (i + 1) & (map->capacity - 1);
    map->slots[i].hash = hash;
    map->slots[i].index = index;
    ++map->count;
}
static int import_equal(const void *array, uint32_t index, const void *key) {
    return same_identity(&((const Import *)array)[index].id, key);
}
static int export_equal(const void *array, uint32_t index, const void *key) {
    return same_identity(&((const Export *)array)[index].id, key);
}
static uint64_t nid_hash(const LoaderIdentity *id) {
    return fnv(fnv(0xcbf29ce484222325u, id->nid, id->nid_length), id->library->name, strlen(id->library->name) + 1);
}
static int nid_equal(const void *array, uint32_t index, const void *key) {
    const LoaderIdentity *a = &((const NidExports *)array)[index].id, *b = key;
    return a->nid_length == b->nid_length && !memcmp(a->nid, b->nid, a->nid_length) && !strcmp(a->library->name, b->library->name);
}

static void ensure_image(Linker *l, uint64_t size) {
    LoaderImage *out = l->out;
    if (size > l->image_capacity) {
        size_t capacity = l->image_capacity ? l->image_capacity : 64u << 20;
        while (capacity < size) capacity *= 2;
        if (!(out->image = realloc(out->image, capacity))) loader_fail("allocation failed");
        l->image_capacity = capacity;
    }
    if (size > out->size) memset(out->image + out->size, 0, size - out->size), out->size = size;
}
/* Only the permission bits: some system modules set high bits in p_flags (libSceAvPlayer: 0x08000006),
 * which are not protections. */
static uint64_t protection(uint32_t flags) { return flags & 7; }
static void add_segment(Linker *l, uint64_t address, uint64_t size, uint32_t flags) {
    LoaderImage *out = l->out;
    out->segments = grow(out->segments, &l->segment_capacity, out->segment_count, sizeof(*out->segments));
    out->segments[out->segment_count++] = (LoaderSegment){address, size, protection(flags)};
}
static uint64_t signed64(__int128 value) {
    if (value < INT64_MIN || value > INT64_MAX) loader_fail("relocation value out of range");
    return (uint64_t)(int64_t)value;
}
static void store64(Linker *l, uint64_t target, __int128 value) {
    if (value < 0 || value > (__int128)UINT64_MAX) loader_fail("relocation value out of range");
    uint64_t v = (uint64_t)value;
    memcpy(l->out->image + target, &v, 8);
}
static void add_reloc(Linker *l, uint64_t target, uint64_t kind, uint64_t value, int64_t addend) {
    LoaderImage *out = l->out;
    out->relocs = grow(out->relocs, &l->reloc_capacity, out->reloc_count, sizeof(*out->relocs));
    out->relocs[out->reloc_count++] = (LoaderReloc){target, kind, value, (uint64_t)addend};
}
static uint64_t add_name(Linker *l, const char *name) {
    LoaderImage *out = l->out;
    if (strlen(name) >= 128) loader_fail("import name too long: %s", name);
    out->names = grow(out->names, &l->name_capacity, out->name_count, sizeof(*out->names));
    memset(out->names[out->name_count], 0, 128);
    strcpy(out->names[out->name_count], name);
    return out->name_count++;
}
static int is_load(const LoaderPh *p) { return p->type == 1 || p->type == PT_SCE_RELRO; }
static int in_loads(const LoaderModule *m, uint64_t address, uint64_t length) {
    for (unsigned i = 0; i < m->phnum; ++i) {
        const LoaderPh *p = &m->ph[i];
        if (is_load(p) && address >= p->vaddr && address - p->vaddr <= p->memsz && length <= p->memsz - (address - p->vaddr))
            return 1;
    }
    return 0;
}
static uint64_t load_size(const LoaderModule *m) {
    uint64_t size = 0;
    int any = 0;
    for (unsigned i = 0; i < m->phnum; ++i)
        if (is_load(&m->ph[i])) {
            any = 1;
            uint64_t end = m->ph[i].vaddr > UINT64_MAX - m->ph[i].memsz ? UINT64_MAX : m->ph[i].vaddr + m->ph[i].memsz;
            if (end > size) size = end;
        }
    if (!any) loader_fail("no loadable segment");
    return size;
}
static void module_segments(const LoaderModule *m, uint64_t base, RuntimeModule *out) {
    out->segment_count = 0;
    for (unsigned i = 0; i < m->phnum && out->segment_count < 4; ++i)
        if (is_load(&m->ph[i]))
            out->segments[out->segment_count++] = (RuntimeModuleSegment){base + m->ph[i].vaddr, m->ph[i].memsz, protection(m->ph[i].flags)};
}
static int exported(const LoaderSymbol *s) {
    return s->section && (s->binding == 1 || s->binding == 2) && s->identity.valid && (s->type == 1 || s->type == 2);
}
static void add_export_entry(RuntimeModule *module, size_t *capacity, const LoaderSymbol *s, uint64_t address) {
    module->exports = grow(module->exports, capacity, module->export_count, sizeof(*module->exports));
    RuntimeModuleExport *e = &module->exports[module->export_count++];
    memset(e->nid, 0, sizeof(e->nid));
    memcpy(e->nid, s->identity.nid, s->identity.nid_length < 16 ? s->identity.nid_length : 16);
    e->address = address;
}

/* Rewrites initial-exec `mov rax, fs:[0]` to GS: glibc owns FS on Linux. */
static uint64_t patch_fs_loads(unsigned char *image, const LoaderModule *m, uint64_t base) {
    uint64_t patched = 0;
    for (unsigned i = 0; i < m->phnum; ++i) {
        const LoaderPh *p = &m->ph[i];
        if (p->type != 1 || !(p->flags & 1)) continue;
        unsigned char *at = image + base + p->vaddr, *end = at + p->filesz;
        while ((at = memmem(at, (size_t)(end - at), fs_load, sizeof(fs_load)))) {
            *at = 0x65;
            ++patched;
            at += sizeof(fs_load);
        }
    }
    return patched;
}

static void note_import(Linker *l, const LoaderIdentity *id, uint64_t index) {
    if (!id->valid) {
        l->none_index = (int64_t)index;
        return;
    }
    uint64_t hash = identity_hash(id);
    uint32_t found = map_find(&l->import_map, hash, import_equal, l->imports, id);
    if (found != UINT32_MAX) {
        l->imports[found].index = index;
        return;
    }
    l->imports = grow(l->imports, &l->import_capacity, l->import_count, sizeof(*l->imports));
    l->imports[l->import_count] = (Import){*id, index};
    map_insert(&l->import_map, hash, (uint32_t)l->import_count++);
    if (!list_has(&l->imported_libraries, id->library->name)) list_add(&l->imported_libraries, id->library->name);
}
/* A module's import joins the table under the eboot's spelling when the eboot names its library
 * and module (the runtime's tables use it), else as NID#library. */
static uint64_t imported(Linker *l, const LoaderSymbol *s) {
    const LoaderIdentity *id = &s->identity;
    if (!id->valid) {
        if (l->none_index < 0) loader_fail("import %s names no library and module", s->name);
        return (uint64_t)l->none_index;
    }
    uint32_t found = map_find(&l->import_map, identity_hash(id), import_equal, l->imports, id);
    if (found != UINT32_MAX) return l->imports[found].index;
    const LoaderId *library = NULL, *module = NULL;
    for (size_t i = 0; i < l->main.library_count; ++i)
        if (l->main.libraries[i].version == id->library->version && !strcmp(l->main.libraries[i].name, id->library->name))
            library = &l->main.libraries[i];
    for (size_t i = 0; i < l->main.module_count; ++i)
        if (l->main.modules[i].version == id->module->version && !strcmp(l->main.modules[i].name, id->module->name))
            module = &l->main.modules[i];
    char name[512];
    if (library && module) snprintf(name, sizeof(name), "%.*s#%s#%s", (int)id->nid_length, id->nid, library->key, module->key);
    else snprintf(name, sizeof(name), "%.*s#%s", (int)id->nid_length, id->nid, id->library->name);
    uint64_t index = add_name(l, name);
    note_import(l, id, index);
    return index;
}

/* The eboot alone: its relocations, its imports in the order they first appear. */
static void prepare(Linker *l) {
    LoaderModule *m = &l->main;
    LoaderImage *out = l->out;
    uint64_t size = load_size(m);
    if (size > IMAGE_LIMIT || !in_loads(m, m->entry, 1)) loader_fail("invalid memory image");
    ensure_image(l, size);
    l->eboot_size = size;
    out->entry = m->entry;
    for (unsigned i = 0; i < m->phnum; ++i) {
        const LoaderPh *p = &m->ph[i];
        if (!is_load(p)) continue;
        if (p->filesz > p->memsz) loader_fail("segment file size exceeds memory size");
        memcpy(out->image + p->vaddr, loader_span(m->elf, m->elf_size, p->offset, p->filesz), p->filesz);
        add_segment(l, p->vaddr, p->memsz, p->flags);
    }
    int64_t *import_of = malloc((m->symbol_count ? m->symbol_count : 1) * sizeof(*import_of));
    if (!import_of) loader_fail("allocation failed");
    for (size_t i = 0; i < m->symbol_count; ++i) import_of[i] = -1;
    for (size_t i = 0; i < m->rela_count; ++i) {
        const LoaderRela *r = &m->relas[i];
        if (!in_loads(m, r->target, 8)) loader_fail("relocation outside mapped segments: %#llx", (unsigned long long)r->target);
        if (r->kind == 8) {
            add_reloc(l, r->target, 0, (uint64_t)r->addend, 0);
            continue;
        }
        if (r->kind != 1 && r->kind != 6 && r->kind != 7) loader_fail("unsupported relocation %u", r->kind);
        if (r->symbol >= m->symbol_count) loader_fail("relocation symbol out of range");
        const LoaderSymbol *s = &m->symbols[r->symbol];
        if (s->section == 0xfff1) store64(l, r->target, (__int128)s->value + r->addend);
        else if (s->section) add_reloc(l, r->target, 0, signed64((__int128)s->value + r->addend), 0);
        else {
            if (import_of[r->symbol] < 0) import_of[r->symbol] = (int64_t)add_name(l, s->name);
            uint64_t kind = s->type == 1 ? 2 : 1;
            if ((kind == 1 && r->addend) || r->addend < 0 || r->addend >= 4096) loader_fail("unsupported import addend");
            add_reloc(l, r->target, kind, (uint64_t)import_of[r->symbol], r->addend);
        }
    }
    /* The eboot's imports are keyed by identity: an identity named twice keeps its first
     * place in the table but binds the later index. */
    uint64_t *symbol_of = malloc((out->name_count ? out->name_count : 1) * sizeof(*symbol_of));
    if (!symbol_of) loader_fail("allocation failed");
    for (size_t i = 0; i < m->symbol_count; ++i)
        if (import_of[i] >= 0) symbol_of[import_of[i]] = i;
    for (uint64_t index = 0; index < out->name_count; ++index) note_import(l, &m->symbols[symbol_of[index]].identity, index);
    free(symbol_of);
    free(import_of);
}

static int compare_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
/* Path order: component by component, so "a/b" sorts before "a-c". */
static int compare_paths(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    for (;;) {
        size_t lx = strcspn(x, "/"), ly = strcspn(y, "/");
        int c = memcmp(x, y, lx < ly ? lx : ly);
        if (c) return c;
        if (lx != ly) return lx < ly ? -1 : 1;
        if (!x[lx] || !y[ly]) return !x[lx] ? (y[ly] ? -1 : 0) : 1;
        x += lx + 1;
        y += ly + 1;
    }
}
static int has_suffix(const char *name, const char *suffix) {
    size_t n = strlen(name), s = strlen(suffix);
    return n >= s && !strcmp(name + n - s, suffix);
}
/* The directory's entries ending in suffix, by name. */
static void glob_directory(const char *directory, const char *suffix, List *names) {
    DIR *dir = opendir(directory);
    if (!dir) return;
    for (struct dirent *e; (e = readdir(dir));)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && has_suffix(e->d_name, suffix)) list_add(names, e->d_name);
    closedir(dir);
    qsort(names->items, names->count, sizeof(*names->items), compare_names);
}
/* Modules the game loads by path (any *.prx or *.sprx outside sce_module and sce_sys, such as
 * Unity's Media/Modules and Media/Plugins), relative to /app0. Linked directories are entered too:
 * mods.py builds a modded game's folder out of links. depth bounds a link loop. */
static void find_loadable(const char *root, const char *relative, List *found, int depth) {
    if (depth > 32) return;
    char path[4096];
    if (relative[0]) join(path, sizeof(path), root, relative);
    else snprintf(path, sizeof(path), "%s", root);
    DIR *dir = opendir(path);
    if (!dir) return;
    for (struct dirent *e; (e = readdir(dir));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!relative[0] && (!strcmp(e->d_name, "sce_module") || !strcmp(e->d_name, "sce_sys"))) continue;
        char child[4096], full[4096];
        if (relative[0]) join(child, sizeof(child), relative, e->d_name);
        else snprintf(child, sizeof(child), "%s", e->d_name);
        join(full, sizeof(full), root, child);
        struct stat st;
        if (stat(full, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            find_loadable(root, child, found, depth + 1);
            continue;
        }
        char extension[8] = "";
        const char *dot = strrchr(e->d_name, '.');
        if (!S_ISREG(st.st_mode) || !dot || dot == e->d_name || !dot[1] || strlen(dot) >= sizeof(extension)) continue;
        for (size_t i = 0; dot[i]; ++i) extension[i] = (char)(dot[i] >= 'A' && dot[i] <= 'Z' ? dot[i] + 32 : dot[i]);
        if (!strcmp(extension, ".prx") || !strcmp(extension, ".sprx")) list_add(found, child);
    }
    closedir(dir);
}

static int linked_stem(const Linker *l, const char *library) {
    char name[256];
    for (size_t i = 0; i < l->module_names.count; ++i) {
        stem(l->module_names.items[i], name, sizeof(name));
        if (!strcmp(name, library)) return 1;
    }
    return 0;
}
static void free_module(LoaderModule *m) {
    free(m->elf); free(m->ph); free(m->dynamic); free(m->libraries); free(m->modules); free(m->symbols); free(m->relas);
}
/* On the console these libraries are clients of system services, which they reach over IPMI
 * (libSceIpmi's IPMI::Client, the kernel's ipmimgr_call, libSceNpCommon's ServiceIpmiClient,
 * libSceAudioOut's IPMI session...): with no service process behind them their initialization
 * fails and they fault on it (libSceAppContent) or stop the game (libSceNpTrophy), so the port's
 * own library serves them instead. The first such import is written to why. */
static int ipmi_client(const char *path, char *why, size_t size) {
    LoaderModule m = {0};
    loader_file = path;
    loader_module(path, &m);
    int found = 0;
    for (size_t i = 0; i < m.symbol_count && !found; ++i) {
        const LoaderIdentity *id = &m.symbols[i].identity;
        if (m.symbols[i].section || !id->valid) continue;
        const char *symbol = runtime_symbol(id->nid);
        if ((symbol && strcasestr(symbol, "ipmi")) || !strcmp(id->library->name, "libSceIpmi")) {
            snprintf(why, size, "%s from %s", symbol ? symbol : "an export", id->library->name);
            found = 1;
        }
    }
    free_module(&m);
    loader_file = NULL;
    return found;
}
/* Bundled and system modules whose library something linked imports and nothing linked provides:
 * the system ones in their order, then the bundled ones no system module shares a name with. */
static void link_wanted(Linker *l) {
    size_t candidates = l->system_stems.count + l->bundled_stems.count;
    for (size_t c = 0; c < candidates; ++c) {
        const char *library = c < l->system_stems.count ? l->system_stems.items[c] : l->bundled_stems.items[c - l->system_stems.count];
        if (c >= l->system_stems.count && list_has(&l->system_stems, library)) continue;
        if (!list_has(&l->imported_libraries, library) || linked_stem(l, library)) continue;
        size_t b = 0;
        while (b < l->bundled_stems.count && strcmp(l->bundled_stems.items[b], library)) ++b;
        if (b < l->bundled_stems.count) {
            list_add(&l->module_names, l->bundled_names.items[b]);
            continue;
        }
        size_t s = 0;
        while (strcmp(l->system_stems.items[s], library)) ++s;
        char why[256];
        if (ipmi_client(l->system_paths.items[s], why, sizeof(why))) {
            printf("Loader: system module %s not linked, the port serves %s: it is a client of a system service (imports %s)\n",
                   strrchr(l->system_paths.items[s], '/') + 1, library, why);
            list_remove(&l->system_stems, s);
            list_remove(&l->system_paths, s);
            --c;
            --candidates;
            continue;
        }
        const char *file = strrchr(l->system_paths.items[s], '/') + 1;
        list_add(&l->module_names, file);
        list_add(&l->from_system, file);
    }
}

static void link_module(Linker *l, const char *filename, LoaderModule *m, RuntimeModule *t, uint64_t base, uint64_t *tls_module,
                        uint64_t *fs_patched) {
    int deferred = list_has(&l->deferred, filename), from_system = list_has(&l->from_system, filename);
    char path[4096], name[256];
    struct stat st;
    if (deferred) join(path, sizeof(path), l->game, filename);
    else {
        char directory[4096];
        join(directory, sizeof(directory), l->game, "sce_module");
        join(path, sizeof(path), directory, filename);
    }
    if (stat(path, &st) && from_system) {
        stem(filename, name, sizeof(name));
        size_t s = 0;
        while (strcmp(l->system_stems.items[s], name)) ++s;
        snprintf(path, sizeof(path), "%s", l->system_paths.items[s]);
    }
    loader_file = path;
    loader_module(path, m);
    uint64_t size = load_size(m);
    if (size > IMAGE_LIMIT || base + size > IMAGE_LIMIT) loader_fail("linked image exceeds probe limit");
    ensure_image(l, base + size);
    unsigned char *image = l->out->image;
    for (unsigned i = 0; i < m->phnum; ++i) {
        const LoaderPh *p = &m->ph[i];
        if (!is_load(p)) continue;
        if (p->filesz > p->memsz) loader_fail("invalid segment size");
        memcpy(image + base + p->vaddr, loader_span(m->elf, m->elf_size, p->offset, p->filesz), p->filesz);
        add_segment(l, base + p->vaddr, p->memsz, p->flags);
    }
    const LoaderPh *tls = loader_find_ph(m, PT_TLS);
    uint64_t module_id = 0;
    /* A PT_TLS of no size needs no module id: ids are what the runtime keeps a block per thread
     * for, and titles with many modules (Atelier Ryza, 11) mostly have empty ones. */
    if (tls && tls->memsz) {
        if (tls->filesz > tls->memsz || !in_loads(m, tls->vaddr, tls->memsz)) loader_fail("unsupported TLS layout");
        module_id = (*tls_module)++;
    }
    for (size_t i = 0; i < m->rela_count; ++i) {
        const LoaderRela *r = &m->relas[i];
        if (!in_loads(m, r->target, 8)) loader_fail("relocation outside image %#llx", (unsigned long long)r->target);
        uint64_t target = base + r->target;
        if (r->kind == 8) {
            add_reloc(l, target, 0, signed64((__int128)base + r->addend), 0);
            continue;
        }
        if (r->kind != 16 && r->kind != 1 && r->kind != 6 && r->kind != 7) loader_fail("unsupported relocation type %u", r->kind);
        if (r->symbol >= m->symbol_count) loader_fail("relocation symbol out of range");
        const LoaderSymbol *s = &m->symbols[r->symbol];
        if (r->kind == 16) {
            int own = s->name[0] ? s->type == 6 && s->section : s->type == 3;
            if (!own || r->addend || !module_id) loader_fail("unsupported external TLS relocation");
            store64(l, target, module_id);
        } else if (s->section == 0xfff1) store64(l, target, (__int128)s->value + r->addend);
        else if (s->section) add_reloc(l, target, 0, signed64((__int128)base + s->value + r->addend), 0);
        else if (!s->identity.valid && s->binding == 2) store64(l, target, 0); /* weak undefined (module_start/stop) */
        else {
            uint64_t kind = s->type == 1 ? 2 : 1;
            if ((kind == 1 && r->addend) || r->addend < 0 || r->addend >= 4096) loader_fail("unsupported import addend");
            add_reloc(l, target, kind, imported(l, s), r->addend);
        }
    }
    size_t export_capacity = 0;
    for (size_t i = 0; i < m->symbol_count; ++i) {
        const LoaderSymbol *s = &m->symbols[i];
        if (!exported(s)) continue;
        if (!in_loads(m, s->value, s->size ? s->size : 1)) loader_fail("export outside load segments");
        uint64_t hash = identity_hash(&s->identity), kind = (s->type == 1 ? 2 : 1) + (deferred ? 4 : from_system ? 2 : 0);
        uint32_t found = map_find(&l->export_map, hash, export_equal, l->exports, &s->identity);
        /* Modules the game loads by path (a PS2 emulator's per-disc AOT modules) may export the same
         * names: the game reaches them through sceKernelDlsym on its own handle, so none of them
         * binds imports at link time. */
        if (found != UINT32_MAX) {
            if (!deferred || !list_has(&l->deferred, l->exports[found].file)) loader_fail("ambiguous export %s", s->name);
            l->exports[found].handle_only = 1;
        } else {
            l->exports = grow(l->exports, &l->export_capacity, l->export_count, sizeof(*l->exports));
            l->exports[l->export_count] = (Export){s->identity, 0, 0, NULL, 0};
            map_insert(&l->export_map, hash, found = (uint32_t)l->export_count++);
        }
        l->exports[found].address = base + s->value;
        l->exports[found].kind = kind;
        l->exports[found].file = filename;
        uint64_t nid = nid_hash(&s->identity);
        uint32_t same = map_find(&l->nid_map, nid, nid_equal, l->nids, &s->identity);
        if (same != UINT32_MAX) ++l->nids[same].count;
        else {
            l->nids = grow(l->nids, &l->nid_capacity, l->nid_count, sizeof(*l->nids));
            l->nids[l->nid_count] = (NidExports){s->identity, base + s->value, kind, 1};
            map_insert(&l->nid_map, nid, (uint32_t)l->nid_count++);
        }
        add_export_entry(t, &export_capacity, s, base + s->value);
    }
    *fs_patched += patch_fs_loads(image, m, base);
    if (strlen(filename) >= 128) loader_fail("path too long");
    strcpy(t->name, filename);
    uint64_t init = 0;
    loader_tag(m, 12, &init);
    t->base = base; t->size = size; t->init = base + init;
    t->tls_address = tls ? base + tls->vaddr : 0;
    t->tls_memsz = tls ? tls->memsz : 0;
    t->tls_filesz = tls ? tls->filesz : 0;
    t->tls_module = module_id;
    t->deferred = (uint64_t)deferred;
    loader_eh_frame(m, base, t);
    module_segments(m, base, t);
}

void loader_load_game(const LoaderOptions *options, LoaderImage *out) {
    static const char *const default_modules[] = {"libc.prx", "libSceFios2.prx"};
    Linker l = {.game = options->game, .out = out, .none_index = -1};
    char path[4096];
    memset(out, 0, sizeof(*out));
    join(path, sizeof(path), l.game, "eboot.bin");
    loader_file = path;
    loader_module(path, &l.main);
    prepare(&l);
    join(path, sizeof(path), l.game, "sce_module/libc.prx");
    out->capabilities = (uint64_t)loader_init_env_is_ret(path);
    loader_file = NULL;

    uint64_t base = (out->size + 65535) & ~(uint64_t)65535, tls_module = 2;
    uint64_t fs_patched = patch_fs_loads(out->image, &l.main, 0);
    for (size_t i = 0; i < 2; ++i) list_add(&l.module_names, default_modules[i]);
    if (options->system_modules && options->system_modules[0]) {
        List hle = {0}, lle = {0}, files = {0};
        split_names(options->hle, &hle);
        split_names(options->lle, &lle);
        glob_directory(options->system_modules, ".sprx", &files);
        for (size_t i = 0; i < files.count; ++i) {
            char name[256];
            stem(files.items[i], name, sizeof(name));
            if (listed(never_system, sizeof(never_system) / sizeof(*never_system), name) || list_has(&hle, name) ||
                (listed(port_implements, sizeof(port_implements) / sizeof(*port_implements), name) && !list_has(&lle, name)))
                continue;
            join(path, sizeof(path), options->system_modules, files.items[i]);
            list_add(&l.system_stems, name);
            list_add(&l.system_paths, path);
        }
    }
    List bundled = {0};
    join(path, sizeof(path), l.game, "sce_module");
    glob_directory(path, ".prx", &bundled);
    for (size_t i = 0; i < bundled.count; ++i) {
        char name[256];
        stem(bundled.items[i], name, sizeof(name));
        list_add(&l.bundled_stems, name);
        list_add(&l.bundled_names, bundled.items[i]);
    }
    link_wanted(&l);
    find_loadable(l.game, "", &l.deferred, 0);
    qsort(l.deferred.items, l.deferred.count, sizeof(*l.deferred.items), compare_paths);
    for (size_t i = 0; i < l.deferred.count; ++i) list_add(&l.module_names, l.deferred.items[i]);

    LoaderModule *linked = NULL;
    RuntimeModule *table = NULL;
    size_t linked_capacity = 0, table_capacity = 0, count = 0;
    for (size_t index = 0; index < l.module_names.count; ++index) {
        linked = grow(linked, &linked_capacity, count, sizeof(*linked));
        table = grow(table, &table_capacity, count, sizeof(*table));
        memset(&table[count], 0, sizeof(*table));
        link_module(&l, l.module_names.items[index], &linked[count], &table[count], base, &tls_module, &fs_patched);
        base = (base + table[count].size + 65535) & ~(uint64_t)65535;
        ++count;
        /* Linking a module can import a library only another bundled or system module provides. */
        link_wanted(&l);
    }
    loader_file = NULL;

    size_t binding_capacity = 0, unresolved = l.none_index >= 0;
    for (size_t i = 0; i < l.import_count; ++i) {
        const Import *import = &l.imports[i];
        uint32_t found = map_find(&l.export_map, identity_hash(&import->id), export_equal, l.exports, &import->id);
        uint64_t address = 0, kind = 0;
        if (found != UINT32_MAX && !l.exports[found].handle_only) address = l.exports[found].address, kind = l.exports[found].kind;
        else if (!strcmp(import->id.library->name, "libSceLibcInternal")) {
            LoaderIdentity libc = import->id;
            LoaderId library = {"", "libc", 0};
            libc.library = &library;
            uint32_t same = map_find(&l.nid_map, nid_hash(&libc), nid_equal, l.nids, &libc);
            if (same != UINT32_MAX && l.nids[same].count == 1) address = l.nids[same].address, kind = l.nids[same].kind;
        }
        if (!kind) {
            ++unresolved;
            continue;
        }
        out->bindings = grow(out->bindings, &binding_capacity, out->binding_count, sizeof(*out->bindings));
        out->bindings[out->binding_count++] = (LoaderBinding){import->index, address, kind};
    }

    const LoaderPh *tls = loader_find_ph(&l.main, PT_TLS);
    if (tls) {
        if (tls->filesz > tls->memsz || tls->memsz > 1024 * 1024 || tls->vaddr > l.eboot_size || tls->filesz > l.eboot_size - tls->vaddr)
            loader_fail("unsupported eboot TLS layout");
        out->main_tls[0] = tls->vaddr; out->main_tls[1] = tls->filesz; out->main_tls[2] = tls->memsz; out->main_tls[3] = tls->align;
    }
    const LoaderPh *procparam = loader_find_ph(&l.main, PT_SCE_PROCPARAM);
    if (!procparam) loader_fail("eboot.bin has no PT_SCE_PROCPARAM");
    out->procparam = procparam->vaddr;
    if (!(out->all_modules = calloc(count + 1, sizeof(*out->all_modules)))) loader_fail("allocation failed");
    RuntimeModule *eboot = &out->all_modules[0];
    strcpy(eboot->name, "eboot.bin");
    loader_eh_frame(&l.main, 0, eboot);
    module_segments(&l.main, 0, eboot);
    size_t export_capacity = 0;
    /* Unity's IL2CPP code asks the eboot (sceKernelDlsym handle 0) for the player's functions. */
    for (size_t i = 0; i < l.main.symbol_count; ++i)
        if (exported(&l.main.symbols[i])) add_export_entry(eboot, &export_capacity, &l.main.symbols[i], l.main.symbols[i].value);
    if (count) memcpy(out->all_modules + 1, table, count * sizeof(*table));
    out->module_count = count;

    printf("Linked modules:");
    for (size_t i = 0; i < count; ++i)
        printf("%s %s@%#llx%s", i ? "," : "", table[i].name, (unsigned long long)table[i].base,
               table[i].deferred ? " (loaded by the game)" : "");
    printf("; %llu native bindings, %zu imports left to the host runtime, fs->gs patched=%llu\n",
           (unsigned long long)out->binding_count, unresolved, (unsigned long long)fs_patched);
    for (size_t i = 0; i < count; ++i) free_module(&linked[i]);
    free_module(&l.main);
    free(linked); free(table); free(l.imports); free(l.exports); free(l.nids);
    free(l.import_map.slots); free(l.export_map.slots); free(l.nid_map.slots);
}
