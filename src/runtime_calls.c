/* Calls the game makes that nothing answers for real, or that fail: each kind logged once per
 * import (and error code) with its first arguments, every call counted, and the counts summed up
 * when the process ends. Writes go straight to the descriptors so the report can run from the
 * fault and watchdog handlers. */
#include "runtime.h"
#include <inttypes.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

enum { FAILURE_SLOTS = 4096, HLE_STUB_SLOTS = 1024 };
static const char (*import_names)[128];
static uint64_t imports;
static uint64_t *unimplemented_calls, *stub_calls;
static struct { uint64_t key, count; } failures[FAILURE_SLOTS];
static struct { const char *function; const uint64_t *count; } hle_stubs[HLE_STUB_SLOTS];
static size_t hle_stub_count;
static pthread_mutex_t hle_stub_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { uint64_t count; uint32_t id, code; int kind; const char *function; } Entry;
static Entry *entries;

void runtime_calls_configure(const char (*names)[128], uint64_t count) {
    import_names = names;
    imports = count;
    unimplemented_calls = calloc(count ? count : 1, sizeof(*unimplemented_calls));
    stub_calls = calloc(count ? count : 1, sizeof(*stub_calls));
    entries = calloc(2 * count + FAILURE_SLOTS + HLE_STUB_SLOTS, sizeof(*entries));
    if (!unimplemented_calls || !stub_calls || !entries) { fputs("Cannot allocate call counters\n", stderr); exit(1); }
    atexit(runtime_calls_report);
}

static void put(int fd, const char *text) {
    for (size_t left = strlen(text); left;) {
        ssize_t written = write(fd, text, left);
        if (written <= 0) return;
        text += written; left -= (size_t)written;
    }
}

static int list_arguments(char *out, size_t size, const uintptr_t *arguments, unsigned count) {
    int used = 0;
    for (unsigned i = 0; i < count && (size_t)used < size; ++i)
        used += snprintf(out + used, size - (size_t)used, "%s0x%" PRIxPTR, i ? ", " : "", arguments[i]);
    return used;
}

void runtime_calls_unimplemented(uint32_t id) {
    if (id < imports) __atomic_fetch_add(&unimplemented_calls[id], 1, __ATOMIC_RELAXED);
}

ABI int64_t runtime_calls_stub(uint32_t id, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    if (id >= imports || __atomic_fetch_add(&stub_calls[id], 1, __ATOMIC_RELAXED)) return 0;
    const uintptr_t arguments[5] = {a0, a1, a2, a3, a4};
    char line[512];
    int used = snprintf(line, sizeof(line), "Calls: stub %s (%s) returns 0 without doing anything; arguments ",
                        import_names[id], runtime_import_name(import_names[id]));
    used += list_arguments(line + used, sizeof(line) - (size_t)used, arguments, 5);
    snprintf(line + used, sizeof(line) - (size_t)used, "\n");
    put(1, line);
    return 0;
}

static int describe_failure(char *out, size_t size, uint32_t code) {
    return code >= 0x80000000u ? snprintf(out, size, "0x%08x", code) : snprintf(out, size, "-1, errno %u", code);
}

