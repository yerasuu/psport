/* Game loading: the eboot and every module it links, placed in one image at offsets from 0 the way
 * probe.c maps it. loader_load_game reads the game's folder (loader_self.c parses the SELF/ELF
 * files, loader_link.c lays them out and binds them). */
#ifndef LOADER_H
#define LOADER_H
#include <stddef.h>
#include <stdint.h>
#include "runtime.h"
#include "gpu/bbgpu.h"

typedef struct { uint64_t address, size, flags; } LoaderSegment;
/* kind 0: value is an image offset; 1/2: value is the import index of a function/data import. */
typedef struct { uint64_t target, kind, value, addend; } LoaderReloc;
typedef struct { uint64_t index, address, kind; } LoaderBinding;
typedef struct {
    uint64_t size, entry, capabilities, procparam, main_tls[4];
    /* all_modules[0] is the eboot; module_count counts the modules after it. */
    RuntimeModule *all_modules;
    uint64_t module_count;
    LoaderSegment *segments;
    uint64_t segment_count;
    LoaderReloc *relocs;
    uint64_t reloc_count;
    char (*names)[128];
    uint64_t name_count;
    LoaderBinding *bindings;
    uint64_t binding_count;
    unsigned char *image; /* malloc'd, size bytes */
    /* The eboot's PT_LOAD segments, where patches may write. */
    LoaderSegment *eboot_loads;
    uint64_t eboot_load_count;
} LoaderImage;
/* hle and lle: comma-separated library names, as run.sh passes PS_HLE and PS_LLE. sz_executable:
 * the host path of the main executable when it is not the game's eboot.bin (sceSystemServiceLoadExec). */
typedef struct { const char *game, *system_modules, *hle, *lle, *sz_executable; } LoaderOptions;

void loader_load_game(const LoaderOptions *options, LoaderImage *out);
/* Loose-file mods (loader_mods.c). pp_serials: the TITLE_IDs a mod's wrapper folder may be named
 * after; pp_folders: the top-level folders of dvdroot_ps4 a mod may consist of without it. */
typedef struct {
    const char *sz_mods_dir, *sz_config, *sz_out_dir;
    const char *const *pp_serials;
    size_t c_serials;
    const char *const *pp_folders;
    size_t c_folders;
} LoaderModOptions;
/* The folder to load, malloc'd: a view of the game with the enabled mods linked in, made under
 * sz_out_dir and removed once psport ends, or the game's real path when no mod has a file for it.
 * An error ends the process. */
char *loader_mods_view(const char *game, const LoaderModOptions *options);
/* --list-mods: the mod folders under sz_mods_dir, as a JSON array of names. game may be NULL: then
 * only layouts with dvdroot_ps4 (directly, under app0 or a wrapper folder) are known. */
void loader_mods_list(const char *game, const LoaderModOptions *options);
/* mods.json and patches.json (loader_json.c): the string arrays of the named members of a JSON
 * object. is_list stays 0 for a member whose value is not an array of strings only. */
typedef struct {
    const char *sz_key;
    char **pp_items;
    size_t c_items;
    int is_present, is_list;
} LoaderJsonList;
/* 0 when the file is not a JSON object (the lists are then empty). */
int loader_json_lists(const char *sz_path, LoaderJsonList *p_lists, size_t c_lists);
int loader_json_has(const LoaderJsonList *p_list, const char *sz_item);
void loader_json_free(LoaderJsonList *p_lists, size_t c_lists);
/* BbGameFiles.pfn_eboot_sha256 (loader_hash.c). */
void loader_eboot_sha256(const char *sz_game, unsigned char *p_out_digest);
/* shadPS4/GoldHEN XML patches (loader_patches.c). Their addresses are the eboot's at 0x400000. */
typedef struct { uint64_t off_image, cb_data; unsigned char *p_data; } LoaderPatchWrite;
struct BbPatchList {
    LoaderPatchWrite *p_writes;
    size_t c_writes, c_capacity;
    const LoaderSegment *p_loads; /* the eboot's PT_LOAD segments: a write must fit in one */
    size_t c_loads;
};
void loader_patch_services(BbPatchList *list, BbPatchServices *out);
/* The patches in the *.xml files of sz_dir for a game: files whose <ID>s name one of its serials (or none),
 * entries for its sz_version and eboot.bin; sz_excluded (a profile's own file) is left out. */
typedef struct {
    const char *sz_dir, *sz_excluded, *sz_version;
    const char *const *pp_serials;
    size_t c_serials;
} LoaderPatchFolder;
/* Appends the selected ones: patches.json's (sz_config) "enabled", else each entry's isEnabled
 * unless "disabled" names it. A patch with an unsupported line is skipped whole, and said. */
void loader_patches_external(BbPatchList *list, const LoaderPatchFolder *folder, const char *sz_config);
/* --list-patches: what a launcher shows of them, as JSON. */
void loader_patches_list(const LoaderPatchFolder *folder);

/* Shared by the loader's files. */
extern const char *loader_file;
__attribute__((noreturn, format(printf, 1, 2))) void loader_fail(const char *format, ...);
unsigned char *loader_read_file(const char *path, size_t *size);
const unsigned char *loader_span(const unsigned char *data, size_t size, uint64_t offset, uint64_t length);
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } LoaderPh;
typedef struct { uint64_t tag, value; } LoaderDyn;
typedef struct { char key[4]; const char *name; unsigned version; } LoaderId;
/* An export's or import's identity: NID, library and module (name and version); nid is the
 * symbol name up to its first '#'. valid is 0 for names without exactly two '#'. */
typedef struct {
    int valid;
    const char *nid;
    size_t nid_length;
    const LoaderId *library, *module;
} LoaderIdentity;
typedef struct {
    const char *name;
    unsigned type, binding, section;
    uint64_t value, size;
    LoaderIdentity identity;
} LoaderSymbol;
typedef struct { uint64_t target; uint32_t kind, symbol; int64_t addend; } LoaderRela;
typedef struct {
    unsigned char *elf;
    size_t elf_size;
    uint64_t entry;
    LoaderPh *ph;
    unsigned phnum;
    LoaderDyn *dynamic;
    size_t dynamic_count;
    const unsigned char *strings;
    size_t strings_size;
    LoaderId *libraries, *modules;
    size_t library_count, module_count;
    LoaderSymbol *symbols;
    size_t symbol_count;
    LoaderRela *relas;
    size_t rela_count;
} LoaderModule;
void loader_parse_self(const unsigned char *data, size_t size, LoaderModule *m);
void loader_module(const char *path, LoaderModule *m);
int loader_tag(const LoaderModule *m, uint64_t tag, uint64_t *value);
const LoaderPh *loader_find_ph(const LoaderModule *m, uint32_t type);
int loader_init_env_is_ret(const char *libc_path);
void loader_eh_frame(const LoaderModule *m, uint64_t base, RuntimeModule *out);
void loader_encode_id(unsigned value, char key[4]);
#endif
