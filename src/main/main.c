/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <cpuid.h>
#include "main.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#endif

char (*g_p_names)[128];
uint64_t g_c_imports;
unsigned char *g_p_image;
size_t g_cb_page;
/* s_p_all_modules[0] describes the eboot; g_p_modules are the linked ones after it. */
static RuntimeModule *s_p_all_modules;
RuntimeModule *g_p_modules;
uint64_t g_c_modules;
int g_has_entered_game;
int g_is_gpu_enabled;
int vulkan_smoke(void);

void fail(const char *sz_message) { fprintf(stderr, "ERROR: %s\n", sz_message); exit(1); }

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
size_t round_page(size_t cb_page) { return (cb_page + g_cb_page - 1) & ~(g_cb_page - 1); }
void *allocate(size_t cb_allocation) {
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
void protect(void *p_range, size_t cb_range, unsigned flags) {
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
#ifndef _WIN32
/* enter_on_stack(p_entry, p_arg0, p_arg1, p_top): call p_entry(p_arg0,p_arg1) on a new stack. */
void enter_on_stack(void *p_entry, void *p_arg0, void *p_arg1, void *p_top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }

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
    int query_status = answer_query(argc, argv);
    if (query_status >= 0) return query_status;
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
        fprintf(stderr, "Usage: %s --game dir [--system-modules dir] [--hle libs] [--lle libs] [--cpu-only] [--strict-imports] [--app0 dir] [--user dir] [--patches-dir dir [--patches-config file]] [--mods dir [--mods-config file]] [--out dir] [--timeout seconds] [--exec guest-path [--arg value]...] | --check-game dir | --has-presets dir | --list-patches dir (--game dir | --title id [--version v]) | --list-mods dir [--game dir | --title id] | --list-quirks [--game dir] | --vulkan-only\n", argv[0]);
        return 1;
    }
    const char *mod_folders[256];
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
    g_cb_page = (size_t)sysconf(_SC_PAGESIZE);
    install_fault_handlers(timeout_seconds);
#endif
    LoaderImage boot_image;
    loader_load_game(&game,&boot_image);
    /* The game's title and its files, then the patches, all before any of its code runs or a
     * window opens: a launch that cannot work stops here, with why. */
    GameInfo info;
    game_info(game.sz_game,&info);
    /* TODO: loader_game_traits() for QUIRK_TRAITS once the loader detects middleware (reorg phase 7). */
    RuntimeQuirkLaunch quirks=quirk_launch(&info,0);
    runtime_quirks_evaluate(&quirks);
    if (info.title.pfn_prepare_environment) info.title.pfn_prepare_environment();
    char sz_problem[64], sz_advice[2048];
    if (info.title.pfn_check_files && info.title.pfn_check_files(&info.files,sz_problem,sizeof(sz_problem),sz_advice,sizeof(sz_advice))) {
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
        int own_patches=info.title.pfn_add_patches ? info.title.pfn_add_patches(&info.files,&patch_services) : -1;
        if (!own_patches) exit(1);
        if (sz_patches_dir) {
            LoaderPatchFolder folder=patch_folder(sz_patches_dir,&info);
            loader_patches_external(&patches,&folder,sz_patches_config);
        }
        if (own_patches<0 && !patches.c_writes)
            printf("Patches: none for %s\n",info.sz_serial[0] ? info.sz_serial : "an unknown title");
    } else if (sz_patches_dir || info.title.has_presets) {
        printf("Patches: not applied: they are for /app0/eboot.bin, the application is %s\n", pp_guest_arguments[0]);
    }
    uint64_t cb_image = boot_image.cb_image, entry = boot_image.entry, c_segments = boot_image.c_segments, c_relocs = boot_image.c_relocs;
    g_c_imports = boot_image.c_names;
    uint64_t capabilities = boot_image.capabilities;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!cb_image || cb_image > 512*1024*1024 || entry >= cb_image || !c_segments || c_segments > 4096 || c_relocs > 4000000 || g_c_imports > 100000)
        fail("boot file limits exceeded");
    int native_libc=!strict_imports && (capabilities&1);
    const uint64_t *p_main_tls=boot_image.main_tls;
    uint64_t nb=boot_image.c_bindings,procparam=boot_image.procparam;
    uint64_t *p_bindings=calloc(g_c_imports ? g_c_imports : 1,sizeof(*p_bindings));
    uint64_t *p_binding_kinds=calloc(g_c_imports ? g_c_imports : 1,sizeof(*p_binding_kinds));
    if (!p_bindings || !p_binding_kinds) fail("allocation failed");
    {
        s_p_all_modules=boot_image.p_all_modules;
        g_c_modules=boot_image.c_modules;
        g_p_modules=s_p_all_modules+1;
        if (!g_c_modules || g_c_modules>1024) fail("invalid module count");
        for (uint64_t m=0;m<g_c_modules;++m)
            for (uint64_t e=0;e<g_p_modules[m].c_exports;++e)
                if (g_p_modules[m].p_exports[e].va_export-g_p_modules[m].base>=g_p_modules[m].cb_module) fail("module export outside its module");
        if (p_main_tls[1]>p_main_tls[2] || p_main_tls[2]>1024*1024 || p_main_tls[0]>cb_image ||
            p_main_tls[1]>cb_image-p_main_tls[0] || (p_main_tls[3] & (p_main_tls[3]-1)) || p_main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<g_c_modules;++m) {
            RuntimeModule *p_module=&g_p_modules[m];
            if (p_module->base>=cb_image || !p_module->cb_module || p_module->cb_module>cb_image-p_module->base || p_module->init<p_module->base || p_module->init-p_module->base>=p_module->cb_module ||
                (p_module->tls_module && (p_module->va_tls>=cb_image || p_module->tls_memsz>cb_image-p_module->va_tls || p_module->tls_memsz>1024*1024 ||
                                   p_module->tls_filesz>p_module->tls_memsz || p_module->tls_module<2 || p_module->tls_module>=RUNTIME_TLS_MODULES)))
                fail("invalid linked module metadata");
        }
        if (nb>g_c_imports) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t i_import=boot_image.p_bindings[i].i_binding,va_target=boot_image.p_bindings[i].va_binding,kind=boot_image.p_bindings[i].kind;
            int inside=0;
            for (uint64_t m=0;m<g_c_modules;++m)
                if (va_target>=g_p_modules[m].base && va_target-g_p_modules[m].base<g_p_modules[m].cb_module) inside=1;
            if (i_import>=g_c_imports || !inside || kind<1 || kind>6 || p_bindings[i_import]) fail("invalid native binding");
            p_bindings[i_import]=va_target; p_binding_kinds[i_import]=kind;
        }
    }
    Segment *p_segments = boot_image.p_segments;
    Reloc *p_relocs = boot_image.p_relocs;
    g_p_names = boot_image.p_names;
    for (uint64_t i = 0; i < c_segments; ++i)
        if (p_segments[i].va_segment > cb_image || p_segments[i].cb_segment > cb_image - p_segments[i].va_segment ||
            p_segments[i].va_segment % g_cb_page || p_segments[i].flags > 7) fail("bad segment");
    for (uint64_t i = 0; i < g_c_imports; ++i)
        if (!memchr(g_p_names[i], 0, 128)) fail("unterminated import name");
    runtime_calls_configure((const char (*)[128])g_p_names, g_c_imports);
    for (uint64_t i = 0; i < c_relocs; ++i)
        if (!mapped(p_segments, c_segments, p_relocs[i].target, 8) || p_relocs[i].kind > 2 ||
            (p_relocs[i].kind && p_relocs[i].value >= g_c_imports) ||
            (p_relocs[i].kind != 2 && p_relocs[i].addend) || p_relocs[i].addend >= g_cb_page) fail("bad relocation");
    for (uint64_t m=0;m<g_c_modules;++m)
        if (!mapped(p_segments,c_segments,g_p_modules[m].init,1) || (g_p_modules[m].tls_module && !mapped(p_segments,c_segments,g_p_modules[m].va_tls,g_p_modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (g_c_modules && !mapped(p_segments,c_segments,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<g_c_imports;++i) {
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
    g_p_image = allocate(round_page(cb_image));
    memcpy(g_p_image, boot_image.p_image, cb_image);
    free(boot_image.p_image);
    if (s_p_all_modules) {
        for (uint64_t i=0;i<s_p_all_modules[0].c_segments;++i) {
            const RuntimeModuleSegment *p_segment=&s_p_all_modules[0].segments[i];
            if (p_segment->va_module_segment+p_segment->cb_module_segment>s_p_all_modules[0].cb_module) s_p_all_modules[0].cb_module=p_segment->va_module_segment+p_segment->cb_module_segment;
        }
        if (!s_p_all_modules[0].cb_module) s_p_all_modules[0].cb_module=g_p_modules[0].base;
        for (uint64_t e=0;e<s_p_all_modules[0].c_exports;++e)
            if (s_p_all_modules[0].p_exports[e].va_export>=s_p_all_modules[0].cb_module) fail("eboot export outside the eboot");
        unsigned char *p_started=calloc(g_c_modules+1,1);
        if (!p_started) fail("allocation failed");
        runtime_modules_configure(g_p_image,s_p_all_modules,g_c_modules+1,p_started);
    }
    char sz_sfo[4096];
    uint32_t attributes=0;
    snprintf(sz_sfo,sizeof(sz_sfo),"%s/sce_sys/param.sfo",sz_app0 ? sz_app0 : ".");
    sfo_value(sz_sfo,"ATTRIBUTE",NULL,0,&attributes);
    uint64_t sdk=0;
    if (procparam) memcpy(&sdk,g_p_image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
    runtime_process_configure((uint32_t)sdk,attributes);
    if (!cpu_only) {
        char sz_title[128]="", sz_serial[16]="UNKNOWN";
        sfo_value(sz_sfo,"TITLE",sz_title,sizeof(sz_title),NULL);
        sfo_value(sz_sfo,"TITLE_ID",sz_serial,sizeof(sz_serial),NULL);
        PsGpuConfig gpu={sz_title,sz_serial,sz_user_dir ? sz_user_dir : "user",(uint32_t)sdk,attributes,1920,1080,sz_app0 ? sz_app0 : "."};
        g_is_gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (psport_gpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,psport_gpu_symbol_count());
    }
#ifndef _WIN32
    install_shutdown_signals();
#endif
    unsigned char *p_traps, *p_thunks;
    bind_imports(p_segments, c_segments, p_relocs, c_relocs, native_libc, p_bindings, p_binding_kinds, &p_traps, &p_thunks);
    if (is_eboot) apply_patches(&patches, p_segments, c_segments, p_relocs, c_relocs);
    if (!cpu_only && info.title.pfn_patch_image) info.title.pfn_patch_image(g_p_image, cb_image);
    protect(p_traps, round_page((g_c_imports + 1) * 32), 5);
    protect(p_thunks, round_page((g_c_imports + 1) * THUNK_SIZE), 5);
    protect(g_p_image, round_page(cb_image), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < c_segments; ++i) {
        protect(g_p_image + p_segments[i].va_segment, round_page(p_segments[i].cb_segment), (unsigned)p_segments[i].flags);
        if ((p_segments[i].flags & 1) && entry >= p_segments[i].va_segment && entry - p_segments[i].va_segment < p_segments[i].cb_segment)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", cb_image, c_segments, c_relocs);
    if (native_libc) {
        for (uint64_t m=0;m<g_c_modules;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<c_segments;++i)
                if ((p_segments[i].flags&1) && g_p_modules[m].init>=p_segments[i].va_segment && g_p_modules[m].init-p_segments[i].va_segment<p_segments[i].cb_segment) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (g_p_modules[m].tls_module)
                runtime_set_module_tls(g_p_modules[m].tls_module,g_p_image+g_p_modules[m].va_tls,g_p_modules[m].tls_filesz,g_p_modules[m].tls_memsz);
        }
        runtime_set_main_tls(g_p_image+p_main_tls[0],p_main_tls[1],p_main_tls[2],p_main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(g_p_image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<g_c_modules;++m) {
            if (g_p_modules[m].deferred) {
                printf("Linked module %" PRIu64 " (%s) waits for the game to load it\n",m,g_p_modules[m].sz_name);
                continue;
            }
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,g_p_modules[m].init,nb);
            int32_t result=0;
            runtime_module_start(m+1,0,NULL,&result);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    g_has_entered_game=1;
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
    enter_on_stack(g_p_image+entry,p_params,(void *)guest_exit,p_stack+MAIN_STACK-64);
#endif
    fail("entry unexpectedly returned");
}
