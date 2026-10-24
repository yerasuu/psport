/* Windows: mappings, sections and the process's list of mappings (ps_host.h).
 *
 * The guest's arena (ps_host_vm_reserve_arena) is one placeholder reservation that REPLACE and
 * EXACT mappings replace in, as MAP_FIXED does on Linux: their range is carved out as a single
 * placeholder first (the views it overlaps unmapped, placeholders coalesced and split, the parts of
 * views outside it mapped again with the page protections they had), then a view of a section takes
 * its place. Private memory there is a view of a section of its own, since only a view can be cut
 * and mapped again without losing its bytes. Outside the arena: VirtualAlloc, or a view anywhere. */
#include "ps_host.h"
#include "host_win32.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    HANDLE h_mapping;
    uint64_t cb_section;
    /* A view of all of it, for discards and commits; NULL for the sections of private mappings. */
    unsigned char *p_whole;
    /* SEC_RESERVE: the whole section did not fit the commit limit, so pages are committed as
     * views map them. */
    int is_commit_on_map;
    /* Its descriptor is still open: ps_win32_section_close has not been called. */
    int is_open;
    /* Arena pieces that view it, which map it again when they are cut. */
    size_t c_pieces;
} Section;

typedef struct {
    uintptr_t va_begin, va_end;
    /* -1: a placeholder. */
    int i_section;
    uint64_t off_section;
} Piece;

typedef struct {
    uintptr_t va_begin, va_end;
    DWORD protection;
} ProtectionRun;

typedef struct {
    ProtectionRun *p_runs;
    size_t c_runs, c_capacity;
} ProtectionRuns;

/* Exclusive while the arena or the sections change; shared by protections and discards, which a
 * carve that unmaps and maps views again must not interleave with. */
static SRWLOCK s_lock = SRWLOCK_INIT;
static Section *s_p_sections;
static size_t s_c_sections;
static Piece *s_p_pieces;
static size_t s_c_pieces, s_c_piece_capacity;
static uintptr_t s_va_arena_begin, s_va_arena_end;
static unsigned s_c_file_views_refused;

static int last_error(void) { return ps_win32_errno(GetLastError()); }

size_t ps_host_vm_page_size(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
}

static uintptr_t allocation_granularity(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwAllocationGranularity;
}

/* Windows has no write-only or execute-only-with-write pages: write implies read. */
static DWORD page_protection(unsigned access) {
    if (access & PS_HOST_VM_EXECUTE)
        return access & PS_HOST_VM_WRITE  ? PAGE_EXECUTE_READWRITE
               : access & PS_HOST_VM_READ ? PAGE_EXECUTE_READ
                                          : PAGE_EXECUTE;
    if (access & PS_HOST_VM_WRITE) return PAGE_READWRITE;
    return access & PS_HOST_VM_READ ? PAGE_READONLY : PAGE_NOACCESS;
}

static int report(const char *sz_call, uintptr_t va_range, uint64_t cb_range) {
    const DWORD error = GetLastError();
    printf("Host memory: %s failed at %#llx (+%#llx): Windows error %lu\n", sz_call, (unsigned long long)va_range,
           (unsigned long long)cb_range, error);
    return ps_win32_errno(error);
}

/* Pages without storage (placeholders, reservations, uncommitted pages of a SEC_RESERVE view) have
 * no protection to change and are passed over, as a PROT_NONE reservation takes mprotect on Linux. */
static int protect_committed(uintptr_t va_begin, size_t cb_range, DWORD protection) {
    const uintptr_t va_end = va_begin + cb_range;
    for (uintptr_t va = va_begin; va < va_end;) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)va, &info, sizeof(info))) return report("VirtualQuery (a protection change)", va, 0);
        if (info.State == MEM_FREE) {
            printf("Host memory: protecting %#llx (+%#llx) as %#lx: %#llx (+%#llx) is not mapped\n",
                   (unsigned long long)va_begin, (unsigned long long)cb_range, protection, (unsigned long long)va,
                   (unsigned long long)info.RegionSize);
            return ENOMEM;
        }
        uintptr_t va_next = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (va_next > va_end) va_next = va_end;
        DWORD previous;
        if (info.State == MEM_COMMIT && !VirtualProtect((void *)va, va_next - va, protection, &previous)) {
            const DWORD error = GetLastError();
            printf("Host memory: VirtualProtect failed at %#llx (+%#llx) as %#lx: Windows error %lu (%s view of "
                   "%#llx, created %#lx, now %#lx)\n",
                   (unsigned long long)va, (unsigned long long)(va_next - va), protection, error,
                   info.Type == MEM_IMAGE ? "image" : info.Type == MEM_MAPPED ? "mapped" : "private",
                   (unsigned long long)(uintptr_t)info.AllocationBase, info.AllocationProtect, info.Protect);
            return ps_win32_errno(error);
        }
        va = va_next;
    }
    return 0;
}

