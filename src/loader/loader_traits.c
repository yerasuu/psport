/* Middleware traits from the game's files. Each rule names what its engine or library leaves in
 * every build: module names its toolchain gives, file names and extensions its runtime opens. Data
 * that only some titles use the same way stays out, so a trait is never guessed from one game. */
#define _GNU_SOURCE
#include "loader_traits.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const struct {
    const char *sz_id, *sz_name;
} s_traits[LOADER_TRAIT_COUNT] = {
    {"unity", "Unity"},
    {"il2cpp", "IL2CPP"},
    {"mono", "Mono"},
    {"unreal", "Unreal Engine"},
    {"ps2-emulator", "PS2 Classics emulator"},
    {"cri-adx2", "CRI ADX2"},
    {"cri-sofdec", "CRI Sofdec"},
    {"cri-file-system", "CRI File System"},
    {"fmod", "FMOD"},
    {"wwise", "Wwise"},
    {"havok", "Havok"},
};
static uint32_t s_game_traits;

const char *loader_trait_id(unsigned i_trait) { return i_trait < LOADER_TRAIT_COUNT ? s_traits[i_trait].sz_id : NULL; }
const char *loader_trait_name(unsigned i_trait) { return i_trait < LOADER_TRAIT_COUNT ? s_traits[i_trait].sz_name : NULL; }
uint32_t loader_game_traits(void) { return s_game_traits; }

static int has_prefix(const char *sz_text, const char *sz_prefix) { return !strncasecmp(sz_text, sz_prefix, strlen(sz_prefix)); }
static int has_suffix(const char *sz_text, const char *sz_suffix) {
    size_t length = strlen(sz_text), suffix_length = strlen(sz_suffix);
    return length >= suffix_length && !strcasecmp(sz_text + length - suffix_length, sz_suffix);
}

/* Module and library names, the eboot's own and those it needs, and stems of shipped modules.
 * Unity's player is PS4Player (PS4Player_IL2CPP for IL2CPP builds, which also need the game's
 * Il2CppUserAssemblies); Unreal Build Tool names the executable <Project>-PS4-<Configuration>. */
static uint32_t name_traits(const char *sz_name) {
    static const char *const s_unreal_configurations[] = {"-PS4-Shipping", "-PS4-Test", "-PS4-Development",
                                                          "-PS4-DebugGame", "-PS4-Debug"};
    uint32_t traits = 0;
    if (has_prefix(sz_name, "PS4Player") || !strcasecmp(sz_name, "libkernel_unity")) traits |= LOADER_TRAIT_UNITY;
    if (!strcasecmp(sz_name, "PS4Player_IL2CPP") || !strcasecmp(sz_name, "Il2CppUserAssemblies"))
        traits |= LOADER_TRAIT_UNITY | LOADER_TRAIT_UNITY_IL2CPP;
    for (size_t i = 0; i < sizeof(s_unreal_configurations) / sizeof(*s_unreal_configurations); ++i)
        if (has_suffix(sz_name, s_unreal_configurations[i])) traits |= LOADER_TRAIT_UNREAL;
    if (has_prefix(sz_name, "libfmod")) traits |= LOADER_TRAIT_FMOD;
    return traits;
}

static uint32_t eboot_traits(const LoaderModule *p_eboot) {
    uint32_t traits = 0;
    for (size_t i = 0; i < p_eboot->c_libraries; ++i) traits |= name_traits(p_eboot->p_libraries[i].sz_name);
    for (size_t i = 0; i < p_eboot->c_modules; ++i) traits |= name_traits(p_eboot->p_modules[i].sz_name);
    /* Havok links statically; its class names stay in the executable's RTTI. Havok Script, a Lua VM
     * some engines ship without the physics, has none of them. */
    if (memmem(p_eboot->p_elf, p_eboot->cb_elf, "hkBaseObject", 12)) traits |= LOADER_TRAIT_HAVOK;
    return traits;
}

