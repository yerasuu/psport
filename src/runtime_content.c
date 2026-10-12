/* Offline AppContent provider for the configured base-game profile.
 * No package/DLC mounting, downloads, entitlement or license emulation; add-on folders in
 * <user>/addcont are reported as installed (see addon_list). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define NOT_MOUNTED ((int32_t)0x80d90004)
static unsigned refs, initialized, configured;
static char title_id[16]="UNKNOWN";
static uint32_t parameters[5];
static unsigned queries, lists;
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
void runtime_content_title(const char *title) {
    if (title && *title) snprintf(title_id,sizeof(title_id),"%s",title);
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); exit(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
/* Internal ids (bit 31 set) name what the SDK's own libraries load; those are the host's too, so
 * loading one counts a reference and reports the start result of a module that has none: 0. */
static unsigned internal_refs[0x10000];
static ABI int32_t module_load_internal(uint32_t id,int32_t argc,const void *argv,uint64_t reserved,int32_t *result) {
    (void)argc; (void)argv;
    const uint32_t index=id&0x7fffffff;
    if (!index || index>=0x10000 || reserved) return INVALID_ID;
    if (internal_refs[index]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    ++internal_refs[index];
    if (result) *result=0;
    printf("Runtime: internal system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); exit(21); }
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    require_loaded();
    if (!init || !boot) return PARAMETER;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    /* Every module of the process shares the library, and each initializes it: Unity's engine and
     * its libil2cpp both do, and the second one gives up on temporary data if refused. */
    memset(boot,0,40);
    if (initialized++) { printf("Runtime: AppContent initialized again (%u calls)\n",initialized); return 0; }
    puts("Runtime: AppContent initialized; offline base-game profile, no mounted add-ons");
    return 0;
}
static void require_initialized(void) {
    require_loaded();
    if (!initialized) { fputs("STOP: AppContent query before initialization\n",stderr); exit(21); }
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    require_initialized();
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
/* SceAppContentAddcontInfo: entitlement label (16 chars + NUL), padding, download status. */
#define LABEL_SIZE 17
#define ADDON_INSTALLED 4u
typedef struct { char label[LABEL_SIZE]; char padding[3]; uint32_t status; } AddonInfo;
/* Add-ons are the folders <user>/addcont/<title id>/<entitlement label> (shadPS4's layout),
 * each reported as installed. No add-on data is mounted: Bloodborne: The Old Hunters
 * (SPEXPANSIONDLC03) needs only the entitlement, its data ships with patch 1.09. Without a list
 * the count is returned, with one at most capacity entries are written. Never touch unused list
 * slots. */
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    require_initialized();
    if (service) { fputs("STOP: nonzero AppContent service label unsupported\n",stderr); exit(21); }
    if ((!capacity || !list) && !hits) return PARAMETER;
    int fill=list && capacity;
    uint32_t count=0;
    char root[4096];
    snprintf(root,sizeof(root),"%s/addcont",runtime_file_user_dir());
    DIR *titles=opendir(root);
    for (struct dirent *t; titles && (t=readdir(titles));) {
        if (t->d_name[0]=='.') continue;
        int title=openat(dirfd(titles),t->d_name,O_RDONLY|O_DIRECTORY);
        DIR *labels=title>=0 ? fdopendir(title) : NULL;
        if (!labels && title>=0) close(title);
        for (struct dirent *l; labels && (l=readdir(labels));) {
            struct stat entry;
            size_t n=strlen(l->d_name);
            if (l->d_name[0]=='.' || n>=LABEL_SIZE || fstatat(dirfd(labels),l->d_name,&entry,0) ||
                !S_ISDIR(entry.st_mode) || (fill && count>=capacity)) continue;
            if (fill) {
                AddonInfo info;
                memset(&info,'\0',sizeof(info)); memcpy(info.label,l->d_name,n);
                info.status=ADDON_INSTALLED;
                memcpy((unsigned char *)list+(size_t)count*sizeof(info),&info,sizeof(info));
            }
            ++count;
        }
        if (labels) closedir(labels);
    }
    if (titles) closedir(titles);
    if (hits) *hits=count;
    if (count) printf("Runtime: AppContent add-ons reported installed (%s): %u\n",root,(unsigned)count);
    ++lists; return 0;
}
/* SceAppContentMountPoint: the mount point's path, NUL-terminated within 16 bytes. The space left
 * for download data is the free space of the host folder behind it. */
static ABI int32_t download_space(const char *mount,uint64_t *kb) {
    require_initialized();
    if (!mount || !kb || !memchr(mount,0,16)) return PARAMETER;
    char host[4096];
    struct statvfs fs;
    if (runtime_file_translate(mount,host,sizeof(host)) || statvfs(host,&fs)) return PARAMETER;
    *kb=(uint64_t)fs.f_bavail*fs.f_frsize/1024;
    return 0;
}
/* Temporary data is /temp0, backed by <user>/temp/<title id> so titles sharing a user folder don't
 * see each other's files. It survives the run until the title formats it. */
#define TEMP_MOUNT "/temp0"
static unsigned temp_mounted;
static void temp_folder(char *out,size_t size) { snprintf(out,size,"%s/temp/%s",runtime_file_user_dir(),title_id); }
static int remove_entry(const char *path,const struct stat *st,int type,struct FTW *ftw) {
    (void)st; (void)type;
    return ftw->level ? remove(path) : 0;
}
static int temp_format(void) {
    char host[4096];
    temp_folder(host,sizeof(host));
    return nftw(host,remove_entry,16,FTW_DEPTH|FTW_PHYS) && errno!=ENOENT ? -1 : 0;
}
static int temp_point(const char *mount) { return mount && memchr(mount,0,16) && !strcmp(mount,TEMP_MOUNT); }
static ABI int32_t temp_mount(uint32_t option,char *mount) {
    require_initialized();
    if (!mount || option>1) return PARAMETER;
    char host[4096];
    temp_folder(host,sizeof(host));
    char parent[4096];
    snprintf(parent,sizeof(parent),"%s/temp",runtime_file_user_dir());
    if ((mkdir(parent,0755) && errno!=EEXIST) || (mkdir(host,0755) && errno!=EEXIST) || (option && temp_format()) ||
        runtime_file_mount(TEMP_MOUNT,host)) {
        fprintf(stderr,"Runtime: AppContent temporary data mount of %s failed: %s\n",host,strerror(errno));
        return PARAMETER;
    }
    memset(mount,0,16); memcpy(mount,TEMP_MOUNT,sizeof(TEMP_MOUNT));
    printf("Runtime: AppContent temporary data mounted at %s -> %s%s (mounts=%u)\n",TEMP_MOUNT,host,option ? ", formatted" : "",++temp_mounted);
    return 0;
}
static ABI int32_t temp_unmount(const char *mount) {
    require_initialized();
    if (!temp_point(mount)) return PARAMETER;
    if (!temp_mounted) return NOT_MOUNTED;
    if (!--temp_mounted) runtime_file_unmount(TEMP_MOUNT);
    printf("Runtime: AppContent temporary data unmounted (mounts=%u)\n",temp_mounted);
    return 0;
}
static ABI int32_t temp_format_mounted(const char *mount) {
    require_initialized();
    if (!temp_point(mount)) return PARAMETER;
    if (!temp_mounted) return NOT_MOUNTED;
    if (temp_format()) { fprintf(stderr,"Runtime: AppContent temporary data format failed: %s\n",strerror(errno)); return PARAMETER; }
    puts("Runtime: AppContent temporary data formatted");
    return 0;
}
static ABI int32_t temp_space(const char *mount,uint64_t *kb) {
    require_initialized();
    if (!temp_point(mount) || !kb) return PARAMETER;
    if (!temp_mounted) return NOT_MOUNTED;
    char host[4096];
    struct statvfs fs;
    temp_folder(host,sizeof(host));
    if (statvfs(host,&fs)) return PARAMETER;
    *kb=(uint64_t)fs.f_bavail*fs.f_frsize/1024;
    return 0;
}
static const RuntimeExport exports[]={
    {"sceSysmoduleLoadModule",module_load}, {"sceSysmoduleIsLoaded",module_loaded},
    {"sceSysmoduleUnloadModule",module_unload}, {"sceSysmoduleLoadModuleInternalWithArg",module_load_internal},
    {"sceAppContentInitialize",content_init}, {"sceAppContentAppParamGetInt",param_int},
    {"sceAppContentGetAddcontInfoList",addon_list},
    {"sceAppContentDownloadDataGetAvailableSpaceKb",download_space},
    {"sceAppContentTemporaryDataMount2",temp_mount}, {"sceAppContentTemporaryDataUnmount",temp_unmount},
    {"sceAppContentTemporaryDataFormat",temp_format_mounted}, {"sceAppContentTemporaryDataGetAvailableSpaceKb",temp_space},
};
uintptr_t runtime_content_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",refs,initialized,queries,lists);
}
