// SPDX-License-Identifier: GPL-2.0-or-later
/* Calls the game makes that nothing answers for real, or that fail or report a polled state: each
 * kind logged once per import (and error code) with its first arguments, every call counted, and the
 * counts summed up when the process ends. Writes go straight to the descriptors so the report can
 * run from the fault and watchdog handlers. */
#include "runtime.h"
#include "runtime_quirks.h"
#include "ps_host.h"
#include "gpu/psport_gpu.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

enum { FAILURE_SLOTS = 4096, HLE_STUB_SLOTS = 1024, SUMMARY_SLOTS = 8, DETAIL_SLOTS = 128, DETAILS_PER_RESULT = 4 };
static const char (*s_p_import_names)[128];
static uint64_t s_c_imports;
static uint64_t *s_p_unimplemented_calls, *s_p_stub_calls;
/* i_detail: 1 + the slot in s_details of a result whose import names a path or an address. */
typedef struct { uint64_t key, c_failures; uint32_t i_detail; } Failure;
static Failure s_failures[FAILURE_SLOTS];
/* The distinct paths or addresses behind one import and result, so that a failed open or memory
 * query in the exit report says what it was asked for; past the first few they are only counted. */
typedef enum { ARGUMENT_PATH, ARGUMENT_ADDRESS, ARGUMENT_OFFSET } ArgumentKind;
static const char *const s_p_argument_kind_names[] = {"path", "address", "offset"};
typedef struct {
    uint32_t i_argument, c_values;
    ArgumentKind kind;
    uint64_t c_unlisted;
    char sz_values[DETAILS_PER_RESULT][128];
} Detail;
static Detail s_details[DETAIL_SLOTS];
static uint32_t s_c_details;
static pthread_mutex_t s_detail_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { const char *sz_function, *sz_detail; const uint64_t *p_count; int kind; } s_hle_stubs[HLE_STUB_SLOTS];
static size_t s_c_hle_stubs;
static RuntimeSummary s_pfn_summaries[SUMMARY_SLOTS];
static size_t s_c_summaries;
static pthread_mutex_t s_hle_stub_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
    uint64_t c_calls;
    uint32_t id, code;
    int kind;
    const char *sz_function, *sz_detail;
    uint32_t i_detail;
} Entry;
enum { KIND_UNIMPLEMENTED, KIND_STUB, KIND_FAILED, KIND_STATUS, KIND_FALLBACK, KIND_OFFLINE, KIND_COUNT };
static Entry *s_p_entries;

void runtime_calls_configure(const char (*p_names)[128], uint64_t c_names) {
    s_p_import_names = p_names;
    s_c_imports = c_names;
    s_p_unimplemented_calls = calloc(c_names ? c_names : 1, sizeof(*s_p_unimplemented_calls));
    s_p_stub_calls = calloc(c_names ? c_names : 1, sizeof(*s_p_stub_calls));
    s_p_entries = calloc(2 * c_names + FAILURE_SLOTS + HLE_STUB_SLOTS, sizeof(*s_p_entries));
    if (!s_p_unimplemented_calls || !s_p_stub_calls || !s_p_entries) {
        free(s_p_entries);
        s_p_entries = NULL; /* so that runtime_exit's call report does not read the missing counters */
        fputs("Cannot allocate call counters\n", stderr);
        runtime_exit(1);
    }
    atexit(runtime_calls_report);
}

static const char *const s_p_kind_names[] = {"unimplemented", "stub", "failed", "status", "fallback", "offline"};

static int result_kind(uint32_t id, uint32_t code) {
    if (runtime_import_status(s_p_import_names[id], code)) return KIND_STATUS;
    return runtime_import_offline(s_p_import_names[id], code) ? KIND_OFFLINE : KIND_FAILED;
}

static int stub_kind(uint32_t id) { return runtime_import_offline(s_p_import_names[id], 0) ? KIND_OFFLINE : KIND_STUB; }

static void put(int h_fd, const char *sz_text) {
    for (size_t left = strlen(sz_text); left;) {
        const int64_t written = ps_host_fs_write(h_fd, sz_text, left);
        if (written <= 0) return;
        sz_text += written; left -= (size_t)written;
    }
}