int ps_host_vm_protect(void *p_range, size_t cb_range, unsigned access) {
    AcquireSRWLockShared(&s_lock);
    const int error = protect_committed((uintptr_t)p_range, cb_range, page_protection(access));
    ReleaseSRWLockShared(&s_lock);
    return error;
}

/* ---- sections ---- */

static int is_commit_limit(DWORD error) {
    return error == ERROR_COMMITMENT_LIMIT || error == ERROR_NO_SYSTEM_RESOURCES;
}

typedef HANDLE(WINAPI *PfnCreateFileMapping2)(HANDLE, SECURITY_ATTRIBUTES *, ULONG, ULONG, ULONG, ULONG64, PCWSTR,
                                              MEM_EXTENDED_PARAMETER *, ULONG);
static PfnCreateFileMapping2 s_pfn_create_file_mapping2;
static INIT_ONCE s_create_file_mapping2_once = INIT_ONCE_STATIC_INIT;
/* Logged at each power of two, so that a run shows how many sections took the fallback. */
static volatile LONG s_c_fallback_mappings;

static BOOL CALLBACK resolve_create_file_mapping2(PINIT_ONCE p_once, void *p_parameter, void **pp_context) {
    (void)p_once;
    (void)p_parameter;
    (void)pp_context;
    const HMODULE h_kernelbase = GetModuleHandleW(L"kernelbase.dll");
    s_pfn_create_file_mapping2 =
        h_kernelbase ? (PfnCreateFileMapping2)(void (*)(void))GetProcAddress(h_kernelbase, "CreateFileMapping2") : NULL;
    return TRUE;
}

/* CreateFileMapping2 is a recent kernelbase export that Wine (UMU-Proton 10.0-4) lacks, where an
 * import would bind to a stub that aborts at the first call: without it CreateFileMappingW makes the
 * same section, since no extended parameter is passed. */
static HANDLE create_mapping(uint64_t cb_section, ULONG attributes) {
    InitOnceExecuteOnce(&s_create_file_mapping2_once, resolve_create_file_mapping2, NULL, NULL);
    if (s_pfn_create_file_mapping2)
        return s_pfn_create_file_mapping2(INVALID_HANDLE_VALUE, NULL, FILE_MAP_ALL_ACCESS, PAGE_EXECUTE_READWRITE,
                                          attributes, cb_section, NULL, NULL, 0);
    const LONG c_fallbacks = InterlockedIncrement(&s_c_fallback_mappings);
    if (!(c_fallbacks & (c_fallbacks - 1)))
        printf("Host memory: CreateFileMapping2 is missing (Wine): section %ld through CreateFileMappingW\n",
               (long)c_fallbacks);
    return CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE | attributes, (DWORD)(cb_section >> 32),
                              (DWORD)cb_section, NULL);
}

static int add_section(const Section *p_section, int *p_out_i_section) {
    size_t i = 0;
    while (i < s_c_sections && s_p_sections[i].h_mapping) ++i;
    if (i == s_c_sections) {
        if (s_c_sections >= (size_t)(INT_MAX - PS_WIN32_SECTION_BASE)) return EMFILE;
        const size_t c_grown = s_c_sections ? s_c_sections * 2 : 64;
        Section *p_grown = realloc(s_p_sections, c_grown * sizeof(*p_grown));
        if (!p_grown) return ENOMEM;
        memset(p_grown + s_c_sections, 0, (c_grown - s_c_sections) * sizeof(*p_grown));
        s_p_sections = p_grown;
        s_c_sections = c_grown;
    }
    s_p_sections[i] = *p_section;
    *p_out_i_section = (int)i;
    return 0;
}

/* Frees the section once neither its descriptor nor a piece of the arena holds it; views outside
 * the arena keep the storage alive by themselves. */
static void release_section(int i_section) {
    Section *p_section = &s_p_sections[i_section];
    if (p_section->is_open || p_section->c_pieces) return;
    if (p_section->p_whole) UnmapViewOfFile(p_section->p_whole);
    CloseHandle(p_section->h_mapping);
    memset(p_section, 0, sizeof(*p_section));
}

