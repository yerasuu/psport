/* The quirk registry's evaluation (docs/en/QUIRKS.md): PS_QUIRKS, the policy per kind, the log
 * lines, the published bitset, the exit summary and psport --list-quirks. */
#define _GNU_SOURCE
#include "runtime_quirks.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUIRK(id, kind, sz_reason, sz_reference)                                                                   \
    _Static_assert(sizeof("" sz_reason) > 1, "quirk " #id ": the reason must be a non-empty string literal");     \
    _Static_assert(sizeof("" sz_reference) > 1, "quirk " #id ": the reference must be a non-empty string literal"); \
    enum { KIND_OF_##id = (int)RUNTIME_QUIRK_KIND_##kind };
#define QUIRK_GPU(id, vendor, driver_id, sz_device, version_first, version_last)                               \
    _Static_assert((int)KIND_OF_##id == (int)RUNTIME_QUIRK_KIND_HOST, "quirk " #id ": only HOST quirks match on the GPU"); \
    _Static_assert(sizeof("" sz_device) > 0, "quirk " #id ": the device is a string literal");
#define QUIRK_TRAITS(id, traits) _Static_assert((traits) != 0, "quirk " #id ": an empty trait mask matches nothing");
#include "runtime_quirks.inc"
#undef QUIRK
#undef QUIRK_GPU
#undef QUIRK_TRAITS
_Static_assert(RUNTIME_QUIRK_COUNT <= RUNTIME_QUIRK_WORDS * 64, "more quirks than g_runtime_quirks holds");

typedef struct {
    const char *sz_id, *sz_reason, *sz_reference;
    RuntimeQuirkKind kind;
} Quirk;
/* Each table ends with an empty entry, so that an empty registry still makes valid tables. */
static const Quirk s_quirks[] = {
#define QUIRK(id, kind, sz_reason, sz_reference) {#id, sz_reason, sz_reference, RUNTIME_QUIRK_KIND_##kind},
#define QUIRK_GPU(id, vendor, driver_id, sz_device, version_first, version_last)
#define QUIRK_TRAITS(id, traits)
#include "runtime_quirks.inc"
#undef QUIRK
#undef QUIRK_GPU
#undef QUIRK_TRAITS
    {NULL, NULL, NULL, RUNTIME_QUIRK_KIND_LAUNCH},
};
static const struct {
    RuntimeQuirkId id;
    uint32_t vendor, driver_id;
    const char *sz_device;
    uint32_t version_first, version_last;
} s_gpu_matches[] = {
#define QUIRK(id, kind, sz_reason, sz_reference)
#define QUIRK_GPU(id, vendor, driver_id, sz_device, version_first, version_last) \
    {RUNTIME_QUIRK_##id, vendor, driver_id, sz_device, version_first, version_last},
#define QUIRK_TRAITS(id, traits)
#include "runtime_quirks.inc"
#undef QUIRK
#undef QUIRK_GPU
#undef QUIRK_TRAITS
    {RUNTIME_QUIRK_COUNT, 0, 0, NULL, 0, 0},
};
static const struct {
    RuntimeQuirkId id;
    uint64_t traits;
} s_trait_matches[] = {
#define QUIRK(id, kind, sz_reason, sz_reference)
#define QUIRK_GPU(id, vendor, driver_id, sz_device, version_first, version_last)
#define QUIRK_TRAITS(id, traits) {RUNTIME_QUIRK_##id, traits},
#include "runtime_quirks.inc"
#undef QUIRK
#undef QUIRK_GPU
#undef QUIRK_TRAITS
    {RUNTIME_QUIRK_COUNT, 0},
};
static const char *const s_kind_names[] = {"LAUNCH", "OPTIMIZATION", "HOST", "WORKAROUND"};

uint64_t g_runtime_quirks[RUNTIME_QUIRK_WORDS];

typedef uint64_t QuirkSet[RUNTIME_QUIRK_WORDS];
static QuirkSet s_disabled, s_forced, s_decided, s_active;
static int s_is_none, s_is_profile_off, s_has_configured, s_has_noted_gpu;
static char s_summaries[2][4096];
static const char *s_sz_summary = "Quirks: 0 active\n";

typedef enum { POLICY_ACTIVE, POLICY_FORCED, POLICY_PENDING, POLICY_USER, POLICY_NONE, POLICY_PROFILE_OFF } Policy;

static int has(const QuirkSet set, RuntimeQuirkId id) { return (int)((set[id / 64] >> (id % 64)) & 1); }
static void put_in(QuirkSet set, RuntimeQuirkId id) { set[id / 64] |= UINT64_C(1) << (id % 64); }
static void take_from(QuirkSet set, RuntimeQuirkId id) { set[id / 64] &= ~(UINT64_C(1) << (id % 64)); }

static int is_known(RuntimeQuirkId id) {
    for (const Quirk *p_quirk = s_quirks; p_quirk->sz_id; ++p_quirk)
        if (p_quirk - s_quirks == id) return 1;
    return 0;
}

static int find(const char *sz_id, RuntimeQuirkId *p_out_id) {
    for (const Quirk *p_quirk = s_quirks; p_quirk->sz_id; ++p_quirk)
        if (!strcmp(p_quirk->sz_id, sz_id)) {
            *p_out_id = (RuntimeQuirkId)(p_quirk - s_quirks);
            return 1;
        }
    return 0;
}

/* PS_QUIRKS, read once: comma-separated, left to right; "none" turns off every quirk but LAUNCH,
 * "-id" one quirk and "+id" forces one on. */
static void configure(void) {
    if (s_has_configured) return;
    s_has_configured = 1;
    const char *sz_profile = getenv("PS_GAME_PROFILE");
    s_is_profile_off = sz_profile && !strcmp(sz_profile, "none");
    const char *sz_setting = getenv("PS_QUIRKS");
    char *sz_copy = sz_setting ? strdup(sz_setting) : NULL;
    char *p_state = NULL;
    for (char *sz_item = sz_copy ? strtok_r(sz_copy, ",", &p_state) : NULL; sz_item; sz_item = strtok_r(NULL, ",", &p_state)) {
        while (*sz_item == ' ') ++sz_item;
        for (size_t cb_item = strlen(sz_item); cb_item && sz_item[cb_item - 1] == ' ';) sz_item[--cb_item] = '\0';
        RuntimeQuirkId id;
        if (!*sz_item) continue;
        if (!strcmp(sz_item, "none")) {
            s_is_none = 1;
        } else if ((*sz_item != '-' && *sz_item != '+') || !sz_item[1]) {
            fprintf(stderr, "ERROR: Quirks: PS_QUIRKS entry '%s' ignored: expected none, -<id> or +<id>\n", sz_item);
        } else if (!find(sz_item + 1, &id)) {
            fprintf(stderr, "ERROR: Quirks: PS_QUIRKS names an unknown quirk '%s' (psport --list-quirks lists them)\n", sz_item + 1);
        } else if (*sz_item == '-') {
            put_in(s_disabled, id);
            take_from(s_forced, id);
        } else {
            put_in(s_forced, id);
            take_from(s_disabled, id);
        }
    }
    free(sz_copy);
}

/* WORKAROUND stays off whatever PS_QUIRKS says until decision #331 sets the policy for
 * title-specific fixes of the emulation. */
static Policy decide(RuntimeQuirkId id) {
    const RuntimeQuirkKind kind = s_quirks[id].kind;
    if (kind == RUNTIME_QUIRK_KIND_WORKAROUND) return POLICY_PENDING;
    if (has(s_forced, id)) return POLICY_FORCED;
    if (has(s_disabled, id)) return POLICY_USER;
    if (s_is_none && kind != RUNTIME_QUIRK_KIND_LAUNCH) return POLICY_NONE;
    if (s_is_profile_off && kind == RUNTIME_QUIRK_KIND_OPTIMIZATION) return POLICY_PROFILE_OFF;
    return POLICY_ACTIVE;
}

static int describe_policy(char *sz_out, size_t cb_out, RuntimeQuirkId id, Policy policy) {
    switch (policy) {
    case POLICY_ACTIVE: return snprintf(sz_out, cb_out, "active");
    case POLICY_FORCED: return snprintf(sz_out, cb_out, "forced by PS_QUIRKS=+%s", s_quirks[id].sz_id);
    case POLICY_PENDING: return snprintf(sz_out, cb_out, "disabled: policy pending (#331)");
    case POLICY_USER: return snprintf(sz_out, cb_out, "disabled by PS_QUIRKS=-%s", s_quirks[id].sz_id);
    case POLICY_NONE: return snprintf(sz_out, cb_out, "disabled by PS_QUIRKS=none");
    case POLICY_PROFILE_OFF: return snprintf(sz_out, cb_out, "disabled by PS_GAME_PROFILE=none");
    }
    return snprintf(sz_out, cb_out, "?");
}

static void consider(RuntimeQuirkId id, const char *sz_matched) {
    if (has(s_decided, id)) return;
    put_in(s_decided, id);
    const Quirk *p_quirk = &s_quirks[id];
    const Policy policy = decide(id);
    if (policy == POLICY_ACTIVE || policy == POLICY_FORCED) put_in(s_active, id);
    if (policy == POLICY_ACTIVE) {
        printf("Quirk: %s (%s) for %s: %s [PS_QUIRKS=-%s disables]\n", p_quirk->sz_id, s_kind_names[p_quirk->kind],
               sz_matched, p_quirk->sz_reason, p_quirk->sz_id);
        return;
    }
    char sz_policy[256];
    describe_policy(sz_policy, sizeof(sz_policy), id, policy);
    printf("Quirk: %s (%s) for %s: %s: %s\n", p_quirk->sz_id, s_kind_names[p_quirk->kind], sz_matched, sz_policy,
           p_quirk->sz_reason);
}

/* The bitset first, then the summary the call report prints; each written by one thread, before
 * the game's code runs (launch) or while the GPU library starts (device). */
static void publish(void) {
    static int s_i_buffer;
    char *sz_summary = s_summaries[s_i_buffer];
    s_i_buffer ^= 1;
    size_t c_active = 0;
    for (int i = 0; i < RUNTIME_QUIRK_WORDS; ++i) {
        __atomic_store_n(&g_runtime_quirks[i], s_active[i], __ATOMIC_RELEASE);
        c_active += (size_t)__builtin_popcountll(s_active[i]);
    }
    int used = snprintf(sz_summary, sizeof(s_summaries[0]), "Quirks: %zu active", c_active);
    const char *sz_separator = ": ";
    for (const Quirk *p_quirk = s_quirks; p_quirk->sz_id && (size_t)used < sizeof(s_summaries[0]); ++p_quirk) {
        if (!has(s_active, (RuntimeQuirkId)(p_quirk - s_quirks))) continue;
        used += snprintf(sz_summary + used, sizeof(s_summaries[0]) - (size_t)used, "%s%s", sz_separator, p_quirk->sz_id);
        sz_separator = ", ";
    }
    if ((size_t)used < sizeof(s_summaries[0])) snprintf(sz_summary + used, sizeof(s_summaries[0]) - (size_t)used, "\n");
    else sz_summary[sizeof(s_summaries[0]) - 2] = '\n';
    __atomic_store_n(&s_sz_summary, sz_summary, __ATOMIC_RELEASE);
}

static void describe_title(char *sz_out, size_t cb_out, const RuntimeQuirkLaunch *p_launch) {
    snprintf(sz_out, cb_out, "%s (%s)", p_launch->sz_name ? p_launch->sz_name : "?",
             p_launch->sz_serial && *p_launch->sz_serial ? p_launch->sz_serial : "?");
}

void runtime_quirks_evaluate(const RuntimeQuirkLaunch *p_launch) {
    static int s_has_evaluated;
    if (s_has_evaluated) return;
    s_has_evaluated = 1;
    configure();
    char sz_matched[256];
    describe_title(sz_matched, sizeof(sz_matched), p_launch);
    for (size_t i = 0; i < p_launch->c_title_quirks; ++i)
        if (is_known(p_launch->p_title_quirks[i])) consider(p_launch->p_title_quirks[i], sz_matched);
        else fprintf(stderr, "ERROR: Quirks: %s names quirk %u, which is not in the registry\n", sz_matched, p_launch->p_title_quirks[i]);
    for (size_t i = 0; s_trait_matches[i].traits; ++i)
        if ((p_launch->traits & s_trait_matches[i].traits) == s_trait_matches[i].traits) {
            snprintf(sz_matched, sizeof(sz_matched), "middleware traits 0x%" PRIx64, s_trait_matches[i].traits);
            consider(s_trait_matches[i].id, sz_matched);
        }
    for (RuntimeQuirkId id = 0; s_quirks[id].sz_id; ++id)
        if (has(s_forced, id)) consider(id, "every game");
    publish();
    if (!s_active[0] && !s_active[1]) puts("Quirks: none active");
}

void runtime_quirks_note_gpu(uint32_t vendor, uint32_t driver_id, const char *sz_name, uint32_t version) {
    if (__atomic_exchange_n(&s_has_noted_gpu, 1, __ATOMIC_ACQ_REL)) {
        printf("Quirks: GPU facts already noted, %s ignored\n", sz_name);
        return;
    }
    configure();
    char sz_matched[512];
    snprintf(sz_matched, sizeof(sz_matched), "GPU %s (vendor 0x%04x, driver %u, version 0x%x)", sz_name, vendor,
             driver_id, version);
    QuirkSet before;
    memcpy(before, s_decided, sizeof(before));
    for (size_t i = 0; s_gpu_matches[i].sz_device; ++i)
        if ((!s_gpu_matches[i].vendor || s_gpu_matches[i].vendor == vendor) &&
            (!s_gpu_matches[i].driver_id || s_gpu_matches[i].driver_id == driver_id) &&
            strstr(sz_name, s_gpu_matches[i].sz_device) && version >= s_gpu_matches[i].version_first &&
            version <= s_gpu_matches[i].version_last)
            consider(s_gpu_matches[i].id, sz_matched);
    if (!memcmp(before, s_decided, sizeof(before))) {
        if (s_gpu_matches[0].sz_device) printf("Quirks: none for %s\n", sz_matched);
        return;
    }
    publish();
}

const char *runtime_quirks_summary(void) { return __atomic_load_n(&s_sz_summary, __ATOMIC_ACQUIRE); }

static void print_json_string(const char *sz_text) {
    putchar('"');
    for (const unsigned char *p_char = (const unsigned char *)sz_text; *p_char; ++p_char)
        if (*p_char == '"' || *p_char == '\\') printf("\\%c", *p_char);
        else if (*p_char < 0x20) printf("\\u%04x", *p_char);
        else putchar(*p_char);
    putchar('"');
}

static void list_applying(RuntimeQuirkId id, const char *sz_matched, int *p_is_first) {
    if (has(s_decided, id)) return;
    put_in(s_decided, id);
    char sz_policy[256];
    describe_policy(sz_policy, sizeof(sz_policy), id, decide(id));
    printf("%s\n    {\"id\": ", *p_is_first ? "" : ",");
    print_json_string(s_quirks[id].sz_id);
    printf(", \"matched\": ");
    print_json_string(sz_matched);
    printf(", \"state\": ");
    print_json_string(sz_policy);
    putchar('}');
    *p_is_first = 0;
}

void runtime_quirks_list(const RuntimeQuirkLaunch *p_launch) {
    configure();
    printf("{\"quirks\": [");
    for (const Quirk *p_quirk = s_quirks; p_quirk->sz_id; ++p_quirk) {
        const RuntimeQuirkId id = (RuntimeQuirkId)(p_quirk - s_quirks);
        printf("%s\n    {\"id\": ", p_quirk == s_quirks ? "" : ",");
        print_json_string(p_quirk->sz_id);
        printf(", \"kind\": \"%s\", \"reason\": ", s_kind_names[p_quirk->kind]);
        print_json_string(p_quirk->sz_reason);
        printf(", \"reference\": ");
        print_json_string(p_quirk->sz_reference);
        printf(", \"gpu\": [");
        const char *sz_separator = "";
        for (size_t i = 0; s_gpu_matches[i].sz_device; ++i) {
            if (s_gpu_matches[i].id != id) continue;
            printf("%s{\"vendor\": %u, \"driver_id\": %u, \"device\": ", sz_separator, s_gpu_matches[i].vendor,
                   s_gpu_matches[i].driver_id);
            print_json_string(s_gpu_matches[i].sz_device);
            printf(", \"version_first\": %u, \"version_last\": %u}", s_gpu_matches[i].version_first,
                   s_gpu_matches[i].version_last);
            sz_separator = ", ";
        }
        printf("], \"traits\": [");
        sz_separator = "";
        for (size_t i = 0; s_trait_matches[i].traits; ++i)
            if (s_trait_matches[i].id == id) {
                printf("%s%" PRIu64, sz_separator, s_trait_matches[i].traits);
                sz_separator = ", ";
            }
        printf("]}");
    }
    printf("%s]", s_quirks[0].sz_id ? "\n  " : "");
    if (p_launch) {
        printf(",\n  \"game\": {\"serial\": ");
        print_json_string(p_launch->sz_serial ? p_launch->sz_serial : "");
        printf(", \"title\": ");
        if (p_launch->sz_name) print_json_string(p_launch->sz_name);
        else printf("null");
        printf("},\n  \"applies\": [");
        char sz_matched[256];
        describe_title(sz_matched, sizeof(sz_matched), p_launch);
        int is_first = 1;
        for (size_t i = 0; i < p_launch->c_title_quirks; ++i)
            if (is_known(p_launch->p_title_quirks[i])) list_applying(p_launch->p_title_quirks[i], sz_matched, &is_first);
        for (RuntimeQuirkId id = 0; s_quirks[id].sz_id; ++id)
            if (has(s_forced, id)) list_applying(id, "every game", &is_first);
        printf("%s]", is_first ? "" : "\n  ");
    }
    printf("}\n");
}