static int list_arguments(char *sz_out, size_t cb_out, const uintptr_t *p_arguments, unsigned c_arguments) {
    int used = 0;
    for (unsigned i = 0; i < c_arguments && (size_t)used < cb_out; ++i)
        used += snprintf(sz_out + used, cb_out - (size_t)used, "%s0x%" PRIxPTR, i ? ", " : "", p_arguments[i]);
    return used;
}

void runtime_calls_unimplemented(uint32_t id) {
    if (id < s_c_imports) __atomic_fetch_add(&s_p_unimplemented_calls[id], 1, __ATOMIC_RELAXED);
}

ABI int64_t runtime_calls_stub(uint32_t id, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    if (id >= s_c_imports || __atomic_fetch_add(&s_p_stub_calls[id], 1, __ATOMIC_RELAXED)) return 0;
    const uintptr_t arguments[5] = {a0, a1, a2, a3, a4};
    char sz_line[512];
    int used = snprintf(sz_line, sizeof(sz_line), "Calls: %s %s (%s) returns 0 without doing anything; arguments ",
                        s_p_kind_names[stub_kind(id)], s_p_import_names[id], runtime_import_name(s_p_import_names[id]));
    used += list_arguments(sz_line + used, sizeof(sz_line) - (size_t)used, arguments, 5);
    snprintf(sz_line + used, sizeof(sz_line) - (size_t)used, "\n");
    put(1, sz_line);
    return 0;
}

static int describe_failure(char *sz_out, size_t cb_out, uint32_t code) {
    return code >= 0x80000000u ? snprintf(sz_out, cb_out, "0x%08x", code) : snprintf(sz_out, cb_out, "-1, errno %u", code);
}

static const struct { const char *sz_function; uint32_t i_argument; ArgumentKind kind; } s_described_arguments[] = {
    {"sceKernelOpen", 0, ARGUMENT_PATH}, {"open", 0, ARGUMENT_PATH}, {"_open", 0, ARGUMENT_PATH},
    {"sceKernelStat", 0, ARGUMENT_PATH}, {"stat", 0, ARGUMENT_PATH}, {"lstat", 0, ARGUMENT_PATH},
    {"fstatat", 1, ARGUMENT_PATH}, {"access", 0, ARGUMENT_PATH}, {"sceKernelCheckReachability", 0, ARGUMENT_PATH},
    {"sceKernelMkdir", 0, ARGUMENT_PATH}, {"mkdir", 0, ARGUMENT_PATH}, {"sceKernelRmdir", 0, ARGUMENT_PATH},
    {"rmdir", 0, ARGUMENT_PATH}, {"sceKernelUnlink", 0, ARGUMENT_PATH}, {"unlink", 0, ARGUMENT_PATH},
    {"sceKernelRename", 0, ARGUMENT_PATH}, {"rename", 0, ARGUMENT_PATH}, {"chdir", 0, ARGUMENT_PATH},
    {"sceKernelVirtualQuery", 0, ARGUMENT_ADDRESS}, {"sceKernelQueryMemoryProtection", 0, ARGUMENT_ADDRESS},
    {"sceKernelMprotect", 0, ARGUMENT_ADDRESS}, {"sceKernelDirectMemoryQuery", 0, ARGUMENT_OFFSET},
    {"sceKernelGetDirectMemoryType", 0, ARGUMENT_OFFSET},
};

/* 1 + a new slot in s_details for the import's path or address argument, or 0 when it has none. */
static uint32_t claim_detail(uint32_t id) {
    const char *sz_function = runtime_import_name(s_p_import_names[id]);
    for (size_t i = 0; i < sizeof(s_described_arguments) / sizeof(*s_described_arguments); ++i) {
        if (strcmp(sz_function, s_described_arguments[i].sz_function)) continue;
        const uint32_t i_detail = __atomic_fetch_add(&s_c_details, 1, __ATOMIC_RELAXED);
        if (i_detail >= DETAIL_SLOTS) return 0;
        s_details[i_detail].i_argument = s_described_arguments[i].i_argument;
        s_details[i_detail].kind = s_described_arguments[i].kind;
        return i_detail + 1;
    }
    return 0;
}

static void format_argument(char *sz_out, size_t cb_out, ArgumentKind kind, uintptr_t argument, uint32_t code) {
    /* EFAULT: the callee could not read the path either, so it is not read here. */
    if (kind != ARGUMENT_PATH) snprintf(sz_out, cb_out, "0x%" PRIxPTR, argument);
    else if (!argument || (code & 0xffffu) == 14) snprintf(sz_out, cb_out, "(unreadable 0x%" PRIxPTR ")", argument);
    else {
        snprintf(sz_out, cb_out, "%.*s", (int)(cb_out - 1), (const char *)argument);
        for (char *p_char = sz_out; *p_char; ++p_char)
            if ((unsigned char)*p_char < 0x20) *p_char = '?'; /* so that a path never splits a report line */
    }
}