static int section_index(int h_section) {
    if (h_section < PS_WIN32_SECTION_BASE) return -1;
    const size_t i = (size_t)(h_section - PS_WIN32_SECTION_BASE);
    return i < s_c_sections && s_p_sections[i].is_open ? (int)i : -1;
}

int ps_host_vm_section_create(const char *sz_name, uint64_t cb_section, int *p_out_h_section) {
    if (!cb_section) return EINVAL;
    Section section = {.cb_section = cb_section, .is_open = 1};
    section.h_mapping = create_mapping(cb_section, SEC_COMMIT);
    /* Supermedo's 7c9ebcb: the direct memory pool is sized for the largest resolutions, and
     * committing all of it at once can exceed RAM plus page file although the game maps far less. */
    if (!section.h_mapping && is_commit_limit(GetLastError())) {
        const DWORD commit_error = GetLastError();
        section.h_mapping = create_mapping(cb_section, SEC_RESERVE);
        if (section.h_mapping) {
            section.is_commit_on_map = 1;
            printf("Host memory: %s (%llu MiB) does not fit Windows' commit limit at once (Windows error %lu): "
                   "committed as views map it\n",
                   sz_name, (unsigned long long)(cb_section >> 20), commit_error);
        }
    }
    if (!section.h_mapping) return report("CreateFileMapping2 or CreateFileMappingW", 0, cb_section);
    section.p_whole = MapViewOfFile3(section.h_mapping, GetCurrentProcess(), NULL, 0, cb_section, 0, PAGE_READWRITE,
                                     NULL, 0);
    if (!section.p_whole) {
        const int error = report("MapViewOfFile3", 0, cb_section);
        CloseHandle(section.h_mapping);
        return error;
    }
    AcquireSRWLockExclusive(&s_lock);
    int i_section = -1;
    const int error = add_section(&section, &i_section);
    ReleaseSRWLockExclusive(&s_lock);
    if (error) {
        UnmapViewOfFile(section.p_whole);
        CloseHandle(section.h_mapping);
        return error;
    }
    *p_out_h_section = PS_WIN32_SECTION_BASE + i_section;
    return 0;
}

int ps_win32_section_close(int h_section) {
    AcquireSRWLockExclusive(&s_lock);
    const int i_section = section_index(h_section);
    if (i_section >= 0) {
        s_p_sections[i_section].is_open = 0;
        release_section(i_section);
    }
    ReleaseSRWLockExclusive(&s_lock);
    return i_section >= 0 ? 0 : EBADF;
}

static int commit_on_map(int i_section, uint64_t off_section, uint64_t cb_range) {
    const Section *p_section = &s_p_sections[i_section];
    if (!p_section->is_commit_on_map || VirtualAlloc(p_section->p_whole + off_section, cb_range, MEM_COMMIT, PAGE_READWRITE))
        return 0;
    const DWORD error = GetLastError();
    printf("Host memory: cannot commit %llu MiB more guest memory (Windows error %lu): the page file is too small; "
           "let Windows manage its size, or lower the output resolution\n",
           (unsigned long long)(cb_range >> 20), error);
    return ps_win32_errno(error);
}

/* Discarded pages are zeroed, not decommitted: Windows cannot decommit pages of a section while
 * views of it exist. Uncommitted pages of a SEC_RESERVE section read zero once committed. */
int ps_host_vm_section_discard(int h_section, uint64_t off_section, uint64_t cb_range) {
    AcquireSRWLockShared(&s_lock);
    const int i_section = section_index(h_section);
    int error = i_section < 0 ? EBADF : 0;
    if (!error && (off_section > s_p_sections[i_section].cb_section ||
                   cb_range > s_p_sections[i_section].cb_section - off_section))
        error = EINVAL;
    if (!error) {
        const uintptr_t va_begin = (uintptr_t)s_p_sections[i_section].p_whole + off_section, va_end = va_begin + cb_range;
        for (uintptr_t va = va_begin; va < va_end && !error;) {
            MEMORY_BASIC_INFORMATION info;
            if (!VirtualQuery((void *)va, &info, sizeof(info))) {
                error = last_error();
                break;
            }
            uintptr_t va_next = (uintptr_t)info.BaseAddress + info.RegionSize;
            if (va_next > va_end) va_next = va_end;
            if (info.State == MEM_COMMIT) memset((void *)va, 0, va_next - va);
            va = va_next;
        }
    }
    ReleaseSRWLockShared(&s_lock);
    return error;
}

