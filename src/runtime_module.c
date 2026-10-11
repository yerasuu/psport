/* libkernel module services over what link_modules.py prelinked. Modules the game loads by path
 * (Unity's Media/Modules, Media/Plugins) are already in the image, relocated and bound; loading
 * one runs its initializer once, as the PS4 linker's module_start. Handles are table indices
 * (0 the eboot), as shadPS4 numbers its modules. */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define ENOENT_ ERR(2)
#define ESRCH_ ERR(3)
#define EFAULT_ ERR(14)
#define EINVAL_ ERR(22)

static unsigned char *image;
static RuntimeModule *modules;
static size_t module_count;
/* 0 unstarted, 1 started. Recursive: a module_start may load another module. */
static unsigned char *started;
static pthread_mutex_t start_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

void runtime_modules_configure(unsigned char *base, RuntimeModule *table, size_t count, unsigned char *state) {
    image=base; modules=table; module_count=count; started=state;
}
int runtime_module_start(size_t handle, uint64_t args, const void *argp, int32_t *result) {
    if (handle>=module_count || !handle) return 0;
    pthread_mutex_lock(&start_lock);
    int32_t value=0;
    int ran=!started[handle];
    if (ran) {
        started[handle]=1; /* before the call: a module_start loading itself must not recurse */
        typedef int32_t (ABI *ModuleStart)(uint64_t,const void *,void *);
        value=((ModuleStart)(image+modules[handle].init))(args,argp,NULL);
    }
    pthread_mutex_unlock(&start_lock);
    if (result) *result=value;
    return ran;
}

/* NID = first 8 bytes of SHA-1(name + salt), read little-endian, base64 with '+' '-'
 * (shadPS4 module.cpp StringToNid). */
