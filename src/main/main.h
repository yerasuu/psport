// SPDX-License-Identifier: GPL-2.0-or-later
/* What the launch's parts share: main.c maps the image and binds its imports (bind.c), the import
 * traps and the fault handlers (signals.c) read it, and game_info.c and cli.c answer for the game. */
#ifndef MAIN_H
#define MAIN_H
#include <stddef.h>
#include <stdint.h>
#include "runtime.h"
#include "runtime_quirks.h"
#include "loader.h"
#include "gpu/psport_gpu.h"

int main(int argc, char **argv);
/* Hidden so that these generic names stay out of the dynamic symbol table -rdynamic exports: they
 * would interpose into host libraries and change the symbols dladdr names in fault reports. */
#pragma GCC visibility push(hidden)
typedef LoaderSegment Segment;
typedef LoaderReloc Reloc;
extern char (*g_p_names)[128];
extern uint64_t g_c_imports;
extern unsigned char *g_p_image;
extern size_t g_cb_page;
extern RuntimeModule *g_p_modules;
extern uint64_t g_c_modules;
extern int g_has_entered_game;
extern int g_is_gpu_enabled;
enum { THUNK_SIZE = 96 };

void fail(const char *sz_message);
size_t round_page(size_t cb_page);
void *allocate(size_t cb_allocation);
void protect(void *p_range, size_t cb_range, unsigned flags);

/* What psport knows of the game for its title and the external patches: param.sfo's TITLE_ID
 * and APP_VER, and the TITLE_IDs of the same executable. */
typedef struct {
    /* sz_patch_file: the title's built-in patch file found, "" without one; sz_patch_folder: the
     * folder the launcher gave for it, "" for none. */
    char sz_serial[16], sz_version[16], sz_patch_file[4096], sz_patch_folder[4096];
    const char *serials[64];
    size_t c_serials;
    PsGameFiles files;
    PsTitleInfo title;
} GameInfo;
void game_info_of(const char *sz_game_dir, const char *sz_serial, const char *sz_version, GameInfo *p_out_info);
void game_info(const char *sz_game_dir, GameInfo *p_out_info);
void builtin_patches_configure(const char *sz_dir);
/* Prints where the title's built-in patch file was found or why it was not: 0 when the title has
 * one and it is missing, so its built-in patches are skipped. */
int announce_builtin_patch_file(const GameInfo *p_info);
int sfo_value(const char *sz_path, const char *sz_key, char *sz_text, size_t cb_text, uint32_t *p_number);
void configure_content(const char *sz_sfo);
LoaderPatchFolder patch_folder(const char *sz_dir, const GameInfo *p_info);
/* What the quirk registry decides on from the game's title; traits are the caller's. */
RuntimeQuirkLaunch quirk_launch(const GameInfo *p_info, uint64_t traits);
void mod_options(const GameInfo *p_info, LoaderModOptions *p_options, const char **pp_folders, size_t c_capacity);
void accept_deprecated_names(char **pp_argv);
/* The options that answer a launcher's question and exit before any game is loaded (--check-game,
 * --has-presets, --list-patches, --list-mods, --list-quirks, --list-traits, --online-overrides): their exit status, -1 when argv asks none of them. */
int answer_query(int argc, char **argv);
int mapped(Segment *p_segments, uint64_t c_segments, uint64_t va_range, uint64_t cb_range);
void bind_imports(Segment *p_segments, uint64_t c_segments, Reloc *p_relocs, uint64_t c_relocs, int native_libc,
                  const uint64_t *p_bindings, const uint64_t *p_binding_kinds, unsigned char **pp_out_traps,
                  unsigned char **pp_out_thunks);
void apply_patches(const PsPatchList *p_patches, Segment *p_segments, uint64_t c_segments, const Reloc *p_relocs, uint64_t c_relocs);
void install_fault_handlers(unsigned timeout_seconds);
void install_shutdown_signals(void);
#pragma GCC visibility pop
#endif