/* ---- the arena ---- */

static int is_in_arena(uintptr_t va_begin, size_t cb_range) {
    return s_va_arena_end && va_begin >= s_va_arena_begin && va_begin < s_va_arena_end &&
           cb_range <= s_va_arena_end - va_begin;
}

static int overlaps_arena(uintptr_t va_begin, size_t cb_range) {
    return s_va_arena_end && va_begin < s_va_arena_end && va_begin + cb_range > s_va_arena_begin;
}

/* The piece holding va, which lies in the arena. */
static size_t piece_index(uintptr_t va) {
    size_t i_low = 0, i_high = s_c_pieces;
    while (i_low < i_high) {
        const size_t i_middle = i_low + (i_high - i_low) / 2;
        if (s_p_pieces[i_middle].va_end <= va)
            i_low = i_middle + 1;
        else
            i_high = i_middle;
    }
    return i_low;
}

static int reserve_pieces(size_t c_more) {
    if (s_c_pieces + c_more <= s_c_piece_capacity) return 0;
    const size_t c_grown = s_c_piece_capacity ? s_c_piece_capacity * 2 : 1024;
    Piece *p_grown = realloc(s_p_pieces, c_grown * sizeof(*p_grown));
    if (!p_grown) return ENOMEM;
    s_p_pieces = p_grown;
    s_c_piece_capacity = c_grown;
    return 0;
}

/* Room for the growth was reserved by reserve_pieces. */
static void replace_pieces(size_t i_piece, size_t c_old, const Piece *p_new, size_t c_new) {
    memmove(s_p_pieces + i_piece + c_new, s_p_pieces + i_piece + c_old,
            (s_c_pieces - i_piece - c_old) * sizeof(*s_p_pieces));
    memcpy(s_p_pieces + i_piece, p_new, c_new * sizeof(*p_new));
    s_c_pieces = s_c_pieces - c_old + c_new;
}

static void drop_view(size_t i_piece) {
    const int i_section = s_p_pieces[i_piece].i_section;
    s_p_pieces[i_piece].i_section = -1;
    --s_p_sections[i_section].c_pieces;
    release_section(i_section);
}

int ps_host_vm_reserve_arena(void *p_begin, size_t cb_range) {
    const uintptr_t va_begin = (uintptr_t)p_begin;
    if (!cb_range || va_begin % allocation_granularity() || cb_range % allocation_granularity()) return EINVAL;
    AcquireSRWLockExclusive(&s_lock);
    int error = s_va_arena_end ? EEXIST : reserve_pieces(1);
    if (!error && !VirtualAlloc2(NULL, p_begin, cb_range, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0))
        error = report("VirtualAlloc2 (the guest's arena)", va_begin, cb_range);
    if (!error) {
        s_va_arena_begin = va_begin;
        s_va_arena_end = va_begin + cb_range;
        s_p_pieces[0] = (Piece){va_begin, s_va_arena_end, -1, 0};
        s_c_pieces = 1;
    }
    ReleaseSRWLockExclusive(&s_lock);
    return error;
}

static int save_protections(uintptr_t va_begin, uintptr_t va_end, ProtectionRuns *p_runs) {
    for (uintptr_t va = va_begin; va < va_end;) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)va, &info, sizeof(info))) return last_error();
        uintptr_t va_next = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (va_next > va_end) va_next = va_end;
        if (info.State == MEM_COMMIT && info.Protect != PAGE_READWRITE) {
            if (p_runs->c_runs == p_runs->c_capacity) {
                const size_t c_grown = p_runs->c_capacity ? p_runs->c_capacity * 2 : 16;
                ProtectionRun *p_grown = realloc(p_runs->p_runs, c_grown * sizeof(*p_grown));
                if (!p_grown) return ENOMEM;
                p_runs->p_runs = p_grown;
                p_runs->c_capacity = c_grown;
            }
            p_runs->p_runs[p_runs->c_runs++] = (ProtectionRun){va, va_next, info.Protect};
        }
        va = va_next;
    }
    return 0;
}

/* A view replaces the placeholder piece, or the piece stays a placeholder; the caller already
 * counted it in the section's c_pieces. */
