/* libSceNpTrophy over the game's trophy pack (runtime_trophy.h) and a per-user unlock state kept in
 * the user directory:
 *   <user>/trophy/<NP communication id>/unlocks_<user id>.tsv  id, unlocked, unlock time (UTC)
 *   <user>/trophy/<NP communication id>/trophyset.json         the pack as the port read it
 * (format in FORK.md). Without a pack the port can read, the set looks as on a console without the
 * title's trophy data installed: no trophies, groups or icons, unlocks still kept. */
#define _GNU_SOURCE
#include "runtime_np.h"
#include "runtime_trophy.h"
#include "ps_host.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TROPHY_INVALID ((int32_t)0x80551604)
#define TROPHY_OUT_OF_MEMORY ((int32_t)0x80551603)
#define TROPHY_INSUFFICIENT_BUFFER ((int32_t)0x80551605)
#define TROPHY_INVALID_HANDLE ((int32_t)0x80551608)
#define TROPHY_INVALID_CONTEXT ((int32_t)0x80551609)
#define TROPHY_INVALID_TROPHY_ID ((int32_t)0x8055160A)
#define TROPHY_INVALID_GROUP_ID ((int32_t)0x8055160B)
#define TROPHY_ALREADY_UNLOCKED ((int32_t)0x8055160C)
#define TROPHY_PLATINUM_CANNOT_UNLOCK ((int32_t)0x8055160D)
#define TROPHY_NOT_REGISTERED ((int32_t)0x8055160F)
#define TROPHY_ALREADY_REGISTERED ((int32_t)0x80551610)
#define TROPHY_BROKEN_DATA ((int32_t)0x80551611)
#define TROPHY_CONTEXT_ALREADY_EXISTS ((int32_t)0x80551613)
#define TROPHY_ICON_FILE_NOT_FOUND ((int32_t)0x80551614)
#define TROPHY_NOT_UNLOCKED ((int32_t)0x8055161A)
#define TROPHY_CONTEXT_EXCEEDS_MAX ((int32_t)0x80551623)
#define C_CONTEXTS_MAX 8
#define RTC_TICK_UNIX_EPOCH 62135596800000000ull

typedef struct {
    uint64_t size;
    uint32_t num_groups, num_trophies, num_platinum, num_gold, num_silver, num_bronze;
    char title[TROPHY_NAME_MAX], description[TROPHY_DETAIL_MAX];
} GameDetails;
typedef struct {
    uint64_t size;
    uint32_t unlocked_trophies, unlocked_platinum, unlocked_gold, unlocked_silver, unlocked_bronze, progress_percentage;
} GameData;
typedef struct {
    uint64_t size;
    int32_t group_id;
    uint32_t num_trophies, num_platinum, num_gold, num_silver, num_bronze;
    char title[TROPHY_NAME_MAX], description[TROPHY_DETAIL_MAX];
} GroupDetails;
typedef struct {
    uint64_t size;
    int32_t group_id;
    uint32_t unlocked_trophies, unlocked_platinum, unlocked_gold, unlocked_silver, unlocked_bronze, progress_percentage;
    uint8_t reserved[4];
} GroupData;
typedef struct {
    uint64_t size;
    int32_t trophy_id, trophy_grade, group_id;
    bool hidden;
    uint8_t reserved[3];
    char name[TROPHY_NAME_MAX], description[TROPHY_DETAIL_MAX];
} TrophyDetails;
typedef struct {
    uint64_t size;
    int32_t trophy_id;
    bool unlocked;
    uint8_t reserved[3];
    uint64_t timestamp;
} TrophyData;
_Static_assert(sizeof(GameDetails) == 0x4A0 && sizeof(GameData) == 0x20, "OrbisNpTrophyGame* layouts");
_Static_assert(sizeof(GroupDetails) == 0x4A0 && sizeof(GroupData) == 0x28, "OrbisNpTrophyGroup* layouts");
_Static_assert(sizeof(TrophyDetails) == 0x498 && sizeof(TrophyData) == 0x18, "OrbisNpTrophy details and data layouts");

typedef struct TrophyContext {
    int32_t h_context, user;
    uint32_t label;
    bool is_registered;
    TrophyPack *p_pack;
    char sz_np_comm_id[16], sz_state_dir[700];
    uint32_t unlocked[TROPHY_NUM_MAX / 32];
    uint64_t unlock_ticks[TROPHY_NUM_MAX];
} TrophyContext;
typedef struct {
    uint32_t c_trophies, c_unlocked, by_grade[5], unlocked_by_grade[5];
} Tally;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static TrophyContext *s_p_contexts[C_CONTEXTS_MAX];

