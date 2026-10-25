/* Offline AppContent provider for the configured base-game profile.
 * No packages, downloads or license emulation; add-on folders in <user>/addcont are reported as
 * installed and entitled (see addon_list), and mounted as /addcont<n> when the game asks. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define NOT_MOUNTED ((int32_t)0x80d90004)
#define NOT_FOUND ((int32_t)0x80d90005)
#define MOUNT_FULL ((int32_t)0x80d90006)
#define DRM_NO_ENTITLEMENT ((int32_t)0x80d90007)
static unsigned s_c_refs, s_c_initializations, s_configured;
static char s_sz_title_id[16]="UNKNOWN";
static uint32_t s_parameters[5];
static unsigned s_c_queries, s_c_lists;
void runtime_content_configure(const uint32_t values[5]) {
    if (s_c_refs || s_c_initializations || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    memcpy(s_parameters,values,sizeof(s_parameters)); s_configured=1;
}
void runtime_content_title(const char *sz_title) {
    if (sz_title && *sz_title) snprintf(s_sz_title_id,sizeof(s_sz_title_id),"%s",sz_title);
}
static void require_provider(void) {
    if (!s_configured) { fputs("STOP: AppContent needs --app0 (its param.sfo)\n",stderr); runtime_stop(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned s_c_module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (s_c_module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); runtime_stop(21); }
    ++s_c_module_refs[id];
    if (id==0xb4) { s_c_refs=s_c_module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",s_c_refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return s_c_module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --s_c_module_refs[id];
    if (id==0xb4) { s_c_refs=s_c_module_refs[id]; if (!s_c_refs) s_c_initializations=0; }
    return 0;
}
/* Internal ids (bit 31 set) name what the SDK's own libraries load; those are the host's too, so
 * loading one counts a reference and reports the start result of a module that has none: 0. */
static unsigned s_c_internal_refs[0x10000];
static ABI int32_t module_load_internal(uint32_t id,int32_t argc,const void *p_argv,uint64_t reserved,int32_t *p_result) {
    (void)argc; (void)p_argv;
    const uint32_t i_module=id&0x7fffffff;
    if (!i_module || i_module>=0x10000 || reserved) return INVALID_ID;
    if (s_c_internal_refs[i_module]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); runtime_stop(21); }
    ++s_c_internal_refs[i_module];
    if (p_result) *p_result=0;
    printf("Runtime: internal system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static ABI int32_t module_unload_internal(uint32_t id,int32_t argc,const void *p_argv,uint64_t flags,int32_t *p_result) {
    (void)argc; (void)p_argv;
    const uint32_t i_module=id&0x7fffffff;
    if (!i_module || i_module>=0x10000 || flags) return INVALID_ID;
    if (!s_c_internal_refs[i_module]) return NOT_LOADED;
    --s_c_internal_refs[i_module];
    if (p_result) *p_result=0;
    printf("Runtime: internal system module 0x%x unloaded\n",id);
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!s_c_refs) { fputs("STOP: AppContent called before module load\n",stderr); runtime_stop(21); }
}
static ABI int32_t content_init(const unsigned char *p_init,unsigned char *p_boot) {
    require_loaded();
    if (!p_init || !p_boot) return PARAMETER;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (p_init[i]) return PARAMETER;
    /* Every module of the process shares the library, and each initializes it: Unity's engine and
     * its libil2cpp both do, and the second one gives up on temporary data if refused. */
    memset(p_boot,0,40);
    if (s_c_initializations++) { printf("Runtime: AppContent initialized again (%u calls)\n",s_c_initializations); return 0; }
    puts("Runtime: AppContent initialized; offline base-game profile, no mounted add-ons");
    return 0;
}
static void require_initialized(void) {
    require_loaded();
    if (!s_c_initializations) { fputs("STOP: AppContent query before initialization\n",stderr); runtime_stop(21); }
}
static ABI int32_t param_int(uint32_t id,int32_t *p_out) {
    require_initialized();
    if (id>4 || !p_out) return PARAMETER;
    memcpy(p_out,&s_parameters[id],4); ++s_c_queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*p_out);
    return 0;
}
/* SceAppContentAddcontInfo: entitlement label (16 chars + NUL), padding, download status. */
#define LABEL_SIZE 17
#define ADDON_INSTALLED 4u
typedef struct { char label[LABEL_SIZE]; char padding[3]; uint32_t status; } AddonInfo;
/* Add-ons are the folders <user>/addcont/<title id>/<entitlement label> (shadPS4's layout),
 * each reported as installed. Data is mounted only when the game asks (addon_mount): an add-on
 * whose data ships with the game or its patch needs only the entitlement. Without a list
 * the count is returned, with one at most capacity entries are written. Never touch unused list
 * slots. */