static int map_view(size_t i_piece, int i_section, uint64_t off_section, DWORD protection) {
    Piece *p_piece = &s_p_pieces[i_piece];
    const size_t cb_view = p_piece->va_end - p_piece->va_begin;
    /* A view cannot start without access; its protection changes after. */
    const DWORD initial = protection == PAGE_NOACCESS ? PAGE_READWRITE : protection;
    if (!MapViewOfFile3(s_p_sections[i_section].h_mapping, GetCurrentProcess(), (void *)p_piece->va_begin, off_section,
                        cb_view, MEM_REPLACE_PLACEHOLDER, initial, NULL, 0))
        return report("MapViewOfFile3", p_piece->va_begin, cb_view);
    const int error = initial == protection ? 0 : protect_committed(p_piece->va_begin, cb_view, protection);
    if (error && UnmapViewOfFile2(GetCurrentProcess(), (void *)p_piece->va_begin, MEM_PRESERVE_PLACEHOLDER)) return error;
    if (error) report("UnmapViewOfFile2 (a view whose protection failed)", p_piece->va_begin, cb_view);
    p_piece->i_section = i_section;
    p_piece->off_section = off_section;
    return error;
}

/* Leaves [va_begin, va_end) one placeholder piece, its index in *p_out_i_piece. On failure the
 * table still says what is mapped, but the parts of views around the range may be gone. */
