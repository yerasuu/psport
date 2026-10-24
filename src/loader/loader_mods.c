/* Loose-file mods: the game folder is loaded through a private view of links, the game's entries
 * with the enabled mods' files linked over them, so neither the game nor the mods are written.
 * A mod replaces or adds files under dvdroot_ps4; the executable and the system folders stay the
 * game's. Mods load in mods.json's order (new folders after it, alphabetically); a later mod's file
 * wins over an earlier one's. The view lives under --out for this process only: a watcher process
 * removes it once psport ends, by any route (an in-game restart starts a new one). */
#define _GNU_SOURCE
#include "runtime_json_print.h"
#include "loader.h"
#include "ps_host.h"
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { char **pp_items; size_t c_items, c_capacity; } Names;
typedef struct { char sz_relative[PATH_MAX], sz_source[PATH_MAX]; } Replacement;
typedef struct {
    const LoaderModOptions *p_options;
    Replacement *p_replacements;
    size_t c_replacements, c_capacity;
    char sz_view[PATH_MAX];
} Mods;

static Mods *s_p_failing;
/* Folders at the top of the game's own dvdroot_ps4, which a mod may consist of like the profile's. */
static Names s_game_folders;

static void remove_tree(const char *sz_path);

/* Errors end the launch, as a half-built view would load a game missing files. */
__attribute__((noreturn, format(printf, 1, 2))) static void mods_fail(const char *sz_format, ...) {
    va_list args;
    va_start(args, sz_format);
    fputs("Mods: ", stderr);
    vfprintf(stderr, sz_format, args);
    fputc('\n', stderr);
    va_end(args);
    if (s_p_failing && s_p_failing->sz_view[0]) remove_tree(s_p_failing->sz_view);
    exit(1);
}

static void join_path(char *sz_out, const char *sz_folder, const char *sz_name) {
    if ((size_t)snprintf(sz_out, PATH_MAX, "%s/%s", sz_folder, sz_name) >= PATH_MAX)
        mods_fail("path too long: %s/%s", sz_folder, sz_name);
}
static int is_dir(const char *sz_path) {
    PsHostStat st;
    return !ps_host_fs_stat(sz_path, &st) && PS_HOST_MODE_IS_DIRECTORY(st.mode);
}
static int is_file(const char *sz_path) {
    PsHostStat st;
    return !ps_host_fs_stat(sz_path, &st) && PS_HOST_MODE_IS_REGULAR(st.mode);
}
static int is_symlink(const char *sz_path) {
    PsHostStat st;
    return !ps_host_fs_stat_link(sz_path, &st) && PS_HOST_MODE_IS_LINK(st.mode);
}
static int exists(const char *sz_path) {
    PsHostStat st;
    return !ps_host_fs_stat(sz_path, &st);
}
/* Links are removed, never followed, since the view's folders link into the game and the mods.
 * Not nftw: MinGW's follows junctions and folder links even with FTW_PHYS. */
static void remove_tree(const char *sz_path) {
    if (is_symlink(sz_path) || !is_dir(sz_path)) {
        (void)ps_host_fs_remove(sz_path); /* best effort: the launch is failing already */
        return;
    }
    PsHostDir *p_dir = ps_host_dir_open(sz_path);
    if (p_dir) {
        for (PsHostDirEntry entry; ps_host_dir_next(p_dir, &entry);) {
            char sz_child[PATH_MAX];
            if (!strcmp(entry.sz_name, ".") || !strcmp(entry.sz_name, "..") ||
                (size_t)snprintf(sz_child, sizeof(sz_child), "%s/%s", sz_path, entry.sz_name) >= sizeof(sz_child))
                continue;
            remove_tree(sz_child);
        }
        ps_host_dir_close(p_dir);
    }
    (void)ps_host_fs_remove_dir(sz_path);
}
static const char *base_name(const char *sz_path) {
    const char *p_slash = strrchr(sz_path, '/');
    return p_slash ? p_slash + 1 : sz_path;
}