/* sz_relative: the file's path under the game's folder, ending in sz_name. */
static uint32_t file_traits(const char *sz_relative, const char *sz_name) {
    const char *sz_dot = strrchr(sz_name, '.');
    const char *sz_extension = sz_dot && sz_dot != sz_name ? sz_dot + 1 : "";
    size_t off_name = strlen(sz_relative) - strlen(sz_name);
    uint32_t traits = 0;
    if (!strcasecmp(sz_extension, "prx") || !strcasecmp(sz_extension, "sprx")) {
        char sz_stem[256];
        snprintf(sz_stem, sizeof(sz_stem), "%.*s", (int)(sz_dot - sz_name), sz_name);
        traits |= name_traits(sz_stem);
        if (!strcasecmp(sz_stem, "PS4Util")) traits |= LOADER_TRAIT_UNITY;
        /* The PS2 emulator's ahead-of-time translations of a disc's EE code: <disc>_eeaotv1.prx. */
        if (strcasestr(sz_stem, "eeaot")) traits |= LOADER_TRAIT_PS2_EMULATOR;
    }
    if (!strcasecmp(sz_relative, "uecommandline.txt")) traits |= LOADER_TRAIT_UNREAL;
    if (!strcasecmp(sz_extension, "pak") && off_name >= 5 && !strncasecmp(sz_relative + off_name - 5, "paks/", 5) &&
        (off_name == 5 || sz_relative[off_name - 6] == '/'))
        traits |= LOADER_TRAIT_UNREAL;
    if (!strcasecmp(sz_relative, "Media/Managed/mscorlib.dll")) traits |= LOADER_TRAIT_UNITY | LOADER_TRAIT_UNITY_MONO;
    if (!strcasecmp(sz_extension, "acb") || !strcasecmp(sz_extension, "awb") || !strcasecmp(sz_extension, "hca") ||
        !strcasecmp(sz_extension, "adx"))
        traits |= LOADER_TRAIT_CRI_ADX2;
    if (!strcasecmp(sz_extension, "usm")) traits |= LOADER_TRAIT_CRI_SOFDEC;
    if (!strcasecmp(sz_extension, "cpk")) traits |= LOADER_TRAIT_CRI_FILE_SYSTEM;
    /* FMOD Ex's event projects and sound banks, and FMOD Studio's strings bank. */
    if (!strcasecmp(sz_extension, "fev") || !strcasecmp(sz_extension, "fsb") || has_suffix(sz_name, ".strings.bank"))
        traits |= LOADER_TRAIT_FMOD;
    /* Wwise's sound engine loads Init.bnk before any other bank. */
    if (!strcasecmp(sz_name, "Init.bnk")) traits |= LOADER_TRAIT_WWISE;
    if (!strcasecmp(sz_extension, "hkx")) traits |= LOADER_TRAIT_HAVOK;
    return traits;
}

/* Linked directories are entered too, since loader_mods.c builds a modded game's folder out of
 * links; depth bounds a link loop. */
static void walk(const char *sz_root, const char *sz_relative, uint32_t *p_traits, int depth) {
    if (depth > 32) return;
    char sz_path[4096];
    if ((size_t)snprintf(sz_path, sizeof(sz_path), "%s%s%s", sz_root, sz_relative[0] ? "/" : "", sz_relative) >=
        sizeof(sz_path))
        return;
    PsHostDir *p_dir = ps_host_dir_open(sz_path);
    if (!p_dir) return;
    for (PsHostDirEntry entry; ps_host_dir_next(p_dir, &entry);) {
        if (!strcmp(entry.sz_name, ".") || !strcmp(entry.sz_name, "..")) continue;
        if (!sz_relative[0] && !strcmp(entry.sz_name, "sce_sys")) continue;
        char sz_child[4096];
        if ((size_t)snprintf(sz_child, sizeof(sz_child), "%s%s%s", sz_relative, sz_relative[0] ? "/" : "",
                             entry.sz_name) >= sizeof(sz_child))
            continue;
        int is_directory = entry.kind == PS_HOST_ENTRY_DIRECTORY, is_file = entry.kind == PS_HOST_ENTRY_REGULAR;
        if (entry.kind == PS_HOST_ENTRY_LINK || entry.kind == PS_HOST_ENTRY_UNKNOWN) {
            char sz_full[8192];
            PsHostStat st;
            snprintf(sz_full, sizeof(sz_full), "%s/%s", sz_root, sz_child);
            if (ps_host_fs_stat(sz_full, &st)) continue;
            is_directory = PS_HOST_MODE_IS_DIRECTORY(st.mode);
            is_file = PS_HOST_MODE_IS_REGULAR(st.mode);
        }
        if (is_directory) walk(sz_root, sz_child, p_traits, depth + 1);
        else if (is_file) *p_traits |= file_traits(sz_child, entry.sz_name);
    }
    ps_host_dir_close(p_dir);
}

uint32_t loader_detect_traits(const char *sz_game, const LoaderModule *p_eboot) {
    uint32_t traits = eboot_traits(p_eboot);
    walk(sz_game, "", &traits, 0);
    return traits;
}

void loader_record_game_traits(const char *sz_game, const LoaderModule *p_eboot) {
    s_game_traits = loader_detect_traits(sz_game, p_eboot);
    fputs("Middleware: ", stdout);
    if (!s_game_traits) fputs("none detected", stdout);
    for (unsigned i = 0, c_printed = 0; i < LOADER_TRAIT_COUNT; ++i)
        if (s_game_traits & 1u << i) printf("%s%s", c_printed++ ? ", " : "", s_traits[i].sz_name);
    putchar('\n');
}

void loader_traits_list(uint32_t traits) {
    putchar('[');
    for (unsigned i = 0, c_printed = 0; i < LOADER_TRAIT_COUNT; ++i)
        if (traits & 1u << i)
            printf("%s{\"id\": \"%s\", \"name\": \"%s\"}", c_printed++ ? ", " : "", s_traits[i].sz_id, s_traits[i].sz_name);
    puts("]");
}
