/* Offline AppContent provider for the configured base-game profile.
 * No packages, downloads or license emulation; add-on folders in <user>/addcont are reported as
 * installed and entitled (see addon_list), and mounted as /addcont<n> when the game asks. */
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
#define NOT_FOUND ((int32_t)0x80d90005)
#define MOUNT_FULL ((int32_t)0x80d90006)
#define DRM_NO_ENTITLEMENT ((int32_t)0x80d90007)
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
    if (!configured) { fputs("STOP: AppContent needs --app0 (its param.sfo)\n",stderr); runtime_stop(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); runtime_stop(21); }
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
    if (internal_refs[index]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); runtime_stop(21); }
    ++internal_refs[index];
    if (result) *result=0;
    printf("Runtime: internal system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); runtime_stop(21); }
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
    if (!initialized) { fputs("STOP: AppContent query before initialization\n",stderr); runtime_stop(21); }
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
 * each reported as installed. Data is mounted only when the game asks (addon_mount): Bloodborne:
 * The Old Hunters (SPEXPANSIONDLC03) needs only the entitlement, its data ships with patch 1.09. Without a list
 * the count is returned, with one at most capacity entries are written. Never touch unused list
 * slots. */
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    require_initialized();
    if (service) { fputs("STOP: nonzero AppContent service label unsupported\n",stderr); runtime_stop(21); }
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
/* SceNpUnifiedEntitlementLabel: 16 characters, NUL, padding. Finds the add-on's folder under any
 * title of <user>/addcont, the folders addon_list reports. */
static int find_addon_folder(const char *sz_label,char *sz_out_path,size_t cb_out_path) {
    if (!sz_label || !memchr(sz_label,0,LABEL_SIZE) || !sz_label[0] || strchr(sz_label,'/') || sz_label[0]=='.') return -1;
    char sz_root[4096];
    snprintf(sz_root,sizeof(sz_root),"%s/addcont",runtime_file_user_dir());
    DIR *p_titles=opendir(sz_root);
    int is_found=0;
    for (struct dirent *p_title; p_titles && !is_found && (p_title=readdir(p_titles));) {
        if (p_title->d_name[0]=='.') continue;
        struct stat entry;
        if (snprintf(sz_out_path,cb_out_path,"%s/%s/%s",sz_root,p_title->d_name,sz_label)>=(int)cb_out_path) continue;
        is_found=!stat(sz_out_path,&entry) && S_ISDIR(entry.st_mode);
    }
    if (p_titles) closedir(p_titles);
    return is_found ? 0 : -1;
}
static ABI int32_t addon_info(uint32_t service,const char *sz_label,AddonInfo *p_out_info) {
    require_initialized();
    if (service || !sz_label || !p_out_info) return PARAMETER;
    char sz_folder[4096];
    if (find_addon_folder(sz_label,sz_folder,sizeof(sz_folder))) return DRM_NO_ENTITLEMENT;
    memset(p_out_info,0,sizeof(*p_out_info));
    memcpy(p_out_info->label,sz_label,strlen(sz_label));
    p_out_info->status=ADDON_INSTALLED;
    return 0;
}
/* SceAppContentEntitlementKey: 16 bytes. Add-on data here is never encrypted, so an installed
 * add-on's key is all zero. */
static ABI int32_t addon_entitlement_key(uint32_t service,const char *sz_label,uint8_t *p_out_key) {
    require_initialized();
    if (service || !sz_label || !p_out_key) return PARAMETER;
    char sz_folder[4096];
    if (find_addon_folder(sz_label,sz_folder,sizeof(sz_folder))) return DRM_NO_ENTITLEMENT;
    memset(p_out_key,0,16);
    return 0;
}
#define ADDON_MOUNT_COUNT 16
static char s_addon_mounted_labels[ADDON_MOUNT_COUNT][LABEL_SIZE];
static ABI int32_t addon_mount(uint32_t service,const char *sz_label,char *sz_out_mount) {
    require_initialized();
    if (service || !sz_label || !sz_out_mount) return PARAMETER;
    char sz_folder[4096];
    if (find_addon_folder(sz_label,sz_folder,sizeof(sz_folder))) return NOT_FOUND;
    int i_free=-1, i_mount=-1;
    for (int i=0;i<ADDON_MOUNT_COUNT;++i) {
        if (!strcmp(s_addon_mounted_labels[i],sz_label)) i_mount=i;
        else if (!s_addon_mounted_labels[i][0] && i_free<0) i_free=i;
    }
    if (i_mount<0) i_mount=i_free;
    if (i_mount<0) return MOUNT_FULL;
    char sz_guest[32];
    snprintf(sz_guest,sizeof(sz_guest),"/addcont%d",i_mount);
    if (runtime_file_mount(sz_guest,sz_folder)) return MOUNT_FULL;
    snprintf(s_addon_mounted_labels[i_mount],LABEL_SIZE,"%s",sz_label);
    memset(sz_out_mount,0,16); memcpy(sz_out_mount,sz_guest,strlen(sz_guest));
    printf("Runtime: AppContent add-on %s mounted at %s -> %s\n",sz_label,sz_guest,sz_folder);
    return 0;
}
static ABI int32_t addon_unmount(const char *sz_mount) {
    require_initialized();
    int i_mount;
    if (!sz_mount || !memchr(sz_mount,0,16) || sscanf(sz_mount,"/addcont%d",&i_mount)!=1 || i_mount<0 ||
        i_mount>=ADDON_MOUNT_COUNT)
        return PARAMETER;
    if (!s_addon_mounted_labels[i_mount][0]) return NOT_MOUNTED;
    runtime_file_unmount(sz_mount);
    s_addon_mounted_labels[i_mount][0]=0;
    return 0;
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
    if ((mkdir(parent,0755) && errno!=EEXIST) || (mkdir(host,0755) && errno!=EEXIST) || (option && temp_format())) {
        fprintf(stderr,"Runtime: AppContent temporary data mount of %s failed: %s\n",host,strerror(errno));
        return PARAMETER;
    }
    if (runtime_file_mount(TEMP_MOUNT,host)) {
        fprintf(stderr,"Runtime: AppContent temporary data mount of %s failed: mount table full or path too long\n",host);
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
    {"sceAppContentGetAddcontInfoList",addon_list}, {"sceAppContentGetAddcontInfo",addon_info},
    {"sceAppContentGetEntitlementKey",addon_entitlement_key}, {"sceAppContentAddcontMount",addon_mount},
    {"sceAppContentAddcontUnmount",addon_unmount},
    {"sceAppContentDownloadDataGetAvailableSpaceKb",download_space},
    {"sceAppContentTemporaryDataMount2",temp_mount}, {"sceAppContentTemporaryDataUnmount",temp_unmount},
    {"sceAppContentTemporaryDataFormat",temp_format_mounted}, {"sceAppContentTemporaryDataGetAvailableSpaceKb",temp_space},
};
uintptr_t runtime_content_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",refs,initialized,queries,lists);
}