static void names_add(Names *p_names, const char *sz_name) {
    if (p_names->c_items == p_names->c_capacity) {
        p_names->c_capacity = p_names->c_capacity ? p_names->c_capacity * 2 : 16;
        p_names->pp_items = realloc(p_names->pp_items, p_names->c_capacity * sizeof(*p_names->pp_items));
        if (!p_names->pp_items) mods_fail("out of memory");
    }
    if (!(p_names->pp_items[p_names->c_items++] = strdup(sz_name))) mods_fail("out of memory");
}
static int names_has(const Names *p_names, const char *sz_name) {
    for (size_t i = 0; i < p_names->c_items; ++i)
        if (!strcmp(p_names->pp_items[i], sz_name)) return 1;
    return 0;
}
static void names_free(Names *p_names) {
    for (size_t i = 0; i < p_names->c_items; ++i) free(p_names->pp_items[i]);
    free(p_names->pp_items);
    memset(p_names, 0, sizeof(*p_names));
}
/* Entries in the directory's own order, as the first match ignoring case depends on it. */
static void list_dir(const char *sz_folder, Names *p_out) {
    PsHostDir *p_dir = ps_host_dir_open(sz_folder);
    if (!p_dir) return;
    for (PsHostDirEntry entry; ps_host_dir_next(p_dir, &entry);)
        if (strcmp(entry.sz_name, ".") && strcmp(entry.sz_name, "..")) names_add(p_out, entry.sz_name);
    ps_host_dir_close(p_dir);
}
static int compare_folded(const void *p_a, const void *p_b) {
    const char *sz_a = *(char *const *)p_a, *sz_b = *(char *const *)p_b;
    int order = strcasecmp(sz_a, sz_b);
    return order ? order : strcmp(sz_a, sz_b);
}
static int compare_bytes(const void *p_a, const void *p_b) {
    return strcmp(*(char *const *)p_a, *(char *const *)p_b);
}

/* sz_name inside sz_folder, matching an existing entry ignoring case (mods made on Windows often
 * differ from the game's lower-case names); sz_folder/sz_name when none does. */
static void child(const char *sz_folder, const char *sz_name, char *sz_out) {
    PsHostDir *p_dir = ps_host_dir_open(sz_folder);
    if (p_dir) {
        for (PsHostDirEntry entry; ps_host_dir_next(p_dir, &entry);) {
            if (!strcmp(entry.sz_name, ".") || !strcmp(entry.sz_name, "..") || strcasecmp(entry.sz_name, sz_name))
                continue;
            join_path(sz_out, sz_folder, entry.sz_name);
            ps_host_dir_close(p_dir);
            return;
        }
        ps_host_dir_close(p_dir);
    }
    join_path(sz_out, sz_folder, sz_name);
}

static int is_listed(const char *const *pp_list, size_t c_list, const char *sz_name) {
    for (size_t i = 0; i < c_list; ++i)
        if (!strcasecmp(pp_list[i], sz_name)) return 1;
    return 0;
}
/* Files a mod may carry beside its content without making it something else, as mod sites ship
 * them: notes (a README without a suffix too), previews, links and licences. */
static int is_document(const char *sz_name) {
    static const char *const s_documents[] = {".txt", ".md",  ".ini",  ".json", ".pdf", ".rtf", ".nfo", ".url", ".htm",
                                              ".html", ".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp"};
    const char *sz_dot = strrchr(sz_name, '.');
    return !sz_dot || sz_dot == sz_name || is_listed(s_documents, sizeof(s_documents) / sizeof(*s_documents), sz_dot);
}
static int is_game_folder(const LoaderModOptions *p_options, const char *sz_name) {
    return is_listed(p_options->pp_folders, p_options->c_folders, sz_name) ||
           is_listed((const char *const *)s_game_folders.pp_items, s_game_folders.c_items, sz_name);
}

#define MAX_WRAPPERS 3
/* Where a mod's files are (sz_out_root) and the game path they go to (*psz_out_prefix), or 0.
 * Accepted layouts: <mod>/dvdroot_ps4, <mod>/app0/dvdroot_ps4, <mod>/<serial>/dvdroot_ps4 (any
 * TITLE_ID of the game's executable), up to MAX_WRAPPERS wrapper folders around any of these
 * (archives extracted into a folder of their name, beside notes and previews), and the game's
 * folders without dvdroot_ps4 (<mod>/chr, ...), also next to other folders (Screenshots), which
 * add_mod leaves out. */