static int carve(uintptr_t va_begin, uintptr_t va_end, size_t *p_out_i_piece) {
    if (reserve_pieces(2)) return ENOMEM;
    const size_t i_first = piece_index(va_begin), i_last = piece_index(va_end - 1);
    const Piece first = s_p_pieces[i_first], last = s_p_pieces[i_last];
    if (i_first == i_last && first.i_section < 0 && first.va_begin == va_begin && first.va_end == va_end) {
        *p_out_i_piece = i_first;
        return 0;
    }
    int is_left_kept = first.i_section >= 0 && first.va_begin < va_begin;
    int is_right_kept = last.i_section >= 0 && va_end < last.va_end;
    ProtectionRuns runs = {0};
    int error = is_left_kept ? save_protections(first.va_begin, va_begin, &runs) : 0;
    if (!error && is_right_kept) error = save_protections(va_end, last.va_end, &runs);
    if (error) {
        free(runs.p_runs);
        return error;
    }
    /* Held for the kept parts, so that unmapping the old views does not free their sections. */
    if (is_left_kept) ++s_p_sections[first.i_section].c_pieces;
    if (is_right_kept) ++s_p_sections[last.i_section].c_pieces;
    for (size_t i = i_first; i <= i_last && !error; ++i) {
        if (s_p_pieces[i].i_section < 0) continue;
        if (UnmapViewOfFile2(GetCurrentProcess(), (void *)s_p_pieces[i].va_begin, MEM_PRESERVE_PLACEHOLDER))
            drop_view(i);
        else
            error = report("UnmapViewOfFile2", s_p_pieces[i].va_begin, s_p_pieces[i].va_end - s_p_pieces[i].va_begin);
    }
    if (!error && i_last > i_first) {
        if (VirtualFree((void *)first.va_begin, last.va_end - first.va_begin, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
            const Piece whole = {first.va_begin, last.va_end, -1, 0};
            replace_pieces(i_first, i_last - i_first + 1, &whole, 1);
        } else {
            error = report("VirtualFree (coalescing placeholders)", first.va_begin, last.va_end - first.va_begin);
        }
    }
    size_t i_carved = i_first;
    if (!error && first.va_begin < va_begin) {
        if (VirtualFree((void *)first.va_begin, va_begin - first.va_begin, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            const Piece split[2] = {{first.va_begin, va_begin, -1, 0}, {va_begin, last.va_end, -1, 0}};
            replace_pieces(i_carved++, 1, split, 2);
        } else {
            error = report("VirtualFree (splitting a placeholder)", first.va_begin, va_begin - first.va_begin);
        }
    }
    if (!error && va_end < last.va_end) {
        if (VirtualFree((void *)va_begin, va_end - va_begin, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            const Piece split[2] = {{va_begin, va_end, -1, 0}, {va_end, last.va_end, -1, 0}};
            replace_pieces(i_carved, 1, split, 2);
        } else {
            error = report("VirtualFree (splitting a placeholder)", va_begin, va_end - va_begin);
        }
    }
    if (!error && is_left_kept) {
        error = map_view(i_carved - 1, first.i_section, first.off_section, PAGE_READWRITE);
        is_left_kept = error != 0;
    }
    if (!error && is_right_kept) {
        error = map_view(i_carved + 1, last.i_section, last.off_section + (va_end - last.va_begin), PAGE_READWRITE);
        is_right_kept = error != 0;
    }
    for (size_t i = 0; i < runs.c_runs && !error; ++i) {
        DWORD previous;
        if (!VirtualProtect((void *)runs.p_runs[i].va_begin, runs.p_runs[i].va_end - runs.p_runs[i].va_begin,
                            runs.p_runs[i].protection, &previous))
            error = report("VirtualProtect (restoring a kept view's protection)", runs.p_runs[i].va_begin,
                           runs.p_runs[i].va_end - runs.p_runs[i].va_begin);
    }
    free(runs.p_runs);
    /* A kept part still flagged here was not mapped again: its hold goes. */
    if (is_left_kept) {
        --s_p_sections[first.i_section].c_pieces;
        release_section(first.i_section);
    }
    if (is_right_kept) {
        --s_p_sections[last.i_section].c_pieces;
        release_section(last.i_section);
    }
    if (error) {
        printf("Host memory: carving %#llx-%#llx out of the arena failed: the views around it may be unmapped\n",
               (unsigned long long)va_begin, (unsigned long long)va_end);
        return error;
    }
    *p_out_i_piece = i_carved;
    return 0;
}

static int is_viewed(uintptr_t va_begin, uintptr_t va_end) {
    for (size_t i = piece_index(va_begin); i < s_c_pieces && s_p_pieces[i].va_begin < va_end; ++i)
        if (s_p_pieces[i].i_section >= 0) return 1;
    return 0;
}

/* i_section -1 maps private memory: a section of its own unless access is none, which leaves a
 * placeholder (a reservation, which EXACT may then take, unlike MAP_FIXED_NOREPLACE). Lock held. */
static int map_in_arena(uintptr_t va_begin, size_t cb_range, unsigned access, unsigned flags, int i_section,
                        uint64_t off_section) {
    if ((flags & PS_HOST_VM_EXACT) && is_viewed(va_begin, va_begin + cb_range)) return EEXIST;
    int i_own = -1;
    if (i_section < 0 && access != PS_HOST_VM_NONE) {
        const Section own = {.h_mapping = create_mapping(cb_range, SEC_COMMIT), .cb_section = cb_range};
        if (!own.h_mapping) return report("CreateFileMapping2 or CreateFileMappingW (private memory)", va_begin, cb_range);
        const int error = add_section(&own, &i_own);
        if (error) {
            CloseHandle(own.h_mapping);
            return error;
        }
        i_section = i_own;
    }
    size_t i_piece;
    int error = carve(va_begin, va_begin + cb_range, &i_piece);
    if (!error && i_section >= 0 && !(flags & PS_HOST_VM_SPARSE)) error = commit_on_map(i_section, off_section, cb_range);
    if (!error && i_section >= 0) {
        ++s_p_sections[i_section].c_pieces;
        error = map_view(i_piece, i_section, off_section, page_protection(access));
        if (error && s_p_pieces[i_piece].i_section < 0) --s_p_sections[i_section].c_pieces;
    }
    if (i_own >= 0) release_section(i_own);
    return error;
}

/* ---- mappings ---- */

static int is_placed(unsigned flags) { return (flags & (PS_HOST_VM_REPLACE | PS_HOST_VM_EXACT)) != 0; }

static void *failed(int error) {
    errno = error;
    return NULL;
}

/* Outside the arena REPLACE cannot take an address something holds: it behaves as EXACT. Hints are
 * not followed, since VirtualAlloc fails instead of moving on. The length is rounded up to whole
 * pages as mmap does, since callers ask for the size of an object. */
void *ps_host_vm_map(void *p_at, size_t cb_range, unsigned access, unsigned flags) {
    const uintptr_t va_at = (uintptr_t)p_at;
    const size_t cb_page = ps_host_vm_page_size();
    if (!cb_range || cb_range > SIZE_MAX - cb_page) return failed(EINVAL);
    cb_range = (cb_range + cb_page - 1) & ~(cb_page - 1);
    if (is_placed(flags)) {
        if (va_at % cb_page) return failed(EINVAL);
        AcquireSRWLockExclusive(&s_lock);
        const int is_arena = is_in_arena(va_at, cb_range), is_straddling = !is_arena && overlaps_arena(va_at, cb_range);
        const int error = is_arena ? map_in_arena(va_at, cb_range, access, flags, -1, 0) : is_straddling ? EINVAL : 0;
        ReleaseSRWLockExclusive(&s_lock);
        if (is_arena || error) return error ? failed(error) : p_at;
    }
    const DWORD type = MEM_RESERVE | (access == PS_HOST_VM_NONE && (flags & PS_HOST_VM_SPARSE) ? 0 : MEM_COMMIT);
    void *p_mapping = VirtualAlloc(is_placed(flags) ? p_at : NULL, cb_range, type, page_protection(access));
    if (!p_mapping) return failed(is_placed(flags) && GetLastError() == ERROR_INVALID_ADDRESS ? EEXIST : last_error());
    if (is_placed(flags) && p_mapping != p_at) {
        VirtualFree(p_mapping, 0, MEM_RELEASE);
        return failed(EINVAL);
    }
    return p_mapping;
}

/* Views outside the arena start at the allocation granularity in the section: the view begins
 * that far before off_file and the caller gets the address inside it. */
static void *view_outside(int i_section, void *p_at, size_t cb_range, unsigned access, unsigned flags,
                          uint64_t off_section) {
    const uint64_t cb_lead = off_section % allocation_granularity();
    if (is_placed(flags) && (cb_lead || (uintptr_t)p_at % allocation_granularity())) return failed(EINVAL);
    if (!(flags & PS_HOST_VM_SPARSE)) {
        const int error = commit_on_map(i_section, off_section, cb_range);
        if (error) return failed(error);
    }
    const DWORD protection = page_protection(access);
    const DWORD initial = protection == PAGE_NOACCESS ? PAGE_READWRITE : protection;
    unsigned char *p_view =
        MapViewOfFile3(s_p_sections[i_section].h_mapping, GetCurrentProcess(), is_placed(flags) ? p_at : NULL,
                       off_section - cb_lead, cb_range + cb_lead, 0, initial, NULL, 0);
    if (!p_view) return failed(is_placed(flags) && GetLastError() == ERROR_INVALID_ADDRESS ? EEXIST : last_error());
    if (initial != protection) {
        const int error = protect_committed((uintptr_t)p_view + cb_lead, cb_range, protection);
        if (error) {
            UnmapViewOfFile(p_view);
            return failed(error);
        }
    }
    return p_view + cb_lead;
}

/* Only sections are mapped: a file view needs the file's HANDLE from host_file.c, which does not
 * hand one out yet. */
void *ps_host_vm_map_shared(void *p_at, size_t cb_range, unsigned access, unsigned flags, int h_file,
                            uint64_t off_file) {
    const uintptr_t va_at = (uintptr_t)p_at;
    if (!cb_range || (is_placed(flags) && (va_at % ps_host_vm_page_size() || cb_range % ps_host_vm_page_size())))
        return failed(EINVAL);
    AcquireSRWLockExclusive(&s_lock);
    const int i_section = section_index(h_file);
    void *p_mapping = NULL;
    int error = 0;
    if (i_section < 0) {
        printf("Host memory: descriptor %d is not a section and file views are not implemented on Windows: refused "
               "(%u so far)\n",
               h_file, ++s_c_file_views_refused);
        error = ENODEV;
    } else if (off_file > s_p_sections[i_section].cb_section ||
               cb_range > s_p_sections[i_section].cb_section - off_file) {
        error = EINVAL;
    } else if (is_placed(flags) && is_in_arena(va_at, cb_range)) {
        error = map_in_arena(va_at, cb_range, access, flags, i_section, off_file);
        p_mapping = error ? NULL : p_at;
    } else if (is_placed(flags) && overlaps_arena(va_at, cb_range)) {
        error = EINVAL;
    } else {
        p_mapping = view_outside(i_section, p_at, cb_range, access, flags, off_file);
        error = p_mapping ? 0 : errno;
    }
    ReleaseSRWLockExclusive(&s_lock);
    return error ? failed(error) : p_mapping;
}

/* The end of the allocation (view or VirtualAlloc block) that starts at va_base. */
static uintptr_t allocation_end(uintptr_t va_base) {
    uintptr_t va = va_base;
    MEMORY_BASIC_INFORMATION info;
    while (VirtualQuery((void *)va, &info, sizeof(info)) && info.State != MEM_FREE &&
           (uintptr_t)info.AllocationBase == va_base)
        va = (uintptr_t)info.BaseAddress + info.RegionSize;
    return va;
}

/* Outside the arena a mapping goes whole: Windows cannot release part of a view or of a
 * VirtualAlloc block, so a part of one is EINVAL. */
static int unmap_outside(uintptr_t va_begin, size_t cb_range) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((void *)va_begin, &info, sizeof(info))) return last_error();
    if (info.State == MEM_FREE) return 0;
    const uintptr_t va_base = (uintptr_t)info.AllocationBase;
    const uintptr_t cb_lead_allowed = info.Type == MEM_MAPPED ? allocation_granularity() : 1;
    if (va_begin - va_base >= cb_lead_allowed || va_begin + cb_range < allocation_end(va_base)) return EINVAL;
    if (info.Type == MEM_MAPPED) return UnmapViewOfFile((void *)va_base) ? 0 : last_error();
    if (info.Type == MEM_PRIVATE) return VirtualFree((void *)va_base, 0, MEM_RELEASE) ? 0 : last_error();
    return EINVAL;
}

int ps_host_vm_unmap(void *p_range, size_t cb_range) {
    const uintptr_t va_begin = (uintptr_t)p_range;
    const size_t cb_page = ps_host_vm_page_size();
    if (!cb_range || va_begin % cb_page || cb_range > SIZE_MAX - cb_page) return EINVAL;
    cb_range = (cb_range + cb_page - 1) & ~(cb_page - 1);
    AcquireSRWLockExclusive(&s_lock);
    size_t i_piece;
    const int error = is_in_arena(va_begin, cb_range)      ? carve(va_begin, va_begin + cb_range, &i_piece)
                      : overlaps_arena(va_begin, cb_range) ? EINVAL
                                                           : unmap_outside(va_begin, cb_range);
    ReleaseSRWLockExclusive(&s_lock);
    return error;
}

/* Every view is of a pagefile-backed section (file views are refused), so there is nothing to
 * write back, and views of one section always see the same bytes. */
int ps_host_vm_flush(void *p_range, size_t cb_range, unsigned flags) {
    (void)p_range;
    (void)cb_range;
    (void)flags;
    return 0;
}

/* The heap comes from VirtualAlloc, which without high-entropy ASLR (psport.exe linked with
 * --disable-high-entropy-va) places it bottom-up in the low gigabytes, below the guest's arena. */
void ps_host_vm_keep_heap_low(void) {}

/* ---- the list of mappings ---- */

static char *append_hex(char *p_out, uint64_t value) {
    for (int i = 11; i >= 0; --i, value >>= 4) p_out[i] = "0123456789abcdef"[value & 15];
    return p_out + 12;
}

static char *append_text(char *p_out, const char *sz_text) {
    while (*sz_text) *p_out++ = *sz_text++;
    return p_out;
}

/* Built in a buffer on the stack, since a fault handler may list them. */
int ps_host_list_mappings(int (*pfn_mapping)(const PsHostMapping *p_mapping, void *p_context), void *p_context) {
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    const uintptr_t va_last = (uintptr_t)system.lpMaximumApplicationAddress;
    for (uintptr_t va = (uintptr_t)system.lpMinimumApplicationAddress; va < va_last;) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)va, &info, sizeof(info))) return last_error();
        const uintptr_t va_next = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (info.State != MEM_FREE) {
            const DWORD protection = info.State == MEM_COMMIT ? info.Protect & 0xff : PAGE_NOACCESS;
            PsHostMapping mapping = {
                .va_begin = (uintptr_t)info.BaseAddress,
                .va_end = va_next,
                .is_readable = protection != PAGE_NOACCESS && protection != PAGE_EXECUTE,
                .is_writable = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                               protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY,
                .is_executable = protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
                                 protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY,
            };
            char sz_line[64];
            char *p_end = append_hex(sz_line, mapping.va_begin);
            *p_end++ = '-';
            p_end = append_hex(p_end, mapping.va_end);
            *p_end++ = ' ';
            *p_end++ = mapping.is_readable ? 'r' : '-';
            *p_end++ = mapping.is_writable ? 'w' : '-';
            *p_end++ = mapping.is_executable ? 'x' : '-';
            p_end = append_text(p_end, info.Type == MEM_IMAGE ? " image" : info.Type == MEM_MAPPED ? " mapped" : " private");
            if (info.State == MEM_RESERVE) p_end = append_text(p_end, " reserved");
            *p_end = 0;
            mapping.sz_description = sz_line;
            if (pfn_mapping(&mapping, p_context)) return 0;
        }
        va = va_next;
    }
    return 0;
}
