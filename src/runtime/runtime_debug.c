/* Debug mode (PS_DEBUG=1 or psport.ini debug=1, decision #620): a run's logs in one folder,
 * <PS_DEBUG_DIR>/<host>/<YYYYmmdd-HHMMSS>-<serial>/, which a file synchroniser copies between
 * machines. Each file is written as <name>.partial and renamed once nothing writes it any more;
 * summary.txt comes last, so a folder holding it is complete. The output helper
 * (ps_host_output_tee) owns run.log and frame-stats.txt and finishes the folder once the process
 * and whatever inherited its output are gone, by any route. */
#define _GNU_SOURCE
#include "runtime.h"
#include "ps_host.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PS_VERSION
#define PS_VERSION "unknown"
#endif
#ifndef PS_COMMIT
#define PS_COMMIT ""
#endif


enum { DEBUG_KEEP_DEFAULT = 20, DEBUG_TAIL_LINES = 40, DEBUG_SERIAL_MAX = 40 };
static char s_sz_run_dir[4352];
static int s_is_active, s_is_tee_active, s_has_finished;
static uint64_t s_start_ns;
static char s_sz_started[40];
static char s_sz_calls[512];
/* What psport writes itself; run.log and frame-stats.txt are the output helper's. */
static const char *const s_psport_files[] = {"gpu.log", "calls.txt", "faults.txt", "validation.txt", "env.txt", "config.txt"};

static void put(int h_file, const char *p_text, size_t cb_text) {
    while (cb_text) {
        const int64_t cb_written = ps_host_fs_write(h_file, p_text, cb_text);
        if (cb_written <= 0) return;
        p_text += cb_written;
        cb_text -= (size_t)cb_written;
    }
}
static void put_text(int h_file, const char *sz_text) { put(h_file, sz_text, strlen(sz_text)); }
static int run_path(char *sz_out, size_t cb_out, const char *sz_name, const char *sz_suffix) {
    const int cb_path = snprintf(sz_out, cb_out, "%s/%s%s", s_sz_run_dir, sz_name, sz_suffix);
    return cb_path > 0 && (size_t)cb_path < cb_out;
}
int runtime_debug_open(const char *sz_name) {
    char sz_path[4500];
    if (!s_is_active || !run_path(sz_path, sizeof(sz_path), sz_name, ".partial")) return -1;
    const int h_file = ps_host_fs_open(sz_path, PS_HOST_OPEN_WRITE | PS_HOST_OPEN_CREATE | PS_HOST_OPEN_APPEND, 0644);
    return h_file >= 0 ? h_file : -1;
}
static void publish(const char *sz_name) {
    char sz_partial[4500], sz_final[4500];
    if (run_path(sz_partial, sizeof(sz_partial), sz_name, ".partial") && run_path(sz_final, sizeof(sz_final), sz_name, ""))
        (void)ps_host_fs_rename(sz_partial, sz_final); /* ENOENT: nothing was written to it */
}
void runtime_debug_note_calls(const char *sz_line) {
    if (!s_is_active) return;
    const size_t cb_line = strnlen(sz_line, sizeof(s_sz_calls) - 1);
    memcpy(s_sz_calls, sz_line, cb_line);
    s_sz_calls[cb_line] = 0;
}

