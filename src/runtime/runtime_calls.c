/* Calls the game makes that nothing answers for real, or that fail or report a polled state: each
 * kind logged once per import (and error code) with its first arguments, every call counted, and the
 * counts summed up when the process ends. Writes go straight to the descriptors so the report can
 * run from the fault and watchdog handlers. */
#include "runtime.h"
#include "runtime_quirks.h"
#include <inttypes.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

enum { FAILURE_SLOTS = 4096, HLE_STUB_SLOTS = 1024, SUMMARY_SLOTS = 8 };
static const char (*s_p_import_names)[128];
static uint64_t s_c_imports;
static uint64_t *s_p_unimplemented_calls, *s_p_stub_calls;
static struct { uint64_t key, c_failures; } s_failures[FAILURE_SLOTS];
static struct { const char *sz_function; const uint64_t *p_count; } s_hle_stubs[HLE_STUB_SLOTS];
static size_t s_c_hle_stubs;
static RuntimeSummary s_pfn_summaries[SUMMARY_SLOTS];
static size_t s_c_summaries;
static pthread_mutex_t s_hle_stub_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { uint64_t c_calls; uint32_t id, code; int kind; const char *sz_function; } Entry;
static Entry *s_p_entries;

void runtime_calls_configure(const char (*p_names)[128], uint64_t c_names) {
    s_p_import_names = p_names;
    s_c_imports = c_names;
    s_p_unimplemented_calls = calloc(c_names ? c_names : 1, sizeof(*s_p_unimplemented_calls));
    s_p_stub_calls = calloc(c_names ? c_names : 1, sizeof(*s_p_stub_calls));
    s_p_entries = calloc(2 * c_names + FAILURE_SLOTS + HLE_STUB_SLOTS, sizeof(*s_p_entries));
    if (!s_p_unimplemented_calls || !s_p_stub_calls || !s_p_entries) { fputs("Cannot allocate call counters\n", stderr); exit(1); }
    atexit(runtime_calls_report);
}