static int32_t new_object(uint32_t kind, int32_t *p_out_id) {
    const int64_t h_object=np_object_create(kind);
    if (h_object<=0 || h_object>INT32_MAX) return TROPHY_OUT_OF_MEMORY;
    *p_out_id=(int32_t)h_object;
    return 0;
}
static int32_t check(int32_t h_context, int32_t h_handle) {
    if (!np_object_exists(NP_TROPHY_CONTEXT,h_context)) return TROPHY_INVALID_CONTEXT;
    return np_object_exists(NP_TROPHY_HANDLE,h_handle) ? 0 : TROPHY_INVALID_HANDLE;
}
static TrophyContext **find_context(int32_t h_context) {
    for (size_t i = 0; i < C_CONTEXTS_MAX; ++i)
        if (s_p_contexts[i] && s_p_contexts[i]->h_context == h_context) return &s_p_contexts[i];
    return NULL;
}
/* Under s_lock: the registered context behind the pair, or why there is none. */
static int32_t find_registered(int32_t h_context, int32_t h_handle, TrophyContext **pp_out_context) {
    const int32_t result = check(h_context, h_handle);
    if (result) return result;
    TrophyContext **pp_context = find_context(h_context);
    if (!pp_context) return TROPHY_INVALID_CONTEXT;
    if (!(*pp_context)->is_registered) return TROPHY_NOT_REGISTERED;
    *pp_out_context = *pp_context;
    return 0;
}
static bool is_unlocked(const TrophyContext *p_context, int32_t id) {
    return p_context->unlocked[id / 32] >> (id % 32) & 1;
}
/* Trophy ids the context accepts: the pack's, or any id the API allows without one. */
static uint32_t count_ids(const TrophyContext *p_context) {
    return p_context->p_pack ? p_context->p_pack->c_trophies : TROPHY_NUM_MAX;
}
static uint32_t count_trophies(const TrophyContext *p_context) {
    return p_context->p_pack ? p_context->p_pack->c_trophies : 0;
}
static const char *grade_name(int32_t grade) {
    static const char *const s_sz_names[] = {"unknown", "platinum", "gold", "silver", "bronze"};
    return grade >= 0 && grade <= TROPHY_GRADE_BRONZE ? s_sz_names[grade] : "unknown";
}

static uint64_t current_tick(void) {
    return RTC_TICK_UNIX_EPOCH + ps_host_time_realtime_ns() / 1000;
}
static void format_tick(uint64_t tick, char *sz_out, size_t cb_out) {
    struct tm utc = {0};
    if (ps_host_time_utc_fields((int64_t)((tick - RTC_TICK_UNIX_EPOCH) / 1000000), &utc))
        fprintf(stderr, "Trophy: cannot split tick %llu into calendar fields\n", (unsigned long long)tick);
    const size_t cb_used = strftime(sz_out, cb_out, "%Y-%m-%dT%H:%M:%S", &utc);
    snprintf(sz_out + cb_used, cb_out - cb_used, ".%06uZ", (unsigned)((tick - RTC_TICK_UNIX_EPOCH) % 1000000));
}
static bool parse_tick(const char *sz_time, uint64_t *p_out_tick) {
    struct tm utc = {0};
    int cb_parsed = 0;
    if (sscanf(sz_time, "%4d-%2d-%2dT%2d:%2d:%2d%n", &utc.tm_year, &utc.tm_mon, &utc.tm_mday, &utc.tm_hour, &utc.tm_min,
               &utc.tm_sec, &cb_parsed) != 6 || !cb_parsed)
        return false;
    utc.tm_year -= 1900;
    --utc.tm_mon;
    char *sz_rest = (char *)sz_time + cb_parsed;
    unsigned long microseconds = 0;
    if (*sz_rest == '.') microseconds = strtoul(sz_rest + 1, &sz_rest, 10);
    int64_t seconds;
    if (ps_host_time_utc_seconds(&utc, &seconds)) return false;
    if (*sz_rest != 'Z' || seconds < 0 || microseconds >= 1000000) return false;
    *p_out_tick = RTC_TICK_UNIX_EPOCH + (uint64_t)seconds * 1000000 + microseconds;
    return true;
}