static void sha1(const unsigned char *data, size_t length, unsigned char out[20]) {
    uint32_t h[5]={0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
    uint64_t bits=(uint64_t)length*8;
    size_t total=(length+9+63)/64*64;
    for (size_t block=0;block<total;block+=64) {
        uint32_t w[80];
        for (int i=0;i<64;++i) {
            size_t at=block+(size_t)i;
            unsigned char byte=at<length ? data[at] : at==length ? 0x80 : at>=total-8 ? (unsigned char)(bits>>(8*(total-1-at))) : 0;
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
static void nid_of(const char *name, char out[12]) {
    static const unsigned char salt[16]={0x51,0x8D,0x64,0xA6,0x35,0xDE,0xD8,0xC1,0xE6,0xB0,0x39,0xB1,0xC3,0xE5,0x52,0x30};
    static const char codes[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    size_t length=strlen(name);
    unsigned char small[256], hash[20];
    /* Mangled C++ names run past any fixed buffer, and a truncated one hashes to another NID. */
    unsigned char *input=length+sizeof(salt)<=sizeof(small) ? small : malloc(length+sizeof(salt));
    if (!input) { out[0]=0; return; }
    memcpy(input,name,length); memcpy(input+length,salt,sizeof(salt));
    sha1(input,length+sizeof(salt),hash);
    if (input!=small) free(input);
    uint64_t digest=0;
    for (int i=7;i>=0;--i) digest=digest<<8|hash[i];
    for (int i=0;i<10;++i) out[i]=codes[(digest>>(58-i*6))&0x3f];
    out[10]=codes[(digest&0xf)*4];
    out[11]=0;
}

/* Guest path to table index: /app0-relative path, else the file name, both case-insensitive
 * (Unity builds the path from its data folder, "/app0/Media" or "/app0/media"). */
static size_t find_path(const char *path) {
    const char *relative=!strncasecmp(path,"/app0/",6) ? path+6 : path;
    const char *base=strrchr(path,'/');
    base=base ? base+1 : path;
    for (size_t i=1;i<module_count;++i)
        if (!strcasecmp(modules[i].name,relative)) return i;
    for (size_t i=1;i<module_count;++i) {
        const char *own=strrchr(modules[i].name,'/');
        if (!strcasecmp(own ? own+1 : modules[i].name,base)) return i;
    }
    return 0;
}
static ABI int32_t load_start_module(const char *path, uint64_t args, const void *argp, uint32_t flags,
                                     const void *options, int32_t *result) {
    (void)options;
    if (!path) return EINVAL_;
    size_t handle=find_path(path);
    if (!handle) {
        printf("Runtime: sceKernelLoadStartModule %s: no such module linked\n",path);
        return ENOENT_;
    }
    int32_t value=0;
    if (runtime_module_start(handle,args,argp,&value))
        printf("Runtime: started module %s (handle %zu, flags %#x): module_start returned %d\n",
               modules[handle].name,handle,flags,value);
    if (result) *result=value;
    return (int32_t)handle;
}
/* Nothing is unmapped: the image stays one mapping and a later load finds the module started. */
static ABI int32_t stop_unload_module(int32_t handle, uint64_t args, const void *argp, uint32_t flags,
                                      const void *options, int32_t *result) {
    (void)args; (void)argp; (void)flags; (void)options;
    if (handle<0 || (size_t)handle>=module_count) return ESRCH_;
    if (result) *result=0;
    return 0;
}
static ABI int32_t dlsym(int32_t handle, const char *symbol, void **address) {
    if (handle<0 || (size_t)handle>=module_count) return ESRCH_;
    if (!symbol || !address) return EFAULT_;
    char nid[12];
    nid_of(symbol,nid);
    const RuntimeModule *m=&modules[handle];
    /* *address stays as it was when nothing matches: IL2CPP stores its fallback there first
     * (malloc for "scriptingGetMem") and calls what it holds afterwards. */
    for (uint64_t i=0;i<m->export_count;++i)
        if (!strcmp(m->exports[i].nid,nid) || !strcmp(m->exports[i].nid,symbol)) {
            *address=image+m->exports[i].address;
            return 0;
        }
    return ESRCH_;
}

static const RuntimeModule *find_address(uintptr_t address) {
    for (size_t i=0;i<module_count;++i)
        if (address-(uintptr_t)(image+modules[i].base)<modules[i].size) return &modules[i];
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
static uint64_t absolute(uint64_t offset) { return (uint64_t)(uintptr_t)(image+offset); }
static ABI int32_t module_info_from_address(uintptr_t address, int32_t flags, GuestModuleInfoEx *info) {
    if (flags>=3) { if (info) memset(info,0,sizeof(*info)); return EINVAL_; }
    if (!info) return EFAULT_;
    const RuntimeModule *m=find_address(address);
    if (!m) return ESRCH_;
    memset(info,0,sizeof(*info));
    info->st_size=sizeof(*info);
    snprintf(info->name,sizeof(info->name),"%s",strrchr(m->name,'/') ? strrchr(m->name,'/')+1 : m->name);
    info->id=(int32_t)(m-modules);
    info->tls_index=(uint32_t)m->tls_module;
    info->tls_init_addr=m->tls_module ? absolute(m->tls_address) : 0;
    info->tls_init_size=(uint32_t)m->tls_filesz;
    info->tls_size=(uint32_t)m->tls_memsz;
    info->init_proc_addr=m!=modules ? absolute(m->init) : 0;
    if (m->eh_frame_hdr_size) info->eh_frame_hdr_addr=absolute(m->eh_frame_hdr);
    if (m->eh_frame_size) info->eh_frame_addr=absolute(m->eh_frame);
    info->eh_frame_hdr_size=(uint32_t)m->eh_frame_hdr_size;
    info->eh_frame_size=(uint32_t)m->eh_frame_size;
    for (uint64_t i=0;i<m->segment_count;++i)
        info->segments[i]=(GuestSegment){absolute(m->segments[i].address),(uint32_t)m->segments[i].size,(int32_t)m->segments[i].prot};
    info->segment_count=(uint32_t)m->segment_count;
    return 0;
}
static ABI int32_t module_info_for_unwind(uintptr_t address, int32_t flags, GuestModuleInfoForUnwind *info) {
    if (flags>=3) { if (info) memset(info,0,sizeof(*info)); return EINVAL_; }
    if (!info) return EFAULT_;
    if (info->st_size<sizeof(*info)) return EINVAL_;
    const RuntimeModule *m=find_address(address);
    if (!m) return ESRCH_;
    memset(info,0,sizeof(*info));
    info->st_size=sizeof(*info);
    snprintf(info->name,sizeof(info->name),"%s",strrchr(m->name,'/') ? strrchr(m->name,'/')+1 : m->name);
    if (m->eh_frame_hdr_size) info->eh_frame_hdr_addr=absolute(m->eh_frame_hdr);
    if (m->eh_frame_size) info->eh_frame_addr=absolute(m->eh_frame);
    info->eh_frame_size=m->eh_frame_size;
    if (m->segment_count) { info->seg0_addr=absolute(m->segments[0].address); info->seg0_size=m->segments[0].size; }
    return 0;
}

static const RuntimeExport exports[]={
    {"sceKernelLoadStartModule",load_start_module}, {"sceKernelStopUnloadModule",stop_unload_module},
    {"sceKernelDlsym",dlsym}, {"sceKernelGetModuleInfoFromAddr",module_info_from_address},
    {"sceKernelGetModuleInfoForUnwind",module_info_for_unwind},
    {"sceSysmoduleGetModuleInfoForUnwind",module_info_for_unwind},
};
uintptr_t runtime_module_resolve(const char *name) { return module_count ? RUNTIME_LOOKUP(exports,name) : 0; }