static ABI int32_t addon_list(uint32_t service,void *p_list,uint32_t c_capacity,uint32_t *p_hits) {
    require_initialized();
    if (service) { fputs("STOP: nonzero AppContent service label unsupported\n",stderr); runtime_stop(21); }
    if ((!c_capacity || !p_list) && !p_hits) return PARAMETER;
    int fill=p_list && c_capacity;
    uint32_t c_found=0;
    char sz_root[4096];
    snprintf(sz_root,sizeof(sz_root),"%s/addcont",runtime_file_user_dir());
    PsHostDir *p_titles=ps_host_dir_open(sz_root);
    for (PsHostDirEntry title; p_titles && ps_host_dir_next(p_titles,&title);) {
        if (title.sz_name[0]=='.') continue;
        char sz_title[4352];
        PsHostDir *p_labels=(size_t)snprintf(sz_title,sizeof(sz_title),"%s/%s",sz_root,title.sz_name)<sizeof(sz_title) ? ps_host_dir_open(sz_title) : NULL;
        for (PsHostDirEntry label; p_labels && ps_host_dir_next(p_labels,&label);) {
            PsHostStat entry;
            size_t cb_label=strlen(label.sz_name);
            if (label.sz_name[0]=='.' || cb_label>=LABEL_SIZE || ps_host_dir_stat(p_labels,label.sz_name,&entry) ||
                !PS_HOST_MODE_IS_DIRECTORY(entry.mode) || (fill && c_found>=c_capacity)) continue;
            if (fill) {
                AddonInfo info;
                memset(&info,'\0',sizeof(info)); memcpy(info.label,label.sz_name,cb_label);
                info.status=ADDON_INSTALLED;
                memcpy((unsigned char *)p_list+(size_t)c_found*sizeof(info),&info,sizeof(info));
            }
            ++c_found;
        }
        if (p_labels) ps_host_dir_close(p_labels);
    }
    if (p_titles) ps_host_dir_close(p_titles);
    if (p_hits) *p_hits=c_found;
    if (c_found) printf("Runtime: AppContent add-ons reported installed (%s): %u\n",sz_root,(unsigned)c_found);
    ++s_c_lists; return 0;
}
/* SceNpUnifiedEntitlementLabel: 16 characters, NUL, padding. Finds the add-on's folder under any
 * title of <user>/addcont, the folders addon_list reports. */