static void put(int h_fd, const char *sz_text) {
    for (size_t left = strlen(sz_text); left;) {
        ssize_t written = write(h_fd, sz_text, left);
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
    int used = snprintf(sz_line, sizeof(sz_line), "Calls: stub %s (%s) returns 0 without doing anything; arguments ",
                        s_p_import_names[id], runtime_import_name(s_p_import_names[id]));
    used += list_arguments(sz_line + used, sizeof(sz_line) - (size_t)used, arguments, 5);
    snprintf(sz_line + used, sizeof(sz_line) - (size_t)used, "\n");
    put(1, sz_line);
    return 0;
}

static int describe_failure(char *sz_out, size_t cb_out, uint32_t code) {
    return code >= 0x80000000u ? snprintf(sz_out, cb_out, "0x%08x", code) : snprintf(sz_out, cb_out, "-1, errno %u", code);
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
        uint64_t *p_key = &s_failures[(slot + i) % FAILURE_SLOTS].key, expected = 0;
        if (__atomic_load_n(p_key, __ATOMIC_ACQUIRE) != key &&
            !__atomic_compare_exchange_n(p_key, &expected, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && expected != key)
            continue;
        if (__atomic_fetch_add(&s_failures[(slot + i) % FAILURE_SLOTS].c_failures, 1, __ATOMIC_RELAXED)) return result;
        char sz_line[512];
        int used = snprintf(sz_line, sizeof(sz_line), "Calls: %s %s (%s) -> ",
                            runtime_import_status(s_p_import_names[id], code) ? "status" : "failed", s_p_import_names[id],
                            runtime_import_name(s_p_import_names[id]));
        used += describe_failure(sz_line + used, sizeof(sz_line) - (size_t)used, code);
        used += snprintf(sz_line + used, sizeof(sz_line) - (size_t)used, "; arguments ");
        used += list_arguments(sz_line + used, sizeof(sz_line) - (size_t)used, p_arguments, 6);
        snprintf(sz_line + used, sizeof(sz_line) - (size_t)used, "\n");
        put(1, sz_line);
        return result;
    }
    return result;
}

void runtime_calls_hle_stub(const char *sz_function, const uint64_t *p_count) {
    pthread_mutex_lock(&s_hle_stub_lock);
    if (s_c_hle_stubs < HLE_STUB_SLOTS) {
        s_hle_stubs[s_c_hle_stubs].sz_function = sz_function;
        s_hle_stubs[s_c_hle_stubs].p_count = p_count;
        __atomic_store_n(&s_c_hle_stubs, s_c_hle_stubs + 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&s_hle_stub_lock);
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
    static const char *const s_kinds[] = {"unimplemented", "stub", "failed", "status"};
    int used = snprintf(sz_out, cb_out, "Calls: summary %s %" PRIu64 " ", s_kinds[p_entry->kind], p_entry->c_calls);
    if (p_entry->sz_function) return used + snprintf(sz_out + used, cb_out - (size_t)used, "%s (HLE)\n", p_entry->sz_function);
    used += snprintf(sz_out + used, cb_out - (size_t)used, "%s (%s)", s_p_import_names[p_entry->id],
                     runtime_import_name(s_p_import_names[p_entry->id]));
    if (p_entry->kind >= 2) {
        used += snprintf(sz_out + used, cb_out - (size_t)used, " -> ");
        used += describe_failure(sz_out + used, cb_out - (size_t)used, p_entry->code);
    }
    return used + snprintf(sz_out + used, cb_out - (size_t)used, "\n");
}

void runtime_calls_report(void) {
    static int s_has_reported;
    if (!s_p_entries || __atomic_exchange_n(&s_has_reported, 1, __ATOMIC_ACQ_REL)) return;
    size_t c_path = 0, totals[4] = {0};
    for (uint64_t id = 0; id < s_c_imports; ++id) {
        const uint64_t unimplemented = __atomic_load_n(&s_p_unimplemented_calls[id], __ATOMIC_RELAXED);
        const uint64_t stub = __atomic_load_n(&s_p_stub_calls[id], __ATOMIC_RELAXED);
        if (unimplemented) s_p_entries[c_path++] = (Entry){unimplemented, (uint32_t)id, 0, 0, NULL};
        if (stub) s_p_entries[c_path++] = (Entry){stub, (uint32_t)id, 0, 1, NULL};
    }
    for (size_t i = 0, c_stubs = __atomic_load_n(&s_c_hle_stubs, __ATOMIC_ACQUIRE); i < c_stubs; ++i)
        s_p_entries[c_path++] =
            (Entry){__atomic_load_n(s_hle_stubs[i].p_count, __ATOMIC_RELAXED), 0, 0, 1, s_hle_stubs[i].sz_function};
    for (size_t i = 0; i < FAILURE_SLOTS; ++i) {
        const uint64_t key = __atomic_load_n(&s_failures[i].key, __ATOMIC_ACQUIRE);
        if (!key) continue;
        const uint32_t id = (uint32_t)(key >> 32) - 1, code = (uint32_t)key;
        s_p_entries[c_path++] = (Entry){__atomic_load_n(&s_failures[i].c_failures, __ATOMIC_RELAXED), id, code,
                                   runtime_import_status(s_p_import_names[id], code) ? 3 : 2, NULL};
    }
    for (size_t i = 1; i < c_path; ++i)
        for (size_t j = i; j && s_p_entries[j - 1].c_calls < s_p_entries[j].c_calls; --j) {
            const Entry swap = s_p_entries[j]; s_p_entries[j] = s_p_entries[j - 1]; s_p_entries[j - 1] = swap;
        }
    for (size_t i = 0; i < c_path; ++i) ++totals[s_p_entries[i].kind];
    const char *sz_path = getenv("PS_CALL_REPORT");
    const int file = sz_path && *sz_path ? open(sz_path, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    char sz_line[512];
    snprintf(sz_line, sizeof(sz_line),
             "Calls: summary: %zu unimplemented, %zu stub, %zu failed, %zu status (import and error code)\n",
             totals[0], totals[1], totals[2], totals[3]);
    put(1, sz_line);
    if (file >= 0) put(file, sz_line);
    for (size_t i = 0; i < c_path; ++i) {
        report_line(sz_line, sizeof(sz_line), &s_p_entries[i]);
        put(1, sz_line);
        if (file >= 0) put(file, sz_line);
    }
    if (file >= 0) close(file);
    put(1, runtime_quirks_summary());
    for (size_t i = 0, c_summaries = __atomic_load_n(&s_c_summaries, __ATOMIC_ACQUIRE); i < c_summaries; ++i) {
        s_pfn_summaries[i](sz_line, sizeof(sz_line));
        put(1, sz_line);
    }
    runtime_eventflag_created_report();
    runtime_memory_flexible_report();
    runtime_file_unmounted_report();
}