static int is_secret_name(const char *p_name, size_t cb_name) {
    static const char *const s_words[] = {"TOKEN", "PASS", "SECRET", "KEY", "CREDENTIAL", "COOKIE", "AUTH"};
    char sz_upper[256];
    if (cb_name >= sizeof(sz_upper)) cb_name = sizeof(sz_upper) - 1;
    for (size_t i = 0; i < cb_name; ++i) sz_upper[i] = (char)(p_name[i] >= 'a' && p_name[i] <= 'z' ? p_name[i] - 32 : p_name[i]);
    sz_upper[cb_name] = 0;
    for (size_t i = 0; i < sizeof(s_words) / sizeof(*s_words); ++i)
        if (strstr(sz_upper, s_words[i])) return 1;
    return 0;
}
static int is_secret_value(const char *sz_value) { return !strncmp(sz_value, "eyJ", 3) || strstr(sz_value, "Bearer "); }
static void put_setting(int h_file, const char *p_name, size_t cb_name, const char *sz_value) {
    put(h_file, p_name, cb_name);
    put_text(h_file, "=");
    put_text(h_file, is_secret_name(p_name, cb_name) || is_secret_value(sz_value) ? "<redacted>" : sz_value);
    put_text(h_file, "\n");
}
static void write_env(void) {
    static const char *const s_prefixes[] = {"PS_", "BB_", "VK_", "RADV_", "MESA_", "DXVK_"};
    const int h_file = runtime_debug_open("env.txt");
    if (h_file < 0) return;
    for (char **pp_entry = environ; *pp_entry; ++pp_entry) {
        const char *sz_equals = strchr(*pp_entry, '=');
        if (!sz_equals) continue;
        for (size_t i = 0; i < sizeof(s_prefixes) / sizeof(*s_prefixes); ++i)
            if (!strncmp(*pp_entry, s_prefixes[i], strlen(s_prefixes[i]))) {
                put_setting(h_file, *pp_entry, (size_t)(sz_equals - *pp_entry), sz_equals + 1);
                break;
            }
    }
    ps_host_fs_close(h_file);
    publish("env.txt");
}
static void write_config(char **pp_argv, const char *sz_host) {
    const int h_file = runtime_debug_open("config.txt");
    if (h_file < 0) return;
    char sz_line[1024];
    snprintf(sz_line, sizeof(sz_line), "psport %s, commit %s\nhost: %s\nstarted: %s\ncommand:", PS_VERSION,
             PS_COMMIT[0] ? PS_COMMIT : "unknown", sz_host, s_sz_started);
    put_text(h_file, sz_line);
    for (char **pp_argument = pp_argv; *pp_argument; ++pp_argument) {
        put_text(h_file, " ");
        put_text(h_file, *pp_argument);
    }
    snprintf(sz_line, sizeof(sz_line), "\n\nsettings (%s%s):\n", runtime_config_path(),
             runtime_config_is_read() ? "" : ", not read");
    put_text(h_file, sz_line);
    const char *sz_key, *sz_value;
    for (size_t i = 0; runtime_config_entry(i, &sz_key, &sz_value); ++i) put_setting(h_file, sz_key, strlen(sz_key), sz_value);
    put_text(h_file, "\nfeatures (--list-features):\n");
    /* ps_host_features_list prints to stdout: pointed at the file for the listing. */
    fflush(stdout);
    const int h_stdout = ps_host_fs_duplicate(1, -1);
    if (h_stdout >= 0 && ps_host_fs_duplicate(h_file, 1) == 1) {
        ps_host_features_list();
        fflush(stdout);
        (void)ps_host_fs_duplicate(h_stdout, 1);
    } else {
        put_text(h_file, "not listed: stdout cannot be duplicated\n");
    }
    if (h_stdout >= 0) ps_host_fs_close(h_stdout);
    put_text(h_file, "\n");
    ps_host_fs_close(h_file);
}

static int is_run_folder_name(const char *sz_name) {
    const size_t cb_name = strlen(sz_name);
    if (cb_name < 17 || cb_name > 16 + DEBUG_SERIAL_MAX + 3 || sz_name[8] != '-' || sz_name[15] != '-') return 0;
    for (size_t i = 0; i < 15; ++i)
        if (i != 8 && (sz_name[i] < '0' || sz_name[i] > '9')) return 0;
    for (size_t i = 16; i < cb_name; ++i)
        if (!(sz_name[i] == '-' || sz_name[i] == '_' || (sz_name[i] >= '0' && sz_name[i] <= '9') ||
              (sz_name[i] >= 'A' && sz_name[i] <= 'Z') || (sz_name[i] >= 'a' && sz_name[i] <= 'z')))
            return 0;
    return 1;
}
static int compare_names(const void *p_a, const void *p_b) { return strcmp(*(char *const *)p_a, *(char *const *)p_b); }
/* Only a folder holding nothing but files goes: one holding anything else (a subfolder) is kept
 * whole and said. */