static void record_detail(uint32_t i_detail, const uintptr_t *p_arguments, uint32_t code) {
    Detail *p_detail = &s_details[i_detail - 1];
    char sz_value[sizeof(p_detail->sz_values[0])];
    format_argument(sz_value, sizeof(sz_value), p_detail->kind, p_arguments[p_detail->i_argument], code);
    pthread_mutex_lock(&s_detail_lock);
    uint32_t i = 0;
    while (i < p_detail->c_values && strcmp(p_detail->sz_values[i], sz_value)) ++i;
    if (i == p_detail->c_values) {
        if (i < DETAILS_PER_RESULT) {
            memcpy(p_detail->sz_values[i], sz_value, sizeof(sz_value));
            __atomic_store_n(&p_detail->c_values, i + 1, __ATOMIC_RELEASE);
        } else {
            __atomic_add_fetch(&p_detail->c_unlisted, 1, __ATOMIC_RELAXED);
        }
    }
    pthread_mutex_unlock(&s_detail_lock);
}

ABI int64_t runtime_calls_failed(uint32_t id, const uintptr_t *p_arguments, int64_t result, uint32_t kind) {
    const uint32_t low = (uint32_t)result;
    uint32_t code;
    if (kind == 2 || kind == 3) code = (uint32_t)*runtime_errno() & 0x7fffffffu;
    else if (low >> 28 == 8 && (kind == 1 || result < 0)) code = low;
    else return result;
    if (id >= s_c_imports) return result;
    const uint64_t key = (uint64_t)(id + 1) << 32 | code;
    for (uint64_t i = 0, slot = key * 0x9E3779B97F4A7C15ull >> 52; i < FAILURE_SLOTS; ++i) {
        Failure *p_failure = &s_failures[(slot + i) % FAILURE_SLOTS];
        uint64_t expected = 0;
        if (__atomic_load_n(&p_failure->key, __ATOMIC_ACQUIRE) != key &&
            !__atomic_compare_exchange_n(&p_failure->key, &expected, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) &&
            expected != key)
            continue;
        if (__atomic_fetch_add(&p_failure->c_failures, 1, __ATOMIC_RELAXED)) {
            const uint32_t i_detail = __atomic_load_n(&p_failure->i_detail, __ATOMIC_ACQUIRE);
            if (i_detail) record_detail(i_detail, p_arguments, code);
            return result;
        }
        const uint32_t i_detail = claim_detail(id);
        if (i_detail) {
            record_detail(i_detail, p_arguments, code);
            __atomic_store_n(&p_failure->i_detail, i_detail, __ATOMIC_RELEASE);
        }
        char sz_line[512];
        int used = snprintf(sz_line, sizeof(sz_line), "Calls: %s %s (%s) -> ",
                            s_p_kind_names[result_kind(id, code)], s_p_import_names[id],
                            runtime_import_name(s_p_import_names[id]));
        used += describe_failure(sz_line + used, sizeof(sz_line) - (size_t)used, code);
        used += snprintf(sz_line + used, sizeof(sz_line) - (size_t)used, "; arguments ");
        used += list_arguments(sz_line + used, sizeof(sz_line) - (size_t)used, p_arguments, 6);
        if (i_detail && (size_t)used < sizeof(sz_line)) {
            const Detail *p_detail = &s_details[i_detail - 1];
            used += snprintf(sz_line + used, sizeof(sz_line) - (size_t)used, "; %s %s",
                             s_p_argument_kind_names[p_detail->kind], p_detail->sz_values[0]);
        }
        if ((size_t)used > sizeof(sz_line) - 2) used = (int)sizeof(sz_line) - 2;
        snprintf(sz_line + used, sizeof(sz_line) - (size_t)used, "\n");
        put(1, sz_line);
        return result;
    }
    return result;
}