static int find_addon_folder(const char *sz_label,char *sz_out_path,size_t cb_out_path) {
    if (!sz_label || !memchr(sz_label,0,LABEL_SIZE) || !sz_label[0] || strchr(sz_label,'/') || sz_label[0]=='.') return -1;
    char sz_root[4096];
    snprintf(sz_root,sizeof(sz_root),"%s/addcont",runtime_file_user_dir());
    PsHostDir *p_titles=ps_host_dir_open(sz_root);
    int is_found=0;
    for (PsHostDirEntry title; p_titles && !is_found && ps_host_dir_next(p_titles,&title);) {
        if (title.sz_name[0]=='.') continue;
        PsHostStat entry;
        if (snprintf(sz_out_path,cb_out_path,"%s/%s/%s",sz_root,title.sz_name,sz_label)>=(int)cb_out_path) continue;
        is_found=!ps_host_fs_stat(sz_out_path,&entry) && PS_HOST_MODE_IS_DIRECTORY(entry.mode);
    }
    if (p_titles) ps_host_dir_close(p_titles);
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
static ABI int32_t download_space(const char *sz_mount,uint64_t *p_kb) {
    require_initialized();
    if (!sz_mount || !p_kb || !memchr(sz_mount,0,16)) return PARAMETER;
    char sz_host[4096];
    uint64_t cb_free;
    if (runtime_file_translate(sz_mount,sz_host,sizeof(sz_host)) || ps_host_fs_free_space(sz_host,&cb_free)) return PARAMETER;
    *p_kb=cb_free/1024;
    return 0;
}
/* Temporary data is /temp0, backed by <user>/temp/<title id> so titles sharing a user folder don't
 * see each other's files. It survives the run until the title formats it. */
#define TEMP_MOUNT "/temp0"
static unsigned s_c_temp_mounts;
static void temp_folder(char *sz_out,size_t cb_out) { snprintf(sz_out,cb_out,"%s/temp/%s",runtime_file_user_dir(),s_sz_title_id); }
static int temp_format(void) {
    char sz_host[4096];
    temp_folder(sz_host,sizeof(sz_host));
    const int error=ps_host_fs_remove_tree(sz_host,1);
    return error && error!=ENOENT ? -1 : 0;
}
static int temp_point(const char *sz_mount) { return sz_mount && memchr(sz_mount,0,16) && !strcmp(sz_mount,TEMP_MOUNT); }
static ABI int32_t temp_mount(uint32_t option,char *sz_mount) {
    require_initialized();
    if (!sz_mount || option>1) return PARAMETER;
    char sz_host[4096];
    temp_folder(sz_host,sizeof(sz_host));
    char sz_parent[4096];
    snprintf(sz_parent,sizeof(sz_parent),"%s/temp",runtime_file_user_dir());
    int error=ps_host_fs_make_dir(sz_parent,0755);
    if (!error || error==EEXIST) error=ps_host_fs_make_dir(sz_host,0755);
    if (error==EEXIST) error=0;
    if (error || (option && temp_format())) {
        fprintf(stderr,"Runtime: AppContent temporary data mount of %s failed: %s\n",sz_host,strerror(error ? error : errno));
        return PARAMETER;
    }
    if (runtime_file_mount(TEMP_MOUNT,sz_host)) {
        fprintf(stderr,"Runtime: AppContent temporary data mount of %s failed: mount table full or path too long\n",sz_host);
        return PARAMETER;
    }
    memset(sz_mount,0,16); memcpy(sz_mount,TEMP_MOUNT,sizeof(TEMP_MOUNT));
    printf("Runtime: AppContent temporary data mounted at %s -> %s%s (mounts=%u)\n",TEMP_MOUNT,sz_host,option ? ", formatted" : "",++s_c_temp_mounts);
    return 0;
}
static ABI int32_t temp_unmount(const char *sz_mount) {
    require_initialized();
    if (!temp_point(sz_mount)) return PARAMETER;
    if (!s_c_temp_mounts) return NOT_MOUNTED;
    if (!--s_c_temp_mounts) runtime_file_unmount(TEMP_MOUNT);
    printf("Runtime: AppContent temporary data unmounted (mounts=%u)\n",s_c_temp_mounts);
    return 0;
}
static ABI int32_t temp_format_mounted(const char *sz_mount) {
    require_initialized();
    if (!temp_point(sz_mount)) return PARAMETER;
    if (!s_c_temp_mounts) return NOT_MOUNTED;
    if (temp_format()) { fprintf(stderr,"Runtime: AppContent temporary data format failed: %s\n",strerror(errno)); return PARAMETER; }
    puts("Runtime: AppContent temporary data formatted");
    return 0;
}
static ABI int32_t temp_space(const char *sz_mount,uint64_t *p_kb) {
    require_initialized();
    if (!temp_point(sz_mount) || !p_kb) return PARAMETER;
    if (!s_c_temp_mounts) return NOT_MOUNTED;
    char sz_host[4096];
    uint64_t cb_free;
    temp_folder(sz_host,sizeof(sz_host));
    if (ps_host_fs_free_space(sz_host,&cb_free)) return PARAMETER;
    *p_kb=cb_free/1024;
    return 0;
}
static const RuntimeExport s_exports[]={
    {"sceSysmoduleLoadModule",module_load}, {"sceSysmoduleIsLoaded",module_loaded},
    {"sceSysmoduleUnloadModule",module_unload}, {"sceSysmoduleLoadModuleInternalWithArg",module_load_internal},
    {"sceSysmoduleUnloadModuleInternalWithArg",module_unload_internal},
    {"sceAppContentInitialize",content_init}, {"sceAppContentAppParamGetInt",param_int},
    {"sceAppContentGetAddcontInfoList",addon_list}, {"sceAppContentGetAddcontInfo",addon_info},
    {"sceAppContentGetEntitlementKey",addon_entitlement_key}, {"sceAppContentAddcontMount",addon_mount},
    {"sceAppContentAddcontUnmount",addon_unmount},
    {"sceAppContentDownloadDataGetAvailableSpaceKb",download_space},
    {"sceAppContentTemporaryDataMount2",temp_mount}, {"sceAppContentTemporaryDataUnmount",temp_unmount},
    {"sceAppContentTemporaryDataFormat",temp_format_mounted}, {"sceAppContentTemporaryDataGetAvailableSpaceKb",temp_space},
};
uintptr_t runtime_content_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",s_c_refs,s_c_initializations,s_c_queries,s_c_lists);
}
