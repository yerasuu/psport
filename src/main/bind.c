/* Import binding: every relocation pointed at the runtime's implementation, a native module's
 * export, a stub or a trap that stops (or, with PS_PERMISSIVE, answers 0); then the patches. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "main.h"
#include "ps_host.h"

/* PS_PERMISSIVE=1: an import the runtime lacks returns 0 instead of stopping, so a title
 * the runtime does not cover fully gets past the first one and its log lists every import it reached
 * (each once). */
static unsigned char *s_p_permissive_seen;
/* Its output arguments (runtime_import_outputs) per import: answered with 0 the caller goes on to
 * read memory nothing wrote, and faults far from the import. PS_PERMISSIVE_OUT=error answers these
 * with SCE_KERNEL_ERROR_EINVAL instead, so the caller's own error path runs. */
static unsigned char *s_p_permissive_outputs;
static int s_is_permissive_out_error;
static ABI int64_t unresolved_permissive(uint32_t id, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                                         uintptr_t a4) {
    if (id >= g_c_imports) return 0;
    runtime_calls_unimplemented(id);
    const unsigned outputs = s_p_permissive_outputs[id];
    const int32_t result = outputs && s_is_permissive_out_error ? (int32_t)0x80020016 : 0;
    if (!__atomic_exchange_n(&s_p_permissive_seen[id], 1, __ATOMIC_RELAXED)) {
        if (!outputs) {
            printf("Runtime: unsupported import %s (%s) returns 0; first argument: 0x%" PRIxPTR "\n",
                   g_p_names[id], runtime_import_name(g_p_names[id]), a0);
            return 0;
        }
        /* The trampoline keeps the first five arguments; the sixth (r9) is gone by now. */
        const uintptr_t arguments[5] = {a0, a1, a2, a3, a4};
        char sz_list[256];
        size_t used = 0;
        for (unsigned i_argument = 0; i_argument < 8 && used < sizeof(sz_list); ++i_argument) {
            if (!(outputs >> i_argument & 1)) continue;
            int written = i_argument < 5 ? snprintf(sz_list + used, sizeof(sz_list) - used, "%s%u (0x%" PRIxPTR ")",
                                           used ? ", " : "", i_argument, arguments[i_argument])
                                : snprintf(sz_list + used, sizeof(sz_list) - used, "%s%u", used ? ", " : "", i_argument);
            if (written > 0) used += (size_t)written;
        }
        printf("Runtime: unsupported import %s (%s) returns 0x%x but leaves its output argument%s %s unwritten\n",
               g_p_names[id], runtime_import_name(g_p_names[id]), (unsigned)result, outputs & (outputs - 1) ? "s" : "",
               sz_list);
    }
    return result;
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= g_c_imports) fail("bad import trap index");
    runtime_calls_unimplemented(id);
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", g_p_names[id], id);
    printf("API: %s\n", runtime_import_name(g_p_names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)g_p_image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)g_p_image;
    for (uint64_t m=0;m<g_c_modules;++m)
        if (caller>=g_p_modules[m].base && caller-g_p_modules[m].base<g_p_modules[m].cb_module)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,g_p_modules[m].sz_name,caller-g_p_modules[m].base);
    runtime_report();
    psport_gpu_flush_caches(1);
    puts(g_has_entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    ps_host_exit_at_once(20); /* no destructors: GPU, audio and guest threads are still running */
}
/* An import bound to the runtime's or the GPU library's own function whose result tells a failure
 * (runtime_import_failure) is called through one of these: it keeps the six argument registers on
 * its stack, calls the function and hands a result that looks like a failure, with them, to
 * runtime_calls_failed. Arguments on the stack would move, so those imports are not listed. */
