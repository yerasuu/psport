/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <cpuid.h>
#include "runtime.h"
#include "loader.h"
#include "gpu/psport_gpu.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

typedef LoaderSegment Segment;
typedef LoaderReloc Reloc;
static char (*s_p_names)[128];
static uint64_t s_c_imports;
static unsigned char *s_p_image;
static size_t s_cb_page;
/* s_p_all_modules[0] describes the eboot; modules are the linked ones after it. */
static RuntimeModule *s_p_all_modules, *s_p_modules;
static uint64_t s_c_modules;
static int s_has_entered_game;
static int s_is_gpu_enabled;
int vulkan_smoke(void);

static void fail(const char *sz_message) { fprintf(stderr, "ERROR: %s\n", sz_message); exit(1); }

/* The game's code was compiled for the PS4's CPU (AMD Jaguar) and runs as it is: AVX, BMI1
 * (andn/bextr/blsr/tzcnt), MOVBE, LZCNT and POPCNT, thousands of each in eboot.bin. A CPU without
 * them stops at the first one with SIGILL (exit code 132, issue #26), and lzcnt/tzcnt even run as
 * bsr/bsf there, with other results. Said before the game starts; PS_SKIP_CPU_CHECK=1 skips it. */
static void check_cpu(void) {
    const char *sz_skip = getenv("PS_SKIP_CPU_CHECK");
    if (sz_skip && !strcmp(sz_skip, "1")) return;
    unsigned a, b, c, d, leaf1_c = 0, leaf7_b = 0, ext1_c = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) leaf1_c = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) leaf7_b = b;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d)) ext1_c = c;
    int avx = (leaf1_c & bit_AVX) && (leaf1_c & bit_OSXSAVE);
    if (avx) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        avx = (lo & 6) == 6; /* the OS saves SSE and AVX state */
    }
    const struct { int is_present; const char *sz_name; } features[] = {
        {avx, "AVX"}, {(leaf7_b & bit_BMI) != 0, "BMI1"}, {(leaf1_c & bit_MOVBE) != 0, "MOVBE"},
        {(ext1_c & bit_LZCNT) != 0, "LZCNT"}, {(leaf1_c & bit_POPCNT) != 0, "POPCNT"},
        {(leaf1_c & bit_SSE4_2) != 0, "SSE4.2"},
    };
    char sz_missing[64] = "";
    for (unsigned i = 0; i < sizeof(features) / sizeof(features[0]); ++i) {
        if (features[i].is_present) continue;
        if (sz_missing[0]) strcat(sz_missing, ", ");
        strcat(sz_missing, features[i].sz_name);
    }
    if (!sz_missing[0]) return;
    fprintf(stderr,
            "ERROR: this CPU lacks %s. The game's code was compiled for the PS4's CPU and uses "
            "these instructions directly: it needs an Intel Haswell (4th generation Core, 2013) or "
            "newer, or an AMD Ryzen. PS_SKIP_CPU_CHECK=1 starts anyway.\n", sz_missing);
    exit(1);
}
static size_t round_page(size_t cb_page) { return (cb_page + s_cb_page - 1) & ~(s_cb_page - 1); }
static void *allocate(size_t cb_allocation) {
#ifndef _WIN32
    void *p_low=runtime_low_map(cb_allocation,PROT_READ|PROT_WRITE);
    if (p_low) return p_low;
#endif
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p_mapping = mmap(NULL, cb_allocation, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p_mapping == MAP_FAILED) fail("mmap failed");
#endif
    return p_mapping;
}
static void protect(void *p_range, size_t cb_range, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p_range, cb_range, mode)) fail("mprotect failed");
#endif
}
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
    if (id >= s_c_imports) return 0;
    runtime_calls_unimplemented(id);
    const unsigned outputs = s_p_permissive_outputs[id];
    const int32_t result = outputs && s_is_permissive_out_error ? (int32_t)0x80020016 : 0;
    if (!__atomic_exchange_n(&s_p_permissive_seen[id], 1, __ATOMIC_RELAXED)) {
        if (!outputs) {
            printf("Runtime: unsupported import %s (%s) returns 0; first argument: 0x%" PRIxPTR "\n",
                   s_p_names[id], runtime_import_name(s_p_names[id]), a0);
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
               s_p_names[id], runtime_import_name(s_p_names[id]), (unsigned)result, outputs & (outputs - 1) ? "s" : "",
               sz_list);
    }
    return result;
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= s_c_imports) fail("bad import trap index");
    runtime_calls_unimplemented(id);
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", s_p_names[id], id);
    printf("API: %s\n", runtime_import_name(s_p_names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)s_p_image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)s_p_image;
    for (uint64_t m=0;m<s_c_modules;++m)
        if (caller>=s_p_modules[m].base && caller-s_p_modules[m].base<s_p_modules[m].cb_module)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,s_p_modules[m].sz_name,caller-s_p_modules[m].base);
    runtime_report();
    puts(s_has_entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
#ifndef _WIN32
/* enter_on_stack(p_entry, p_arg0, p_arg1, p_top): call p_entry(p_arg0,p_arg1) on a new stack. */
void enter_on_stack(void *p_entry, void *p_arg0, void *p_arg1, void *p_top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
/* An import bound to the runtime's or the GPU library's own function whose result tells a failure
 * (runtime_import_failure) is called through one of these: it keeps the six argument registers on
 * its stack, calls the function and hands a result that looks like a failure, with them, to
 * runtime_calls_failed. Arguments on the stack would move, so those imports are not listed. */
enum { THUNK_SIZE = 96 };
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
#ifndef _WIN32
/* Termination signals SDL does not take (--cpu-only): the call summary, then the default action. */
static void shutdown_signal(int sig) {
    runtime_calls_report();
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif
#ifndef _WIN32
static void dump_frames(ucontext_t *p_ucontext);
static void fault(int sig, siginfo_t *p_info, void *p_context) {
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (s_is_gpu_enabled && sig == SIGSEGV && psport_gpu_handle_fault(p_context, p_info->si_addr)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *p_recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        pthread_sigmask(SIG_SETMASK, &((ucontext_t *)p_context)->uc_sigmask, NULL);
        siglongjmp(*p_recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *p_ucontext = p_context;
    uintptr_t rip = (uintptr_t)p_ucontext->uc_mcontext.gregs[REG_RIP];
    char sz_line[512];
    Dl_info where;
    if (rip - (uintptr_t)s_p_image < 0x10000000)
        snprintf(sz_line, sizeof(sz_line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)s_p_image), p_info->si_addr);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(sz_line, sizeof(sz_line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", p_info->si_addr);
    else
        snprintf(sz_line, sizeof(sz_line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, p_info->si_addr);
    { ssize_t written_=write(2, sz_line, strlen(sz_line)); (void)written_; }
    if (s_is_gpu_enabled) psport_gpu_dump_guest_writes(p_context);
    /* Guest code keeps frame pointers: its call chain says which caller handed over the bad
     * value, where the host backtrace below stops at the first guest frame. */
    if (rip - (uintptr_t)s_p_image < 0x10000000) dump_frames(p_ucontext);
    /* A call through a null or stale pointer (an import PS_PERMISSIVE answered with 0, a symbol
     * lookup that failed): the return address on the stack names the guest caller. */
    else if (!(dladdr((void *)rip, &where) && where.dli_fname)) {
        uintptr_t caller = 0;
        struct iovec local = {&caller, sizeof(caller)}, remote = {(void *)p_ucontext->uc_mcontext.gregs[REG_RSP], sizeof(caller)};
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)sizeof(caller) &&
            caller - (uintptr_t)s_p_image < 0x10000000) {
            snprintf(sz_line, sizeof(sz_line), "  called from guest offset 0x%lx\n", (unsigned long)(caller - (uintptr_t)s_p_image));
            { ssize_t written_=write(2, sz_line, strlen(sz_line)); (void)written_; }
            p_ucontext->uc_mcontext.gregs[REG_RIP] = (greg_t)caller;
            dump_frames(p_ucontext);
        }
    }
    /* Outside the image and any shared object (generated code, a freed mapping): the mapping
     * from /proc/self/maps, and the thread. */
    if (rip - (uintptr_t)s_p_image >= 0x10000000 && !(dladdr((void *)rip, &where) && where.dli_fname)) {
        char sz_thread[32] = "?";
        pthread_getname_np(pthread_self(), sz_thread, sizeof(sz_thread));
        snprintf(sz_line, sizeof(sz_line), "  thread %s; mapping of RIP: ", sz_thread);
        { ssize_t written_=write(2, sz_line, strlen(sz_line)); (void)written_; }
        FILE *p_maps = fopen("/proc/self/maps", "r");
        int is_found = 0;
        while (p_maps && fgets(sz_line, sizeof(sz_line), p_maps)) {
            unsigned long from, to;
            if (sscanf(sz_line, "%lx-%lx", &from, &to) == 2 && rip >= from && rip < to) {
                ssize_t written_=write(2, sz_line, strlen(sz_line)); (void)written_;
                is_found = 1;
                break;
            }
        }
        if (p_maps) fclose(p_maps);
        if (!is_found) { ssize_t written_=write(2, "none\n", 5); (void)written_; }
    }
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(sz_line, sizeof(sz_line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(sz_line, sizeof(sz_line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, sz_line, strlen(sz_line)); (void)written_;
    }
    runtime_calls_report();
    _exit(128 + sig);
}
#endif
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t s_exe_base;
static void write_hex(char *sz_out, uint64_t v) {
    const char sz_digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) sz_out[i] = sz_digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *p_ucontext) {
    char sz_line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)p_ucontext->uc_mcontext.gregs[REG_RIP], rbp=(uintptr_t)p_ucontext->uc_mcontext.gregs[REG_RBP];
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char sz_arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(sz_arg+6,tid); write_hex(sz_arg+27,(uint64_t)p_ucontext->uc_mcontext.gregs[REG_RDI]);
    { ssize_t written_=write(2,sz_arg,sizeof(sz_arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(sz_line+6,tid); write_hex(sz_line+27,rip); write_hex(sz_line+59,rip-(uintptr_t)s_p_image); write_hex(sz_line+90,rip-s_exe_base);
        { ssize_t written_=write(2,sz_line,sizeof(sz_line)-1); (void)written_; }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *p_info, void *p_context) { (void)sig; (void)p_info; dump_frames(p_context); }
static void watchdog(int sig, siginfo_t *p_info, void *p_context) {
    (void)p_info;
    const char sz_head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,sz_head,sizeof(sz_head)-1); (void)written_; }
    dump_frames(p_context);
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char sz_buffer[4096];
    long cb_read;
    pid_t self=gettid();
    while (dir>=0 && (cb_read=syscall(SYS_getdents64,dir,sz_buffer,sizeof(sz_buffer)))>0)
        for (long at=0; at<cb_read;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char sz_name[]; } *p_entry=(void *)(sz_buffer+at);
            pid_t tid=(pid_t)strtol(p_entry->sz_name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=p_entry->reclen;
        }
    usleep(100000);
    runtime_calls_report();
    _exit(128 + sig);
}
/* param.sfo lookup: string or integer value of key; the entry's format (0x0404 an integer), 0 when absent. */
static int sfo_value(const char *sz_path, const char *sz_key, char *sz_text, size_t cb_text, uint32_t *p_number) {
    FILE *p_file=fopen(sz_path,"rb");
    if (!p_file) return 0;
    unsigned char data[65536];
    size_t cb_read=fread(data,1,sizeof(data),p_file); fclose(p_file);
    if (cb_read<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, c_entries;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&c_entries,data+16,4);
    for (uint32_t i=0;i<c_entries && 20+i*16+16<=cb_read;++i) {
        const unsigned char *p_entry=data+20+i*16;
        uint16_t off_key, format; uint32_t length, off_data;
        memcpy(&off_key,p_entry,2); memcpy(&format,p_entry+2,2); memcpy(&length,p_entry+4,4); memcpy(&off_data,p_entry+12,4);
        if (keys+off_key>=cb_read || values+off_data+length>cb_read || strcmp((const char *)data+keys+off_key,sz_key)) continue;
        if (format==0x0404 && p_number && length>=4) { memcpy(p_number,data+values+off_data,4); return format; }
        if (sz_text && cb_text) {
            size_t copy=length<cb_text-1 ? length : cb_text-1;
            memcpy(sz_text,data+values+off_data,copy); sz_text[copy]=0;
        }
        return format ? format : 1;
    }
    return 0;
}
/* AppContent's base-game profile: param.sfo's user-defined parameters; the SKU is a launch setting
 * (PS_CONTENT_SKU=full or trial), not a license check. */
static void configure_content(const char *sz_sfo) {
    const char *sz_sku=getenv("PS_CONTENT_SKU");
    if (!sz_sku || !*sz_sku) sz_sku="full";
    uint32_t values[5]={0};
    if (!strcmp(sz_sku,"full")) values[0]=3;
    else if (!strcmp(sz_sku,"trial")) values[0]=1;
    else fail("PS_CONTENT_SKU must be full or trial");
    FILE *p_file=fopen(sz_sfo,"rb");
    if (!p_file) { fprintf(stderr,"ERROR: cannot read %s\n",sz_sfo); exit(1); }
    fclose(p_file);
    for (unsigned i=0;i<4;++i) {
        char sz_key[32];
        snprintf(sz_key,sizeof(sz_key),"USER_DEFINED_PARAM_%u",i+1);
        int format=sfo_value(sz_sfo,sz_key,NULL,0,&values[1+i]);
        if (format && format!=0x0404) fail("invalid user-defined parameter in param.sfo");
    }
    runtime_content_configure(values);
    printf("AppContent profile: SKU=%s (probe setting), user params=[%u, %u, %u, %u], mounted add-ons=0\n",
           sz_sku,values[1],values[2],values[3],values[4]);
}
static int mapped(Segment *p_segments, uint64_t c_segments, uint64_t va_range, uint64_t cb_range) {
    for (uint64_t i = 0; i < c_segments; ++i)
        if (va_range >= p_segments[i].va_segment && cb_range <= p_segments[i].cb_segment &&
            va_range - p_segments[i].va_segment <= p_segments[i].cb_segment - cb_range) return 1;
    return 0;
}
/* The patches' writes at image offsets, after relocation. A write may replace a whole
 * base-relative pointer slot (60/90 FPS++ swap function pointers): the patch holds the address at
 * the eboot's patch base, 0x400000, rebased here. */
static void apply_patches(const PsPatchList *p_patches, Segment *p_segments, uint64_t c_segments, const Reloc *p_relocs, uint64_t c_relocs) {
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
        memcpy(s_p_image+off_image,p_data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,s_p_image+slots[s],8);
            value=(uint64_t)(uintptr_t)s_p_image+(value-base);
            memcpy(s_p_image+slots[s],&value,8);
        }
        rebased+=nslots;
        cb_patched+=length;
    }
    printf("Patches: %zu writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",p_patches->c_writes,cb_patched,rebased);
}
/* A path under psport's own folder, the one holding out/ or bin/ (a checkout or a package). */
static void install_path(const char *sz_relative, char *sz_out, size_t cb_out) {
    char sz_exe[4096];
    ssize_t cb_exe=readlink("/proc/self/exe",sz_exe,sizeof(sz_exe)-1);
    sz_exe[cb_exe>0 ? cb_exe : 0]=0;
    for (int up=0; up<2; ++up) { char *p_slash=strrchr(sz_exe,'/'); if (p_slash) *p_slash=0; }
    if ((size_t)snprintf(sz_out,cb_out,"%s/%s",sz_exe[0] ? sz_exe : ".",sz_relative)>=cb_out) fail("install path too long");
}
/* What psport knows of the game for its profile and the external patches: param.sfo's TITLE_ID
 * and APP_VER, and the TITLE_IDs of the same executable. */
typedef struct {
    char sz_serial[16], sz_version[16], sz_patch_file[4096];
    const char *serials[64];
    size_t c_serials;
    PsGameFiles files;
} GameInfo;
/* sz_game_dir may be NULL: a launcher asking about a title by its TITLE_ID and APP_VER. */
static void game_info_of(const char *sz_game_dir, const char *sz_serial, const char *sz_version, GameInfo *p_out_info) {
    memset(p_out_info,0,sizeof(*p_out_info));
    snprintf(p_out_info->sz_serial,sizeof(p_out_info->sz_serial),"%s",sz_serial);
    snprintf(p_out_info->sz_version,sizeof(p_out_info->sz_version),"%s",sz_version);
    if (p_out_info->sz_serial[0]) p_out_info->serials[p_out_info->c_serials++]=p_out_info->sz_serial;
    for (size_t i=0; p_out_info->c_serials<64; ++i) {
        const char *sz_same=psport_gpu_game_serial(p_out_info->sz_serial,i);
        if (!sz_same) break;
        p_out_info->serials[p_out_info->c_serials++]=sz_same;
    }
    const char *sz_patch_file=psport_gpu_game_patch_file(p_out_info->sz_serial);
    if (sz_patch_file) install_path(sz_patch_file,p_out_info->sz_patch_file,sizeof(p_out_info->sz_patch_file));
    p_out_info->files=(PsGameFiles){sz_game_dir,p_out_info->sz_serial,p_out_info->sz_version,
                                    sz_patch_file ? p_out_info->sz_patch_file : NULL,loader_eboot_sha256};
}
static void game_info(const char *sz_game_dir, GameInfo *p_out_info) {
    char sz_sfo[4096], sz_serial[16]="", sz_version[16]="";
    snprintf(sz_sfo,sizeof(sz_sfo),"%s/sce_sys/param.sfo",sz_game_dir);
    sfo_value(sz_sfo,"TITLE_ID",sz_serial,sizeof(sz_serial),NULL);
    sfo_value(sz_sfo,"APP_VER",sz_version,sizeof(sz_version),NULL);
    game_info_of(sz_game_dir,sz_serial,sz_version,p_out_info);
}
/* A launcher's question names the game by its folder (--game dir) or, without one, by its title
 * (--title TITLE_ID [--version APP_VER]); the arguments from argv[first] on. 0 when they are
 * neither. *psz_out_game_dir is NULL for a title. */
static int query_game(int argc, char **pp_argv, int first, GameInfo *p_out_info, const char **psz_out_game_dir) {
    *psz_out_game_dir=NULL;
    if (argc == first) { game_info_of(NULL,"","",p_out_info); return 1; }
    if (argc == first+2 && !strcmp(pp_argv[first],"--game")) {
        *psz_out_game_dir=pp_argv[first+1];
        game_info(pp_argv[first+1],p_out_info);
        return 1;
    }
    if (strcmp(pp_argv[first],"--title") || (argc != first+2 && (argc != first+4 || strcmp(pp_argv[first+2],"--version"))))
        return 0;
    game_info_of(NULL,pp_argv[first+1],argc == first+4 ? pp_argv[first+3] : "",p_out_info);
    return 1;
}
/* The game's external patch folder for loader_patches.c: its profile's own file left out. */
static LoaderPatchFolder patch_folder(const char *sz_dir, const GameInfo *p_info) {
    return (LoaderPatchFolder){sz_dir,p_info->sz_patch_file[0] ? p_info->sz_patch_file : NULL,p_info->sz_version,
                               p_info->serials,p_info->c_serials};
}
/* The mods rules' game data: wrapper folders named after any TITLE_ID of the game's executable,
 * the folders of dvdroot_ps4 its profile knows. */
static void mod_options(const GameInfo *p_info, LoaderModOptions *p_options, const char **pp_folders, size_t c_capacity) {
    p_options->pp_serials=p_info->serials;
    p_options->c_serials=p_info->c_serials;
    p_options->pp_folders=pp_folders;
    for (p_options->c_folders=0; p_options->c_folders<c_capacity; ++p_options->c_folders) {
        pp_folders[p_options->c_folders]=psport_gpu_game_mod_folder(p_info->sz_serial,p_options->c_folders);
        if (!pp_folders[p_options->c_folders]) break;
    }
}

#ifndef _WIN32
extern char **environ;
/* bbport, and psport releases before the rename, read BB_* names: launchers written for them keep
   working for a transition. Each one set is copied to its PS_* name unless that is set too, then
   the process starts again, since the GPU library's initializers have already read the
   environment by the time main runs. Through run.sh, which copies them first, nothing is left. */
static void accept_deprecated_names(char **pp_argv) {
    size_t c_deprecated=0;
    for (char **pp_entry=environ; *pp_entry; ++pp_entry) c_deprecated+=!strncmp(*pp_entry,"BB_",3);
    if (!c_deprecated) return;
    char **pp_found=calloc(c_deprecated,sizeof(*pp_found));
    if (!pp_found) return;
    size_t c_found=0;
    for (char **pp_entry=environ; *pp_entry && c_found<c_deprecated; ++pp_entry)
        if (!strncmp(*pp_entry,"BB_",3) && strchr(*pp_entry,'=')) pp_found[c_found++]=strdup(*pp_entry);
    char sz_list[4096]="";
    size_t cb_used=0, c_copied=0;
    for (size_t i=0; i<c_found; ++i) {
        if (!pp_found[i]) continue;
        char *sz_value=strchr(pp_found[i],'=');
        *sz_value++='\0';
        char sz_renamed[256];
        if (snprintf(sz_renamed,sizeof(sz_renamed),"PS_%s",pp_found[i]+3)<(int)sizeof(sz_renamed) && !getenv(sz_renamed)) {
            setenv(sz_renamed,sz_value,0);
            int wrote=snprintf(sz_list+cb_used,sizeof(sz_list)-cb_used,"%s%s",c_copied?", ":"",pp_found[i]);
            if (wrote>0 && (size_t)wrote<sizeof(sz_list)-cb_used) cb_used+=(size_t)wrote;
            c_copied++;
        }
        free(pp_found[i]);
    }
    free(pp_found);
    if (!c_copied) return;
    printf("psport: BB_* variables are deprecated, use PS_*: %s\n",sz_list);
    execv("/proc/self/exe",pp_argv);
    perror("psport: restart with the PS_* variables");
}
#endif

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    runtime_restart_configure(argc, argv);
#ifndef _WIN32
    accept_deprecated_names(argv);
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
    if (argc == 3 && !strcmp(argv[1], "--check-game")) {
        GameInfo info;
        char sz_kind[64], sz_advice[2048];
        game_info(argv[2],&info);
        if (!psport_gpu_check_game(&info.files,sz_kind,sizeof(sz_kind),sz_advice,sizeof(sz_advice))) return 0;
        printf("%s\t%s\t%s\n",sz_kind,info.sz_serial[0] ? info.sz_serial : "?",info.sz_version[0] ? info.sz_version : "?");
        fprintf(stderr,"%s\n",sz_advice);
        return 2;
    }
    if (argc == 3 && !strcmp(argv[1], "--has-presets")) {
        GameInfo info;
        game_info(argv[2],&info);
        return psport_gpu_game_has_presets(info.sz_serial) ? 0 : 1;
    }
    const char *mod_folders[256];
    if (argc >= 3 && (!strcmp(argv[1], "--list-patches") || !strcmp(argv[1], "--list-mods"))) {
        GameInfo info;
        const char *sz_game_dir;
        int is_patches=!strcmp(argv[1], "--list-patches");
        if (!query_game(argc,argv,3,&info,&sz_game_dir) || (is_patches && argc == 3)) {
            fprintf(stderr, "Usage: %s %s dir %s(--game dir | --title TITLE_ID [--version APP_VER])%s\n", argv[0], argv[1],
                    is_patches ? "" : "[", is_patches ? "" : "]");
            return 1;
        }
        if (is_patches) {
            LoaderPatchFolder folder=patch_folder(argv[2],&info);
            loader_patches_list(&folder);
        } else {
            LoaderModOptions mods={.sz_mods_dir=argv[2]};
            mod_options(&info,&mods,mod_folders,256);
            loader_mods_list(sz_game_dir,&mods);
        }
        return 0;
    }
    check_cpu();
    int cpu_only = 0, strict_imports = 0;
    unsigned timeout_seconds = 10;
    const char *sz_app0=NULL, *sz_user_dir=NULL, *sz_patches_dir=NULL, *sz_patches_config=NULL, *sz_exec=NULL, *sz_mods_dir=NULL;
    const char *sz_mods_config=NULL, *sz_out_dir="out";
    LoaderOptions game={0};
    /* The guest's command line: the executable's guest path, the --arg values, NULL. Copied to the
     * heap, below 1 TiB, since the host stack holding argv is above it. */
    char **pp_guest_arguments = calloc((size_t)argc + 2, sizeof(*pp_guest_arguments));
    int c_guest_arguments = 1;
    if (!pp_guest_arguments) fail("allocation failed");
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--game") && i+1<argc) game.sz_game=argv[++i];
        else if (!strcmp(argv[i], "--system-modules") && i+1<argc) game.sz_system_modules=argv[++i];
        else if (!strcmp(argv[i], "--hle") && i+1<argc) game.sz_hle=argv[++i];
        else if (!strcmp(argv[i], "--lle") && i+1<argc) game.sz_lle=argv[++i];
        else if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--app0") && i+1<argc) sz_app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) sz_user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches-dir") && i+1<argc) sz_patches_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches-config") && i+1<argc) sz_patches_config=argv[++i];
        else if (!strcmp(argv[i], "--mods") && i+1<argc) sz_mods_dir=argv[++i];
        else if (!strcmp(argv[i], "--mods-config") && i+1<argc) sz_mods_config=argv[++i];
        else if (!strcmp(argv[i], "--out") && i+1<argc) sz_out_dir=argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else if (!strcmp(argv[i], "--exec") && i+1<argc) sz_exec=strdup(argv[++i]);
        else if (!strcmp(argv[i], "--arg") && i+1<argc) pp_guest_arguments[c_guest_arguments++]=strdup(argv[++i]);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (!game.sz_game) {
        fprintf(stderr, "Usage: %s --game dir [--system-modules dir] [--hle libs] [--lle libs] [--cpu-only] [--strict-imports] [--app0 dir] [--user dir] [--patches-dir dir [--patches-config file]] [--mods dir [--mods-config file]] [--out dir] [--timeout seconds] [--exec guest-path [--arg value]...] | --check-game dir | --has-presets dir | --list-patches dir (--game dir | --title id [--version v]) | --list-mods dir [--game dir | --title id] | --vulkan-only\n", argv[0]);
        return 1;
    }
    if (sz_mods_dir) {
        GameInfo original;
        game_info(game.sz_game,&original);
        LoaderModOptions mods={.sz_mods_dir=sz_mods_dir,.sz_config=sz_mods_config,.sz_out_dir=sz_out_dir};
        mod_options(&original,&mods,mod_folders,256);
        char *sz_view=loader_mods_view(game.sz_game,&mods);
        if (sz_app0 && !strcmp(sz_app0,game.sz_game)) sz_app0=sz_view;
        game.sz_game=sz_view;
    }
    if (sz_app0) {
        runtime_file_configure(sz_app0, sz_user_dir ? sz_user_dir : "user");
        char sz_sfo[4096], sz_title_id[16]="";
        snprintf(sz_sfo,sizeof(sz_sfo),"%s/sce_sys/param.sfo",sz_app0);
        configure_content(sz_sfo);
        if (sfo_value(sz_sfo,"INSTALL_DIR_SAVEDATA",sz_title_id,sizeof(sz_title_id),NULL) || sfo_value(sz_sfo,"TITLE_ID",sz_title_id,sizeof(sz_title_id),NULL))
            runtime_savedata_configure(sz_title_id);
        char sz_serial[16]="", sz_title[128]="", sz_version[16]="";
        sfo_value(sz_sfo,"TITLE_ID",sz_serial,sizeof(sz_serial),NULL);
        sfo_value(sz_sfo,"TITLE",sz_title,sizeof(sz_title),NULL);
        sfo_value(sz_sfo,"APP_VER",sz_version,sizeof(sz_version),NULL);
        runtime_net_configure(sz_app0,sz_serial,sz_title,sz_version);
        runtime_content_title(sz_serial);
    }
    pp_guest_arguments[0] = (char *)(sz_exec ? sz_exec : "/app0/eboot.bin");
    char sz_executable[4096];
    if (sz_exec) {
        if (!sz_app0 || runtime_file_translate(sz_exec, sz_executable, sizeof(sz_executable))) fail("--exec names no file of the package");
        game.sz_executable = sz_executable;
        printf("Runtime: main executable %s (sceSystemServiceLoadExec), %d arguments\n", sz_exec, c_guest_arguments - 1);
    }
    runtime_set_arguments(pp_guest_arguments, c_guest_arguments);
    psport_gpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
#else
    s_cb_page = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask); sigaddset(&sa.sa_mask, RUNTIME_EXCEPTION_SIGNAL);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) s_exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
