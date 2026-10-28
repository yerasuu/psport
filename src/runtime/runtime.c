// SPDX-License-Identifier: GPL-2.0-or-later
/* Narrow, explicit PS4 libc contracts. No automatic success stubs. */
#define _CRT_RAND_S
#include "runtime.h"
#include "gpu/psport_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>

typedef struct {
    union { GuestCallback pfn_plain; void (ABI *pfn_with_arg)(void *); } callback;
    void *p_argument, *p_dso;
    int with_arg, is_active;
} ExitHandler;
static ExitHandler *s_p_handlers;
static size_t s_c_handlers, s_c_handler_capacity, s_c_init_calls, s_c_atexit_calls, s_c_cxa_calls;
static uint64_t s_capabilities;
static _Atomic size_t s_c_guards_acquired, s_c_guards_released;
static uint64_t s_stack_canary;
static _Atomic size_t s_c_memory_calls;
#define TLS_MODULES RUNTIME_TLS_MODULES
/* Longer than every exit report bounded lock wait together. */
#define REPORT_WAIT_MS 3000
static struct { const void *p_data; uint64_t filesz, memsz; } s_tls_modules[TLS_MODULES];
static _Thread_local unsigned char *s_tls_blocks[TLS_MODULES];
static void *s_p_process_param;
void runtime_set_procparam(void *p_param) {
    s_p_process_param=p_param;
    /* psport: the game's libc heap settings (SceProcParam +0x38: SceLibcParam; its +0x10.. pointers to
     * the heap size, delayed and extended allocation flags, initial size). +0x30 is the main thread's
     * stack size pointer, which newer SDKs fill, so reading the libc block from there faulted. */
    const uint64_t *p_words=(const uint64_t *)p_param;
    if (!p_words || p_words[0]<0x40 || !p_words[7]) return;
    const uint64_t *p_libc=(const uint64_t *)p_words[7];
    printf("Runtime: libc param size %#llx, entries %u", (unsigned long long)p_libc[0], (unsigned)p_libc[1]);
    static const char *s_names[]={"heap size","delayed alloc","extended alloc","initial size"};
    for (int i=0;i<4 && (uint64_t)(2+i)*8<p_libc[0];++i) {
        const void *p_field=(const void *)p_libc[2+i];
        if (!p_field) { printf(", %s -",s_names[i]); continue; }
        if (i==0 || i==3) printf(", %s %#llx",s_names[i],(unsigned long long)*(const uint64_t *)p_field);
        else printf(", %s %u",s_names[i],*(const uint32_t *)p_field);
    }
    printf("\n");
}
static ABI void *guest_procparam(void) { return s_p_process_param; }
/* The game's own allocator for the system libraries to use; the runtime's never call it. */
static ABI void guest_set_heap_api(void **pp_api) {
    printf("Runtime: application heap API registered at %p, not used by the runtime\n",(void *)pp_api);
}
void runtime_set_module_tls(uint64_t module, const void *p_data, uint64_t filesz, uint64_t memsz) {
    if (module<2 || module>=TLS_MODULES) { fputs("STOP: unsupported TLS module id\n",stderr); runtime_stop(21); }
    s_tls_modules[module].p_data=p_data; s_tls_modules[module].filesz=filesz; s_tls_modules[module].memsz=memsz;
}
static ABI void *guest_tls_get_addr(const uint64_t *p_index) {
    if (!p_index || p_index[0]<2 || p_index[0]>=TLS_MODULES || !s_tls_modules[p_index[0]].p_data || p_index[1]>=s_tls_modules[p_index[0]].memsz) {
        fputs("STOP: unsupported TLS module/offset\n",stderr); runtime_stop(21);
    }
    unsigned char **pp_block=&s_tls_blocks[p_index[0]];
    if (!*pp_block) {
        *pp_block=calloc(1,s_tls_modules[p_index[0]].memsz);
        if (!*pp_block) { fputs("Cannot allocate module TLS\n",stderr); exit(1); }
        memcpy(*pp_block,s_tls_modules[p_index[0]].p_data,s_tls_modules[p_index[0]].filesz);
    }
    return *pp_block+p_index[1];
}

void runtime_start(uint64_t flags) {
    s_capabilities = flags;
    if (!(flags & 1)) return;
    if (ps_host_random_bytes(&s_stack_canary, sizeof(s_stack_canary))) {
        fputs("Cannot initialize stack canary\n", stderr); exit(1);
    }
    s_stack_canary &= ~UINT64_C(255);
}
/* The window thread and a game thread leaving through exit or _exit may both report: one prints,
 * and the other waits for it (bounded, its reports' locks are) since it ends the process after. */