static int remove_run_folder(const char *sz_folder) {
    char sz_path[4500];
    for (int is_removing = 0; is_removing < 2; ++is_removing) {
        PsHostDir *p_dir = ps_host_dir_open(sz_folder);
        if (!p_dir) return errno;
        PsHostDirEntry entry;
        int error = 0;
        while (!error && ps_host_dir_next(p_dir, &entry)) {
            if (!strcmp(entry.sz_name, ".") || !strcmp(entry.sz_name, "..")) continue;
            PsHostStat stat;
            const int cb_path = snprintf(sz_path, sizeof(sz_path), "%s/%s", sz_folder, entry.sz_name);
            if (cb_path <= 0 || (size_t)cb_path >= sizeof(sz_path)) error = ENAMETOOLONG;
            else if (!(error = ps_host_fs_stat_link(sz_path, &stat)) && !PS_HOST_MODE_IS_REGULAR(stat.mode) && !PS_HOST_MODE_IS_LINK(stat.mode))
                error = ENOTEMPTY;
            else if (!error && is_removing) error = ps_host_fs_remove(sz_path);
        }
        ps_host_dir_close(p_dir);
        if (error) return error;
    }
    return ps_host_fs_remove_dir(sz_folder);
}
static void keep_newest(const char *sz_host_dir, const char *sz_current, long c_keep) {
    PsHostDir *p_dir = ps_host_dir_open(sz_host_dir);
    if (!p_dir) {
        printf("Debug: cannot list %s for retention: %s\n", sz_host_dir, strerror(errno));
        return;
    }
    char **pp_names = NULL;
    size_t c_names = 0, c_capacity = 0;
    PsHostDirEntry entry;
    char sz_path[4500];
    while (ps_host_dir_next(p_dir, &entry)) {
        PsHostStat stat;
        if (!is_run_folder_name(entry.sz_name) || !strcmp(entry.sz_name, sz_current)) continue;
        const int cb_path = snprintf(sz_path, sizeof(sz_path), "%s/%s", sz_host_dir, entry.sz_name);
        if (cb_path <= 0 || (size_t)cb_path >= sizeof(sz_path) || ps_host_fs_stat_link(sz_path, &stat) ||
            !PS_HOST_MODE_IS_DIRECTORY(stat.mode))
            continue;
        if (c_names == c_capacity) {
            c_capacity = c_capacity ? 2 * c_capacity : 64;
            char **pp_grown = realloc(pp_names, c_capacity * sizeof(*pp_grown));
            if (!pp_grown) break;
            pp_names = pp_grown;
        }
        if (!(pp_names[c_names] = strdup(entry.sz_name))) break;
        ++c_names;
    }
    ps_host_dir_close(p_dir);
    /* The current run counts as one of those kept. */
    const size_t c_kept_others = (size_t)c_keep - 1;
    size_t c_removed = 0;
    if (c_names > c_kept_others) {
        qsort(pp_names, c_names, sizeof(*pp_names), compare_names);
        for (size_t i = 0; i < c_names - c_kept_others; ++i) {
            snprintf(sz_path, sizeof(sz_path), "%s/%s", sz_host_dir, pp_names[i]);
            const int error = remove_run_folder(sz_path);
            if (error) printf("Debug: old run folder %s kept: %s\n", sz_path, strerror(error));
            else ++c_removed;
        }
    }
    for (size_t i = 0; i < c_names; ++i) free(pp_names[i]);
    free(pp_names);
    if (c_removed) printf("Debug: %zu old run folders removed, the newest %ld kept\n", c_removed, c_keep);
}
static int make_dirs(const char *sz_path) {
    char sz_prefix[4096];
    if (snprintf(sz_prefix, sizeof(sz_prefix), "%s", sz_path) >= (int)sizeof(sz_prefix)) return ENAMETOOLONG;
    for (char *p_slash = sz_prefix + 1;; ++p_slash) {
        if (*p_slash && *p_slash != '/') continue;
        const char saved = *p_slash;
        *p_slash = 0;
        const int error = ps_host_fs_make_dir(sz_prefix, 0755);
        if (error && error != EEXIST) return error;
        *p_slash = saved;
        if (!saved) break;
    }
    PsHostStat stat;
    const int error = ps_host_fs_stat(sz_path, &stat);
    return error ? error : PS_HOST_MODE_IS_DIRECTORY(stat.mode) ? 0 : ENOTDIR;
}
/* The game's title ID from param.sfo as a folder name: letters, digits, '-' and '_' only. */
static void sanitise_serial(char *sz_out, size_t cb_out, const char *sz_in) {
    size_t cb_used = 0;
    for (const char *p = sz_in; *p && cb_used + 1 < cb_out; ++p)
        if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '-' || *p == '_')
            sz_out[cb_used++] = *p;
    sz_out[cb_used] = 0;
    if (!cb_used) snprintf(sz_out, cb_out, "nogame");
}
/* As decanter names the same folder (#622): anything outside [A-Za-z0-9._-] becomes '_', leading and
 * trailing dots go, unknown-host when nothing is left. */