static int content_root(const LoaderModOptions *p_options, const char *sz_folder, int c_wrappers, char *sz_out_root,
                        const char **psz_out_prefix) {
    if (!is_dir(sz_folder)) return 0;
    for (size_t i = 0; i < 2 + p_options->c_serials; ++i) {
        char sz_base[PATH_MAX], sz_dvdroot[PATH_MAX];
        if (i == 0) snprintf(sz_base, sizeof(sz_base), "%s", sz_folder);
        else child(sz_folder, i == 1 ? "app0" : p_options->pp_serials[i - 2], sz_base);
        child(sz_base, "dvdroot_ps4", sz_dvdroot);
        if (is_dir(sz_dvdroot)) {
            snprintf(sz_out_root, PATH_MAX, "%s", sz_base);
            *psz_out_prefix = "";
            return 1;
        }
    }
    Names entries = {0};
    list_dir(sz_folder, &entries);
    size_t c_folders = 0;
    int is_game_layout = 0, has_other_files = 0;
    char sz_only[PATH_MAX] = "";
    for (size_t i = 0; i < entries.c_items; ++i) {
        if (entries.pp_items[i][0] == '.') continue;
        char sz_path[PATH_MAX];
        join_path(sz_path, sz_folder, entries.pp_items[i]);
        if (is_dir(sz_path)) {
            if (!c_folders++) snprintf(sz_only, sizeof(sz_only), "%s", sz_path);
            if (is_game_folder(p_options, entries.pp_items[i])) is_game_layout = 1;
        } else if (is_file(sz_path) && !is_document(entries.pp_items[i])) has_other_files = 1;
    }
    names_free(&entries);
    if (is_game_layout) {
        snprintf(sz_out_root, PATH_MAX, "%s", sz_folder);
        *psz_out_prefix = "dvdroot_ps4";
        return 1;
    }
    if (c_folders == 1 && !has_other_files && c_wrappers < MAX_WRAPPERS)
        return content_root(p_options, sz_only, c_wrappers + 1, sz_out_root, psz_out_prefix);
    return 0;
}

typedef struct { size_t c_files; int has_archive; } FolderScan;
static void scan_folder(const char *sz_folder, int depth, FolderScan *p_scan) {
    static const char *const s_archives[] = {".zip", ".7z", ".rar"};
    Names entries = {0};
    list_dir(sz_folder, &entries);
    for (size_t i = 0; i < entries.c_items && p_scan->c_files < 2000; ++i) {
        char sz_path[PATH_MAX];
        join_path(sz_path, sz_folder, entries.pp_items[i]);
        /* walk() refuses links too; following them could leave the mods folder or loop. */
        if (is_symlink(sz_path)) continue;
        if (is_dir(sz_path)) {
            if (depth < 8) scan_folder(sz_path, depth + 1, p_scan);
            continue;
        }
        ++p_scan->c_files;
        const char *sz_dot = strrchr(entries.pp_items[i], '.');
        p_scan->has_archive |= sz_dot && is_listed(s_archives, sizeof(s_archives) / sizeof(*s_archives), sz_dot);
    }
    names_free(&entries);
}
/* Why a folder of the mods folder is not a mod (content_root gave 0), for the log. */
static const char *rejection(const char *sz_folder) {
    FolderScan scan = {0};
    scan_folder(sz_folder, 0, &scan);
    if (!scan.c_files) return "empty";
    if (scan.has_archive) return "still an archive: extract it here";
    Names entries = {0};
    list_dir(sz_folder, &entries);
    size_t c_folders = 0;
    for (size_t i = 0; i < entries.c_items; ++i) {
        char sz_path[PATH_MAX];
        join_path(sz_path, sz_folder, entries.pp_items[i]);
        c_folders += entries.pp_items[i][0] != '.' && is_dir(sz_path);
    }
    names_free(&entries);
    if (c_folders > 1) return "several folders: each mod goes in its own folder, with dvdroot_ps4 or the game's folders";
    return "no game files: expected dvdroot_ps4 or the game's folders (chr, parts, map...)";
}

static void discover(const LoaderModOptions *p_options, const char *sz_root, Names *p_out) {
    if (!is_dir(sz_root)) return;
    Names entries = {0};
    list_dir(sz_root, &entries);
    for (size_t i = 0; i < entries.c_items; ++i) {
        char sz_path[PATH_MAX], sz_content[PATH_MAX];
        const char *sz_prefix;
        join_path(sz_path, sz_root, entries.pp_items[i]);
        if (!is_dir(sz_path)) continue;
        if (content_root(p_options, sz_path, 0, sz_content, &sz_prefix)) names_add(p_out, entries.pp_items[i]);
        else fprintf(stderr, "Mods: %s is not a mod: %s\n", entries.pp_items[i], rejection(sz_path));
    }
    names_free(&entries);
    if (p_out->c_items) qsort(p_out->pp_items, p_out->c_items, sizeof(*p_out->pp_items), compare_folded);
}