void runtime_report(void) {
    static int s_state;
    int expected=0;
    if (!__atomic_compare_exchange_n(&s_state,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        for (int i=0;i<REPORT_WAIT_MS && __atomic_load_n(&s_state,__ATOMIC_ACQUIRE)!=2;++i) ps_host_time_sleep_ns(1000000,NULL);
        return;
    }
    printf("Runtime: _init_env=%zu, atexit=%zu, __cxa_atexit=%zu, registered handlers=%zu\n",
           s_c_init_calls, s_c_atexit_calls, s_c_cxa_calls, s_c_handlers);
    printf("Runtime: static guards acquired=%zu, released=%zu\n", (size_t)s_c_guards_acquired, (size_t)s_c_guards_released);
    runtime_thread_report();
    runtime_exception_report();
    runtime_mutex_report();
    runtime_rwlock_report();
    runtime_barrier_report();
    runtime_sema_report();
    runtime_eventflag_report();
    runtime_ult_report();
    runtime_content_report();
    runtime_memory_report();
    runtime_mempool_report();
    runtime_ajm_report();
    runtime_audio_report();
    runtime_audio3d_report();
    runtime_pad_report();
    runtime_mouse_report();
    runtime_keyboard_report();
    runtime_toypad_report();
    runtime_savedata_report();
    runtime_file_report();
    runtime_aio_report();
    printf("Runtime: memory operations=%zu\n", (size_t)s_c_memory_calls);
    runtime_calls_report();
    __atomic_store_n(&s_state,2,__ATOMIC_RELEASE);
}
void runtime_stop(int status) {
    psport_gpu_flush_caches(1);
    runtime_calls_report();
    runtime_debug_finish("STOP", status);
    fflush(NULL);
    ps_host_exit_at_once(status);
}
static ABI void init_env(void) {
    /* The loader checked that libc.prx's export is exactly C3 (loader_init_env_is_ret). */
    ++s_c_init_calls;
    puts("Runtime: _init_env returned (verified libc implementation: RET)");
}
static pthread_mutex_t s_handler_lock = PTHREAD_MUTEX_INITIALIZER;
static int register_handler(ExitHandler value) {
    pthread_mutex_lock(&s_handler_lock);
    if (s_c_handlers == s_c_handler_capacity) {
        size_t c_capacity = s_c_handler_capacity ? s_c_handler_capacity * 2 : 64;
        ExitHandler *p_next = c_capacity > 1024 * 1024 ? NULL : realloc(s_p_handlers, c_capacity * sizeof(*s_p_handlers));
        if (!p_next) { pthread_mutex_unlock(&s_handler_lock); return -1; }
        s_p_handlers = p_next; s_c_handler_capacity = c_capacity;
    }
    value.is_active = 1; s_p_handlers[s_c_handlers++] = value;
    pthread_mutex_unlock(&s_handler_lock);
    return 0;
}
static ABI int guest_atexit(GuestCallback pfn_callback) {
    if (!pfn_callback) return -1;
    ++s_c_atexit_calls;
    return register_handler((ExitHandler){.callback.pfn_plain=pfn_callback});
}
static ABI int guest_cxa_atexit(void (ABI *pfn_callback)(void *), void *p_argument, void *p_dso) {
    if (!pfn_callback) return -1;
    ++s_c_cxa_calls;
    return register_handler((ExitHandler){.callback.pfn_with_arg=pfn_callback, .p_argument=p_argument, .p_dso=p_dso, .with_arg=1});
}
void runtime_finalize(void *p_dso) {
    /* Mark before invoking: repeated or recursive finalization cannot run twice.
       Restart at the end to include handlers registered by a destructor. */
    for (;;) {
        pthread_mutex_lock(&s_handler_lock);
        size_t i = s_c_handlers;
        while (i && (!s_p_handlers[i-1].is_active || (p_dso && s_p_handlers[i-1].p_dso != p_dso))) --i;
        if (!i) { pthread_mutex_unlock(&s_handler_lock); return; }
        ExitHandler handler = s_p_handlers[i-1]; s_p_handlers[i-1].is_active = 0;
        pthread_mutex_unlock(&s_handler_lock);
        if (handler.with_arg) handler.callback.pfn_with_arg(handler.p_argument);
        else handler.callback.pfn_plain();
    }
}
static ABI void guest_finalize(void *p_dso) { runtime_finalize(p_dso); }
/* ELF's 64-bit program header, the guest's own format whatever the host's executables are. */
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align; } GuestPhdr;
enum { GUEST_PT_LOAD = 1, GUEST_PF_X = 1 };
/* FreeBSD's struct dl_phdr_info, as libc's __cxa_finalize hands it over for an object it unloads. */
typedef struct { uint64_t va_dlpi; const char *sz_dlpi_name; const GuestPhdr *p_dlpi_phdr; uint16_t dlpi_phnum; } PhdrInfo;
/* Whether the address lies in one of the object's executable segments, as FreeBSD's libc answers it. */
static ABI int32_t guest_phdr_match_address(const PhdrInfo *p_info, const void *p_code) {
    const uintptr_t va_code=(uintptr_t)p_code;
    for (uint16_t i=0;i<p_info->dlpi_phnum;++i) {
        const GuestPhdr *p_segment=&p_info->p_dlpi_phdr[i];
        if (p_segment->p_type!=GUEST_PT_LOAD || !(p_segment->p_flags&GUEST_PF_X)) continue;
        const uintptr_t va_start=p_info->va_dlpi+p_segment->p_vaddr;
        if (va_start<=va_code && va_code+sizeof(p_code)<va_start+p_segment->p_memsz) return 1;
    }
    return 0;
}
/* libthr drops the unloading object's pthread_atfork handlers here; the runtime keeps none (it has
 * no fork), so there is nothing to drop. */