static void write_thunk(unsigned char *p_thunk, uint32_t id, uint32_t kind, uintptr_t target) {
    static const unsigned char s_save[33] = {
        0x48, 0x83, 0xec, 0x38,       /* sub rsp,56 */
        0x48, 0x89, 0x3c, 0x24,       /* mov [rsp],rdi */
        0x48, 0x89, 0x74, 0x24, 0x08, /* mov [rsp+8],rsi */
        0x48, 0x89, 0x54, 0x24, 0x10, /* mov [rsp+16],rdx */
        0x48, 0x89, 0x4c, 0x24, 0x18, /* mov [rsp+24],rcx */
        0x4c, 0x89, 0x44, 0x24, 0x20, /* mov [rsp+32],r8 */
        0x4c, 0x89, 0x4c, 0x24, 0x28, /* mov [rsp+40],r9 */
    };
    static const unsigned char s_checks[5][7] = {
        {0},
        {0x3d, 0x00, 0x00, 0x00, 0x80, 0x73, 0x05}, /* cmp eax,0x80000000; jae slow */
        {0x90, 0x90, 0x83, 0xf8, 0xff, 0x74, 0x05}, /* cmp eax,-1; je slow */
        {0x90, 0x48, 0x83, 0xf8, 0xff, 0x74, 0x05}, /* cmp rax,-1; je slow */
        {0x90, 0x90, 0x48, 0x85, 0xc0, 0x78, 0x05}, /* test rax,rax; js slow */
    };
    static const unsigned char s_done[5] = {0x48, 0x83, 0xc4, 0x38, 0xc3}; /* add rsp,56; ret */
    const uintptr_t handler = (uintptr_t)runtime_calls_failed;
    memcpy(p_thunk, s_save, 33);
    p_thunk[33] = 0x49; p_thunk[34] = 0xbb; memcpy(p_thunk + 35, &target, 8); /* movabs r11,target */
    p_thunk[43] = 0x41; p_thunk[44] = 0xff; p_thunk[45] = 0xd3;               /* call r11 */
    memcpy(p_thunk + 46, s_checks[kind], 7);
    memcpy(p_thunk + 53, s_done, 5);
    p_thunk[58] = 0xbf; memcpy(p_thunk + 59, &id, 4);                    /* slow: mov edi,id */
    p_thunk[63] = 0xb9; memcpy(p_thunk + 64, &kind, 4);                  /* mov ecx,kind */
    p_thunk[68] = 0x48; p_thunk[69] = 0x89; p_thunk[70] = 0xe6;                /* mov rsi,rsp */
    p_thunk[71] = 0x48; p_thunk[72] = 0x89; p_thunk[73] = 0xc2;                /* mov rdx,rax */
    p_thunk[74] = 0x49; p_thunk[75] = 0xbb; memcpy(p_thunk + 76, &handler, 8); /* movabs r11,runtime_calls_failed */
    p_thunk[84] = 0x41; p_thunk[85] = 0xff; p_thunk[86] = 0xd3;               /* call r11 */
    memcpy(p_thunk + 87, s_done, 5);
}
int mapped(Segment *p_segments, uint64_t c_segments, uint64_t va_range, uint64_t cb_range) {
    for (uint64_t i = 0; i < c_segments; ++i)
        if (va_range >= p_segments[i].va_segment && cb_range <= p_segments[i].cb_segment &&
            va_range - p_segments[i].va_segment <= p_segments[i].cb_segment - cb_range) return 1;
    return 0;
}
/* The patches' writes at image offsets, after relocation. A write may replace a whole
 * base-relative pointer slot (60/90 FPS++ swap function pointers): the patch holds the address at
 * the eboot's patch base, 0x400000, rebased here. */