/* The enabled mods in load order: mods.json's "order", then the new folders alphabetically; the
 * folders in its "disabled" are left out. */
static void selected(const LoaderModOptions *p_options, Names *p_out) {
    Names available = {0};
    discover(p_options, p_options->sz_mods_dir, &available);
    const char *sz_config = p_options->sz_config;
    if (!sz_config || !is_file(sz_config)) {
        *p_out = available;
        return;
    }
    LoaderJsonList settings[2] = {{.sz_key = "disabled"}, {.sz_key = "order"}};
    if (!loader_json_lists(sz_config, settings, 2)) mods_fail("%s is not a JSON object", sz_config);
    if (settings[0].is_present && !settings[0].is_list) mods_fail("Disabled mods must be a list of folder names");
    if (settings[1].is_present && !settings[1].is_list) mods_fail("Mod order must be a list of folder names");
    for (size_t pass = 0; pass < 2; ++pass) {
        size_t c_names = pass ? available.c_items : settings[1].c_items;
        for (size_t i = 0; i < c_names; ++i) {
            const char *sz_name = pass ? available.pp_items[i] : settings[1].pp_items[i];
            if (names_has(&available, sz_name) && !loader_json_has(&settings[0], sz_name) && !names_has(p_out, sz_name))
                names_add(p_out, sz_name);
        }
    }
    loader_json_free(settings, 2);
    names_free(&available);
}

static void add_replacement(Mods *p_mods, const char *sz_relative, const char *sz_source) {
    if (p_mods->c_replacements == p_mods->c_capacity) {
        p_mods->c_capacity = p_mods->c_capacity ? p_mods->c_capacity * 2 : 64;
        p_mods->p_replacements = realloc(p_mods->p_replacements, p_mods->c_capacity * sizeof(*p_mods->p_replacements));
        if (!p_mods->p_replacements) mods_fail("out of memory");
    }
    Replacement *p_replacement = &p_mods->p_replacements[p_mods->c_replacements++];
    snprintf(p_replacement->sz_relative, sizeof(p_replacement->sz_relative), "%s", sz_relative);
    snprintf(p_replacement->sz_source, sizeof(p_replacement->sz_source), "%s", sz_source);
}
static int is_first_part(const char *sz_relative, const char *sz_name) {
    size_t cb_name = strlen(sz_name);
    return !strncasecmp(sz_relative, sz_name, cb_name) && (!sz_relative[cb_name] || sz_relative[cb_name] == '/');
}
/* Top-down, each directory's files sorted and its subdirectories in their own order; a link
 * anywhere in a mod is refused, as the view must not reach outside the mod through it. At the top
 * of a mod made of the game's folders (is_game_layout), its other folders and its notes stay out. */