static bool is_directory_name(const char *sz_name) {
    if (!*sz_name || strlen(sz_name) > 15) return false;
    for (const char *sz_cursor = sz_name; *sz_cursor; ++sz_cursor)
        if (!((*sz_cursor >= 'A' && *sz_cursor <= 'Z') || (*sz_cursor >= 'a' && *sz_cursor <= 'z') || (*sz_cursor >= '0' && *sz_cursor <= '9') || *sz_cursor == '_' || *sz_cursor == '-'))
            return false;
    return true;
}
/* A whole file replaced in one rename, so a crash mid-write keeps the previous one. */
/* A file written whole replaces sz_path in one step: begun into "<path>.tmp", then committed. */
static FILE *begin_atomic(const char *sz_path, char *sz_out_temp, size_t cb_out_temp) {
    if (snprintf(sz_out_temp, cb_out_temp, "%s.tmp", sz_path) >= (int)cb_out_temp) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return ps_host_fs_open_stream(sz_out_temp, "wb");
}
static bool commit_atomic(FILE *p_file, bool is_written, const char *sz_temp, const char *sz_path) {
    is_written = is_written && !fflush(p_file) && !ps_host_fs_sync_stream(p_file);
    is_written = !fclose(p_file) && is_written;
    if (!is_written || ps_host_fs_rename(sz_temp, sz_path)) {
        ps_host_fs_remove(sz_temp);
        return false;
    }
    return true;
}
static bool write_atomic(const char *sz_path, const void *p_data, size_t cb_data) {
    char sz_temp[760];
    FILE *p_file = begin_atomic(sz_path, sz_temp, sizeof(sz_temp));
    return p_file && commit_atomic(p_file, fwrite(p_data, 1, cb_data, p_file) == cb_data, sz_temp, sz_path);
}
static void state_path(const TrophyContext *p_context, char *sz_out, size_t cb_out) {
    snprintf(sz_out, cb_out, "%s/unlocks_%d.tsv", p_context->sz_state_dir, p_context->user);
}
static uint32_t load_state(TrophyContext *p_context) {
    char sz_path[760], sz_line[128];
    state_path(p_context, sz_path, sizeof(sz_path));
    FILE *p_file = ps_host_fs_open_stream(sz_path, "r");
    if (!p_file) return 0;
    uint32_t c_unlocked = 0;
    while (fgets(sz_line, sizeof(sz_line), p_file)) {
        int id, unlocked;
        char sz_time[40] = "";
        if (sz_line[0] == '#' || sscanf(sz_line, "%d\t%d\t%39s", &id, &unlocked, sz_time) < 2) continue;
        if (id < 0 || id >= TROPHY_NUM_MAX || unlocked != 1 || is_unlocked(p_context, id)) continue;
        p_context->unlocked[id / 32] |= 1u << (id % 32);
        if (!parse_tick(sz_time, &p_context->unlock_ticks[id])) p_context->unlock_ticks[id] = 0;
        c_unlocked++;
    }
    fclose(p_file);
    return c_unlocked;
}
static bool save_state(const TrophyContext *p_context) {
    char sz_path[760], sz_text[64 + TROPHY_NUM_MAX * 48];
    uint32_t c_listed = count_trophies(p_context);
    for (uint32_t i = c_listed; i < TROPHY_NUM_MAX; ++i)
        if (is_unlocked(p_context, (int32_t)i)) c_listed = i + 1;
    size_t cb_text = (size_t)snprintf(sz_text, sizeof(sz_text), "# psport trophy unlocks 1: id, unlocked, unlock time (UTC)\n");
    for (uint32_t i = 0; i < c_listed; ++i) {
        char sz_time[40] = "";
        if (is_unlocked(p_context, (int32_t)i) && p_context->unlock_ticks[i]) format_tick(p_context->unlock_ticks[i], sz_time, sizeof(sz_time));
        cb_text += (size_t)snprintf(sz_text + cb_text, sizeof(sz_text) - cb_text, "%u\t%d\t%s\n", i, is_unlocked(p_context, (int32_t)i), sz_time);
    }
    state_path(p_context, sz_path, sizeof(sz_path));
    return write_atomic(sz_path, sz_text, cb_text);
}

static void put_json_string(FILE *p_out, const char *sz_text) {
    fputc('"', p_out);
    for (const unsigned char *p_cursor = (const unsigned char *)sz_text; *p_cursor; ++p_cursor)
        if (*p_cursor == '"' || *p_cursor == '\\') fprintf(p_out, "\\%c", *p_cursor);
        else if (*p_cursor < 0x20) fprintf(p_out, "\\u%04x", *p_cursor);
        else fputc(*p_cursor, p_out);
    fputc('"', p_out);
}
static void save_trophy_set(const TrophyContext *p_context, const char *sz_pack_guest, const char *sz_configuration) {
    char sz_path[760], sz_temp[760];
    snprintf(sz_path, sizeof(sz_path), "%s/trophyset.json", p_context->sz_state_dir);
    FILE *p_out = begin_atomic(sz_path, sz_temp, sizeof(sz_temp));
    if (!p_out) {
        printf("Runtime: NpTrophy: could not write %s: %s\n", sz_path, strerror(errno));
        return;
    }
    const TrophyPack *p_pack = p_context->p_pack;
    fprintf(p_out, "{\n  \"format\": 1,\n  \"np_comm_id\": ");
    put_json_string(p_out, p_context->sz_np_comm_id);
    fprintf(p_out, ",\n  \"pack\": ");
    put_json_string(p_out, sz_pack_guest);
    fprintf(p_out, ",\n  \"configuration\": \"%s\",\n  \"language\": %d,\n  \"names\": ", sz_configuration, runtime_system_language());
    put_json_string(p_out, p_pack ? p_pack->sz_names_file : "");
    fprintf(p_out, ",\n  \"title\": ");
    put_json_string(p_out, p_pack ? p_pack->sz_title : "");
    fprintf(p_out, ",\n  \"detail\": ");
    put_json_string(p_out, p_pack ? p_pack->sz_detail : "");
    fprintf(p_out, ",\n  \"groups\": [");
    for (uint32_t i = 0; p_pack && i < p_pack->c_groups; ++i) {
        fprintf(p_out, "%s\n    {\"id\": %d, \"icon\": \"GR%03d.PNG\", \"name\": ", i ? "," : "", p_pack->groups[i].id, p_pack->groups[i].id);
        put_json_string(p_out, p_pack->groups[i].sz_name);
        fprintf(p_out, ", \"detail\": ");
        put_json_string(p_out, p_pack->groups[i].sz_detail);
        fputc('}', p_out);
    }
    fprintf(p_out, "%s],\n  \"trophies\": [", p_pack && p_pack->c_groups ? "\n  " : "");
    for (uint32_t i = 0; i < count_trophies(p_context); ++i) {
        const TrophyEntry *p_trophy = &p_pack->trophies[i];
        fprintf(p_out, "%s\n    {\"id\": %u, \"icon\": \"TROP%03u.PNG\", \"grade\": \"%s\", \"hidden\": %s, \"group\": %d, \"platinum\": %d, \"name\": ",
                i ? "," : "", i, i, grade_name(p_trophy->grade), p_trophy->is_hidden ? "true" : "false", p_trophy->group, p_trophy->platinum);
        put_json_string(p_out, p_trophy->sz_name);
        fprintf(p_out, ", \"detail\": ");
        put_json_string(p_out, p_trophy->sz_detail);
        fputc('}', p_out);
    }
    fprintf(p_out, "%s]\n}\n", count_trophies(p_context) ? "\n  " : "");
    if (!commit_atomic(p_out, !ferror(p_out), sz_temp, sz_path))
        printf("Runtime: NpTrophy: could not write %s: %s\n", sz_path, strerror(errno));
}

