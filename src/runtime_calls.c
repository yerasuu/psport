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
#include <unistd.h>

static const char (*import_names)[128];
static uint64_t imports;
static uint64_t *unimplemented_calls, *stub_calls;
typedef struct { uint64_t count; uint32_t id; int kind; } Entry;
static Entry *entries;

void runtime_calls_configure(const char (*names)[128], uint64_t count) {
    import_names = names;
    imports = count;
    unimplemented_calls = calloc(count ? count : 1, sizeof(*unimplemented_calls));
    stub_calls = calloc(count ? count : 1, sizeof(*stub_calls));
    entries = calloc(2 * count + 1, sizeof(*entries));
    if (!unimplemented_calls || !stub_calls || !entries) { fputs("Cannot allocate call counters\n", stderr); exit(1); }
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

static int report_line(char *out, size_t size, const Entry *e) {
    static const char *const kinds[] = {"unimplemented", "stub", "failed"};
    int used = snprintf(out, size, "Calls: summary %s %" PRIu64 " ", kinds[e->kind], e->count);
    return used + snprintf(out + used, size - (size_t)used, "%s (%s)\n", import_names[e->id],
                           runtime_import_name(import_names[e->id]));
}

void runtime_calls_report(void) {
    static int reported;
    if (!entries || __atomic_exchange_n(&reported, 1, __ATOMIC_ACQ_REL)) return;
    size_t count = 0, totals[3] = {0};
    for (uint64_t id = 0; id < imports; ++id) {
        const uint64_t unimplemented = __atomic_load_n(&unimplemented_calls[id], __ATOMIC_RELAXED);
        const uint64_t stub = __atomic_load_n(&stub_calls[id], __ATOMIC_RELAXED);
        if (unimplemented) entries[count++] = (Entry){unimplemented, (uint32_t)id, 0};
        if (stub) entries[count++] = (Entry){stub, (uint32_t)id, 1};
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