static void walk(Mods *p_mods, const char *sz_directory, const char *sz_relative, int is_game_layout, size_t *p_count) {
    Names entries = {0}, folders = {0}, files = {0};
    list_dir(sz_directory, &entries);
    for (size_t i = 0; i < entries.c_items; ++i) {
        char sz_path[PATH_MAX];
        join_path(sz_path, sz_directory, entries.pp_items[i]);
        const int is_folder = is_dir(sz_path);
        const int is_kept = is_folder ? is_game_folder(p_mods->p_options, entries.pp_items[i]) : !is_document(entries.pp_items[i]);
        if (is_game_layout && !is_kept) {
            fprintf(stderr, "Mods: %s left out: not one of the game's folders or files\n", sz_path);
            continue;
        }
        names_add(is_folder ? &folders : &files, entries.pp_items[i]);
    }
    names_free(&entries);
    for (size_t pass = 0; pass < 2; ++pass) {
        const Names *p_names = pass ? &files : &folders;
        for (size_t i = 0; i < p_names->c_items; ++i) {
            char sz_path[PATH_MAX];
            join_path(sz_path, sz_directory, p_names->pp_items[i]);
            if (is_symlink(sz_path)) mods_fail("Mod symlinks are unsupported: %s", sz_path);
        }
    }
    if (files.c_items) qsort(files.pp_items, files.c_items, sizeof(*files.pp_items), compare_bytes);
    for (size_t i = 0; i < files.c_items; ++i) {
        char sz_source[PATH_MAX], sz_game_path[PATH_MAX];
        join_path(sz_source, sz_directory, files.pp_items[i]);
        if (sz_relative[0]) join_path(sz_game_path, sz_relative, files.pp_items[i]);
        else snprintf(sz_game_path, sizeof(sz_game_path), "%s", files.pp_items[i]);
        /* Readme and metadata stay outside the game; executable changes are patches, not mods. */
        if (!is_first_part(sz_game_path, "dvdroot_ps4")) {
            if (is_first_part(sz_game_path, "eboot.bin") || is_first_part(sz_game_path, "sce_module") ||
                is_first_part(sz_game_path, "sce_sys"))
                mods_fail("Executable/system replacement is unsupported: %s", sz_source);
            continue;
        }
        if (!is_file(sz_source)) mods_fail("Not a regular mod file: %s", sz_source);
        add_replacement(p_mods, sz_game_path, sz_source);
        ++*p_count;
    }
    for (size_t i = 0; i < folders.c_items; ++i) {
        char sz_path[PATH_MAX], sz_game_path[PATH_MAX];
        join_path(sz_path, sz_directory, folders.pp_items[i]);
        if (sz_relative[0]) join_path(sz_game_path, sz_relative, folders.pp_items[i]);
        else snprintf(sz_game_path, sizeof(sz_game_path), "%s", folders.pp_items[i]);
        walk(p_mods, sz_path, sz_game_path, 0, p_count);
    }
    names_free(&folders);
    names_free(&files);
}
static void add_mod(Mods *p_mods, Names *p_owners, const char *sz_name, const char *sz_folder) {
    char sz_root[PATH_MAX], sz_real[PATH_MAX];
    const char *sz_prefix;
    if (!content_root(p_mods->p_options, sz_folder, 0, sz_root, &sz_prefix))
        mods_fail("%s: expected dvdroot_ps4 (or chr/, parts/, ...) inside the mod folder", sz_folder);
    const int error = ps_host_fs_real_path(sz_root, sz_real, sizeof(sz_real));
    if (error) mods_fail("%s: %s", sz_root, strerror(error));
    size_t first = p_mods->c_replacements, c_added = 0;
    walk(p_mods, sz_real, sz_prefix, sz_prefix[0] != 0, &c_added);
    for (size_t i = first; i < p_mods->c_replacements; ++i) {
        const char *sz_relative = p_mods->p_replacements[i].sz_relative;
        for (size_t j = 0; j < p_owners->c_items; j += 2) {
            if (strcasecmp(p_owners->pp_items[j], sz_relative)) continue;
            fprintf(stderr, "Mods: %s: %s -> %s\n", sz_relative, p_owners->pp_items[j + 1], sz_name);
            free(p_owners->pp_items[j + 1]);
            if (!(p_owners->pp_items[j + 1] = strdup(sz_name))) mods_fail("out of memory");
            sz_relative = NULL;
            break;
        }
        if (sz_relative) {
            names_add(p_owners, sz_relative);
            names_add(p_owners, sz_name);
        }
    }
    fprintf(stderr, "Mods: %s: %zu files\n", sz_name, c_added);
}

/* One directory level of the view made real, its entries links to where the link pointed: the
 * view never writes through a link into the game. */
