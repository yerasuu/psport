/* Game loading: the eboot and every module it links, placed in one image at offsets from 0 the way
 * main.c maps it. loader_load_game reads the game's folder (loader_self.c parses the SELF/ELF
 * files, loader_link.c lays them out and binds them). */
#ifndef LOADER_H
#define LOADER_H
#include <stddef.h>
#include <stdint.h>
#include "runtime.h"
#include "gpu/psport_gpu.h"

typedef struct { uint64_t va_segment, cb_segment, flags; } LoaderSegment;
/* kind 0: value is an image offset; 1/2: value is the import index of a function/data import. */
typedef struct { uint64_t target, kind, value, addend; } LoaderReloc;
typedef struct { uint64_t i_binding, va_binding, kind; } LoaderBinding;
typedef struct {
    uint64_t cb_image, entry, capabilities, procparam, main_tls[4];
    /* p_all_modules[0] is the eboot; c_modules counts the modules after it. */
    RuntimeModule *p_all_modules;
    uint64_t c_modules;
    LoaderSegment *p_segments;
    uint64_t c_segments;
    LoaderReloc *p_relocs;
    uint64_t c_relocs;
    char (*p_names)[128];
    uint64_t c_names;
    LoaderBinding *p_bindings;
    uint64_t c_bindings;
    unsigned char *p_image; /* malloc'd, size bytes */
    /* The eboot's PT_LOAD segments, where patches may write. */
    LoaderSegment *p_eboot_loads;
    uint64_t c_eboot_loads;
} LoaderImage;
/* hle and lle: comma-separated library names, as run.sh passes PS_HLE and PS_LLE. sz_executable:
 * the host path of the main executable when it is not the game's eboot.bin (sceSystemServiceLoadExec). */
typedef struct { const char *sz_game, *sz_system_modules, *sz_hle, *sz_lle, *sz_executable; } LoaderOptions;

void loader_load_game(const LoaderOptions *p_options, LoaderImage *p_out);
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
char *loader_mods_view(const char *sz_game, const LoaderModOptions *p_options);
/* --list-mods: the mod folders under sz_mods_dir, as a JSON array of names. game may be NULL: then
 * only layouts with dvdroot_ps4 (directly, under app0 or wrapper folders) are known. Folders that
 * are not mods are named on stderr, with why. */
void loader_mods_list(const char *sz_game, const LoaderModOptions *p_options);
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
/* PsGameFiles.pfn_eboot_sha256 (loader_hash.c). */
void loader_eboot_sha256(const char *sz_game, unsigned char *p_out_digest);
/* shadPS4/GoldHEN XML patches (loader_patches.c). Their addresses are the eboot's at 0x400000. */
typedef struct { uint64_t off_image, cb_data; unsigned char *p_data; } LoaderPatchWrite;
/* A profile patch's writes in the list, so that an external patch over any of its bytes replaces it whole. */
typedef struct { char sz_name[128]; size_t i_first, c_writes; } LoaderPatchGroup;
struct PsPatchList {
    LoaderPatchWrite *p_writes;
    size_t c_writes, c_capacity;
    const LoaderSegment *p_loads; /* the eboot's PT_LOAD segments: a write must fit in one */
    size_t c_loads;
    LoaderPatchGroup *p_groups;
    size_t c_groups, c_group_capacity;
};
void loader_patch_services(PsPatchList *p_list, PsPatchServices *p_out);
/* The patches in the *.xml files of sz_dir for a game: files whose <ID>s name one of its serials (or none),
 * entries for its sz_version and eboot.bin; sz_excluded (a profile's own file) is left out, and so is
 * each patch pfn_refusal gives a reason for (by its Name). pfn_replaced_consequence: what the game
 * loses when an external patch replaces the profile patch of that name, or NULL. */
typedef struct {
    const char *sz_dir, *sz_excluded, *sz_version;
    const char *const *pp_serials;
    size_t c_serials;
    const char *(*pfn_refusal)(const char *sz_name);
    const char *(*pfn_replaced_consequence)(const char *sz_name);
} LoaderPatchFolder;
/* Appends the selected ones: patches.json's (sz_config) "enabled", else each entry's isEnabled
 * unless "disabled" names it. A patch with an unsupported line is skipped whole, and said. One that
 * writes any byte a profile patch writes removes that profile patch whole, and says so. */
void loader_patches_external(PsPatchList *p_list, const LoaderPatchFolder *p_folder, const char *sz_config);
/* --list-patches: what a launcher shows of them, as JSON. */
void loader_patches_list(const LoaderPatchFolder *p_folder);

/* Shared by the loader's files. */
extern const char *g_sz_loader_file;
__attribute__((noreturn, format(printf, 1, 2))) void loader_fail(const char *sz_format, ...);
unsigned char *loader_read_file(const char *sz_path, size_t *p_size);
const unsigned char *loader_span(const unsigned char *p_data, size_t cb_data, uint64_t off_data, uint64_t length);
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } LoaderPh;
typedef struct { uint64_t tag, value; } LoaderDyn;
typedef struct { char sz_key[4]; const char *sz_name; unsigned version; } LoaderId;
/* An export's or import's identity: NID, library and module (name and version); nid is the
 * symbol name up to its first '#'. valid is 0 for names without exactly two '#'. */
typedef struct {
    int is_valid;
    const char *sz_nid;
    size_t nid_length;
    const LoaderId *p_library, *p_module;
} LoaderIdentity;
typedef struct {
    const char *sz_name;
    unsigned type, binding, section;
    uint64_t value, cb_symbol;
    LoaderIdentity identity;
} LoaderSymbol;
typedef struct { uint64_t target; uint32_t kind, symbol; int64_t addend; } LoaderRela;
typedef struct {
    unsigned char *p_elf;
    size_t cb_elf;
    uint64_t entry;
    LoaderPh *p_ph;
    unsigned phnum;
    LoaderDyn *p_dynamic;
    size_t c_dynamic_entries;
    const unsigned char *p_strings;
    size_t cb_strings;
    LoaderId *p_libraries, *p_modules;
    size_t c_libraries, c_modules;
    LoaderSymbol *p_symbols;
    size_t c_symbols;
    LoaderRela *p_relas;
    size_t c_relas;
} LoaderModule;
void loader_parse_self(const unsigned char *p_data, size_t cb_data, LoaderModule *p_module);
void loader_module(const char *sz_path, LoaderModule *p_module);
int loader_tag(const LoaderModule *p_module, uint64_t tag, uint64_t *p_value);
const LoaderPh *loader_find_ph(const LoaderModule *p_module, uint32_t type);
int loader_init_env_is_ret(const char *sz_libc_path);
void loader_eh_frame(const LoaderModule *p_module, uint64_t base, RuntimeModule *p_out);
void loader_encode_id(unsigned value, char sz_key[4]);
#endif
