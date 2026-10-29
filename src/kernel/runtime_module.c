// SPDX-License-Identifier: GPL-2.0-or-later
/* libkernel module services over what the loader prelinked (src/loader/loader_link.c). Modules the game loads by path
 * (Unity's Media/Modules, Media/Plugins) are already in the image, relocated and bound; loading
 * one runs its initializer once, as the PS4 linker's module_start. Handles are table indices
 * (0 the eboot), as shadPS4 numbers its modules. */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define ENOENT_ ERR(2)
#define ESRCH_ ERR(3)
#define EFAULT_ ERR(14)
#define EINVAL_ ERR(22)

/* The runtime's kernel, GPU, video, pad, audio output, clock and user-level threads; the ported
 * gpu/fiber, gpu/font, gpu/audiodec and gpu/ngs2; libSceHmd, since the console's module_start fails
 * without the headset's devices (/dev/hmd_cmd) while runtime_vr.c answers as a console with no
 * headset (kb #1248). */
static const char *const s_sz_native_libraries[]={"libkernel", "libkernel_unity", "libkernel_sys", "libSceGnmDriver",
                                                  "libSceVideoOut", "libScePad", "libSceAudioOut", "libSceFiber",
                                                  "libSceFont", "libSceFontFt", "libSceAudiodec", "libSceNgs2",
                                                  "libSceUlt", "libSceRtc", "libSceHmd"};
int runtime_module_is_native(const char *sz_library) {
    for (size_t i=0;i<sizeof(s_sz_native_libraries)/sizeof(*s_sz_native_libraries);++i)
        if (!strcmp(s_sz_native_libraries[i],sz_library)) return 1;
    return 0;
}

static unsigned char *s_p_image;
static RuntimeModule *s_p_modules;
static size_t s_c_modules;
/* 0 unstarted, 1 started. Recursive: a module_start may load another module. */
static unsigned char *s_p_started;
static pthread_mutex_t s_start_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

void runtime_modules_configure(unsigned char *p_base, RuntimeModule *p_table, size_t c_modules, unsigned char *p_state) {
    s_p_image=p_base; s_p_modules=p_table; s_c_modules=c_modules; s_p_started=p_state;
}
int runtime_module_start(size_t h_handle, uint64_t args, const void *p_argp, int32_t *p_result) {
    if (h_handle>=s_c_modules || !h_handle) return 0;
    if (s_p_modules[h_handle].has_np_manager_dependency) runtime_np_manager_start();
    pthread_mutex_lock(&s_start_lock);
    int32_t value=0;
    int ran=!s_p_started[h_handle];
    if (ran) {
        /* before the call: a module_start loading itself must not recurse */
        __atomic_store_n(&s_p_started[h_handle],1,__ATOMIC_RELEASE);
        typedef int32_t (ABI *ModuleStart)(uint64_t,const void *,void *);
        value=((ModuleStart)(s_p_image+s_p_modules[h_handle].init))(args,p_argp,NULL);
    }
    pthread_mutex_unlock(&s_start_lock);
    if (p_result) *p_result=value;
    return ran;
}
/* A deferred module has no handle until the game loads it, as on the PS4 where it is not mapped. */
static int loaded(int32_t h_handle) {
    if (h_handle<0 || (size_t)h_handle>=s_c_modules) return 0;
    return !s_p_modules[h_handle].deferred || __atomic_load_n(&s_p_started[h_handle],__ATOMIC_ACQUIRE);
}

/* NID = first 8 bytes of SHA-1(name + salt), read little-endian, base64 with '+' '-'
 * (shadPS4 module.cpp StringToNid). */