static void expand(const char *sz_directory) {
    if (is_symlink(sz_directory)) {
        char sz_target[PATH_MAX];
        int error = ps_host_fs_real_path(sz_directory, sz_target, sizeof(sz_target));
        if (error) mods_fail("%s: %s", sz_directory, strerror(error));
        if (!is_dir(sz_target)) mods_fail("File/directory conflict at %s", base_name(sz_directory));
        error = ps_host_fs_remove(sz_directory);
        if (!error) error = ps_host_fs_make_dir(sz_directory, 0777);
        if (error) mods_fail("%s: %s", sz_directory, strerror(error));
        Names entries = {0};
        list_dir(sz_target, &entries);
        for (size_t i = 0; i < entries.c_items; ++i) {
            char sz_from[PATH_MAX], sz_link[PATH_MAX];
            join_path(sz_from, sz_target, entries.pp_items[i]);
            join_path(sz_link, sz_directory, entries.pp_items[i]);
            error = ps_host_fs_symlink(sz_from, sz_link);
            if (error) mods_fail("%s: %s", sz_link, strerror(error));
        }
        names_free(&entries);
    } else if (exists(sz_directory) && !is_dir(sz_directory)) {
        mods_fail("File/directory conflict at %s", base_name(sz_directory));
    } else {
        const int error = ps_host_fs_make_dir(sz_directory, 0777);
        if (error && error != EEXIST) mods_fail("%s: %s", sz_directory, strerror(error));
    }
}
static int is_inside(const char *sz_path, const char *sz_folder) {
    size_t cb_folder = strlen(sz_folder);
    return !strncmp(sz_path, sz_folder, cb_folder) && (!sz_path[cb_folder] || sz_path[cb_folder] == '/');
}
static void build_view(Mods *p_mods, const char *sz_game, const char *sz_out_dir) {
    char sz_out[PATH_MAX];
    if ((size_t)snprintf(sz_out, sizeof(sz_out), "%s", sz_out_dir) >= sizeof(sz_out)) mods_fail("path too long: %s", sz_out_dir);
    for (char *sz_cursor = sz_out + 1; *sz_cursor; ++sz_cursor)
        if (*sz_cursor == '/') { *sz_cursor = 0; ps_host_fs_make_dir(sz_out, 0777); *sz_cursor = '/'; }
    int error = ps_host_fs_make_dir(sz_out, 0777);
    if (error && error != EEXIST) mods_fail("%s: %s", sz_out, strerror(error));
    char sz_template[PATH_MAX];
    error = ps_host_fs_real_path(sz_out, sz_template, sizeof(sz_template));
    if (error) mods_fail("%s: %s", sz_out, strerror(error));
    strncat(sz_template, "/mod-game-XXXXXX", sizeof(sz_template) - strlen(sz_template) - 1);
    error = ps_host_fs_make_unique_dir(sz_template);
    if (error) mods_fail("%s: %s", sz_template, strerror(error));
    snprintf(p_mods->sz_view, sizeof(p_mods->sz_view), "%s", sz_template);
    Names entries = {0};
    list_dir(sz_game, &entries);
    for (size_t i = 0; i < entries.c_items; ++i) {
        char sz_from[PATH_MAX], sz_link[PATH_MAX];
        join_path(sz_from, sz_game, entries.pp_items[i]);
        join_path(sz_link, p_mods->sz_view, entries.pp_items[i]);
        error = ps_host_fs_symlink(sz_from, sz_link);
        if (error) mods_fail("%s: %s", sz_link, strerror(error));
    }
    names_free(&entries);
    size_t c_replaced = 0, c_added = 0;
    for (size_t i = 0; i < p_mods->c_replacements; ++i) {
        const Replacement *p_replacement = &p_mods->p_replacements[i];
        char sz_parent[PATH_MAX], sz_part[PATH_MAX], sz_destination[PATH_MAX];
        snprintf(sz_parent, sizeof(sz_parent), "%s", p_mods->sz_view);
        const char *sz_cursor = p_replacement->sz_relative;
        for (const char *sz_slash; (sz_slash = strchr(sz_cursor, '/')); sz_cursor = sz_slash + 1) {
            snprintf(sz_part, sizeof(sz_part), "%.*s", (int)(sz_slash - sz_cursor), sz_cursor);
            char sz_next[PATH_MAX];
            child(sz_parent, sz_part, sz_next);
            expand(sz_next);
            snprintf(sz_parent, sizeof(sz_parent), "%s", sz_next);
        }
        child(sz_parent, sz_cursor, sz_destination);
        if (is_symlink(sz_destination)) {
            char sz_target[PATH_MAX];
            c_replaced += !ps_host_fs_real_path(sz_destination, sz_target, sizeof(sz_target)) && is_inside(sz_target, sz_game);
            error = ps_host_fs_remove(sz_destination);
            if (error) mods_fail("%s: %s", sz_destination, strerror(error));
        } else if (is_dir(sz_destination)) {
            mods_fail("File/directory conflict: %s", p_replacement->sz_relative);
        } else if (exists(sz_destination)) {
            /* A hard link or copy standing in for a file link, which Windows makes without the
             * privilege for symbolic links. */
            ++c_replaced;
            error = ps_host_fs_remove(sz_destination);
            if (error) mods_fail("%s: %s", sz_destination, strerror(error));
        } else {
            ++c_added;
        }
        error = ps_host_fs_symlink(p_replacement->sz_source, sz_destination);
        if (error) mods_fail("%s: %s", sz_destination, strerror(error));
    }
    fprintf(stderr, "Mods: %zu game files replaced, %zu added\n", c_replaced, c_added);
}

