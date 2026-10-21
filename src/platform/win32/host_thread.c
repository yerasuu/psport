/* Windows: thread ids, names, stacks and scheduling, address waits and the guest's thread pointer
 * (ps_host.h). */
#include "ps_host.h"
#include "host_win32.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <tlhelp32.h>

int32_t ps_host_thread_id(void) { return (int32_t)GetCurrentThreadId(); }

int ps_host_backtrace(void **pp_frames, int c_frames) {
    return c_frames > 0 ? RtlCaptureStackBackTrace(1, (DWORD)c_frames, pp_frames, NULL) : 0;
}

/* WaitOnAddress pairs waits and wakes within one process only, which is all psport's guest has:
 * a shared wait (is_private 0) is served the same way. No signal ends a wait early, so EINTR never
 * comes; a wake may still return without the word changing, as futex allows, and an interrupt to
 * the thread wakes it that way (ps_host_interrupt_thread). */
int ps_host_wait_on_address(const void *p_word, uint32_t expected, int is_private, PsHostClock clock,
                            const struct timespec *p_deadline) {
    (void)is_private;
    if (clock != PS_HOST_CLOCK_MONOTONIC && clock != PS_HOST_CLOCK_REALTIME) return EINVAL;
    volatile uint32_t *p_watched = (volatile uint32_t *)(uintptr_t)p_word;
    if (*p_watched != expected) return EAGAIN;
    DWORD timeout_ms = INFINITE;
    if (p_deadline) {
        struct timespec now;
        ps_host_time_read(clock, &now);
        const int64_t remaining_ns = ((int64_t)p_deadline->tv_sec - (int64_t)now.tv_sec) * 1000000000 +
                                     ((int64_t)p_deadline->tv_nsec - (int64_t)now.tv_nsec);
        if (remaining_ns <= 0) return ETIMEDOUT;
        const uint64_t ms = ((uint64_t)remaining_ns + 999999u) / 1000000u;
        timeout_ms = ms >= INFINITE ? INFINITE - 1 : (DWORD)ms;
    }
    ps_win32_wait_begin(p_word);
    const BOOL is_woken = WaitOnAddress(p_watched, &expected, sizeof(expected), timeout_ms);
    const DWORD error = GetLastError();
    ps_win32_wait_end();
    if (is_woken) return 0;
    return error == ERROR_TIMEOUT ? ETIMEDOUT : ps_win32_errno(error);
}

/* WakeByAddress does not say how many it woke: the count asked for stands in for it. */
int ps_host_wake_address(const void *p_word, int c_waiters, int is_private) {
    (void)is_private;
    void *p_watched = (void *)(uintptr_t)p_word;
    if (c_waiters >= INT_MAX)
        WakeByAddressAll(p_watched);
    else
        for (int i = 0; i < c_waiters; ++i) WakeByAddressSingle(p_watched);
    return c_waiters > 0 ? c_waiters : 0;
}

void ps_host_thread_set_name(const char *sz_name) {
    wchar_t sz_wide[256];
    if (!ps_win32_to_wide(sz_name, sz_wide, sizeof(sz_wide) / sizeof(sz_wide[0])))
        SetThreadDescription(GetCurrentThread(), sz_wide);
}

static int thread_description(HANDLE h_thread, char *sz_out, size_t cb_out) {
    wchar_t *sz_wide = NULL;
    const HRESULT result = GetThreadDescription(h_thread, &sz_wide);
    if (FAILED(result)) return ps_win32_errno((DWORD)HRESULT_CODE(result));
    const int error = ps_win32_to_utf8(sz_wide, sz_out, cb_out);
    LocalFree(sz_wide);
    return error == ENAMETOOLONG ? ERANGE : error;
}

int ps_host_thread_name(char *sz_out, size_t cb_out) { return thread_description(GetCurrentThread(), sz_out, cb_out); }

/* Calls pfn_thread with each thread of this process from a Toolhelp snapshot, until it returns
 * nonzero: 0, or the errno value of the snapshot's failure. */