#endif
    LoaderImage boot_image;
    loader_load_game(&game,&boot_image);
    /* The game's profile and its files, then the patches, all before any of its code runs or a
     * window opens: a launch that cannot work stops here, with why. */
    GameInfo info;
    game_info(game.sz_game,&info);
    psport_gpu_prepare_game(&info.files);
    char sz_problem[64], sz_advice[2048];
    if (psport_gpu_check_game(&info.files,sz_problem,sizeof(sz_problem),sz_advice,sizeof(sz_advice))) {
        fprintf(stderr,"ERROR: Unsupported game files: %s Set PS_SKIP_GAME_CHECK=1 to start anyway.\n",sz_advice);
        exit(2);
    }
    /* The patches' addresses are eboot.bin's: an executable sceSystemServiceLoadExec started gets
     * none of them. */
    const int is_eboot = !strcmp(pp_guest_arguments[0], "/app0/eboot.bin");
    PsPatchList patches={.p_loads=boot_image.p_eboot_loads,.c_loads=boot_image.c_eboot_loads};
    if (is_eboot) {
        PsPatchServices patch_services;
        loader_patch_services(&patches,&patch_services);
        int own_patches=psport_gpu_game_patches(&info.files,&patch_services);
        if (!own_patches) exit(1);
        if (sz_patches_dir) {
            LoaderPatchFolder folder=patch_folder(sz_patches_dir,&info);
            loader_patches_external(&patches,&folder,sz_patches_config);
        }
        if (own_patches<0 && !patches.c_writes)
            printf("Patches: none for %s\n",info.sz_serial[0] ? info.sz_serial : "an unknown title");
    } else if (sz_patches_dir || psport_gpu_game_has_presets(info.sz_serial)) {
        printf("Patches: not applied: they are for /app0/eboot.bin, the application is %s\n", pp_guest_arguments[0]);
    }
    uint64_t cb_image = boot_image.cb_image, entry = boot_image.entry, c_segments = boot_image.c_segments, c_relocs = boot_image.c_relocs;
    s_c_imports = boot_image.c_names;
    uint64_t capabilities = boot_image.capabilities;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!cb_image || cb_image > 512*1024*1024 || entry >= cb_image || !c_segments || c_segments > 4096 || c_relocs > 4000000 || s_c_imports > 100000)
        fail("boot file limits exceeded");
    int native_libc=!strict_imports && (capabilities&1);
    const uint64_t *p_main_tls=boot_image.main_tls;
    uint64_t nb=boot_image.c_bindings,procparam=boot_image.procparam;
    uint64_t *p_bindings=calloc(s_c_imports ? s_c_imports : 1,sizeof(*p_bindings));
    uint64_t *p_binding_kinds=calloc(s_c_imports ? s_c_imports : 1,sizeof(*p_binding_kinds));
    if (!p_bindings || !p_binding_kinds) fail("allocation failed");
    {
        s_p_all_modules=boot_image.p_all_modules;
        s_c_modules=boot_image.c_modules;
        s_p_modules=s_p_all_modules+1;
        if (!s_c_modules || s_c_modules>1024) fail("invalid module count");
        for (uint64_t m=0;m<s_c_modules;++m)
            for (uint64_t e=0;e<s_p_modules[m].c_exports;++e)
                if (s_p_modules[m].p_exports[e].va_export-s_p_modules[m].base>=s_p_modules[m].cb_module) fail("module export outside its module");
        if (p_main_tls[1]>p_main_tls[2] || p_main_tls[2]>1024*1024 || p_main_tls[0]>cb_image ||
            p_main_tls[1]>cb_image-p_main_tls[0] || (p_main_tls[3] & (p_main_tls[3]-1)) || p_main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<s_c_modules;++m) {
            RuntimeModule *p_module=&s_p_modules[m];
            if (p_module->base>=cb_image || !p_module->cb_module || p_module->cb_module>cb_image-p_module->base || p_module->init<p_module->base || p_module->init-p_module->base>=p_module->cb_module ||
                (p_module->tls_module && (p_module->va_tls>=cb_image || p_module->tls_memsz>cb_image-p_module->va_tls || p_module->tls_memsz>1024*1024 ||
                                   p_module->tls_filesz>p_module->tls_memsz || p_module->tls_module<2 || p_module->tls_module>=RUNTIME_TLS_MODULES)))
                fail("invalid linked module metadata");
        }
        if (nb>s_c_imports) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t i_import=boot_image.p_bindings[i].i_binding,va_target=boot_image.p_bindings[i].va_binding,kind=boot_image.p_bindings[i].kind;
            int inside=0;
            for (uint64_t m=0;m<s_c_modules;++m)
                if (va_target>=s_p_modules[m].base && va_target-s_p_modules[m].base<s_p_modules[m].cb_module) inside=1;
            if (i_import>=s_c_imports || !inside || kind<1 || kind>6 || p_bindings[i_import]) fail("invalid native binding");
            p_bindings[i_import]=va_target; p_binding_kinds[i_import]=kind;
        }
    }
    Segment *p_segments = boot_image.p_segments;
    Reloc *p_relocs = boot_image.p_relocs;
    s_p_names = boot_image.p_names;
    for (uint64_t i = 0; i < c_segments; ++i)
        if (p_segments[i].va_segment > cb_image || p_segments[i].cb_segment > cb_image - p_segments[i].va_segment ||
            p_segments[i].va_segment % s_cb_page || p_segments[i].flags > 7) fail("bad segment");
    for (uint64_t i = 0; i < s_c_imports; ++i)
        if (!memchr(s_p_names[i], 0, 128)) fail("unterminated import name");
    runtime_calls_configure((const char (*)[128])s_p_names, s_c_imports);
    for (uint64_t i = 0; i < c_relocs; ++i)
        if (!mapped(p_segments, c_segments, p_relocs[i].target, 8) || p_relocs[i].kind > 2 ||
            (p_relocs[i].kind && p_relocs[i].value >= s_c_imports) ||
            (p_relocs[i].kind != 2 && p_relocs[i].addend) || p_relocs[i].addend >= s_cb_page) fail("bad relocation");
    for (uint64_t m=0;m<s_c_modules;++m)
        if (!mapped(p_segments,c_segments,s_p_modules[m].init,1) || (s_p_modules[m].tls_module && !mapped(p_segments,c_segments,s_p_modules[m].va_tls,s_p_modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (s_c_modules && !mapped(p_segments,c_segments,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<s_c_imports;++i) {
        if (!p_bindings[i]) continue;
        if (!mapped(p_segments,c_segments,p_bindings[i],1)) fail("unmapped native export");
        if (p_binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<c_segments;++s)
                if ((p_segments[s].flags&1) && p_bindings[i]>=p_segments[s].va_segment &&
                    p_bindings[i]-p_segments[s].va_segment<p_segments[s].cb_segment) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    s_p_image = allocate(round_page(cb_image));
    memcpy(s_p_image, boot_image.p_image, cb_image);
    free(boot_image.p_image);
    if (s_p_all_modules) {
        for (uint64_t i=0;i<s_p_all_modules[0].c_segments;++i) {
            const RuntimeModuleSegment *p_segment=&s_p_all_modules[0].segments[i];
            if (p_segment->va_module_segment+p_segment->cb_module_segment>s_p_all_modules[0].cb_module) s_p_all_modules[0].cb_module=p_segment->va_module_segment+p_segment->cb_module_segment;
        }
        if (!s_p_all_modules[0].cb_module) s_p_all_modules[0].cb_module=s_p_modules[0].base;
        for (uint64_t e=0;e<s_p_all_modules[0].c_exports;++e)
            if (s_p_all_modules[0].p_exports[e].va_export>=s_p_all_modules[0].cb_module) fail("eboot export outside the eboot");
        unsigned char *p_started=calloc(s_c_modules+1,1);
        if (!p_started) fail("allocation failed");
        runtime_modules_configure(s_p_image,s_p_all_modules,s_c_modules+1,p_started);
    }
    char sz_sfo[4096];
    uint32_t attributes=0;
    snprintf(sz_sfo,sizeof(sz_sfo),"%s/sce_sys/param.sfo",sz_app0 ? sz_app0 : ".");
    sfo_value(sz_sfo,"ATTRIBUTE",NULL,0,&attributes);
    uint64_t sdk=0;
    if (procparam) memcpy(&sdk,s_p_image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
    runtime_process_configure((uint32_t)sdk,attributes);
    if (!cpu_only) {
        char sz_title[128]="", sz_serial[16]="UNKNOWN";
        sfo_value(sz_sfo,"TITLE",sz_title,sizeof(sz_title),NULL);
        sfo_value(sz_sfo,"TITLE_ID",sz_serial,sizeof(sz_serial),NULL);
        PsGpuConfig gpu={sz_title,sz_serial,sz_user_dir ? sz_user_dir : "user",(uint32_t)sdk,attributes,1920,1080,sz_app0 ? sz_app0 : "."};
        s_is_gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (psport_gpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,psport_gpu_symbol_count());
    }
#ifndef _WIN32
    static const int s_shutdown_signals[] = {SIGTERM, SIGINT, SIGHUP};
    for (size_t i = 0; i < sizeof(s_shutdown_signals) / sizeof(*s_shutdown_signals); ++i) {
        struct sigaction current;
        if (!sigaction(s_shutdown_signals[i], NULL, &current) && current.sa_handler == SIG_DFL)
            signal(s_shutdown_signals[i], shutdown_signal);
    }
#endif
    const char *sz_permissive = getenv("PS_PERMISSIVE");
    if (sz_permissive && !strcmp(sz_permissive, "1")) {
        s_p_permissive_seen = calloc(s_c_imports ? s_c_imports : 1, 1);
        s_p_permissive_outputs = calloc(s_c_imports ? s_c_imports : 1, 1);
        if (!s_p_permissive_seen || !s_p_permissive_outputs) fail("allocation failed");
        for (uint64_t i = 0; i < s_c_imports; ++i)
            s_p_permissive_outputs[i] = (unsigned char)runtime_import_outputs(s_p_names[i]);
        const char *sz_out = getenv("PS_PERMISSIVE_OUT");
        s_is_permissive_out_error = sz_out && !strcmp(sz_out, "error");
    }
    unsigned char *p_traps = allocate(round_page((s_c_imports + 1) * 32));
    unsigned char *p_thunks = allocate(round_page((s_c_imports + 1) * THUNK_SIZE));
    unsigned char *p_data_traps = allocate((s_c_imports + 1) * s_cb_page);
    protect(p_data_traps, (s_c_imports + 1) * s_cb_page, 0);
    for (uint64_t i = 0; i < s_c_imports; ++i) {
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
    unsigned char *p_import_state = calloc(s_c_imports ? s_c_imports : 1, 1);
    if (!p_import_state) fail("allocation failed");
    for (uint64_t i = 0; i < c_relocs; ++i) {
        uintptr_t value = p_relocs[i].kind == 2 ? (uintptr_t)(p_data_traps + s_cb_page * p_relocs[i].value + p_relocs[i].addend)
                        : p_relocs[i].kind == 1 ? (uintptr_t)(p_traps + 32 * p_relocs[i].value)
                        : (uintptr_t)s_p_image + p_relocs[i].value;
        if (p_relocs[i].kind) {
            g_runtime_last_stub = 0;
            const uint64_t bound_kind = native_libc ? p_binding_kinds[p_relocs[i].value] : 0;
            uintptr_t resolved = bound_kind > 2 ? 0 : runtime_resolve(s_p_names[p_relocs[i].value], p_relocs[i].kind == 2);
            if (resolved) {
                value = resolved + p_relocs[i].addend;
                p_import_state[p_relocs[i].value] = g_runtime_last_stub ? 3 : 1;
                const unsigned failure = p_relocs[i].kind == 1 ? runtime_import_failure(s_p_names[p_relocs[i].value]) : 0;
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
                value=(uintptr_t)s_p_image+va_target+p_relocs[i].addend;
                p_import_state[p_relocs[i].value] = bound_kind > 4 ? 6 : bound_kind > 2 ? 5 : 2;
            } else p_import_state[p_relocs[i].value] = 4;
        }
        memcpy(s_p_image + p_relocs[i].target, &value, 8);
    }
    /* Before the game runs: every import that will not do what it asks, so a log shows what
     * cannot work even when the game never gets as far as calling it. */
    unsigned counts[7] = {0};
    for (uint64_t i = 0; i < s_c_imports; ++i) {
        ++counts[p_import_state[i]];
        if (p_import_state[i] == 3)
            printf("Runtime: import %s (%s) is a stub: it succeeds without doing anything\n",
                   s_p_names[i], runtime_import_name(s_p_names[i]));
        else if (p_import_state[i] == 4)
            printf("Runtime: import %s (%s) has no implementation: %s\n", s_p_names[i], runtime_import_name(s_p_names[i]),
                   !s_p_permissive_seen ? "the game stops if it calls it"
                   : s_is_permissive_out_error && s_p_permissive_outputs[i]
                       ? "returns 0x80020016 when called (PS_PERMISSIVE_OUT=error)"
                       : "returns 0 when called (PS_PERMISSIVE)");
    }
    printf("Runtime: imports %llu: host %u, game modules %u, system modules %u, modules the game loads %u, stubs %u, without implementation %u\n",
           (unsigned long long)s_c_imports, counts[1], counts[2], counts[5], counts[6], counts[3], counts[4]);
    free(p_import_state);
    if (is_eboot) apply_patches(&patches, p_segments, c_segments, p_relocs, c_relocs);
    if (!cpu_only) psport_gpu_patch_image(s_p_image, cb_image);
    protect(p_traps, round_page((s_c_imports + 1) * 32), 5);
    protect(p_thunks, round_page((s_c_imports + 1) * THUNK_SIZE), 5);
    protect(s_p_image, round_page(cb_image), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < c_segments; ++i) {
        protect(s_p_image + p_segments[i].va_segment, round_page(p_segments[i].cb_segment), (unsigned)p_segments[i].flags);
        if ((p_segments[i].flags & 1) && entry >= p_segments[i].va_segment && entry - p_segments[i].va_segment < p_segments[i].cb_segment)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", cb_image, c_segments, c_relocs);
    if (native_libc) {
        for (uint64_t m=0;m<s_c_modules;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<c_segments;++i)
                if ((p_segments[i].flags&1) && s_p_modules[m].init>=p_segments[i].va_segment && s_p_modules[m].init-p_segments[i].va_segment<p_segments[i].cb_segment) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (s_p_modules[m].tls_module)
                runtime_set_module_tls(s_p_modules[m].tls_module,s_p_image+s_p_modules[m].va_tls,s_p_modules[m].tls_filesz,s_p_modules[m].tls_memsz);
        }
        runtime_set_main_tls(s_p_image+p_main_tls[0],p_main_tls[1],p_main_tls[2],p_main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(s_p_image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<s_c_modules;++m) {
            if (s_p_modules[m].deferred) {
                printf("Linked module %" PRIu64 " (%s) waits for the game to load it\n",m,s_p_modules[m].sz_name);
                continue;
            }
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,s_p_modules[m].init,nb);
            int32_t result=0;
            runtime_module_start(m+1,0,NULL,&result);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    s_has_entered_game=1;
    /* The entry's argument block: argc, then argv inline, NULL-terminated. */
    uint64_t *p_params = calloc((size_t)c_guest_arguments + 2, sizeof(*p_params));
    if (!p_params) fail("allocation failed");
    p_params[0] = (uint64_t)c_guest_arguments;
    for (int i = 0; i < c_guest_arguments; ++i) p_params[i + 1] = (uint64_t)(uintptr_t)pp_guest_arguments[i];
#ifdef _WIN32
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(params, guest_exit);
#else
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *p_stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!p_stack) fail("cannot allocate guest main stack");
    if (native_libc) runtime_thread_set_stack(p_stack,MAIN_STACK);
    enter_on_stack(s_p_image+entry,p_params,(void *)guest_exit,p_stack+MAIN_STACK-64);
#endif
    fail("entry unexpectedly returned");
}