static ABI int32_t trophy_context(int32_t *p_ctx,int32_t user,uint32_t label,uint64_t options) {
    if (!p_ctx || options) return TROPHY_INVALID;
    pthread_mutex_lock(&s_lock);
    int32_t result = 0;
    size_t i_free = C_CONTEXTS_MAX;
    for (size_t i = 0; i < C_CONTEXTS_MAX; ++i)
        if (!s_p_contexts[i]) {
            if (i_free == C_CONTEXTS_MAX) i_free = i;
        } else if (s_p_contexts[i]->user == user && s_p_contexts[i]->label == label)
            result = TROPHY_CONTEXT_ALREADY_EXISTS;
    TrophyContext *p_context = NULL;
    if (!result && i_free == C_CONTEXTS_MAX) result = TROPHY_CONTEXT_EXCEEDS_MAX;
    if (!result && !(p_context = calloc(1, sizeof(*p_context)))) result = TROPHY_OUT_OF_MEMORY;
    if (!result && (result = new_object(NP_TROPHY_CONTEXT, &p_context->h_context))) free(p_context);
    if (!result) {
        p_context->user = user;
        p_context->label = label;
        s_p_contexts[i_free] = p_context;
        *p_ctx = p_context->h_context;
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}
static ABI int32_t trophy_handle(int32_t *p_handle) { if (!p_handle) return TROPHY_INVALID; return new_object(NP_TROPHY_HANDLE,p_handle); }
static ABI int32_t trophy_destroy_context(int32_t h_context) {
    pthread_mutex_lock(&s_lock);
    TrophyContext **pp_context = find_context(h_context);
    if (pp_context) {
        if ((*pp_context)->p_pack) trophy_pack_close((*pp_context)->p_pack);
        free((*pp_context)->p_pack);
        free(*pp_context);
        *pp_context = NULL;
    }
    pthread_mutex_unlock(&s_lock);
    return np_object_delete(NP_TROPHY_CONTEXT,h_context) ? 0 : TROPHY_INVALID_CONTEXT;
}
static ABI int32_t trophy_destroy_handle(int32_t h_handle) {
    return np_object_delete(NP_TROPHY_HANDLE,h_handle) ? 0 : TROPHY_INVALID_HANDLE;
}
/* Every call on a handle completes before it returns, so an abort finds nothing in progress. */
static ABI int32_t trophy_abort_handle(int32_t h_handle) {
    return np_object_exists(NP_TROPHY_HANDLE,h_handle) ? 0 : TROPHY_INVALID_HANDLE;
}

/* Loads the pack of the context's service label and the user's unlocks. A title without a pack the
 * port can read still registers, as before the port read packs, and keeps its unlocks. */
static void register_pack(TrophyContext *p_context) {
    static uint64_t s_c_unreadable;
    char sz_pack_guest[64], sz_pack_host[1024], sz_npbind_host[1024], sz_reason[320] = "";
    snprintf(sz_pack_guest, sizeof(sz_pack_guest), "/app0/sce_sys/trophy/trophy%02u.trp", p_context->label);
    const char *sz_configuration = "missing";
    TrophyPack *p_pack = malloc(sizeof(*p_pack));
    if (p_pack && !runtime_file_translate(sz_pack_guest, sz_pack_host, sizeof(sz_pack_host)) && !ps_host_fs_access(sz_pack_host, PS_HOST_ACCESS_READ) &&
        trophy_pack_open(sz_pack_host, runtime_system_language(), p_pack)) {
        p_context->p_pack = p_pack;
        sz_configuration = p_pack->has_config ? "plain" : p_pack->sz_encrypted_file[0] ? "encrypted" : "invalid";
        if (p_pack->has_config)
            snprintf(sz_reason, sizeof(sz_reason), "configuration TROPCONF.SFM, names from %s",
                     p_pack->sz_names_file[0] ? p_pack->sz_names_file : "TROPCONF.SFM");
        else
            snprintf(sz_reason, sizeof(sz_reason), "configuration %s%s: trophies and groups counted from their icons, no names, grades, groups or platinum",
                     p_pack->sz_encrypted_file[0] ? p_pack->sz_encrypted_file : "TROPCONF.SFM unreadable",
                     p_pack->sz_encrypted_file[0] ? " encrypted with the console's trophy key, which the port does not have" : "");
    } else {
        free(p_pack);
        snprintf(sz_reason, sizeof(sz_reason), "no trophy pack the port can read at %s", sz_pack_guest);
    }
    if (runtime_file_translate("/app0/sce_sys/npbind.dat", sz_npbind_host, sizeof(sz_npbind_host)) ||
        !trophy_npbind_comm_id(sz_npbind_host, p_context->label, p_context->sz_np_comm_id)) {
        const char *sz_fallback = p_context->p_pack && is_directory_name(p_context->p_pack->sz_np_comm_id) ? p_context->p_pack->sz_np_comm_id
                                                                                                           : runtime_savedata_title();
        snprintf(p_context->sz_np_comm_id, sizeof(p_context->sz_np_comm_id), "%s", is_directory_name(sz_fallback) ? sz_fallback : "UNKNOWN");
    }
    char sz_trophy_root[600];
    snprintf(sz_trophy_root, sizeof(sz_trophy_root), "%s/trophy", runtime_file_user_dir());
    snprintf(p_context->sz_state_dir, sizeof(p_context->sz_state_dir), "%s/%s", sz_trophy_root, p_context->sz_np_comm_id);
    int error = ps_host_fs_make_dir(sz_trophy_root, 0755);
    if (!error || error == EEXIST) error = ps_host_fs_make_dir(p_context->sz_state_dir, 0755);
    if (error && error != EEXIST)
        printf("Runtime: NpTrophy: could not create %s: %s; unlocks are not kept\n", p_context->sz_state_dir, strerror(error));
    const uint32_t c_unlocked = load_state(p_context);
    save_trophy_set(p_context, sz_pack_guest, sz_configuration);
    printf("Runtime: NpTrophy: context %d (user %d, service label %u) registered: NP communication id %s, %u trophies, %u groups, %s; %u unlocked (%s)\n",
           p_context->h_context, p_context->user, p_context->label, p_context->sz_np_comm_id, count_trophies(p_context),
           p_context->p_pack ? p_context->p_pack->c_groups : 0, sz_reason, c_unlocked, p_context->sz_state_dir);
    if ((!p_context->p_pack || !p_context->p_pack->has_config) && !__atomic_fetch_add(&s_c_unreadable, 1, __ATOMIC_RELAXED))
        runtime_calls_hle_stub(p_context->p_pack ? "sceNpTrophyRegisterContext (trophy configuration unreadable: no names, grades or platinum)"
                                                 : "sceNpTrophyRegisterContext (no trophy pack)",
                               &s_c_unreadable);
}
static ABI int32_t trophy_register(int32_t h_context, int32_t h_handle, uint64_t options) {
    if (options) return TROPHY_INVALID;
    pthread_mutex_lock(&s_lock);
    int32_t result = check(h_context, h_handle);
    TrophyContext **pp_context = result ? NULL : find_context(h_context);
    if (!result && !pp_context) result = TROPHY_INVALID_CONTEXT;
    if (!result && (*pp_context)->is_registered) result = TROPHY_ALREADY_REGISTERED;
    if (!result) {
        register_pack(*pp_context);
        (*pp_context)->is_registered = true;
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}

static ABI int32_t trophy_unlock(int32_t ctx,int32_t h_handle,int32_t id,int32_t *p_platinum) {
    if (id < 0 || id >= TROPHY_NUM_MAX) return TROPHY_INVALID_TROPHY_ID;
    if (!p_platinum) return TROPHY_INVALID;
    pthread_mutex_lock(&s_lock);
    TrophyContext *p_context = NULL;
    int32_t result = find_registered(ctx, h_handle, &p_context);
    const TrophyPack *p_pack = p_context ? p_context->p_pack : NULL;
    const TrophyEntry *p_trophy = p_pack && p_pack->has_config ? &p_pack->trophies[id] : NULL;
    char sz_time[40] = "";
    if (!result && (uint32_t)id >= count_ids(p_context)) result = TROPHY_INVALID_TROPHY_ID;
    if (!result && is_unlocked(p_context, id)) {
        if (p_context->unlock_ticks[id]) format_tick(p_context->unlock_ticks[id], sz_time, sizeof(sz_time));
        printf("Runtime: trophy %d already unlocked%s%s\n", id, sz_time[0] ? " at " : "", sz_time);
        result = TROPHY_ALREADY_UNLOCKED;
    }
    if (!result && p_trophy && p_trophy->grade == TROPHY_GRADE_PLATINUM) result = TROPHY_PLATINUM_CANNOT_UNLOCK;
    if (!result) {
        const uint64_t tick = current_tick();
        int32_t id_platinum = TROPHY_NO_PLATINUM;
        p_context->unlocked[id / 32] |= 1u << (id % 32);
        p_context->unlock_ticks[id] = tick;
        if (p_trophy && p_trophy->platinum >= 0 && (uint32_t)p_trophy->platinum < p_pack->c_trophies &&
            p_pack->trophies[p_trophy->platinum].grade == TROPHY_GRADE_PLATINUM && !is_unlocked(p_context, p_trophy->platinum)) {
            bool is_complete = true;
            for (uint32_t i = 0; i < p_pack->c_trophies && is_complete; ++i)
                is_complete = (int32_t)i == p_trophy->platinum || p_pack->trophies[i].platinum != p_trophy->platinum || is_unlocked(p_context, (int32_t)i);
            if (is_complete) {
                id_platinum = p_trophy->platinum;
                p_context->unlocked[id_platinum / 32] |= 1u << (id_platinum % 32);
                p_context->unlock_ticks[id_platinum] = tick;
            }
        }
        *p_platinum = id_platinum;
        format_tick(tick, sz_time, sizeof(sz_time));
        printf("Runtime: trophy %d unlocked at %s (%s%s%s)\n", id, sz_time, grade_name(p_trophy ? p_trophy->grade : TROPHY_GRADE_UNKNOWN),
               p_trophy && p_trophy->sz_name[0] ? ", " : "", p_trophy ? p_trophy->sz_name : "");
        if (id_platinum != TROPHY_NO_PLATINUM)
            printf("Runtime: trophy %d unlocked at %s (platinum, every trophy it counts is unlocked)\n", id_platinum, sz_time);
        if (!save_state(p_context))
            printf("Runtime: NpTrophy: could not save the unlocks in %s: %s; they last until the game exits\n", p_context->sz_state_dir, strerror(errno));
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}

static Tally tally(const TrophyContext *p_context, bool is_group, int32_t group) {
    Tally counts = {0};
    for (uint32_t i = 0; i < count_trophies(p_context); ++i) {
        const TrophyEntry *p_trophy = &p_context->p_pack->trophies[i];
        if (is_group && p_trophy->group != group) continue;
        const uint32_t grade = p_trophy->grade >= 0 && p_trophy->grade <= TROPHY_GRADE_BRONZE ? (uint32_t)p_trophy->grade : 0;
        counts.c_trophies++;
        counts.by_grade[grade]++;
        if (is_unlocked(p_context, (int32_t)i)) {
            counts.c_unlocked++;
            counts.unlocked_by_grade[grade]++;
        }
    }
    return counts;
}
static uint32_t progress_of(const Tally *p_counts) { return p_counts->c_trophies ? p_counts->c_unlocked * 100 / p_counts->c_trophies : 0; }

static ABI int32_t trophy_game_info(int32_t ctx,int32_t h_handle,void *p_out_details,void *p_out_data) {
    GameDetails *p_details = p_out_details;
    GameData *p_data = p_out_data;
    if (!p_details || !p_data || p_details->size != sizeof(GameDetails) || p_data->size != sizeof(GameData)) return TROPHY_INVALID;
    pthread_mutex_lock(&s_lock);
    TrophyContext *p_context = NULL;
    const int32_t result = find_registered(ctx, h_handle, &p_context);
    if (!result) {
        const Tally counts = tally(p_context, false, 0);
        const TrophyPack *p_pack = p_context->p_pack;
        memset((char *)p_details + 8, 0, sizeof(*p_details) - 8);
        memset((char *)p_data + 8, 0, sizeof(*p_data) - 8);
        p_details->num_groups = p_pack ? p_pack->c_groups : 0;
        p_details->num_trophies = counts.c_trophies;
        p_details->num_platinum = counts.by_grade[TROPHY_GRADE_PLATINUM];
        p_details->num_gold = counts.by_grade[TROPHY_GRADE_GOLD];
        p_details->num_silver = counts.by_grade[TROPHY_GRADE_SILVER];
        p_details->num_bronze = counts.by_grade[TROPHY_GRADE_BRONZE];
        if (p_pack) {
            snprintf(p_details->title, sizeof(p_details->title), "%s", p_pack->sz_title);
            snprintf(p_details->description, sizeof(p_details->description), "%s", p_pack->sz_detail);
        }
        p_data->unlocked_trophies = counts.c_unlocked;
        p_data->unlocked_platinum = counts.unlocked_by_grade[TROPHY_GRADE_PLATINUM];
        p_data->unlocked_gold = counts.unlocked_by_grade[TROPHY_GRADE_GOLD];
        p_data->unlocked_silver = counts.unlocked_by_grade[TROPHY_GRADE_SILVER];
        p_data->unlocked_bronze = counts.unlocked_by_grade[TROPHY_GRADE_BRONZE];
        p_data->progress_percentage = progress_of(&counts);
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}
static ABI int32_t trophy_info(int32_t ctx,int32_t h_handle,int32_t id,void *p_out_details,void *p_out_data) {
    TrophyDetails *p_details = p_out_details;
    TrophyData *p_data = p_out_data;
    if (id < 0 || id >= TROPHY_NUM_MAX) return TROPHY_INVALID_TROPHY_ID;
    if (!p_details || !p_data || p_details->size != sizeof(TrophyDetails) || p_data->size != sizeof(TrophyData)) return TROPHY_INVALID;
    pthread_mutex_lock(&s_lock);
    TrophyContext *p_context = NULL;
    int32_t result = find_registered(ctx, h_handle, &p_context);
    if (!result && (uint32_t)id >= count_ids(p_context)) result = TROPHY_INVALID_TROPHY_ID;
    if (!result) {
        memset((char *)p_details + 8, 0, sizeof(*p_details) - 8);
        memset((char *)p_data + 8, 0, sizeof(*p_data) - 8);
        p_details->trophy_id = p_data->trophy_id = id;
        p_details->group_id = BASE_GAME_GROUP;
        if (p_context->p_pack) {
            const TrophyEntry *p_trophy = &p_context->p_pack->trophies[id];
            p_details->trophy_grade = p_trophy->grade;
            p_details->group_id = p_trophy->group;
            p_details->hidden = p_trophy->is_hidden;
            snprintf(p_details->name, sizeof(p_details->name), "%s", p_trophy->sz_name);
            snprintf(p_details->description, sizeof(p_details->description), "%s", p_trophy->sz_detail);
        }
        p_data->unlocked = is_unlocked(p_context, id);
        p_data->timestamp = p_data->unlocked ? p_context->unlock_ticks[id] : 0;
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}
static ABI int32_t trophy_unlock_state(int32_t ctx,int32_t h_handle,uint32_t *p_flags,uint32_t *p_count) {
    if (!p_flags || !p_count) return TROPHY_INVALID;
    pthread_mutex_lock(&s_lock);
    TrophyContext *p_context = NULL;
    const int32_t result = find_registered(ctx, h_handle, &p_context);
    if (!result) {
        memcpy(p_flags, p_context->unlocked, sizeof(p_context->unlocked));
        *p_count = count_trophies(p_context);
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}

/* The SDK's size query: without a buffer the icon's size goes to *p_cb_icon; with one, the icon is
 * copied when *p_cb_icon holds it. */
static int32_t copy_icon(const TrophyContext *p_context, const char *sz_entry, void *p_buffer, uint64_t *p_cb_icon) {
    const int64_t cb_icon = p_context->p_pack ? trophy_pack_read(p_context->p_pack, sz_entry, NULL, 0) : -1;
    if (cb_icon < 0) return TROPHY_ICON_FILE_NOT_FOUND;
    if (p_buffer) {
        if (*p_cb_icon < (uint64_t)cb_icon) return TROPHY_INSUFFICIENT_BUFFER;
        runtime_memory_note_write((uintptr_t)p_buffer, (uint64_t)cb_icon);
        if (trophy_pack_read(p_context->p_pack, sz_entry, p_buffer, *p_cb_icon) != cb_icon) return TROPHY_BROKEN_DATA;
    }
    *p_cb_icon = (uint64_t)cb_icon;
    return 0;
}
static ABI int32_t trophy_game_icon(int32_t h_context, int32_t h_handle, void *p_buffer, uint64_t *p_cb_icon) {
    if (!p_cb_icon) return TROPHY_INVALID;
    pthread_mutex_lock(&s_lock);
    TrophyContext *p_context = NULL;
    int32_t result = find_registered(h_context, h_handle, &p_context);
    if (!result) {
        char sz_icon[16];
        snprintf(sz_icon, sizeof(sz_icon), "ICON0_%02d.PNG", runtime_system_language());
        result = copy_icon(p_context, sz_icon, p_buffer, p_cb_icon);
        if (result == TROPHY_ICON_FILE_NOT_FOUND) result = copy_icon(p_context, "ICON0.PNG", p_buffer, p_cb_icon);
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}
static ABI int32_t trophy_group_icon(int32_t h_context, int32_t h_handle, int32_t group, void *p_buffer, uint64_t *p_cb_icon) {
    if (!p_cb_icon) return TROPHY_INVALID;
    pthread_mutex_lock(&s_lock);
    TrophyContext *p_context = NULL;
    int32_t result = find_registered(h_context, h_handle, &p_context);
    if (!result && group != BASE_GAME_GROUP && (!p_context->p_pack || !trophy_pack_group(p_context->p_pack, group)))
        result = TROPHY_INVALID_GROUP_ID;
    if (!result) {
        char sz_icon[16] = "ICON0.PNG";
        if (group != BASE_GAME_GROUP) snprintf(sz_icon, sizeof(sz_icon), "GR%03d.PNG", group);
        result = copy_icon(p_context, sz_icon, p_buffer, p_cb_icon);
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}
/* A hidden trophy's icon stays the system's secret until it is unlocked. */
static ABI int32_t trophy_icon(int32_t h_context, int32_t h_handle, int32_t trophy, void *p_buffer, uint64_t *p_cb_icon) {
    if (!p_cb_icon) return TROPHY_INVALID;
    if (trophy<0 || trophy>=TROPHY_NUM_MAX) return TROPHY_INVALID_TROPHY_ID;
    pthread_mutex_lock(&s_lock);
    TrophyContext *p_context = NULL;
    int32_t result = find_registered(h_context, h_handle, &p_context);
    if (!result && (uint32_t)trophy >= count_ids(p_context)) result = TROPHY_INVALID_TROPHY_ID;
    if (!result && p_context->p_pack && p_context->p_pack->trophies[trophy].is_hidden && !is_unlocked(p_context, trophy))
        result = TROPHY_NOT_UNLOCKED;
    if (!result) {
        char sz_icon[16];
        snprintf(sz_icon, sizeof(sz_icon), "TROP%03d.PNG", trophy);
        result = copy_icon(p_context, sz_icon, p_buffer, p_cb_icon);
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}
/* The base game's group is the trophies outside every group, under the game's title. */
static ABI int32_t trophy_group_info(int32_t h_context, int32_t h_handle, int32_t group, void *p_details, void *p_data) {
    GroupDetails *p_group_details = p_details;
    GroupData *p_group_data = p_data;
    if (!p_group_details || !p_group_data || p_group_details->size != sizeof(GroupDetails) || p_group_data->size != sizeof(GroupData))
        return TROPHY_INVALID;
    pthread_mutex_lock(&s_lock);
    TrophyContext *p_context = NULL;
    int32_t result = find_registered(h_context, h_handle, &p_context);
    TrophyPack *p_pack = p_context ? p_context->p_pack : NULL;
    const TrophyGroup *p_group = !result && p_pack && group != BASE_GAME_GROUP ? trophy_pack_group(p_pack, group) : NULL;
    if (!result && group != BASE_GAME_GROUP && !p_group) result = TROPHY_INVALID_GROUP_ID;
    if (!result) {
        const Tally counts = tally(p_context, true, group);
        memset((char *)p_group_details + 8, 0, sizeof(*p_group_details) - 8);
        memset((char *)p_group_data + 8, 0, sizeof(*p_group_data) - 8);
        p_group_details->group_id = p_group_data->group_id = group;
        p_group_details->num_trophies = counts.c_trophies;
        p_group_details->num_platinum = counts.by_grade[TROPHY_GRADE_PLATINUM];
        p_group_details->num_gold = counts.by_grade[TROPHY_GRADE_GOLD];
        p_group_details->num_silver = counts.by_grade[TROPHY_GRADE_SILVER];
        p_group_details->num_bronze = counts.by_grade[TROPHY_GRADE_BRONZE];
        if (p_pack) {
            snprintf(p_group_details->title, sizeof(p_group_details->title), "%s", p_group ? p_group->sz_name : p_pack->sz_title);
            snprintf(p_group_details->description, sizeof(p_group_details->description), "%s", p_group ? p_group->sz_detail : p_pack->sz_detail);
        }
        p_group_data->unlocked_trophies = counts.c_unlocked;
        p_group_data->unlocked_platinum = counts.unlocked_by_grade[TROPHY_GRADE_PLATINUM];
        p_group_data->unlocked_gold = counts.unlocked_by_grade[TROPHY_GRADE_GOLD];
        p_group_data->unlocked_silver = counts.unlocked_by_grade[TROPHY_GRADE_SILVER];
        p_group_data->unlocked_bronze = counts.unlocked_by_grade[TROPHY_GRADE_BRONZE];
        p_group_data->progress_percentage = progress_of(&counts);
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}
/* The trophy list is the system UI's, which this port does not have. */
static ABI int32_t trophy_show_list(int32_t h_context, int32_t h_handle) {
    static uint64_t s_c_shown;
    const int32_t result=check(h_context,h_handle);
    if (result) return result;
    if (!__atomic_fetch_add(&s_c_shown,1,__ATOMIC_RELAXED)) {
        puts("Runtime: NpTrophy: the system's trophy list was asked for; there is no system UI to show it");
        runtime_calls_hle_stub("sceNpTrophyShowTrophyList (no system UI)",&s_c_shown);
    }
    return 0;
}
/* The screenshot the system would attach to the trophy: the port takes none. */
static ABI int32_t trophy_capture_screenshot(int32_t h_handle, const void *p_targets, uint32_t c_targets) {
    static uint64_t s_c_captures;
    (void)p_targets; (void)c_targets;
    if (!np_object_exists(NP_TROPHY_HANDLE,h_handle)) return TROPHY_INVALID_HANDLE;
    if (!__atomic_fetch_add(&s_c_captures,1,__ATOMIC_RELAXED)) {
        puts("Runtime: NpTrophy: trophy screenshot asked for; the port takes none");
        runtime_calls_hle_stub("sceNpTrophyCaptureScreenshot (no screenshot taken)",&s_c_captures);
    }
    return 0;
}

static const RuntimeExport s_exports[]={
    {"sceNpTrophyCreateContext",trophy_context}, {"sceNpTrophyCreateHandle",trophy_handle},
    {"sceNpTrophyRegisterContext",trophy_register}, {"sceNpTrophyUnlockTrophy",trophy_unlock},
    {"sceNpTrophyDestroyHandle",trophy_destroy_handle}, {"sceNpTrophyDestroyContext",trophy_destroy_context},
    {"sceNpTrophyAbortHandle",trophy_abort_handle}, {"sceNpTrophyGetGameIcon",trophy_game_icon},
    {"sceNpTrophyGetGroupIcon",trophy_group_icon}, {"sceNpTrophyGetTrophyIcon",trophy_icon},
    {"sceNpTrophyGetGroupInfo",trophy_group_info}, {"sceNpTrophyShowTrophyList",trophy_show_list},
    {"sceNpTrophyCaptureScreenshot",trophy_capture_screenshot},
    {"sceNpTrophyGetGameInfo",trophy_game_info}, {"sceNpTrophyGetTrophyInfo",trophy_info},
    {"sceNpTrophyGetTrophyUnlockState",trophy_unlock_state},
};
uintptr_t runtime_trophy_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
