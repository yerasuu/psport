/* Game loading: the eboot and every module it links, placed in one image at offsets from 0 the way
 * probe.c maps it. loader_load_game reads the game's folder (loader_self.c parses the SELF/ELF
 * files, loader_link.c lays them out and binds them). */
#ifndef LOADER_H
#define LOADER_H
#include <stddef.h>
#include <stdint.h>
#include "runtime.h"

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
} LoaderImage;
/* hle and lle: comma-separated library names, as run.sh passes PS_HLE and PS_LLE. */
typedef struct { const char *game, *system_modules, *hle, *lle; } LoaderOptions;

void loader_load_game(const LoaderOptions *options, LoaderImage *out);

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
