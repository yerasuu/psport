/* The middleware a game is built with, read from its files without running any of its code: the
 * eboot's module and library names, the file names of the modules it ships, its data files'
 * names and extensions. Informational for now; the quirk registry will select entries by them. */
#ifndef LOADER_TRAITS_H
#define LOADER_TRAITS_H
#include <stdint.h>
#include "loader.h"

typedef enum {
    LOADER_TRAIT_UNITY = 1u << 0,
    LOADER_TRAIT_UNITY_IL2CPP = 1u << 1,
    LOADER_TRAIT_UNITY_MONO = 1u << 2,
    LOADER_TRAIT_UNREAL = 1u << 3,
    LOADER_TRAIT_PS2_EMULATOR = 1u << 4,
    LOADER_TRAIT_CRI_ADX2 = 1u << 5,
    LOADER_TRAIT_CRI_SOFDEC = 1u << 6,
    LOADER_TRAIT_CRI_FILE_SYSTEM = 1u << 7,
    LOADER_TRAIT_FMOD = 1u << 8,
    LOADER_TRAIT_WWISE = 1u << 9,
    LOADER_TRAIT_HAVOK = 1u << 10,
} LoaderTrait;
enum { LOADER_TRAIT_COUNT = 11 };

/* p_eboot: the main executable as loader_module parsed it, its whole ELF still held. */
uint32_t loader_detect_traits(const char *sz_game, const LoaderModule *p_eboot);
/* i_trait counts bits from 0; NULL from LOADER_TRAIT_COUNT on. The id is the stable key for
 * launchers and configuration, the name is for logs. */
const char *loader_trait_id(unsigned i_trait);
const char *loader_trait_name(unsigned i_trait);
/* At launch (loader_load_game): detects the game's traits once, keeps them for loader_game_traits
 * and logs "Middleware: <names>" or "Middleware: none detected". */
void loader_record_game_traits(const char *sz_game, const LoaderModule *p_eboot);
/* 0 before loader_record_game_traits ran. */
uint32_t loader_game_traits(void);
/* --list-traits: a JSON array of {"id", "name"} objects. */
void loader_traits_list(uint32_t traits);
#endif