void apply_patches(const PsPatchList *p_patches, Segment *p_segments, uint64_t c_segments, const Reloc *p_relocs, uint64_t c_relocs) {
    const uint64_t base=0x400000;
    uint64_t cb_patched=0, rebased=0;
    for (size_t i=0;i<p_patches->c_writes;++i) {
        uint64_t off_image=p_patches->p_writes[i].off_image, length=p_patches->p_writes[i].cb_data;
        const unsigned char *p_data=p_patches->p_writes[i].p_data;
        if (!length || length>4096 || !mapped(p_segments,c_segments,off_image,length)) fail("bad patch entry");
        uint64_t slots[4096/8]; size_t nslots=0;
        for (uint64_t r=0;r<c_relocs;++r) {
            if (!(p_relocs[r].target<off_image+length && off_image<p_relocs[r].target+8)) continue;
            uint64_t target=p_relocs[r].target, value;
            if (p_relocs[r].kind || target<off_image || target+8>off_image+length) fail("patch overlaps a relocation");
            memcpy(&value,p_data+(target-off_image),8);
            if (value<base || !mapped(p_segments,c_segments,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(g_p_image+off_image,p_data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,g_p_image+slots[s],8);
            value=(uint64_t)(uintptr_t)g_p_image+(value-base);
            memcpy(g_p_image+slots[s],&value,8);
        }
        rebased+=nslots;
        cb_patched+=length;
    }
    printf("Patches: %zu writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",p_patches->c_writes,cb_patched,rebased);
}
void bind_imports(Segment *p_segments, uint64_t c_segments, Reloc *p_relocs, uint64_t c_relocs, int native_libc,
                  const uint64_t *p_bindings, const uint64_t *p_binding_kinds, unsigned char **pp_out_traps,
                  unsigned char **pp_out_thunks) {
    const char *sz_permissive = runtime_env("PS_PERMISSIVE", NULL);
    if (sz_permissive && !strcmp(sz_permissive, "1")) {
        s_p_permissive_seen = calloc(g_c_imports ? g_c_imports : 1, 1);
        s_p_permissive_outputs = calloc(g_c_imports ? g_c_imports : 1, 1);
        if (!s_p_permissive_seen || !s_p_permissive_outputs) fail("allocation failed");
        for (uint64_t i = 0; i < g_c_imports; ++i)
            s_p_permissive_outputs[i] = (unsigned char)runtime_import_outputs(g_p_names[i]);
        const char *sz_out = runtime_env("PS_PERMISSIVE_OUT", NULL);
        s_is_permissive_out_error = sz_out && !strcmp(sz_out, "error");
    }
    unsigned char *p_traps = allocate(round_page((g_c_imports + 1) * 32));
    unsigned char *p_thunks = allocate(round_page((g_c_imports + 1) * THUNK_SIZE));
    unsigned char *p_data_traps = allocate((g_c_imports + 1) * g_cb_page);
    protect(p_data_traps, (g_c_imports + 1) * g_cb_page, 0);
    for (uint64_t i = 0; i < g_c_imports; ++i) {
        unsigned char *p_trap = p_traps + i * 32;
        /* SysV: the arguments move up one register (the sixth drops off) so the handler gets the
         * index first and the guest's first five after it; then movabs rax, handler; jmp rax. No
         * fake return values, unless PS_PERMISSIVE asked for them. Exactly the 32 bytes a slot has. */
        uint32_t i_import = (uint32_t)i;
        uintptr_t handler = s_p_permissive_seen ? (uintptr_t)unresolved_permissive : (uintptr_t)unresolved;
        static const unsigned char s_shift[15] = {
            0x4d, 0x89, 0xc1, /* mov r9,r8 */
            0x49, 0x89, 0xc8, /* mov r8,rcx */
            0x48, 0x89, 0xd1, /* mov rcx,rdx */
            0x48, 0x89, 0xf2, /* mov rdx,rsi */
            0x48, 0x89, 0xfe, /* mov rsi,rdi */
        };
        memcpy(p_trap, s_shift, sizeof(s_shift));
        p_trap[15] = 0xbf; memcpy(p_trap + 16, &i_import, 4);
        p_trap[20] = 0x48; p_trap[21] = 0xb8; memcpy(p_trap + 22, &handler, 8);
        p_trap[30] = 0xff; p_trap[31] = 0xe0;
    }
    /* What each import got, for the report below: 1 host, 2 native module, 3 stub, 4 nothing,
     * 5 a system module (loader binding kinds 3 and 4, which win over the host), 6 a module
     * the game loads itself (kinds 5 and 6, which win too). */
    unsigned char *p_import_state = calloc(g_c_imports ? g_c_imports : 1, 1);
    if (!p_import_state) fail("allocation failed");
    for (uint64_t i = 0; i < c_relocs; ++i) {
        uintptr_t value = p_relocs[i].kind == 2 ? (uintptr_t)(p_data_traps + g_cb_page * p_relocs[i].value + p_relocs[i].addend)
                        : p_relocs[i].kind == 1 ? (uintptr_t)(p_traps + 32 * p_relocs[i].value)
                        : (uintptr_t)g_p_image + p_relocs[i].value;
        if (p_relocs[i].kind) {
            g_runtime_last_stub = 0;
            const uint64_t bound_kind = native_libc ? p_binding_kinds[p_relocs[i].value] : 0;
            uintptr_t resolved = bound_kind > 2 ? 0 : runtime_resolve(g_p_names[p_relocs[i].value], p_relocs[i].kind == 2);
            if (resolved) {
                value = resolved + p_relocs[i].addend;
                p_import_state[p_relocs[i].value] = g_runtime_last_stub ? 3 : 1;
                const unsigned failure = p_relocs[i].kind == 1 ? runtime_import_failure(g_p_names[p_relocs[i].value]) : 0;
                if (p_relocs[i].kind == 1 && g_runtime_last_stub) {
                    const uintptr_t handler = (uintptr_t)runtime_calls_stub;
                    memcpy(p_traps + 32 * p_relocs[i].value + 22, &handler, 8);
                    value = (uintptr_t)(p_traps + 32 * p_relocs[i].value);
                } else if (failure) {
                    write_thunk(p_thunks + THUNK_SIZE * p_relocs[i].value, (uint32_t)p_relocs[i].value, failure, resolved);
                    value = (uintptr_t)(p_thunks + THUNK_SIZE * p_relocs[i].value);
                }
            } else if (native_libc && p_bindings[p_relocs[i].value]) {
                uint64_t va_target=p_bindings[p_relocs[i].value];
                if ((bound_kind - 1) % 2 + 1 != p_relocs[i].kind || !mapped(p_segments,c_segments,va_target,p_relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)g_p_image+va_target+p_relocs[i].addend;
                p_import_state[p_relocs[i].value] = bound_kind > 4 ? 6 : bound_kind > 2 ? 5 : 2;
            } else p_import_state[p_relocs[i].value] = 4;
        }
        memcpy(g_p_image + p_relocs[i].target, &value, 8);
    }
    /* Before the game runs: every import that will not do what it asks, so a log shows what
     * cannot work even when the game never gets as far as calling it. */
    unsigned counts[7] = {0};
    for (uint64_t i = 0; i < g_c_imports; ++i) {
        ++counts[p_import_state[i]];
        if (p_import_state[i] == 3)
            printf("Runtime: import %s (%s) is a stub: it succeeds without doing anything\n",
                   g_p_names[i], runtime_import_name(g_p_names[i]));
        else if (p_import_state[i] == 4)
            printf("Runtime: import %s (%s) has no implementation: %s\n", g_p_names[i], runtime_import_name(g_p_names[i]),
                   !s_p_permissive_seen ? "the game stops if it calls it"
                   : s_is_permissive_out_error && s_p_permissive_outputs[i]
                       ? "returns 0x80020016 when called (PS_PERMISSIVE_OUT=error)"
                       : "returns 0 when called (PS_PERMISSIVE)");
    }
    printf("Runtime: imports %llu: host %u, game modules %u, system modules %u, modules the game loads %u, stubs %u, without implementation %u\n",
           (unsigned long long)g_c_imports, counts[1], counts[2], counts[5], counts[6], counts[3], counts[4]);
    free(p_import_state);
    *pp_out_traps = p_traps;
    *pp_out_thunks = p_thunks;
}