static int each_thread(int (*pfn_thread)(DWORD tid, void *p_context), void *p_context) {
    HANDLE h_snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (h_snapshot == INVALID_HANDLE_VALUE) return ps_win32_errno(GetLastError());
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 entry = {.dwSize = sizeof(entry)};
    for (BOOL has_entry = Thread32First(h_snapshot, &entry); has_entry; has_entry = Thread32Next(h_snapshot, &entry))
        if (entry.th32OwnerProcessID == pid && pfn_thread(entry.th32ThreadID, p_context)) break;
    CloseHandle(h_snapshot);
    return 0;
}

typedef struct {
    const char *sz_prefix;
    int32_t tid;
} ThreadSearch;

static int match_thread(DWORD tid, void *p_search) {
    ThreadSearch *p = p_search;
    HANDLE h_thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!h_thread) return 0;
    char sz_name[256];
    if (!thread_description(h_thread, sz_name, sizeof(sz_name)) && !strncmp(sz_name, p->sz_prefix, strlen(p->sz_prefix)))
        p->tid = (int32_t)tid;
    CloseHandle(h_thread);
    return p->tid != 0;
}

int32_t ps_host_thread_find(const char *sz_prefix) {
    ThreadSearch search = {sz_prefix, 0};
    each_thread(match_thread, &search);
    return search.tid;
}

typedef struct {
    void (*pfn_thread)(int32_t tid, void *p_context);
    void *p_context;
} ThreadWalk;

static int visit_thread(DWORD tid, void *p_walk) {
    const ThreadWalk *p = p_walk;
    p->pfn_thread((int32_t)tid, p->p_context);
    return 0;
}

int ps_host_thread_each(void (*pfn_thread)(int32_t tid, void *p_context), void *p_context) {
    ThreadWalk walk = {pfn_thread, p_context};
    return each_thread(visit_thread, &walk);
}

int ps_host_thread_stack(uintptr_t *p_out_va_low, size_t *p_out_cb_stack) {
    ULONG_PTR va_low = 0, va_high = 0;
    GetCurrentThreadStackLimits(&va_low, &va_high);
    *p_out_va_low = (uintptr_t)va_low;
    *p_out_cb_stack = (size_t)(va_high - va_low);
    return 0;
}

unsigned ps_host_thread_cpu_count(void) {
    DWORD_PTR process_mask = 0, system_mask = 0;
    /* Both masks read 0 when the process spans several processor groups (more than 64 threads). */
    if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) && process_mask)
        return (unsigned)__builtin_popcountll(process_mask);
    return (unsigned)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
}

/* Windows has no idle scheduling class for one thread: the idle priority is the lowest. */
int ps_host_thread_make_background(void) {
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE) ? 0 : ps_win32_errno(GetLastError());
}

/* GS's base is the TEB, whose TlsSlots array holds the first 64 TlsAlloc slots inline at 0x1480:
 * the guest's TCB pointer lives in one of them, which a single gs-relative load reaches. Slots past
 * 64 sit behind another pointer, which a patched load cannot follow. */
#define TEB_TLS_SLOTS 0x1480
#define TEB_INLINE_SLOTS 64
static DWORD s_i_tls_slot = TLS_OUT_OF_INDEXES;

int ps_host_tls_init(void) {
    if (s_i_tls_slot != TLS_OUT_OF_INDEXES) return 0;
    const DWORD i_slot = TlsAlloc();
    if (i_slot == TLS_OUT_OF_INDEXES) return ps_win32_errno(GetLastError());
    if (i_slot >= TEB_INLINE_SLOTS) {
        TlsFree(i_slot);
        return ENOSPC;
    }
    s_i_tls_slot = i_slot;
    return 0;
}

int32_t ps_host_tls_tcb_offset(void) { return TEB_TLS_SLOTS + 8 * (int32_t)s_i_tls_slot; }

int ps_host_tls_set_base(void *p_base) {
    if (s_i_tls_slot == TLS_OUT_OF_INDEXES) return EINVAL;
    return TlsSetValue(s_i_tls_slot, p_base) ? 0 : ps_win32_errno(GetLastError());
}

/* Read straight from the TEB, since TlsGetValue changes the thread's last error. */
int ps_host_tls_base(uintptr_t *p_out_va_base) {
    if (s_i_tls_slot == TLS_OUT_OF_INDEXES) return EINVAL;
    *p_out_va_base = (uintptr_t)__readgsqword(TEB_TLS_SLOTS + 8 * s_i_tls_slot);
    return 0;
}