static void sanitise_host(char *sz_out, size_t cb_out, const char *sz_in) {
    while (*sz_in == '.') ++sz_in;
    size_t cb_used = 0;
    for (const char *p = sz_in; *p && cb_used + 1 < cb_out; ++p) {
        const int is_kept = (*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '-' ||
                            *p == '_' || *p == '.';
        sz_out[cb_used++] = is_kept ? *p : '_';
    }
    while (cb_used && sz_out[cb_used - 1] == '.') --cb_used;
    sz_out[cb_used] = 0;
    if (!cb_used) snprintf(sz_out, cb_out, "unknown-host");
}

static void debug_exit(int status) { runtime_debug_finish("exit", status); }

/* The output helper's side, in its own process. */
static int s_h_run_log = -1, s_h_frame_stats = -1;
static char s_tail[32768];
static size_t s_off_tail;
static int s_is_tail_wrapped, s_is_stats_block;
static char s_sz_stats_line[2048];
static size_t s_cb_stats_line;
/* The frame statistics' report lines (PS_FRAME_STATS), with the indented lines under them. */
static void end_stats_line(void) {
    static const char *const s_heads[] = {"Frame stats:", "Stall:", "Draw pipe:", "Shader specializations:",
                                          "Native probe:", "Guest waits per frame", "Guest heap:"};
    int is_head = 0;
    for (size_t i = 0; i < sizeof(s_heads) / sizeof(*s_heads); ++i)
        if (!strncmp(s_sz_stats_line, s_heads[i], strlen(s_heads[i]))) is_head = 1;
    if (is_head) s_is_stats_block = 1;
    else if (s_sz_stats_line[0] != ' ') s_is_stats_block = 0;
    if (s_is_stats_block) {
        if (s_h_frame_stats < 0) s_h_frame_stats = runtime_debug_open("frame-stats.txt");
        if (s_h_frame_stats >= 0) {
            put(s_h_frame_stats, s_sz_stats_line, s_cb_stats_line);
            put_text(s_h_frame_stats, "\n");
        }
    }
    s_cb_stats_line = 0;
}
static void tee_chunk(int is_error, const char *p_data, size_t cb_data) {
    if (s_h_run_log < 0) s_h_run_log = runtime_debug_open("run.log");
    if (s_h_run_log >= 0) put(s_h_run_log, p_data, cb_data);
    for (size_t i = 0; i < cb_data; ++i) {
        s_tail[s_off_tail++] = p_data[i];
        if (s_off_tail == sizeof(s_tail)) { s_off_tail = 0; s_is_tail_wrapped = 1; }
        if (is_error) continue;
        if (p_data[i] == '\n') end_stats_line();
        else if (s_cb_stats_line < sizeof(s_sz_stats_line)) s_sz_stats_line[s_cb_stats_line++] = p_data[i];
    }
}
static char tail_at(size_t i_logical, size_t cb_tail) {
    return s_is_tail_wrapped ? s_tail[(s_off_tail + sizeof(s_tail) - cb_tail + i_logical) % sizeof(s_tail)] : s_tail[i_logical];
}
static void put_tail(int h_file) {
    const size_t cb_tail = s_is_tail_wrapped ? sizeof(s_tail) : s_off_tail;
    size_t i_start = cb_tail, c_lines = 0;
    if (i_start && tail_at(i_start - 1, cb_tail) == '\n') --i_start;
    for (; i_start; --i_start)
        if (tail_at(i_start - 1, cb_tail) == '\n' && ++c_lines == DEBUG_TAIL_LINES) break;
    char chunk[512];
    size_t cb_chunk = 0;
    for (size_t i = i_start; i < cb_tail; ++i) {
        chunk[cb_chunk++] = tail_at(i, cb_tail);
        if (cb_chunk == sizeof(chunk)) { put(h_file, chunk, cb_chunk); cb_chunk = 0; }
    }
    put(h_file, chunk, cb_chunk);
    if (cb_tail && tail_at(cb_tail - 1, cb_tail) != '\n') put_text(h_file, "\n");
}
static void tee_end(void) {
    if (s_cb_stats_line) end_stats_line();
    if (s_h_run_log >= 0) ps_host_fs_close(s_h_run_log);
    if (s_h_frame_stats >= 0) ps_host_fs_close(s_h_frame_stats);
    publish("run.log");
    publish("frame-stats.txt");
    /* psport ended: nothing writes its files any more, also when it could not finish them itself. */
    for (size_t i = 0; i < sizeof(s_psport_files) / sizeof(*s_psport_files); ++i) publish(s_psport_files[i]);
    const int h_summary = runtime_debug_open("summary.txt");
    if (h_summary < 0) return;
    PsHostStat stat;
    char sz_line[512];
    if (!ps_host_fs_stat_file(h_summary, &stat) && !stat.cb_size) {
        const uint64_t ns = ps_host_time_monotonic_ns() - s_start_ns;
        snprintf(sz_line, sizeof(sz_line),
                 "how: ended without psport's own summary (killed, or a crash its handlers could not report)\n"
                 "exit code: unknown\nstarted: %s\nduration: %llu.%03llu s (until its output closed)\n",
                 s_sz_started, (unsigned long long)(ns / 1000000000u), (unsigned long long)(ns / 1000000u % 1000u));
        put_text(h_summary, sz_line);
    }
    snprintf(sz_line, sizeof(sz_line), "\nlast %d lines of run.log:\n", DEBUG_TAIL_LINES);
    put_text(h_summary, sz_line);
    put_tail(h_summary);
    ps_host_fs_close(h_summary);
    publish("summary.txt");
}

void runtime_debug_start(char **pp_argv, const char *sz_serial) {
    const int requested = runtime_env_flag("PS_DEBUG", -1);
    const char *sz_setting = runtime_config_get("debug");
    if (requested == 0 || (requested < 0 && !(sz_setting && atoi(sz_setting) > 0))) return;
    /* The GPU library's initializers read PS_FRAME_STATS before main: set, the process starts
     * again (as accept_deprecated_names does for BB_* names). */
    if (!ps_host_env_get("PS_FRAME_STATS")) {
        ps_host_env_set("PS_FRAME_STATS", "1", 0);
        ps_host_exec_self(pp_argv);
        printf("Debug: restart with PS_FRAME_STATS=1 failed (%s): GPU frame statistics stay off\n", strerror(errno));
    }
    long c_keep = DEBUG_KEEP_DEFAULT;
    const char *sz_keep = runtime_env("PS_DEBUG_KEEP", NULL);
    if (sz_keep && *sz_keep) {
        char *sz_end;
        errno = 0;
        const long value = strtol(sz_keep, &sz_end, 10);
        if (errno || *sz_end || value < 1 || value > 100000) printf("Debug: PS_DEBUG_KEEP '%s' ignored: expected 1 to 100000; %d kept\n", sz_keep, DEBUG_KEEP_DEFAULT);
        else c_keep = value;
    }
    char sz_root[4096], sz_host_raw[256] = "", sz_host[256], sz_title[DEBUG_SERIAL_MAX + 1], sz_host_dir[4096];
    const char *sz_dir = runtime_env("PS_DEBUG_DIR", "");
    const char *sz_data = runtime_env("PS_DATA_DIR", "");
    const int cb_root = *sz_dir ? snprintf(sz_root, sizeof(sz_root), "%s", sz_dir)
                                : snprintf(sz_root, sizeof(sz_root), "%s/debug", *sz_data ? sz_data : ".");
    (void)ps_host_host_name(sz_host_raw, sizeof(sz_host_raw));
    sanitise_host(sz_host, sizeof(sz_host), sz_host_raw);
    sanitise_serial(sz_title, sizeof(sz_title), sz_serial ? sz_serial : "");
    const int cb_host_dir = snprintf(sz_host_dir, sizeof(sz_host_dir), "%s/%s", sz_root, sz_host);
    const int error = cb_root <= 0 || (size_t)cb_root >= sizeof(sz_root) - 128 || cb_host_dir <= 0 ||
                              (size_t)cb_host_dir >= sizeof(sz_host_dir) - 128
                          ? ENAMETOOLONG
                          : make_dirs(sz_host_dir);
    if (error) {
        printf("Debug: cannot create %s (%s): debug mode off for this run\n", sz_host_dir, strerror(error));
        return;
    }
    const time_t now = time(NULL);
    struct tm local;
    char sz_stamp[32] = "00000000-000000", sz_name[128];
    if (!ps_host_time_local_fields((int64_t)now, &local)) {
        strftime(sz_stamp, sizeof(sz_stamp), "%Y%m%d-%H%M%S", &local);
        strftime(s_sz_started, sizeof(s_sz_started), "%Y-%m-%d %H:%M:%S %z", &local);
    }
    s_start_ns = ps_host_time_monotonic_ns();
    int error_create = EEXIST;
    for (int i_attempt = 1; i_attempt < 10 && error_create == EEXIST; ++i_attempt) {
        if (i_attempt == 1) snprintf(sz_name, sizeof(sz_name), "%s-%s", sz_stamp, sz_title);
        else snprintf(sz_name, sizeof(sz_name), "%s-%s-%d", sz_stamp, sz_title, i_attempt);
        snprintf(s_sz_run_dir, sizeof(s_sz_run_dir), "%s/%s", sz_host_dir, sz_name);
        error_create = ps_host_fs_make_dir(s_sz_run_dir, 0755);
    }
    if (error_create) {
        printf("Debug: cannot create a run folder in %s (%s): debug mode off for this run\n", sz_host_dir, strerror(error_create));
        return;
    }
    s_is_active = 1;
    write_env();
    write_config(pp_argv, sz_host_raw[0] ? sz_host_raw : sz_host);
    if (ps_host_on_exit(debug_exit)) printf("Debug: exit hook not installed: an exit() leaves summary.txt to the output helper\n");
    const int error_tee = ps_host_output_tee(tee_chunk, tee_end);
    s_is_tee_active = !error_tee;
    if (error_tee) printf("Debug: output not captured (%s): no run.log, frame-stats.txt or tail in summary.txt\n", strerror(error_tee));
    printf("Debug: run folder %s (complete once summary.txt is there)\n", s_sz_run_dir);
    keep_newest(sz_host_dir, sz_name, c_keep);
}

void runtime_debug_finish(const char *sz_how, int exit_code) {
    if (!s_is_active || __atomic_exchange_n(&s_has_finished, 1, __ATOMIC_ACQ_REL)) return;
    const int h_summary = runtime_debug_open("summary.txt");
    if (h_summary >= 0) {
        char sz_text[1200];
        const uint64_t ns = ps_host_time_monotonic_ns() - s_start_ns;
        snprintf(sz_text, sizeof(sz_text), "how: %s\nexit code: %d\nstarted: %s\nduration: %llu.%03llu s\n%s", sz_how,
                 exit_code, s_sz_started, (unsigned long long)(ns / 1000000000u), (unsigned long long)(ns / 1000000u % 1000u),
                 s_sz_calls[0] ? s_sz_calls : "Calls: no call report (the run ended before the game's imports were bound)\n");
        put_text(h_summary, sz_text);
        ps_host_fs_close(h_summary);
    }
    for (size_t i = 0; i < sizeof(s_psport_files) / sizeof(*s_psport_files); ++i) publish(s_psport_files[i]);
    if (!s_is_tee_active) publish("summary.txt");
}
