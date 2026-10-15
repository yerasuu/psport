/* Narrow, explicit PS4 libc contracts. No automatic success stubs. */
#define _CRT_RAND_S
#include "runtime.h"
#include "gpu/psport_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <elf.h>

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
static void **s_pp_application_heap_api;
static ABI void guest_set_heap_api(void **pp_api) {
    s_pp_application_heap_api=pp_api;
    puts("Runtime: application heap API registered");
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
#ifdef _WIN32
    unsigned int halves[2];
    if (rand_s(&halves[0]) || rand_s(&halves[1])) { fputs("Cannot initialize stack canary\n", stderr); exit(1); }
    memcpy(&stack_canary, halves, sizeof(stack_canary));
#else
    FILE *p_random = fopen("/dev/urandom", "rb");
    if (!p_random || fread(&s_stack_canary, sizeof(s_stack_canary), 1, p_random) != 1) {
        fputs("Cannot initialize stack canary\n", stderr); exit(1);
    }
    fclose(p_random);
#endif
    s_stack_canary &= ~UINT64_C(255);
}
void runtime_report(void) {
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
    runtime_content_report();
    runtime_memory_report();
    runtime_mempool_report();
    runtime_ajm_report();
    runtime_audio_report();
    runtime_audio3d_report();
    runtime_pad_report();
    runtime_mouse_report();
    runtime_keyboard_report();
    runtime_savedata_report();
    runtime_file_report();
    runtime_aio_report();
    printf("Runtime: memory operations=%zu\n", (size_t)s_c_memory_calls);
    runtime_calls_report();
}
void runtime_stop(int status) {
    runtime_calls_report();
    fflush(NULL);
    _exit(status);
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
/* FreeBSD's struct dl_phdr_info, as libc's __cxa_finalize hands it over for an object it unloads. */
typedef struct { uint64_t va_dlpi; const char *sz_dlpi_name; const Elf64_Phdr *p_dlpi_phdr; uint16_t dlpi_phnum; } PhdrInfo;
/* Whether the address lies in one of the object's executable segments, as FreeBSD's libc answers it. */
static ABI int32_t guest_phdr_match_address(const PhdrInfo *p_info, const void *p_code) {
    const uintptr_t va_code=(uintptr_t)p_code;
    for (uint16_t i=0;i<p_info->dlpi_phnum;++i) {
        const Elf64_Phdr *p_segment=&p_info->p_dlpi_phdr[i];
        if (p_segment->p_type!=PT_LOAD || !(p_segment->p_flags&PF_X)) continue;
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
        sched_yield(); /* another thread is running the initializer */
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
static ABI void *guest_memset(void *p_dst, int value, size_t cb_fill) {
    atomic_fetch_add_explicit(&s_c_memory_calls,1,memory_order_relaxed);
    runtime_memory_prepare_cpu_write((uintptr_t)p_dst, cb_fill);
    memset(p_dst, value, cb_fill); runtime_memory_note_cpu_write((uintptr_t)p_dst, cb_fill); return p_dst;
}
static ABI void *guest_memcpy(void *p_dst, const void *p_src, size_t cb_copy) {
    atomic_fetch_add_explicit(&s_c_memory_calls,1,memory_order_relaxed);
    runtime_memory_prepare_cpu_write((uintptr_t)p_dst, cb_copy);
    memcpy(p_dst, p_src, cb_copy); runtime_memory_note_cpu_write((uintptr_t)p_dst, cb_copy); return p_dst;
}
static ABI void *guest_memmove(void *p_dst, const void *p_src, size_t cb_copy) {
    atomic_fetch_add_explicit(&s_c_memory_calls,1,memory_order_relaxed);
    runtime_memory_prepare_cpu_write((uintptr_t)p_dst, cb_copy);
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
    exit(status);
}
/* Imports the runtime answers itself ahead of the libraries (the game's libc wrappers, TLS, its
 * process parameters), whichever library the executable names for them. */
static int32_t need_libc_internal = 1; /* SDK marker variable referenced by Fios2 */
/* The command line the eboot's entry receives (probe.c): its path alone, as the console starts an
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
    uintptr_t mutex = runtime_mutex_resolve(sz_name);
    if (mutex) return mutex;
    uintptr_t thread = runtime_thread_resolve(sz_name);
    if (thread) return thread;
    uintptr_t sched = runtime_sched_resolve(sz_name);
    if (sched) return sched;
    uintptr_t content=runtime_content_resolve(sz_name);
    if (content) return content;
    uintptr_t sema = runtime_sema_resolve(sz_name);
    if (sema) return sema;
    uintptr_t eventflag = runtime_eventflag_resolve(sz_name);
    if (eventflag) return eventflag;
    uintptr_t ulobjmgr = runtime_ulobjmgr_resolve(sz_name);
    if (ulobjmgr) return ulobjmgr;
    uintptr_t ult = runtime_ult_resolve(sz_name);
    if (ult) return ult;
    uintptr_t rwlock = runtime_rwlock_resolve(sz_name);
    if (rwlock) return rwlock;
    uintptr_t va_barrier = runtime_barrier_resolve(sz_name);
    if (va_barrier) return va_barrier;
    uintptr_t memory = runtime_memory_resolve(sz_name);
    if (memory) return memory;
    uintptr_t mempool = runtime_mempool_resolve(sz_name);
    if (mempool) return mempool;
    uintptr_t exception = runtime_exception_resolve(sz_name);
    if (exception) return exception;
    uintptr_t kernel = runtime_kernel_resolve(sz_name);
    if (kernel) return kernel;
    uintptr_t loader = runtime_module_resolve(sz_name);
    if (loader) return loader;
    uintptr_t playgo = runtime_playgo_resolve(sz_name);
    if (playgo) return playgo;
    uintptr_t voice = runtime_voice_resolve(sz_name);
    if (voice) return voice;
    uintptr_t services = runtime_services_resolve(sz_name);
    if (services) return services;
    uintptr_t ajm = runtime_ajm_resolve(sz_name);
    if (ajm) return ajm;
    uintptr_t audio = runtime_audio_resolve(sz_name);
    if (audio) return audio;
    uintptr_t audio3d = runtime_audio3d_resolve(sz_name);
    if (audio3d) return audio3d;
    uintptr_t pad = runtime_pad_resolve(sz_name);
    if (pad) return pad;
    uintptr_t gesture = runtime_gesture_resolve(sz_name);
    if (gesture) return gesture;
    uintptr_t va_devices = runtime_devices_resolve(sz_name);
    if (va_devices) return va_devices;
    uintptr_t va_capture = runtime_capture_resolve(sz_name);
    if (va_capture) return va_capture;
    uintptr_t va_vr = runtime_vr_resolve(sz_name);
    if (va_vr) return va_vr;
    uintptr_t mouse = runtime_mouse_resolve(sz_name);
    if (mouse) return mouse;
    uintptr_t keyboard = runtime_keyboard_resolve(sz_name);
    if (keyboard) return keyboard;
    uintptr_t rtc = runtime_rtc_resolve(sz_name);
    if (rtc) return rtc;
    uintptr_t save = runtime_savedata_resolve(sz_name);
    if (save) return save;
    uintptr_t file = runtime_file_resolve(sz_name);
    if (file) return file;
    uintptr_t aio = runtime_aio_resolve(sz_name);
    if (aio) return aio;
    return psport_gpu_resolve(sz_name);
}
/* The NIDs before any "#<library>#<module>" scope are equal. */
int runtime_same_nid(const char *sz_a, const char *sz_b) {
    while (*sz_a && *sz_a!='#' && *sz_a==*sz_b) ++sz_a, ++sz_b;
    return (!*sz_a || *sz_a=='#') && (!*sz_b || *sz_b=='#');
}
/* Symbol name for a NID in any scope, or NULL when no listed title imports it. */
static const struct { char sz_nid[12]; const char *sz_symbol; } s_all_names[]={
#include "import_names_all.inc"
};
const char *runtime_symbol(const char *sz_nid) {
    size_t low=0, high=sizeof(s_all_names)/sizeof(*s_all_names);
    while (low<high) {
        size_t mid=low+(high-low)/2;
        int order=strncmp(sz_nid,s_all_names[mid].sz_nid,11);
        if (!order) return s_all_names[mid].sz_symbol;
        if (order<0) high=mid; else low=mid+1;
    }
    /* Exports aerolib does not name, called as shadPS4 calls them. */
    static const struct { const char *sz_nid, *sz_symbol; } s_extra[]={
        {"BG26hBGiNlw","_sceUlobjmgrRegisterObject"}, {"Smf+fUNblPc","_sceUlobjmgrUnregisterObject"},
        {"HZ9Q2c+4BU4","ulobjmgr Func_1D9F50D9CFB8054E"}, {"SweJO7t3pkk","ulobjmgr Func_4B07893BBB77A649"},
        {"fJgP+wqifno","sceDiscMap_7C980FFB0AA27E7A"}, {"ioKMruft1ek","sceDiscMap_8A828CAEE7EDD5E9"},
    };
    for (size_t i=0;i<sizeof(s_extra)/sizeof(*s_extra);++i)
        if (runtime_same_nid(sz_nid,s_extra[i].sz_nid)) return s_extra[i].sz_symbol;
    return NULL;
}
/* Same symbol name means same contract whether imported from libkernel or
 * libScePosix, so newer modules resolve by name through runtime_symbol. */
/* Arguments are ignored: under the System V ABI the caller removes them, so one function stands
 * for every signature returning a status. */
ABI int32_t runtime_stub(void) { return 0; }
int g_runtime_last_stub;
uintptr_t runtime_lookup(const RuntimeExport *p_table,size_t c_exports,const char *sz_nid) {
    const char *sz_symbol=runtime_symbol(sz_nid);
    if (!sz_symbol) return 0;
    for (size_t i=0;i<c_exports;++i)
        if (!strcmp(sz_symbol,p_table[i].sz_name)) {
            g_runtime_last_stub=p_table[i].p_function==(void *)runtime_stub;
            return (uintptr_t)p_table[i].p_function;
        }
    return 0;
}
const char *runtime_import_name(const char *sz_name) {
    const char *sz_symbol=runtime_symbol(sz_name);
    return sz_symbol ? sz_symbol : "name not resolved";
}
static const struct { char sz_nid[12]; const char *sz_function; unsigned char outputs; } s_output_arguments[]={
#include "import_outparams.inc"
};
unsigned runtime_import_outputs(const char *sz_nid) {
    size_t low=0, high=sizeof(s_output_arguments)/sizeof(*s_output_arguments);
    while (low<high) {
        size_t mid=low+(high-low)/2;
        int order=strncmp(sz_nid,s_output_arguments[mid].sz_nid,11);
        if (!order) return s_output_arguments[mid].outputs;
        if (order<0) high=mid; else low=mid+1;
    }
    return 0;
}
static const struct { char sz_nid[12]; unsigned char kind; } s_failure_kinds[]={
#include "import_returns.inc"
};
unsigned runtime_import_failure(const char *sz_nid) {
    size_t low=0, high=sizeof(s_failure_kinds)/sizeof(*s_failure_kinds);
    while (low<high) {
        size_t mid=low+(high-low)/2;
        int order=strncmp(sz_nid,s_failure_kinds[mid].sz_nid,11);
        if (!order) return s_failure_kinds[mid].kind;
        if (order<0) high=mid; else low=mid+1;
    }
    return 0;
}
static const struct { char sz_nid[12]; const char *sz_function; uint32_t code; } s_status_results[]={
#include "import_statuses.inc"
};
int runtime_import_status(const char *sz_nid, uint32_t code) {
    size_t low=0, high=sizeof(s_status_results)/sizeof(*s_status_results);
    while (low<high) {
        size_t mid=low+(high-low)/2;
        int order=strncmp(sz_nid,s_status_results[mid].sz_nid,11);
        if (!order) order=code<s_status_results[mid].code ? -1 : code>s_status_results[mid].code;
        if (!order) return 1;
        if (order<0) high=mid; else low=mid+1;
    }
    return 0;
}