static void sha1(const unsigned char *p_data, size_t length, unsigned char out[20]) {
    uint32_t h[5]={0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
    uint64_t bits=(uint64_t)length*8;
    size_t cb_padded=(length+9+63)/64*64;
    for (size_t block=0;block<cb_padded;block+=64) {
        uint32_t w[80];
        for (int i=0;i<64;++i) {
            size_t at=block+(size_t)i;
            unsigned char byte=at<length ? p_data[at] : at==length ? 0x80 : at>=cb_padded-8 ? (unsigned char)(bits>>(8*(cb_padded-1-at))) : 0;
            if (i%4==0) w[i/4]=0;
            w[i/4]|=(uint32_t)byte<<(24-8*(i%4));
        }
        for (int i=16;i<80;++i) { uint32_t x=w[i-3]^w[i-8]^w[i-14]^w[i-16]; w[i]=x<<1|x>>31; }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for (int i=0;i<80;++i) {
            uint32_t f=i<20 ? ((b&c)|(~b&d))+0x5a827999 : i<40 ? (b^c^d)+0x6ed9eba1
                     : i<60 ? ((b&c)|(b&d)|(c&d))+0x8f1bbcdc : (b^c^d)+0xca62c1d6;
            uint32_t t=(a<<5|a>>27)+f+e+w[i];
            e=d; d=c; c=b<<30|b>>2; b=a; a=t;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }
    for (int i=0;i<20;++i) out[i]=(unsigned char)(h[i/4]>>(24-8*(i%4)));
}
static void nid_of(const char *sz_name, char sz_out[12]) {
    static const unsigned char s_salt[16]={0x51,0x8D,0x64,0xA6,0x35,0xDE,0xD8,0xC1,0xE6,0xB0,0x39,0xB1,0xC3,0xE5,0x52,0x30};
    static const char s_sz_codes[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    size_t length=strlen(sz_name);
    unsigned char small[256], hash[20];
    /* Mangled C++ names run past any fixed buffer, and a truncated one hashes to another NID. */
    unsigned char *p_input=length+sizeof(s_salt)<=sizeof(small) ? small : malloc(length+sizeof(s_salt));
    if (!p_input) { sz_out[0]=0; return; }
    memcpy(p_input,sz_name,length); memcpy(p_input+length,s_salt,sizeof(s_salt));
    sha1(p_input,length+sizeof(s_salt),hash);
    if (p_input!=small) free(p_input);
    uint64_t digest=0;
    for (int i=7;i>=0;--i) digest=digest<<8|hash[i];
    for (int i=0;i<10;++i) sz_out[i]=s_sz_codes[(digest>>(58-i*6))&0x3f];
    sz_out[10]=s_sz_codes[(digest&0xf)*4];
    sz_out[11]=0;
}

/* Guest path to table index: /app0-relative path, else the file name, both case-insensitive
 * (Unity builds the path from its data folder, "/app0/Media" or "/app0/media"). */
static size_t find_path(const char *sz_path) {
    const char *sz_relative=!strncasecmp(sz_path,"/app0/",6) ? sz_path+6 : sz_path;
    const char *sz_base=strrchr(sz_path,'/');
    sz_base=sz_base ? sz_base+1 : sz_path;
    for (size_t i=1;i<s_c_modules;++i)
        if (!strcasecmp(s_p_modules[i].sz_name,sz_relative)) return i;
    for (size_t i=1;i<s_c_modules;++i) {
        const char *sz_own=strrchr(s_p_modules[i].sz_name,'/');
        if (!strcasecmp(sz_own ? sz_own+1 : s_p_modules[i].sz_name,sz_base)) return i;
    }
    return 0;
}

/* A system library the guest loads by path that no linked image provides: libSceSysmodule loads
 * every library of a module id (id 7, libSceUlt, starts with libSceRazorCpu), and the loader links
 * only what something imports or what the runtime does not serve. Where a console has the file,
 * the load succeeds with a handle past the table's that has no image; anything the guest imports
 * from it stays resolved, or unimplemented, as at link time. */
static const char *const s_sz_system_libraries[]={
#include "system_libraries.inc"
};
#define C_SYSTEM_LIBRARIES (sizeof(s_sz_system_libraries)/sizeof(*s_sz_system_libraries))
/* Per library: loads not yet unloaded (the handle is valid while nonzero), loads in total, and the
 * call report's line, written once before its first count. */
static uint64_t s_c_unlinked_references[C_SYSTEM_LIBRARIES], s_c_unlinked_loads[C_SYSTEM_LIBRARIES];
static char s_sz_unlinked_reports[C_SYSTEM_LIBRARIES][160];
static uint64_t s_c_unlinked_unloads, s_c_unlinked_infos, s_c_unlinked_lookups;
static int compare_library(const void *p_key, const void *p_entry) { return strcmp(p_key,*(const char *const *)p_entry); }
/* The index in s_sz_system_libraries of a file in a system library folder (/system or /<sandbox
 * word>, then common, priv, common_ex or priv_ex, then lib), -1 for any other path. */
static ptrdiff_t system_library(const char *sz_path) {
    static const char *const s_sz_folders[]={"common/lib/","priv/lib/","common_ex/lib/","priv_ex/lib/"};
    if (sz_path[0]!='/') return -1;
    const char *sz_root=sz_path+1, *sz_after_root=strchr(sz_root,'/');
    if (!sz_after_root) return -1;
    const size_t cb_root=(size_t)(sz_after_root-sz_root);
    const char *sz_word=runtime_process_sandbox_word();
    if (!(cb_root==6 && !memcmp(sz_root,"system",6)) && !(cb_root==strlen(sz_word) && !memcmp(sz_root,sz_word,cb_root)))
        return -1;
    const char *sz_file=NULL;
    for (size_t i=0;i<sizeof(s_sz_folders)/sizeof(*s_sz_folders) && !sz_file;++i)
        if (!strncmp(sz_after_root+1,s_sz_folders[i],strlen(s_sz_folders[i]))) sz_file=sz_after_root+1+strlen(s_sz_folders[i]);
    if (!sz_file || strchr(sz_file,'/')) return -1;
    const char *sz_extension=strrchr(sz_file,'.');
    if (!sz_extension || (strcmp(sz_extension,".sprx") && strcmp(sz_extension,".prx"))) return -1;
    char sz_stem[128];
    const size_t cb_stem=(size_t)(sz_extension-sz_file);
    if (cb_stem>=sizeof(sz_stem)) return -1;
    memcpy(sz_stem,sz_file,cb_stem);
    sz_stem[cb_stem]=0;
    const char *const *pp_found=bsearch(sz_stem,s_sz_system_libraries,C_SYSTEM_LIBRARIES,sizeof(*s_sz_system_libraries),compare_library);
    return pp_found ? pp_found-s_sz_system_libraries : -1;
}
static int is_unlinked(int32_t h_handle) {
    return h_handle>=0 && (size_t)h_handle>=s_c_modules && (size_t)h_handle-s_c_modules<C_SYSTEM_LIBRARIES;
}
static int32_t load_unlinked(const char *sz_path, size_t i_library, uint32_t flags, int32_t *p_result) {
    const char *sz_library=s_sz_system_libraries[i_library];
    const char *sz_why=runtime_module_is_native(sz_library) ? "served by the runtime" : "a console's system library nothing imports";
    if (!__atomic_fetch_add(&s_c_unlinked_loads[i_library],1,__ATOMIC_RELAXED)) {
        snprintf(s_sz_unlinked_reports[i_library],sizeof(*s_sz_unlinked_reports),
                 "sceKernelLoadStartModule %s (%s: handle without an image)",sz_library,sz_why);
        runtime_calls_hle_stub(s_sz_unlinked_reports[i_library],&s_c_unlinked_loads[i_library]);
    }
    const size_t h_handle=s_c_modules+i_library;
    if (!__atomic_fetch_add(&s_c_unlinked_references[i_library],1,__ATOMIC_ACQ_REL))
        printf("Runtime: sceKernelLoadStartModule %s (flags %#x): %s, handle %zu without an image\n",sz_path,flags,sz_why,h_handle);
    if (p_result) *p_result=0;
    return (int32_t)h_handle;
}
static ABI int32_t load_start_module(const char *sz_path, uint64_t args, const void *p_argp, uint32_t flags,
                                     const void *p_options, int32_t *p_result) {
    (void)p_options;
    if (!sz_path) return EINVAL_;
    size_t h_handle=find_path(sz_path);
    if (!h_handle) {
        const ptrdiff_t i_library=system_library(sz_path);
        if (i_library>=0) return load_unlinked(sz_path,(size_t)i_library,flags,p_result);
        printf("Runtime: sceKernelLoadStartModule %s: no such module linked\n",sz_path);
        return ENOENT_;
    }
    int32_t value=0;
    if (runtime_module_start(h_handle,args,p_argp,&value))
        printf("Runtime: started module %s (handle %zu, flags %#x): module_start returned %d\n",
               s_p_modules[h_handle].sz_name,h_handle,flags,value);
    if (p_result) *p_result=value;
    return (int32_t)h_handle;
}
/* What libSceSysmodule loads system libraries with: sceKernelLoadStartModule with the system flag
 * 0x10000 that the public call would refuse. */
static ABI int32_t load_start_module_for_sysmodule(const char *sz_path, uint64_t args, const void *p_argp, uint32_t flags,
                                                   const void *p_options, int32_t *p_result) {
    if (p_options) return EINVAL_;
    if (sz_path && !runtime_memory_is_guest_range(sz_path,1)) return EFAULT_;
    if (p_result && !runtime_memory_is_guest_range(p_result,sizeof(*p_result))) return EFAULT_;
    return load_start_module(sz_path,args,p_argp,flags|0x10000,NULL,p_result);
}
/* The eboot is always the table's first module. */
static ABI int32_t executable_module_handle(void) { return 0; }
/* Nothing is unmapped: the image stays one mapping and a later load finds the module started. */
static ABI int32_t stop_unload_module(int32_t h_handle, uint64_t args, const void *p_argp, uint32_t flags,
                                      const void *p_options, int32_t *p_result) {
    (void)args; (void)p_argp; (void)flags; (void)p_options;
    if (is_unlinked(h_handle)) {
        uint64_t *p_references=&s_c_unlinked_references[(size_t)h_handle-s_c_modules];
        uint64_t c_references=__atomic_load_n(p_references,__ATOMIC_ACQUIRE);
        do {
            if (!c_references) return ESRCH_;
        } while (!__atomic_compare_exchange_n(p_references,&c_references,c_references-1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE));
        if (!__atomic_fetch_add(&s_c_unlinked_unloads,1,__ATOMIC_RELAXED))
            runtime_calls_hle_stub("sceKernelStopUnloadModule of a handle without an image (nothing to stop)",&s_c_unlinked_unloads);
        if (p_result) *p_result=0;
        return 0;
    }
    if (!loaded(h_handle)) return ESRCH_;
    if (p_result) *p_result=0;
    return 0;
}
static ABI int32_t dlsym(int32_t h_handle, const char *sz_symbol, void **pp_address) {
    if (is_unlinked(h_handle) && __atomic_load_n(&s_c_unlinked_references[(size_t)h_handle-s_c_modules],__ATOMIC_ACQUIRE)) {
        if (!__atomic_fetch_add(&s_c_unlinked_lookups,1,__ATOMIC_RELAXED))
            runtime_calls_hle_stub("sceKernelDlsym in a system library without an image (ESRCH)",&s_c_unlinked_lookups);
        return ESRCH_;
    }
    if (!loaded(h_handle)) return ESRCH_;
    if (!sz_symbol || !pp_address) return EFAULT_;
    char sz_nid[12];
    nid_of(sz_symbol,sz_nid);
    const RuntimeModule *p_module=&s_p_modules[h_handle];
    /* *address stays as it was when nothing matches: IL2CPP stores its fallback there first
     * (malloc for "scriptingGetMem") and calls what it holds afterwards. */
    for (uint64_t i=0;i<p_module->c_exports;++i)
        if (!strcmp(p_module->p_exports[i].sz_nid,sz_nid) || !strcmp(p_module->p_exports[i].sz_nid,sz_symbol)) {
            *pp_address=s_p_image+p_module->p_exports[i].va_export;
            return 0;
        }
    return ESRCH_;
}

static const RuntimeModule *find_address(uintptr_t va_guest) {
    for (size_t i=0;i<s_c_modules;++i)
        if (va_guest-(uintptr_t)(s_p_image+s_p_modules[i].base)<s_p_modules[i].cb_module) return &s_p_modules[i];
    return NULL;
}
typedef struct { uint64_t address; uint32_t size; int32_t prot; } GuestSegment;
typedef struct {
    uint64_t st_size;
    char name[256];
    int32_t id;
    uint32_t tls_index;
    uint64_t tls_init_addr;
    uint32_t tls_init_size, tls_size, tls_offset, tls_align;
    uint64_t init_proc_addr, fini_proc_addr, reserved1, reserved2, eh_frame_hdr_addr, eh_frame_addr;
    uint32_t eh_frame_hdr_size, eh_frame_size;
    GuestSegment segments[4];
    uint32_t segment_count;
} GuestModuleInfoEx;
typedef struct {
    uint64_t st_size;
    char name[256];
    uint64_t eh_frame_hdr_addr, eh_frame_addr, eh_frame_size, seg0_addr, seg0_size;
} GuestModuleInfoForUnwind;
/* The caller presets st_size; a non-zero result makes libSceSysmodule's initialization fail. */
typedef struct {
    uint64_t st_size;
    char name[256];
    GuestSegment segments[4];
    uint32_t segment_count;
    uint8_t fingerprint[20];
} GuestModuleInfo;
_Static_assert(sizeof(GuestModuleInfo)==0x160,"SceKernelModuleInfo layout");
static uint64_t absolute(uint64_t off_image) { return (uint64_t)(uintptr_t)(s_p_image+off_image); }
/* The fingerprint stays zero: the loader keeps no SELF fingerprint, and nothing read compares it. */
static ABI int32_t module_info(int32_t h_handle, GuestModuleInfo *p_inout_info) {
    if (!runtime_memory_is_guest_range(p_inout_info,sizeof(*p_inout_info))) return EFAULT_;
    if (p_inout_info->st_size<sizeof(*p_inout_info)) return EINVAL_;
    if (is_unlinked(h_handle) && __atomic_load_n(&s_c_unlinked_references[(size_t)h_handle-s_c_modules],__ATOMIC_ACQUIRE)) {
        if (!__atomic_fetch_add(&s_c_unlinked_infos,1,__ATOMIC_RELAXED))
            runtime_calls_hle_stub("sceKernelGetModuleInfo of a handle without an image (name only, no segments)",&s_c_unlinked_infos);
        memset(p_inout_info,0,sizeof(*p_inout_info));
        p_inout_info->st_size=sizeof(*p_inout_info);
        snprintf(p_inout_info->name,sizeof(p_inout_info->name),"%s.sprx",s_sz_system_libraries[(size_t)h_handle-s_c_modules]);
        return 0;
    }
    if (!loaded(h_handle)) return ESRCH_;
    const RuntimeModule *p_module=&s_p_modules[h_handle];
    memset(p_inout_info,0,sizeof(*p_inout_info));
    p_inout_info->st_size=sizeof(*p_inout_info);
    snprintf(p_inout_info->name,sizeof(p_inout_info->name),"%s",strrchr(p_module->sz_name,'/') ? strrchr(p_module->sz_name,'/')+1 : p_module->sz_name);
    for (uint64_t i=0;i<p_module->c_segments;++i)
        p_inout_info->segments[i]=(GuestSegment){absolute(p_module->segments[i].va_module_segment),(uint32_t)p_module->segments[i].cb_module_segment,(int32_t)p_module->segments[i].prot};
    p_inout_info->segment_count=(uint32_t)p_module->c_segments;
    return 0;
}
static ABI int32_t module_info_from_address(uintptr_t va_guest, int32_t flags, GuestModuleInfoEx *p_info) {
    if (flags>=3) { if (p_info) memset(p_info,0,sizeof(*p_info)); return EINVAL_; }
    if (!p_info) return EFAULT_;
    const RuntimeModule *p_module=find_address(va_guest);
    if (!p_module) return ESRCH_;
    memset(p_info,0,sizeof(*p_info));
    p_info->st_size=sizeof(*p_info);
    snprintf(p_info->name,sizeof(p_info->name),"%s",strrchr(p_module->sz_name,'/') ? strrchr(p_module->sz_name,'/')+1 : p_module->sz_name);
    p_info->id=(int32_t)(p_module-s_p_modules);
    p_info->tls_index=(uint32_t)p_module->tls_module;
    p_info->tls_init_addr=p_module->tls_module ? absolute(p_module->va_tls) : 0;
    p_info->tls_init_size=(uint32_t)p_module->tls_filesz;
    p_info->tls_size=(uint32_t)p_module->tls_memsz;
    p_info->init_proc_addr=p_module!=s_p_modules ? absolute(p_module->init) : 0;
    if (p_module->cb_eh_frame_hdr) p_info->eh_frame_hdr_addr=absolute(p_module->eh_frame_hdr);
    if (p_module->cb_eh_frame) p_info->eh_frame_addr=absolute(p_module->eh_frame);
    p_info->eh_frame_hdr_size=(uint32_t)p_module->cb_eh_frame_hdr;
    p_info->eh_frame_size=(uint32_t)p_module->cb_eh_frame;
    for (uint64_t i=0;i<p_module->c_segments;++i)
        p_info->segments[i]=(GuestSegment){absolute(p_module->segments[i].va_module_segment),(uint32_t)p_module->segments[i].cb_module_segment,(int32_t)p_module->segments[i].prot};
    p_info->segment_count=(uint32_t)p_module->c_segments;
    return 0;
}
static ABI int32_t module_info_for_unwind(uintptr_t va_guest, int32_t flags, GuestModuleInfoForUnwind *p_info) {
    if (flags>=3) { if (p_info) memset(p_info,0,sizeof(*p_info)); return EINVAL_; }
    if (!p_info) return EFAULT_;
    if (p_info->st_size<sizeof(*p_info)) return EINVAL_;
    const RuntimeModule *p_module=find_address(va_guest);
    if (!p_module) return ESRCH_;
    memset(p_info,0,sizeof(*p_info));
    p_info->st_size=sizeof(*p_info);
    snprintf(p_info->name,sizeof(p_info->name),"%s",strrchr(p_module->sz_name,'/') ? strrchr(p_module->sz_name,'/')+1 : p_module->sz_name);
    if (p_module->cb_eh_frame_hdr) p_info->eh_frame_hdr_addr=absolute(p_module->eh_frame_hdr);
    if (p_module->cb_eh_frame) p_info->eh_frame_addr=absolute(p_module->eh_frame);
    p_info->eh_frame_size=p_module->cb_eh_frame;
    if (p_module->c_segments) { p_info->seg0_addr=absolute(p_module->segments[0].va_module_segment); p_info->seg0_size=p_module->segments[0].cb_module_segment; }
    return 0;
}

static const RuntimeExport s_exports[]={
    {"sceKernelLoadStartModule",load_start_module}, {"sceKernelStopUnloadModule",stop_unload_module},
    {"sceKernelLoadStartModuleForSysmodule",load_start_module_for_sysmodule},
    {"sceKernelGetExecutableModuleHandle",executable_module_handle}, {"sceKernelGetModuleInfo",module_info},
    {"sceKernelDlsym",dlsym}, {"sceKernelGetModuleInfoFromAddr",module_info_from_address},
    {"sceKernelGetModuleInfoForUnwind",module_info_for_unwind},
    {"sceSysmoduleGetModuleInfoForUnwind",module_info_for_unwind},
};
uintptr_t runtime_module_resolve(const char *sz_name) { return s_c_modules ? RUNTIME_LOOKUP(s_exports,sz_name) : 0; }
const char *runtime_module_locate(uintptr_t va_guest, uint64_t *p_out_offset) {
    const RuntimeModule *p_module=find_address(va_guest);
    if (!p_module) return NULL;
    *p_out_offset=va_guest-(uintptr_t)(s_p_image+p_module->base);
    const char *sz_own=strrchr(p_module->sz_name,'/');
    return sz_own ? sz_own+1 : p_module->sz_name;
}