static void add_hle_entry(const char *sz_function, const char *sz_detail, const uint64_t *p_count, int kind) {
    pthread_mutex_lock(&s_hle_stub_lock);
    if (s_c_hle_stubs < HLE_STUB_SLOTS) {
        s_hle_stubs[s_c_hle_stubs].sz_function = sz_function;
        s_hle_stubs[s_c_hle_stubs].sz_detail = sz_detail;
        s_hle_stubs[s_c_hle_stubs].p_count = p_count;
        s_hle_stubs[s_c_hle_stubs].kind = kind;
        __atomic_store_n(&s_c_hle_stubs, s_c_hle_stubs + 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&s_hle_stub_lock);
}

void runtime_calls_hle_stub(const char *sz_function, const uint64_t *p_count) {
    add_hle_entry(sz_function, NULL, p_count, 1);
}

void runtime_calls_fallback(const char *sz_function, const char *sz_detail, const uint64_t *p_count) {
    add_hle_entry(sz_function, sz_detail, p_count, 4);
}

void runtime_calls_add_summary(RuntimeSummary pfn_summary) {
    pthread_mutex_lock(&s_hle_stub_lock);
    if (s_c_summaries < SUMMARY_SLOTS) {
        s_pfn_summaries[s_c_summaries] = pfn_summary;
        __atomic_store_n(&s_c_summaries, s_c_summaries + 1, __ATOMIC_RELEASE);
    } else {
        printf("Calls: no slot left for a summary line (%d in use); it will not be printed\n", SUMMARY_SLOTS);
    }
    pthread_mutex_unlock(&s_hle_stub_lock);
}

static int report_line(char *sz_out, size_t cb_out, const Entry *p_entry) {
    int used = snprintf(sz_out, cb_out, "Calls: summary %s %" PRIu64 " ", s_p_kind_names[p_entry->kind], p_entry->c_calls);
    if (p_entry->sz_detail)
        return used + snprintf(sz_out + used, cb_out - (size_t)used, "%s (GPU): %s\n", p_entry->sz_function, p_entry->sz_detail);
    if (p_entry->sz_function) return used + snprintf(sz_out + used, cb_out - (size_t)used, "%s (HLE)\n", p_entry->sz_function);
    used += snprintf(sz_out + used, cb_out - (size_t)used, "%s (%s)", s_p_import_names[p_entry->id],
                     runtime_import_name(s_p_import_names[p_entry->id]));
    if (p_entry->kind == KIND_FAILED || p_entry->kind == KIND_STATUS || p_entry->code) {
        used += snprintf(sz_out + used, cb_out - (size_t)used, " -> ");
        used += describe_failure(sz_out + used, cb_out - (size_t)used, p_entry->code);
    }
    return used + snprintf(sz_out + used, cb_out - (size_t)used, "\n");
}

/* Line i_value of the entry's paths or addresses, after its summary line; i_value past the listed
 * ones counts the calls that asked for another. A count field that is not a number keeps parsers
 * of the summary lines from taking these for calls. False when there is no such line. */
static int detail_line(char *sz_out, size_t cb_out, const Entry *p_entry, uint32_t i_value) {
    if (!p_entry->i_detail) return 0;
    const Detail *p_detail = &s_details[p_entry->i_detail - 1];
    const uint32_t c_values = __atomic_load_n(&p_detail->c_values, __ATOMIC_ACQUIRE);
    const uint64_t c_unlisted = __atomic_load_n(&p_detail->c_unlisted, __ATOMIC_RELAXED);
    if (i_value > c_values || (i_value == c_values && !c_unlisted)) return 0;
    int used = snprintf(sz_out, cb_out, "Calls: summary %s %s (%s) -> ", s_p_argument_kind_names[p_detail->kind],
                        s_p_import_names[p_entry->id], runtime_import_name(s_p_import_names[p_entry->id]));
    used += describe_failure(sz_out + used, cb_out - (size_t)used, p_entry->code);
    if (i_value < c_values)
        snprintf(sz_out + used, cb_out - (size_t)used, ": %s\n", p_detail->sz_values[i_value]);
    else
        snprintf(sz_out + used, cb_out - (size_t)used, "; %" PRIu64 " more calls with another %s\n", c_unlisted,
                 s_p_argument_kind_names[p_detail->kind]);
    return 1;
}

void runtime_calls_report(void) {
    static int s_has_reported;
    if (!s_p_entries || __atomic_exchange_n(&s_has_reported, 1, __ATOMIC_ACQ_REL)) return;
    size_t c_path = 0, totals[KIND_COUNT] = {0};
    for (uint64_t id = 0; id < s_c_imports; ++id) {
        const uint64_t unimplemented = __atomic_load_n(&s_p_unimplemented_calls[id], __ATOMIC_RELAXED);
        const uint64_t stub = __atomic_load_n(&s_p_stub_calls[id], __ATOMIC_RELAXED);
        if (unimplemented)
            s_p_entries[c_path++] = (Entry){unimplemented, (uint32_t)id, 0, KIND_UNIMPLEMENTED, NULL, NULL, 0};
        if (stub) s_p_entries[c_path++] = (Entry){stub, (uint32_t)id, 0, stub_kind((uint32_t)id), NULL, NULL, 0};
    }
    for (size_t i = 0, c_stubs = __atomic_load_n(&s_c_hle_stubs, __ATOMIC_ACQUIRE); i < c_stubs; ++i)
        s_p_entries[c_path++] = (Entry){__atomic_load_n(s_hle_stubs[i].p_count, __ATOMIC_RELAXED), 0, 0,
                                        s_hle_stubs[i].kind, s_hle_stubs[i].sz_function, s_hle_stubs[i].sz_detail, 0};
    for (size_t i = 0; i < FAILURE_SLOTS; ++i) {
        const uint64_t key = __atomic_load_n(&s_failures[i].key, __ATOMIC_ACQUIRE);
        if (!key) continue;
        const uint32_t id = (uint32_t)(key >> 32) - 1, code = (uint32_t)key;
        s_p_entries[c_path++] = (Entry){__atomic_load_n(&s_failures[i].c_failures, __ATOMIC_RELAXED), id, code,
                                   result_kind(id, code), NULL, NULL,
                                   __atomic_load_n(&s_failures[i].i_detail, __ATOMIC_ACQUIRE)};
    }
    for (size_t i = 1; i < c_path; ++i)
        for (size_t j = i; j && s_p_entries[j - 1].c_calls < s_p_entries[j].c_calls; --j) {
            const Entry swap = s_p_entries[j]; s_p_entries[j] = s_p_entries[j - 1]; s_p_entries[j - 1] = swap;
        }
    for (size_t i = 0; i < c_path; ++i) ++totals[s_p_entries[i].kind];
    const char *sz_path = runtime_env("PS_CALL_REPORT", NULL);
    const int file = sz_path && *sz_path ? ps_host_fs_open(sz_path, PS_HOST_OPEN_WRITE | PS_HOST_OPEN_CREATE | PS_HOST_OPEN_TRUNCATE, 0644) : -1;
    const int h_debug = runtime_debug_open("calls.txt");
    char sz_line[512];
    snprintf(sz_line, sizeof(sz_line),
             "Calls: summary: %zu unimplemented, %zu stub, %zu failed, %zu status (import and error code), "
             "%zu fallback (GPU), %zu offline (import and result)\n",
             totals[KIND_UNIMPLEMENTED], totals[KIND_STUB], totals[KIND_FAILED], totals[KIND_STATUS],
             totals[KIND_FALLBACK], totals[KIND_OFFLINE]);
    put(1, sz_line);
    if (file >= 0) put(file, sz_line);
    if (h_debug >= 0) put(h_debug, sz_line);
    runtime_debug_note_calls(sz_line);
    for (size_t i = 0; i < c_path; ++i) {
        report_line(sz_line, sizeof(sz_line), &s_p_entries[i]);
        for (uint32_t i_value = 0;; ++i_value) {
            put(1, sz_line);
            if (file >= 0) put(file, sz_line);
            if (h_debug >= 0) put(h_debug, sz_line);
            if (!detail_line(sz_line, sizeof(sz_line), &s_p_entries[i], i_value)) break;
        }
    }
    if (file >= 0) ps_host_fs_close(file);
    if (h_debug >= 0) ps_host_fs_close(h_debug);
    psport_gpu_cp_census_report();
    psport_gpu_native_probe_report();
    psport_gpu_small_textures_report();
    put(1, runtime_quirks_summary());
    put(1, ps_host_features_summary());
    for (size_t i = 0, c_summaries = __atomic_load_n(&s_c_summaries, __ATOMIC_ACQUIRE); i < c_summaries; ++i) {
        s_pfn_summaries[i](sz_line, sizeof(sz_line));
        put(1, sz_line);
    }
    runtime_eventflag_created_report();
    runtime_memory_flexible_report();
    runtime_file_unmounted_report();
}
