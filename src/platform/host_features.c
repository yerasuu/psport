// SPDX-License-Identifier: GPL-2.0-or-later
/* The host capability table's evaluation (host_features.inc): this host's state per feature, the
 * run-time probes' answers, the startup line, the exit summary and psport --list-features. */
#include "ps_host.h"
#include "runtime_json_print.h"
#include <stdio.h>

typedef enum { OS_LINUX, OS_WIN32, OS_MACOS } HostOs;
static const char *const s_os_names[] = {"linux", "win32", "macos"};
#if defined(__linux__)
static const HostOs s_this_os = OS_LINUX;
#elif defined(_WIN32)
static const HostOs s_this_os = OS_WIN32;
#elif defined(__APPLE__)
static const HostOs s_this_os = OS_MACOS;
#else
#error "the platform layer knows Linux, Windows and macOS"
#endif

typedef enum { AREA_vm, AREA_gpu_memory, AREA_fault, AREA_tls, AREA_thread, AREA_time, AREA_fs, AREA_proc, AREA_dl,
               AREA_rand, AREA_io, AREA_net, AREA_upscaler, AREA_cpu } FeatureArea;

#define FEATURE(id, area, sz_desc)                                                                                  \
    _Static_assert(sizeof("" sz_desc) > 1, "feature " #id ": the description must be a non-empty string literal");   \
    _Static_assert(AREA_##area >= 0, "feature " #id ": unknown area");
#define FEATURE_HOST(id, os, state, sz_reason, sz_ref)                                                              \
    _Static_assert(sizeof("" sz_reason) > 1, "feature " #id ": the reason must be a non-empty string literal");      \
    _Static_assert(sizeof("" sz_ref) > 1, "feature " #id ": the reference must be a non-empty string literal");      \
    _Static_assert(OS_##os >= 0 && PS_HOST_FEATURE_STATE_##state >= 0, "feature " #id ": unknown OS or state");
#define FEATURE_PROBED(id) _Static_assert(PS_HOST_FEATURE_##id < PS_HOST_FEATURE_COUNT, "feature " #id);
#include "host_features.inc"
#undef FEATURE
#undef FEATURE_HOST
#undef FEATURE_PROBED

typedef struct {
    const char *sz_id, *sz_area, *sz_desc;
} Feature;
/* Each table ends with an empty entry, so that an empty table still makes valid arrays. */
static const Feature s_features[] = {
#define FEATURE(id, area, sz_desc) {#id, #area, sz_desc},
#define FEATURE_HOST(id, os, state, sz_reason, sz_ref)
#define FEATURE_PROBED(id)
#include "host_features.inc"
#undef FEATURE
#undef FEATURE_HOST
#undef FEATURE_PROBED
    {NULL, NULL, NULL},
};
static const struct {
    PsHostFeatureId id;
    HostOs os;
    PsHostFeatureState state;
    const char *sz_reason, *sz_ref;
} s_hosts[] = {
#define FEATURE(id, area, sz_desc)
#define FEATURE_HOST(id, os, state, sz_reason, sz_ref) \
    {PS_HOST_FEATURE_##id, OS_##os, PS_HOST_FEATURE_STATE_##state, sz_reason, sz_ref},
#define FEATURE_PROBED(id)
#include "host_features.inc"
#undef FEATURE
#undef FEATURE_HOST
#undef FEATURE_PROBED
    {PS_HOST_FEATURE_COUNT, OS_LINUX, PS_HOST_FEATURE_STATE_UNAVAILABLE, NULL, NULL},
};
static const PsHostFeatureId s_probed[] = {
#define FEATURE(id, area, sz_desc)
#define FEATURE_HOST(id, os, state, sz_reason, sz_ref)
#define FEATURE_PROBED(id) PS_HOST_FEATURE_##id,
#include "host_features.inc"
#undef FEATURE
#undef FEATURE_HOST
#undef FEATURE_PROBED
    PS_HOST_FEATURE_COUNT,
};
static const char *const s_state_names[] = {"IMPLEMENTED", "EXPERIMENTAL", "UNAVAILABLE"};

typedef struct {
    PsHostFeatureState state;
    const char *sz_reason, *sz_ref;
} HostRow;

static HostRow row_for(PsHostFeatureId id, HostOs os) {
    for (size_t i = 0; s_hosts[i].sz_reason; ++i)
        if (s_hosts[i].id == id && s_hosts[i].os == os)
            return (HostRow){s_hosts[i].state, s_hosts[i].sz_reason, s_hosts[i].sz_ref};
    return (HostRow){PS_HOST_FEATURE_STATE_UNAVAILABLE, "no implementation for this OS", "#540"};
}

static int is_probed(PsHostFeatureId id) {
    for (size_t i = 0; s_probed[i] != PS_HOST_FEATURE_COUNT; ++i)
        if (s_probed[i] == id) return 1;
    return 0;
}

static struct {
    int has_answered, is_available;
    const char *sz_result;
} s_answers[PS_HOST_FEATURE_COUNT + 1];
static char s_summaries[2][2048];
static const char *s_sz_summary = "Host features: not evaluated\n";

/* "linux, 13 implemented, 0 experimental, 2 unavailable (a, b)": a probe that found the feature
 * missing counts it unavailable. */
static int describe_counts(char *sz_out, size_t cb_out) {
    size_t c_states[3] = {0};
    for (PsHostFeatureId id = 0; s_features[id].sz_id; ++id) {
        const PsHostFeatureState state = s_answers[id].has_answered && !s_answers[id].is_available
                                             ? PS_HOST_FEATURE_STATE_UNAVAILABLE
                                             : row_for(id, s_this_os).state;
        ++c_states[state];
    }
    int used = snprintf(sz_out, cb_out, "%s, %zu implemented, %zu experimental, %zu unavailable", s_os_names[s_this_os],
                        c_states[0], c_states[1], c_states[2]);
    const char *sz_separator = " (";
    for (PsHostFeatureId id = 0; s_features[id].sz_id && used > 0 && (size_t)used < cb_out; ++id) {
        if (row_for(id, s_this_os).state == PS_HOST_FEATURE_STATE_IMPLEMENTED &&
            !(s_answers[id].has_answered && !s_answers[id].is_available))
            continue;
        used += snprintf(sz_out + used, cb_out - (size_t)used, "%s%s", sz_separator, s_features[id].sz_id);
        sz_separator = ", ";
    }
    if (*sz_separator == ',' && used > 0 && (size_t)used < cb_out) used += snprintf(sz_out + used, cb_out - (size_t)used, ")");
    return used;
}

/* The summary for the call report, which may print it from a signal handler: written into the other
 * buffer, then published. */
static void publish(void) {
    static int s_i_buffer;
    char *sz_summary = s_summaries[s_i_buffer];
    const size_t cb_summary = sizeof(s_summaries[0]);
    s_i_buffer ^= 1;
    int used = snprintf(sz_summary, cb_summary, "Host features: ");
    used += describe_counts(sz_summary + used, cb_summary - (size_t)used);
    const char *sz_separator = "; probed: ";
    for (size_t i = 0; s_probed[i] != PS_HOST_FEATURE_COUNT && (size_t)used < cb_summary; ++i) {
        const PsHostFeatureId id = s_probed[i];
        used += snprintf(sz_summary + used, cb_summary - (size_t)used, "%s%s %s", sz_separator, s_features[id].sz_id,
                         s_answers[id].has_answered ? s_answers[id].sz_result : "not probed");
        sz_separator = ", ";
    }
    if ((size_t)used < cb_summary - 1) snprintf(sz_summary + used, cb_summary - (size_t)used, "\n");
    else sz_summary[cb_summary - 2] = '\n';
    __atomic_store_n(&s_sz_summary, sz_summary, __ATOMIC_RELEASE);
}

void ps_host_features_log(void) {
    char sz_counts[1024];
    describe_counts(sz_counts, sizeof(sz_counts));
    printf("Host features: %s; probed at run time:", sz_counts);
    for (size_t i = 0; s_probed[i] != PS_HOST_FEATURE_COUNT; ++i)
        printf("%s %s", i ? "," : "", s_features[s_probed[i]].sz_id);
    printf(" [psport --list-features lists them]\n");
    publish();
}

/* Called from one thread: the GPU library's while it chooses how guest memory reaches the GPU, or
 * psport --list-features' before it lists the table. */
void ps_host_feature_probed(PsHostFeatureId id, int is_available, const char *sz_result) {
    if (id >= PS_HOST_FEATURE_COUNT || !is_probed(id)) {
        printf("Host features: probe answer for feature %u ignored: not a probed feature\n", id);
        return;
    }
    if (s_answers[id].has_answered) {
        printf("Host features: %s already probed (%s), %s ignored\n", s_features[id].sz_id, s_answers[id].sz_result,
               sz_result);
        return;
    }
    s_answers[id].has_answered = 1;
    s_answers[id].is_available = is_available;
    s_answers[id].sz_result = sz_result;
    printf("Host features: %s probed: %s (%s)\n", s_features[id].sz_id, is_available ? "available" : "unavailable",
           sz_result);
    publish();
}

const char *ps_host_features_summary(void) { return __atomic_load_n(&s_sz_summary, __ATOMIC_ACQUIRE); }

static void print_row(const HostRow *p_row) {
    printf("\"state\": \"%s\", \"reason\": ", s_state_names[p_row->state]);
    runtime_json_print_string(p_row->sz_reason);
    printf(", \"reference\": ");
    runtime_json_print_string(p_row->sz_ref);
}

void ps_host_features_list(void) {
    printf("{\"os\": \"%s\", \"features\": [", s_os_names[s_this_os]);
    for (PsHostFeatureId id = 0; s_features[id].sz_id; ++id) {
        HostRow row = row_for(id, s_this_os);
        if (s_answers[id].has_answered && !s_answers[id].is_available)
            row = (HostRow){PS_HOST_FEATURE_STATE_UNAVAILABLE, s_answers[id].sz_result, row.sz_ref};
        printf("%s\n    {\"id\": ", id ? "," : "");
        runtime_json_print_string(s_features[id].sz_id);
        printf(", \"area\": ");
        runtime_json_print_string(s_features[id].sz_area);
        printf(", \"description\": ");
        runtime_json_print_string(s_features[id].sz_desc);
        printf(", ");
        print_row(&row);
        printf(", \"probed_at_run_time\": %s, \"probe_result\": ", is_probed(id) ? "true" : "false");
        if (s_answers[id].has_answered) runtime_json_print_string(s_answers[id].sz_result);
        else fputs("null", stdout);
        printf(", \"hosts\": [");
        const char *sz_separator = "";
        for (size_t i = 0; s_hosts[i].sz_reason; ++i) {
            if (s_hosts[i].id != id) continue;
            const HostRow host = {s_hosts[i].state, s_hosts[i].sz_reason, s_hosts[i].sz_ref};
            printf("%s{\"os\": \"%s\", ", sz_separator, s_os_names[s_hosts[i].os]);
            print_row(&host);
            putchar('}');
            sz_separator = ", ";
        }
        printf("]}");
    }
    printf("%s]}\n", s_features[0].sz_id ? "\n  " : "");
}