static ABI void guest_pthread_cxa_finalize(const PhdrInfo *p_info) { (void)p_info; }
static _Atomic uint32_t s_next_guard_owner=1;
static _Thread_local uint32_t s_guard_owner;
static ABI int guard_acquire(uint64_t *p_guard) {
    _Atomic uint64_t *p_state = (_Atomic uint64_t *)p_guard;
    /* Dump libc uses the low byte for completion and the dword at +4 as lock.
       The lock word holds a per-thread owner tag so recursion is detectable. */
    if (!s_guard_owner) s_guard_owner = atomic_fetch_add(&s_next_guard_owner, 1);
    for (;;) {
        uint64_t expected = 0;
        if (atomic_load_explicit(p_state, memory_order_acquire) & 1) return 0;
        if (atomic_compare_exchange_strong_explicit(p_state, &expected, (uint64_t)s_guard_owner << 32,
                                                    memory_order_acq_rel, memory_order_acquire)) break;
        if (expected & 1) return 0;
        if ((uint32_t)(expected >> 32) == s_guard_owner) {
            fputs("STOP: recursive/concurrent static initialization is not supported yet\n", stderr);
            runtime_stop(21);
        }
        ps_host_thread_yield(); /* another thread is running the initializer */
    }
    atomic_fetch_add(&s_c_guards_acquired, 1);
    return 1;
}
static ABI void guard_release(uint64_t *p_guard) {
    atomic_store_explicit((_Atomic uint64_t *)p_guard, 1, memory_order_release);
    atomic_fetch_add(&s_c_guards_released, 1);
}
static ABI void guard_abort(uint64_t *p_guard) {
    atomic_fetch_and_explicit((_Atomic uint64_t *)p_guard, UINT64_C(0xffffffff), memory_order_release);
}
static ABI __attribute__((noreturn)) void stack_fail(void) {
    fputs("STOP: guest stack protector detected corruption\n", stderr);
    runtime_stop(22);
}
/* A source inside the destination is read after part of it was written: its pages' GPU data must
 * be read back first, so such a copy is not announced as an overwrite. */