ABI int64_t runtime_calls_failed(uint32_t id, const uintptr_t *arguments, int64_t result, uint32_t kind) {
    const uint32_t low = (uint32_t)result;
    uint32_t code;
    if (kind == 2 || kind == 3) code = (uint32_t)*runtime_errno() & 0x7fffffffu;
    else if (low >> 28 == 8 && (kind == 1 || result < 0)) code = low;
    else return result;
    if (id >= imports) return result;
    const uint64_t key = (uint64_t)(id + 1) << 32 | code;
    for (uint64_t i = 0, slot = key * 0x9E3779B97F4A7C15ull >> 52; i < FAILURE_SLOTS; ++i) {
        uint64_t *at = &failures[(slot + i) % FAILURE_SLOTS].key, expected = 0;
        if (__atomic_load_n(at, __ATOMIC_ACQUIRE) != key &&
            !__atomic_compare_exchange_n(at, &expected, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && expected != key)
            continue;
        if (__atomic_fetch_add(&failures[(slot + i) % FAILURE_SLOTS].count, 1, __ATOMIC_RELAXED)) return result;
        char line[512];
        int used = snprintf(line, sizeof(line), "Calls: failed %s (%s) -> ", import_names[id],
                            runtime_import_name(import_names[id]));
        used += describe_failure(line + used, sizeof(line) - (size_t)used, code);
        used += snprintf(line + used, sizeof(line) - (size_t)used, "; arguments ");
        used += list_arguments(line + used, sizeof(line) - (size_t)used, arguments, 6);
        snprintf(line + used, sizeof(line) - (size_t)used, "\n");
        put(1, line);
        return result;
    }
    return result;
}

void runtime_calls_hle_stub(const char *function, const uint64_t *count) {
    pthread_mutex_lock(&hle_stub_lock);
    if (hle_stub_count < HLE_STUB_SLOTS) {
        hle_stubs[hle_stub_count].function = function;
        hle_stubs[hle_stub_count].count = count;
        __atomic_store_n(&hle_stub_count, hle_stub_count + 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&hle_stub_lock);
}

static int report_line(char *out, size_t size, const Entry *e) {
    static const char *const kinds[] = {"unimplemented", "stub", "failed"};
    int used = snprintf(out, size, "Calls: summary %s %" PRIu64 " ", kinds[e->kind], e->count);
    if (e->function) return used + snprintf(out + used, size - (size_t)used, "%s (HLE)\n", e->function);
    used += snprintf(out + used, size - (size_t)used, "%s (%s)", import_names[e->id],
                     runtime_import_name(import_names[e->id]));
    if (e->kind == 2) {
        used += snprintf(out + used, size - (size_t)used, " -> ");
        used += describe_failure(out + used, size - (size_t)used, e->code);
    }
    return used + snprintf(out + used, size - (size_t)used, "\n");
}

void runtime_calls_report(void) {
    static int reported;
    if (!entries || __atomic_exchange_n(&reported, 1, __ATOMIC_ACQ_REL)) return;
    size_t count = 0, totals[3] = {0};
    for (uint64_t id = 0; id < imports; ++id) {
        const uint64_t unimplemented = __atomic_load_n(&unimplemented_calls[id], __ATOMIC_RELAXED);
        const uint64_t stub = __atomic_load_n(&stub_calls[id], __ATOMIC_RELAXED);
        if (unimplemented) entries[count++] = (Entry){unimplemented, (uint32_t)id, 0, 0, NULL};
        if (stub) entries[count++] = (Entry){stub, (uint32_t)id, 0, 1, NULL};
    }
    for (size_t i = 0, n = __atomic_load_n(&hle_stub_count, __ATOMIC_ACQUIRE); i < n; ++i)
        entries[count++] =
            (Entry){__atomic_load_n(hle_stubs[i].count, __ATOMIC_RELAXED), 0, 0, 1, hle_stubs[i].function};
    for (size_t i = 0; i < FAILURE_SLOTS; ++i) {
        const uint64_t key = __atomic_load_n(&failures[i].key, __ATOMIC_ACQUIRE);
        if (key)
            entries[count++] = (Entry){__atomic_load_n(&failures[i].count, __ATOMIC_RELAXED), (uint32_t)(key >> 32) - 1,
                                       (uint32_t)key, 2, NULL};
    }
    for (size_t i = 1; i < count; ++i)
        for (size_t j = i; j && entries[j - 1].count < entries[j].count; --j) {
            const Entry swap = entries[j]; entries[j] = entries[j - 1]; entries[j - 1] = swap;
        }
    for (size_t i = 0; i < count; ++i) ++totals[entries[i].kind];
    const char *path = getenv("BB_CALL_REPORT");
    const int file = path && *path ? open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    char line[512];
    snprintf(line, sizeof(line), "Calls: summary: %zu unimplemented, %zu stub, %zu failed (import and error code)\n",
             totals[0], totals[1], totals[2]);
    put(1, line);
    if (file >= 0) put(file, line);
    for (size_t i = 0; i < count; ++i) {
        report_line(line, sizeof(line), &entries[i]);
        put(1, line);
        if (file >= 0) put(file, line);
    }
    if (file >= 0) close(file);
}