static void remove_when_done(const char *sz_view) {
    const int error = ps_host_remove_tree_at_exit(sz_view);
    if (error) fprintf(stderr, "Mods: %s stays after this run: %s\n", sz_view, strerror(error));
}

/* The mods folder as named, without trailing slashes: its last part names a folder that is itself
 * one loose overlay. */
static void trim_slashes(LoaderModOptions *p_options, char *sz_out_mods_dir) {
    snprintf(sz_out_mods_dir, PATH_MAX, "%s", p_options->sz_mods_dir);
    size_t cb_dir = strlen(sz_out_mods_dir);
    while (cb_dir > 1 && sz_out_mods_dir[cb_dir - 1] == '/') sz_out_mods_dir[--cb_dir] = 0;
    p_options->sz_mods_dir = sz_out_mods_dir;
}

static void find_game_folders(const char *sz_game) {
    char sz_dvdroot[PATH_MAX];
    Names entries = {0};
    names_free(&s_game_folders);
    join_path(sz_dvdroot, sz_game, "dvdroot_ps4");
    list_dir(sz_dvdroot, &entries);
    for (size_t i = 0; i < entries.c_items; ++i) {
        char sz_path[PATH_MAX];
        join_path(sz_path, sz_dvdroot, entries.pp_items[i]);
        if (is_dir(sz_path)) names_add(&s_game_folders, entries.pp_items[i]);
    }
    names_free(&entries);
}

char *loader_mods_view(const char *sz_game, const LoaderModOptions *p_options) {
    LoaderModOptions options = *p_options;
    char sz_mods_dir[PATH_MAX], sz_game_real[PATH_MAX], sz_legacy[PATH_MAX], sz_layer[PATH_MAX];
    trim_slashes(&options, sz_mods_dir);
    Mods mods = {.p_options = &options};
    s_p_failing = &mods;
    const int error = ps_host_fs_real_path(sz_game, sz_game_real, sizeof(sz_game_real));
    if (error) mods_fail("%s: %s", sz_game, strerror(error));
    find_game_folders(sz_game_real);
    Names owners = {0};
    /* shadPS4's convention: a loose overlay beside the game folder, <game>-mods. */
    if ((size_t)snprintf(sz_legacy, sizeof(sz_legacy), "%s-mods", sz_game_real) >= sizeof(sz_legacy))
        mods_fail("path too long: %s", sz_game_real);
    if (is_dir(sz_legacy)) add_mod(&mods, &owners, base_name(sz_legacy), sz_legacy);
    child(options.sz_mods_dir, "dvdroot_ps4", sz_layer);
    if (is_dir(sz_layer)) {
        add_mod(&mods, &owners, base_name(options.sz_mods_dir), options.sz_mods_dir);
    } else {
        Names names = {0};
        selected(&options, &names);
        for (size_t i = 0; i < names.c_items; ++i) {
            join_path(sz_layer, options.sz_mods_dir, names.pp_items[i]);
            add_mod(&mods, &owners, names.pp_items[i], sz_layer);
        }
        names_free(&names);
    }
    names_free(&owners);
    char *sz_result;
    if (!mods.c_replacements) {
        sz_result = strdup(sz_game_real);
    } else {
        build_view(&mods, sz_game_real, options.sz_out_dir);
        remove_when_done(mods.sz_view);
        sz_result = strdup(mods.sz_view);
    }
    free(mods.p_replacements);
    s_p_failing = NULL;
    if (!sz_result) mods_fail("out of memory");
    return sz_result;
}

void loader_mods_list(const char *sz_game, const LoaderModOptions *p_options) {
    LoaderModOptions options = *p_options;
    char sz_mods_dir[PATH_MAX];
    trim_slashes(&options, sz_mods_dir);
    if (sz_game) find_game_folders(sz_game);
    Names names = {0};
    discover(&options, options.sz_mods_dir, &names);
    putchar('[');
    for (size_t i = 0; i < names.c_items; ++i) {
        if (i) fputs(", ", stdout);
        runtime_json_print_string(names.pp_items[i]);
    }
    puts("]");
    names_free(&names);
}