static void prepare_copy(void *p_dst, const void *p_src, size_t cb_copy) {
    const uintptr_t va_dst=(uintptr_t)p_dst, va_src=(uintptr_t)p_src;
    if (va_src>=va_dst+cb_copy || va_dst>=va_src+cb_copy) runtime_memory_prepare_overwrite(va_dst, cb_copy);
    runtime_memory_prepare_cpu_write(va_dst, cb_copy);
}
static ABI void *guest_memset(void *p_dst, int value, size_t cb_fill) {
    atomic_fetch_add_explicit(&s_c_memory_calls,1,memory_order_relaxed);
    runtime_memory_prepare_overwrite((uintptr_t)p_dst, cb_fill);
    runtime_memory_prepare_cpu_write((uintptr_t)p_dst, cb_fill);
    memset(p_dst, value, cb_fill); runtime_memory_note_cpu_write((uintptr_t)p_dst, cb_fill); return p_dst;
}
static ABI void *guest_memcpy(void *p_dst, const void *p_src, size_t cb_copy) {
    atomic_fetch_add_explicit(&s_c_memory_calls,1,memory_order_relaxed);
    prepare_copy(p_dst, p_src, cb_copy);
    memcpy(p_dst, p_src, cb_copy); runtime_memory_note_cpu_write((uintptr_t)p_dst, cb_copy); return p_dst;
}
static ABI void *guest_memmove(void *p_dst, const void *p_src, size_t cb_copy) {
    atomic_fetch_add_explicit(&s_c_memory_calls,1,memory_order_relaxed);
    prepare_copy(p_dst, p_src, cb_copy);
    memmove(p_dst, p_src, cb_copy); runtime_memory_note_cpu_write((uintptr_t)p_dst, cb_copy); return p_dst;
}
static ABI int guest_memcmp(const void *p_a, const void *p_b, size_t cb_compare) {
    atomic_fetch_add_explicit(&s_c_memory_calls,1,memory_order_relaxed); return memcmp(p_a, p_b, cb_compare);
}
static ABI size_t guest_strlen(const char *sz_text) { return strlen(sz_text); }
static ABI __attribute__((noreturn)) void guest_libc_exit(int status) {
    printf("Runtime: guest requested exit(%d)\n", status);
    runtime_finalize(NULL);
    runtime_report();
    psport_gpu_flush_caches(1);
    exit(status);
}
/* Imports the runtime answers itself ahead of the libraries (the game's libc wrappers, TLS, its
 * process parameters), whichever library the executable names for them. */
static int32_t need_libc_internal = 1; /* SDK marker variable referenced by Fios2 */
/* The command line the eboot's entry receives (main.c): its path alone, as the console starts an
 * application, or the executable and arguments sceSystemServiceLoadExec named; __progname is its
 * last component, as FreeBSD's crt sets it. */
static char *s_default_arguments[] = {"/app0/eboot.bin", NULL};
static char **s_pp_arguments = s_default_arguments;
static int s_c_arguments = 1;
static const char *s_sz_progname = "eboot.bin";
void runtime_set_arguments(char **pp_arguments, int c_arguments) {
    s_pp_arguments = pp_arguments;
    s_c_arguments = c_arguments;
    const char *sz_slash = strrchr(pp_arguments[0], '/');
    s_sz_progname = sz_slash ? sz_slash + 1 : pp_arguments[0];
}
static ABI int32_t guest_getargc(void) { return s_c_arguments; }
static ABI char **guest_getargv(void) { return s_pp_arguments; }
static const RuntimeExport s_own_data[]={
    {"__stack_chk_guard",&s_stack_canary}, {"Need_sceLibcInternal",&need_libc_internal},
    {"__progname",&s_sz_progname},
};
static const RuntimeExport s_own_functions[]={
    {"_init_env",init_env}, {"atexit",guest_atexit}, {"__cxa_atexit",guest_cxa_atexit}, {"exit",guest_libc_exit},
    {"__cxa_finalize",guest_finalize}, {"__cxa_guard_acquire",guard_acquire}, {"__cxa_guard_release",guard_release},
    {"__cxa_guard_abort",guard_abort}, {"__stack_chk_fail",stack_fail}, {"memset",guest_memset},
    {"memcpy",guest_memcpy}, {"memmove",guest_memmove}, {"memcmp",guest_memcmp}, {"strlen",guest_strlen},
    {"__tls_get_addr",guest_tls_get_addr}, {"sceKernelGetProcParam",guest_procparam},
    {"_sceKernelRtldSetApplicationHeapAPI",guest_set_heap_api},
    {"getargc",guest_getargc}, {"getargv",guest_getargv},
    {"__elf_phdr_match_addr",guest_phdr_match_address}, {"__pthread_cxa_finalize",guest_pthread_cxa_finalize},
};
uintptr_t runtime_resolve(const char *sz_name, int is_data) {
    if (!(s_capabilities & 1)) return 0;
    if (is_data) return RUNTIME_LOOKUP(s_own_data,sz_name);
    uintptr_t own = RUNTIME_LOOKUP(s_own_functions,sz_name);
    if (own) return own;
    return runtime_library_resolve(sz_name);
}
