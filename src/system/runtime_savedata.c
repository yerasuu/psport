/* libSceSaveData on host directories:
 *   <user>/savedata/<user id>/<title id>/<dir name>/        files the game writes
 *   <user>/savedata/<user id>/<title id>/<dir name>.sce_sys/ param.bin, icon0.png
 *   <user>/savedata/<user id>/<title id>.memory/memory.dat  SaveDataMemory
 *   <user>/savedata/<user id>/<title id>.memory<slot>/      SaveDataMemory2 slots above 0
 * A mounted directory appears to the guest as /savedata0../savedata15.
 * Metadata lives beside the directory so the guest's own files are untouched. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#ifndef _WIN32
#include <errno.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#define ERR_PARAMETER ((int32_t)0x809F0000)
#define ERR_NOT_INITIALIZED ((int32_t)0x809F0001)
#define ERR_NOT_MOUNTED ((int32_t)0x809F0004)
#define ERR_EXISTS ((int32_t)0x809F0007)
#define ERR_NOT_FOUND ((int32_t)0x809F0008)
#define ERR_INTERNAL ((int32_t)0x809F000B)
#define ERR_MOUNT_FULL ((int32_t)0x809F000C)
#define ERR_BAD_MOUNTED ((int32_t)0x809F000D)
#define ERR_INVALID_USER ((int32_t)0x809F0011)
#define ERR_MEMORY_NOT_READY ((int32_t)0x809F0012)
#define MODE_RDONLY 1
#define MODE_CREATE 4
#define MODE_COPY_ICON 16
#define MODE_CREATE2 32
#define MEMORY_SET_PARAM 1
#define SLOTS 16

typedef struct { char data[10]; char pad[6]; } TitleId;
typedef struct { char data[32]; } DirName;
typedef struct { char data[16]; } MountPoint;
typedef struct {
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param; int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
} Param;
typedef struct { int32_t user; int32_t pad; const TitleId *p_title; const DirName *p_dir; const void *p_fingerprint;
                 uint64_t blocks; uint32_t mode; uint8_t reserved[32]; } Mount1;
typedef struct { int32_t user; int32_t pad; const DirName *p_dir; uint64_t blocks; uint32_t mode;
                 uint8_t reserved[32]; int32_t pad2; } Mount2;
typedef struct { MountPoint point; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[28]; int32_t pad; } MountResult;
typedef struct { int32_t user; int32_t pad; const TitleId *p_title; const DirName *p_dir; uint32_t unused;
                 uint8_t reserved[32]; int32_t pad2; } Delete;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t key, order;
                 uint8_t reserved[32]; } SearchCond;
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } SearchInfo;
typedef struct { uint32_t hits; int32_t pad; DirName *p_names; uint32_t c_names_capacity, c_sets;
                 Param *p_params; SearchInfo *p_infos; uint8_t reserved[12]; int32_t pad2; } SearchResult;
typedef struct { const void *buffer; uint64_t buffer_size, data_size; uint8_t reserved[32]; } Icon;
typedef struct { uint32_t type; int32_t error, user, pad; TitleId title; DirName dir; uint8_t reserved[40]; } Event;
typedef struct { uint32_t option; int32_t user; uint64_t size, icon_size; const Param *param; const Icon *icon;
                 uint32_t slot; uint8_t reserved[20]; } MemorySetup2;
typedef struct { uint64_t existed_size; uint8_t reserved[16]; } MemorySetupResult;
typedef struct { void *buffer; uint64_t size; int64_t offset; uint8_t reserved[40]; } MemoryData;
typedef struct { int32_t user; int32_t pad; MemoryData *p_data; Param *p_param; Icon *p_icon; uint32_t slot;
                 uint8_t reserved[28]; } MemoryGet2;
typedef struct { int32_t user; int32_t pad; const MemoryData *p_data; const Param *p_param; const Icon *p_icon;
                 uint32_t c_data, slot; uint8_t reserved[24]; } MemorySet2;
_Static_assert(sizeof(Param)==1328,"OrbisSaveDataParam layout");
_Static_assert(sizeof(Mount1)==80 && sizeof(Mount2)==64,"OrbisSaveDataMount layouts");
_Static_assert(sizeof(MountResult)==64,"OrbisSaveDataMountResult layout");
_Static_assert(sizeof(SearchCond)==64 && sizeof(SearchResult)==56,"dir name search layouts");
_Static_assert(sizeof(Event)==104,"OrbisSaveDataEvent layout");
_Static_assert(sizeof(MemorySetup2)==64 && sizeof(MemorySetupResult)==24,"OrbisSaveDataMemorySetup2 layouts");
_Static_assert(sizeof(MemoryData)==64 && sizeof(MemoryGet2)==64 && sizeof(MemorySet2)==64,"SaveDataMemory2 layouts");
#define EVENT_UMOUNT_BACKUP 1
#define EVENT_BACKUP 2
#define EVENT_MEMORY_SYNC 3

static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static int s_is_initialized;
static char s_sz_title_id[16]="UNKNOWN";
static struct { int used; int32_t user; TitleId title; DirName dir; uint64_t c_blocks; char sz_host[700], sz_meta[700]; } s_slots[SLOTS];
static Event s_events[SLOTS];
static size_t s_c_events;
static struct { char sz_dir[640]; uint64_t cb_memory; } s_memories[SLOTS];
static size_t s_c_mounts_done, s_c_memory_writes;

void runtime_savedata_configure(const char *sz_title) {
    if (sz_title && *sz_title) snprintf(s_sz_title_id,sizeof(s_sz_title_id),"%s",sz_title);
}
const char *runtime_savedata_title(void) { return s_sz_title_id; }

static int make_dirs(const char *sz_path) {
    char sz_buffer[700];
    snprintf(sz_buffer,sizeof(sz_buffer),"%s",sz_path);
    for (char *sz_cursor=sz_buffer+1;*sz_cursor;++sz_cursor) if (*sz_cursor=='/') { *sz_cursor=0; ps_host_fs_make_dir(sz_buffer,0755); *sz_cursor='/'; }
    const int e=ps_host_fs_make_dir(sz_buffer,0755);
    return e && e!=EEXIST ? -1 : 0;
}
static void root(int32_t user, const char *sz_title, char *sz_out, size_t cb_out) {
    snprintf(sz_out,cb_out,"%s/savedata/%d/%s",runtime_file_user_dir(),user,sz_title && *sz_title ? sz_title : s_sz_title_id);
}
static int valid_name(const char *sz_name, size_t max) {
    size_t cb_name=strnlen(sz_name,max);
    if (!cb_name || cb_name==max) return 0;
    for (size_t i=0;i<cb_name;++i) if (sz_name[i]=='/' || sz_name[i]=='\\' || (sz_name[i]=='.' && (i==0 || sz_name[i-1]=='.'))) return 0;
    return 1;
}
/* A whole file replaced in one rename (a crash mid-write keeps the old one). */
static int write_atomic(const char *sz_path, const void *p_data, size_t cb_data) {
    char sz_temp[760]; snprintf(sz_temp,sizeof(sz_temp),"%s.bbtmp",sz_path);
    FILE *p_file=fopen(sz_temp,"wb");
    if (!p_file) return -1;
    int is_ok=fwrite(p_data,1,cb_data,p_file)==cb_data && !fflush(p_file) && !ps_host_fs_sync(fileno(p_file));
    is_ok=!fclose(p_file) && is_ok;
    if (!is_ok || ps_host_fs_rename(sz_temp,sz_path)) { ps_host_fs_remove(sz_temp); return -1; }
    return 0;
}
static void write_param(const char *sz_meta, const Param *p_param) {
    char sz_path[700]; snprintf(sz_path,sizeof(sz_path),"%s/param.bin",sz_meta);
    Param copy=*p_param; copy.mtime=time(NULL);
    write_atomic(sz_path,&copy,sizeof(copy));
}
static int read_param(const char *sz_meta, Param *p_param) {
    char sz_path[700]; snprintf(sz_path,sizeof(sz_path),"%s/param.bin",sz_meta);
    memset(p_param,0,sizeof(*p_param));
    FILE *p_file=fopen(sz_path,"rb");
    if (!p_file) return -1;
    size_t c_read=fread(p_param,sizeof(*p_param),1,p_file); fclose(p_file);
    PsHostStat st;
    if (!ps_host_fs_stat(sz_path,&st)) p_param->mtime=st.mtime.tv_sec;
    return c_read==1 ? 0 : -1;
}
static int remove_entry(const char *sz_path, const struct stat *p_stat, int flag, struct FTW *p_ftw) {
    (void)p_stat; (void)flag; (void)p_ftw; return remove(sz_path);
}

static ABI int32_t save_initialize(const void *p_param) { (void)p_param; s_is_initialized=1; return 0; }
static ABI int32_t save_terminate(void) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    s_is_initialized=0; return 0;
}
static int32_t mount(int32_t user, const char *sz_title, const DirName *p_dir, uint32_t mode, uint64_t c_blocks,
                     MountResult *p_result) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (user<0) return ERR_INVALID_USER;
    if (!p_dir || !p_result || !valid_name(p_dir->data,sizeof(p_dir->data))) return ERR_PARAMETER;
    char sz_base[600], sz_host[640], sz_meta[680];
    root(user,sz_title,sz_base,sizeof(sz_base));
    snprintf(sz_host,sizeof(sz_host),"%s/%s",sz_base,p_dir->data);
    snprintf(sz_meta,sizeof(sz_meta),"%s/%s.sce_sys",sz_base,p_dir->data);
    PsHostStat st;
    int exists=!ps_host_fs_stat(sz_host,&st) && PS_HOST_MODE_IS_DIRECTORY(st.mode);
    if ((mode & MODE_CREATE) && exists) return ERR_EXISTS;
    if (!(mode & (MODE_CREATE|MODE_CREATE2)) && !exists) return ERR_NOT_FOUND;
    pthread_mutex_lock(&s_lock);
    int slot=-1;
    for (int i=0;i<SLOTS;++i) {
        if (s_slots[i].used && !strcmp(s_slots[i].sz_host,sz_host)) { pthread_mutex_unlock(&s_lock); return ERR_BAD_MOUNTED; }
        if (!s_slots[i].used && slot<0) slot=i;
    }
    if (slot<0) { pthread_mutex_unlock(&s_lock); return ERR_MOUNT_FULL; }
    if (!exists && (make_dirs(sz_host) || make_dirs(sz_meta))) { pthread_mutex_unlock(&s_lock); return ERR_INTERNAL; }
    if (!exists) { Param empty={0}; write_param(sz_meta,&empty); }
    s_slots[slot].used=1;
    s_slots[slot].user=user;
    memset(&s_slots[slot].title,0,sizeof(TitleId));
    snprintf(s_slots[slot].title.data,sizeof(s_slots[slot].title.data),"%.9s",sz_title && *sz_title ? sz_title : s_sz_title_id);
    s_slots[slot].dir=*p_dir;
    s_slots[slot].c_blocks=c_blocks;
    snprintf(s_slots[slot].sz_host,sizeof(s_slots[slot].sz_host),"%s",sz_host);
    snprintf(s_slots[slot].sz_meta,sizeof(s_slots[slot].sz_meta),"%s",sz_meta);
    memset(p_result,0,sizeof(*p_result));
    char sz_point[32];
    snprintf(sz_point,sizeof(sz_point),"/savedata%d",slot&15);
    memcpy(p_result->point.data,sz_point,strlen(sz_point)+1);
    p_result->status=exists ? 0 : 1; /* CREATED */
    runtime_file_mount(p_result->point.data,sz_host);
    ++s_c_mounts_done;
    pthread_mutex_unlock(&s_lock);
    printf("Runtime: save data '%s' mounted at %s (%s%s)\n",p_dir->data,p_result->point.data,
           exists ? "existing" : "created",(mode & MODE_RDONLY) ? ", read-only" : "");
    return 0;
}
static ABI int32_t save_mount(const Mount1 *p_mount1, MountResult *p_result) {
    if (!p_mount1) return ERR_PARAMETER;
    return mount(p_mount1->user,p_mount1->p_title ? p_mount1->p_title->data : NULL,p_mount1->p_dir,p_mount1->mode,p_mount1->blocks,p_result);
}
static ABI int32_t save_mount2(const Mount2 *p_mount2, MountResult *p_result) {
    if (!p_mount2) return ERR_PARAMETER;
    return mount(p_mount2->user,NULL,p_mount2->p_dir,p_mount2->mode,p_mount2->blocks,p_result);
}
static int slot_of(const MountPoint *p_point) {
    if (!p_point || strncmp(p_point->data,"/savedata",9)) return -1;
    int slot=atoi(p_point->data+9);
    return slot>=0 && slot<SLOTS && s_slots[slot].used ? slot : -1;
}
static int32_t copy_save_locked(int32_t user, const TitleId *p_title, const char *sz_dir, int is_restore);
static int32_t umount(const MountPoint *p_point, int backup) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&s_lock);
    int slot=slot_of(p_point);
    if (slot>=0) {
        runtime_file_unmount(p_point->data); s_slots[slot].used=0;
        if (backup) copy_save_locked(s_slots[slot].user,&s_slots[slot].title,s_slots[slot].dir.data,0);
        if (backup && s_c_events<SLOTS)
            s_events[s_c_events++]=(Event){.type=EVENT_UMOUNT_BACKUP,.user=s_slots[slot].user,.title=s_slots[slot].title,.dir=s_slots[slot].dir};
    }
    pthread_mutex_unlock(&s_lock);
    return slot>=0 ? 0 : ERR_NOT_FOUND;
}
static ABI int32_t save_umount(const MountPoint *p_point) { return umount(p_point,0); }
#define BLOCK_SIZE_BYTES 32768
static uint64_t s_cb_used;
static int add_used_size(const char *sz_path, const struct stat *p_stat, int flag, struct FTW *p_ftw) {
    (void)sz_path; (void)p_ftw;
    if (flag==FTW_F) s_cb_used+=(uint64_t)p_stat->st_blocks*512;
    return 0;
}
/* OrbisSaveDataMountInfo: blocks and free blocks of the mounted save, the blocks it was mounted
 * with less the space its files take on the host. */
typedef struct { uint64_t blocks, freeBlocks; uint8_t reserved[32]; } MountInfo;
static ABI int32_t save_mount_info(const MountPoint *p_point, MountInfo *p_out_info) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (!p_point || !p_out_info) return ERR_PARAMETER;
    pthread_mutex_lock(&s_lock);
    int i_slot=slot_of(p_point);
    if (i_slot<0) { pthread_mutex_unlock(&s_lock); return ERR_NOT_MOUNTED; }
    s_cb_used=0;
    nftw(s_slots[i_slot].sz_host,add_used_size,16,FTW_PHYS);
    const uint64_t c_used_blocks=(s_cb_used+BLOCK_SIZE_BYTES-1)/BLOCK_SIZE_BYTES;
    memset(p_out_info,0,sizeof(*p_out_info));
    p_out_info->blocks=s_slots[i_slot].c_blocks;
    p_out_info->freeBlocks=c_used_blocks<s_slots[i_slot].c_blocks ? s_slots[i_slot].c_blocks-c_used_blocks : 0;
    pthread_mutex_unlock(&s_lock);
    return 0;
}
static ABI int32_t save_umount_with_backup(const MountPoint *p_point) { return umount(p_point,1); }
static ABI int32_t save_event_result(const void *p_param, Event *p_event) {
    (void)p_param;
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (!p_event) return ERR_PARAMETER;
    pthread_mutex_lock(&s_lock);
    const int is_found=s_c_events>0;
    if (is_found) { *p_event=s_events[0]; memmove(s_events,s_events+1,--s_c_events*sizeof(Event)); }
    pthread_mutex_unlock(&s_lock);
    return is_found ? 0 : ERR_NOT_FOUND;
}
static ABI int32_t save_set_param(const MountPoint *p_point, uint32_t type, const void *p_buffer, uint64_t cb_buffer) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (!p_buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&s_lock);
    int slot=slot_of(p_point);
    if (slot<0) { pthread_mutex_unlock(&s_lock); return ERR_NOT_MOUNTED; }
    Param p; read_param(s_slots[slot].sz_meta,&p);
    switch (type) {
    case 0: if (cb_buffer<sizeof(Param)) goto bad; memcpy(&p,p_buffer,sizeof(p)); break;     /* ALL */
    case 1: snprintf(p.title,sizeof(p.title),"%.*s",(int)cb_buffer,(const char *)p_buffer); break;
    case 2: snprintf(p.subtitle,sizeof(p.subtitle),"%.*s",(int)cb_buffer,(const char *)p_buffer); break;
    case 3: snprintf(p.detail,sizeof(p.detail),"%.*s",(int)cb_buffer,(const char *)p_buffer); break;
    case 4: if (cb_buffer<4) goto bad; memcpy(&p.user_param,p_buffer,4); break;
    default: goto bad;
    }
    write_param(s_slots[slot].sz_meta,&p);
    pthread_mutex_unlock(&s_lock);
    return 0;
bad:
    pthread_mutex_unlock(&s_lock);
    return ERR_PARAMETER;
}
static uint64_t copy_text(void *p_buffer, uint64_t cb_buffer, const char *sz_text, size_t c_capacity) {
    size_t length=strnlen(sz_text,c_capacity-1);
    if (length>=cb_buffer) length=cb_buffer-1;
    memcpy(p_buffer,sz_text,length); ((char *)p_buffer)[length]=0;
    return length+1;
}
static ABI int32_t save_get_param(const MountPoint *p_point, uint32_t type, void *p_buffer, uint64_t cb_buffer, uint64_t *p_got) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (!p_buffer || !cb_buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&s_lock);
    int slot=slot_of(p_point);
    Param p;
    if (slot>=0) read_param(s_slots[slot].sz_meta,&p);
    pthread_mutex_unlock(&s_lock);
    if (slot<0) return ERR_NOT_MOUNTED;
    uint64_t written;
    switch (type) {
    case 0: if (cb_buffer<sizeof(p)) return ERR_PARAMETER; memcpy(p_buffer,&p,written=sizeof(p)); break;
    case 1: written=copy_text(p_buffer,cb_buffer,p.title,sizeof(p.title)); break;
    case 2: written=copy_text(p_buffer,cb_buffer,p.subtitle,sizeof(p.subtitle)); break;
    case 3: written=copy_text(p_buffer,cb_buffer,p.detail,sizeof(p.detail)); break;
    case 4: if (cb_buffer<sizeof(p.user_param)) return ERR_PARAMETER; memcpy(p_buffer,&p.user_param,written=sizeof(p.user_param)); break;
    case 5: if (cb_buffer<sizeof(p.mtime)) return ERR_PARAMETER; memcpy(p_buffer,&p.mtime,written=sizeof(p.mtime)); break;
    default: return ERR_PARAMETER;
    }
    if (p_got) *p_got=written;
    return 0;
}
static ABI int32_t save_icon(const MountPoint *p_point, const Icon *p_icon) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (!p_icon || !p_icon->buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&s_lock);
    int slot=slot_of(p_point);
    int32_t r=ERR_NOT_MOUNTED;
    if (slot>=0) {
        char sz_path[700]; snprintf(sz_path,sizeof(sz_path),"%s/icon0.png",s_slots[slot].sz_meta);
        r=write_atomic(sz_path,p_icon->buffer,p_icon->data_size) ? ERR_INTERNAL : 0;
    }
    pthread_mutex_unlock(&s_lock);
    return r;
}
static ABI int32_t save_transferring_mount(const void *p_mount, void *p_result) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (!p_mount || !p_result) return ERR_PARAMETER;
    return ERR_NOT_FOUND;
}
static ABI int32_t save_delete(const Delete *p_delete) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (!p_delete || !p_delete->p_dir || !valid_name(p_delete->p_dir->data,sizeof(p_delete->p_dir->data))) return ERR_PARAMETER;
    char sz_base[600], sz_host[640], sz_meta[680];
    root(p_delete->user,p_delete->p_title ? p_delete->p_title->data : NULL,sz_base,sizeof(sz_base));
    snprintf(sz_host,sizeof(sz_host),"%s/%s",sz_base,p_delete->p_dir->data);
    snprintf(sz_meta,sizeof(sz_meta),"%s/%s.sce_sys",sz_base,p_delete->p_dir->data);
    PsHostStat st;
    if (ps_host_fs_stat(sz_host,&st)) return ERR_NOT_FOUND;
    nftw(sz_host,remove_entry,16,FTW_DEPTH|FTW_PHYS);
    nftw(sz_meta,remove_entry,16,FTW_DEPTH|FTW_PHYS);
    printf("Runtime: save data '%s' deleted\n",p_delete->p_dir->data);
    return 0;
}
/* SQL-LIKE pattern: % any run, _ one character. */
static int like(const char *sz_text, const char *sz_pattern) {
    if (!*sz_pattern) return !*sz_text;
    if (*sz_pattern=='%') { for (;;++sz_text) { if (like(sz_text,sz_pattern+1)) return 1; if (!*sz_text) return 0; } }
    return *sz_text && (*sz_pattern=='_' || *sz_pattern==*sz_text) && like(sz_text+1,sz_pattern+1);
}
typedef struct { char sz_name[32]; Param param; } Entry;
static uint32_t s_sort_key, s_sort_order;
static int compare(const void *p_a, const void *p_b) {
    const Entry *p_entry_a=p_a, *p_entry_b=p_b;
    int c = s_sort_key==1 ? (p_entry_a->param.user_param>p_entry_b->param.user_param)-(p_entry_a->param.user_param<p_entry_b->param.user_param)
          : s_sort_key==3 ? (p_entry_a->param.mtime>p_entry_b->param.mtime)-(p_entry_a->param.mtime<p_entry_b->param.mtime)
          : strcmp(p_entry_a->sz_name,p_entry_b->sz_name);
    return s_sort_order ? -c : c;
}
static ABI int32_t save_search(const SearchCond *p_cond, SearchResult *p_result) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (!p_cond || !p_result) return ERR_PARAMETER;
    char sz_base[600];
    root(p_cond->user,p_cond->title ? p_cond->title->data : NULL,sz_base,sizeof(sz_base));
    Entry *p_entries=NULL; size_t c_entries=0, c_capacity=0;
    PsHostDir *p_dir=ps_host_dir_open(sz_base);
    for (PsHostDirEntry entry; p_dir && ps_host_dir_next(p_dir,&entry);) {
        size_t cb_name=strlen(entry.sz_name);
        if (entry.sz_name[0]=='.' || cb_name>=32 || (cb_name>8 && !strcmp(entry.sz_name+cb_name-8,".sce_sys"))) continue;
        if (p_cond->dir && p_cond->dir->data[0] && !like(entry.sz_name,p_cond->dir->data)) continue;
        if (c_entries==c_capacity) { c_capacity=c_capacity ? c_capacity*2 : 16; p_entries=realloc(p_entries,c_capacity*sizeof(*p_entries)); }
        snprintf(p_entries[c_entries].sz_name,32,"%s",entry.sz_name);
        char sz_meta[680]; snprintf(sz_meta,sizeof(sz_meta),"%s/%s.sce_sys",sz_base,entry.sz_name);
        read_param(sz_meta,&p_entries[c_entries].param);
        ++c_entries;
    }
    if (p_dir) ps_host_dir_close(p_dir);
    s_sort_key=p_cond->key; s_sort_order=p_cond->order;
    if (c_entries) qsort(p_entries,c_entries,sizeof(*p_entries),compare);
    p_result->hits=(uint32_t)c_entries;
    uint32_t set=c_entries<p_result->c_names_capacity ? (uint32_t)c_entries : p_result->c_names_capacity;
    for (uint32_t i=0;i<set;++i) {
        if (p_result->p_names) memcpy(p_result->p_names[i].data,p_entries[i].sz_name,32);
        if (p_result->p_params) p_result->p_params[i]=p_entries[i].param;
        if (p_result->p_infos) { memset(&p_result->p_infos[i],0,sizeof(SearchInfo)); p_result->p_infos[i].blocks=32768; p_result->p_infos[i].free_blocks=16384; }
    }
    p_result->c_sets=set;
    free(p_entries);
    return 0;
}
/* SaveDataMemory: one fixed-size blob per user/title and memory slot. */
static int32_t memory_open(int32_t user, uint32_t slot, uint64_t cb_memory, uint64_t *p_existed) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (user<0) return ERR_INVALID_USER;
    if (!cb_memory || slot>=SLOTS) return ERR_PARAMETER;
    char sz_base[600], sz_dir[640], sz_path[700];
    root(user,NULL,sz_base,sizeof(sz_base));
    if (slot) snprintf(sz_dir,sizeof(sz_dir),"%s.memory%u",sz_base,slot);
    else snprintf(sz_dir,sizeof(sz_dir),"%s.memory",sz_base);
    if (make_dirs(sz_dir)) return ERR_INTERNAL;
    snprintf(sz_path,sizeof(sz_path),"%s/memory.dat",sz_dir);
    pthread_mutex_lock(&s_lock);
    FILE *p_file=fopen(sz_path,"r+b");
    if (!p_file) p_file=fopen(sz_path,"w+b");
    int32_t r=ERR_INTERNAL;
    uint64_t old=0;
    if (p_file) {
        fseek(p_file,0,SEEK_END);
        old=(uint64_t)ftell(p_file);
        if (old<cb_memory && ps_host_fs_truncate(fileno(p_file),(int64_t)cb_memory)) goto done;
        *p_existed=old;
        snprintf(s_memories[slot].sz_dir,sizeof(s_memories[slot].sz_dir),"%s",sz_dir);
        s_memories[slot].cb_memory=cb_memory; r=0;
    done:
        fclose(p_file);
    }
    pthread_mutex_unlock(&s_lock);
    if (!r) printf("Runtime: save data memory %u ready (%llu bytes, %llu existed)\n",slot,(unsigned long long)cb_memory,
                   (unsigned long long)old);
    return r;
}
/* memory_ready, memory_io and memory_meta run under the lock. */
static int32_t memory_ready(uint32_t slot) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (slot>=SLOTS) return ERR_PARAMETER;
    return s_memories[slot].cb_memory ? 0 : ERR_MEMORY_NOT_READY;
}
static int32_t memory_io(uint32_t slot, void *p_buffer, uint64_t cb_buffer, int64_t off_memory, int write) {
    if (!p_buffer || off_memory<0) return ERR_PARAMETER;
    if ((uint64_t)off_memory+cb_buffer>s_memories[slot].cb_memory) return ERR_PARAMETER;
    char sz_path[700]; snprintf(sz_path,sizeof(sz_path),"%s/memory.dat",s_memories[slot].sz_dir);
    FILE *p_file=fopen(sz_path,"r+b");
    int32_t r=ERR_INTERNAL;
    if (p_file && !fseek(p_file,off_memory,SEEK_SET) &&
        (write ? fwrite(p_buffer,1,cb_buffer,p_file) : fread(p_buffer,1,cb_buffer,p_file))==cb_buffer) r=0;
    if (p_file) fclose(p_file);
    if (!r && write) ++s_c_memory_writes;
    return r;
}
static int32_t memory_meta(uint32_t slot, const Param *p_param, const Icon *p_icon) {
    if (p_param) write_param(s_memories[slot].sz_dir,p_param);
    if (!p_icon || !p_icon->buffer) return 0;
    char sz_path[700]; snprintf(sz_path,sizeof(sz_path),"%s/icon0.png",s_memories[slot].sz_dir);
    return write_atomic(sz_path,p_icon->buffer,p_icon->data_size) ? ERR_INTERNAL : 0;
}
static int32_t memory_transfer(void *p_buffer, uint64_t cb_buffer, int64_t off_memory, int write) {
    pthread_mutex_lock(&s_lock);
    int32_t r=memory_ready(0);
    if (!r) r=memory_io(0,p_buffer,cb_buffer,off_memory,write);
    pthread_mutex_unlock(&s_lock);
    return r;
}
static ABI int32_t memory_setup(int32_t user, uint64_t cb_memory, const Param *p_param) {
    uint64_t existed;
    int32_t r=memory_open(user,0,cb_memory,&existed);
    if (r) return r;
    pthread_mutex_lock(&s_lock);
    r=memory_meta(0,p_param,NULL);
    pthread_mutex_unlock(&s_lock);
    return r;
}
static ABI int32_t memory_get(int32_t user, void *p_buffer, uint64_t cb_buffer, int64_t off_memory) { (void)user; return memory_transfer(p_buffer,cb_buffer,off_memory,0); }
static ABI int32_t memory_set(int32_t user, void *p_buffer, uint64_t cb_buffer, int64_t off_memory) { (void)user; return memory_transfer(p_buffer,cb_buffer,off_memory,1); }
/* initParam and initIcon describe a new memory; MEMORY_SET_PARAM also replaces an existing one's param. */
static ABI int32_t memory_setup2(const MemorySetup2 *p_setup, MemorySetupResult *p_result) {
    if (!p_setup) return ERR_PARAMETER;
    uint64_t existed;
    int32_t r=memory_open(p_setup->user,p_setup->slot,p_setup->size,&existed);
    if (r) return r;
    pthread_mutex_lock(&s_lock);
    r=memory_meta(p_setup->slot,!existed || p_setup->option&MEMORY_SET_PARAM ? p_setup->param : NULL,
                  existed ? NULL : p_setup->icon);
    pthread_mutex_unlock(&s_lock);
    if (!r && p_result) { memset(p_result,0,sizeof(*p_result)); p_result->existed_size=existed; }
    return r;
}
static ABI int32_t memory_get2(const MemoryGet2 *p_get) {
    if (!p_get) return ERR_PARAMETER;
    pthread_mutex_lock(&s_lock);
    int32_t r=memory_ready(p_get->slot);
    if (!r && p_get->p_data) r=memory_io(p_get->slot,p_get->p_data->buffer,p_get->p_data->size,p_get->p_data->offset,0);
    if (!r && p_get->p_param) read_param(s_memories[p_get->slot].sz_dir,p_get->p_param);
    if (!r && p_get->p_icon && p_get->p_icon->buffer) {
        char sz_path[700]; snprintf(sz_path,sizeof(sz_path),"%s/icon0.png",s_memories[p_get->slot].sz_dir);
        FILE *p_file=fopen(sz_path,"rb");
        p_get->p_icon->data_size=p_file ? fread((void *)p_get->p_icon->buffer,1,p_get->p_icon->buffer_size,p_file) : 0;
        if (p_file) fclose(p_file);
    }
    pthread_mutex_unlock(&s_lock);
    return r;
}
/* A zero dataNum with data set is one element, since callers built that way leave the field zeroed. */
static ABI int32_t memory_set2(const MemorySet2 *p_set) {
    if (!p_set) return ERR_PARAMETER;
    pthread_mutex_lock(&s_lock);
    int32_t r=memory_ready(p_set->slot);
    for (uint32_t i=0;!r && p_set->p_data && i<(p_set->c_data ? p_set->c_data : 1);++i)
        r=memory_io(p_set->slot,p_set->p_data[i].buffer,p_set->p_data[i].size,p_set->p_data[i].offset,1);
    if (!r) r=memory_meta(p_set->slot,p_set->p_param,p_set->p_icon);
    pthread_mutex_unlock(&s_lock);
    return r;
}

/* Backups: <saves>/.backup/<dir> and its .sce_sys beside it, out of DirNameSearch's sight (it skips
 * names starting with a dot). Each request completes before it returns, so the progress is always
 * full and the event the game waits for is already queued. */
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *param;
                 uint8_t reserved[32]; } BackupRequest;
typedef struct { int32_t user; int32_t pad; const TitleId *p_title; const DirName *p_dir; Param *p_param; Icon *p_icon;
                 uint8_t reserved[32]; } BackupCheck;
typedef struct { int32_t user; int32_t pad; const TitleId *p_title; const DirName *p_dir; const void *p_fingerprint;
                 uint32_t unused; uint8_t reserved[32]; int32_t pad2; } BackupRestore;
typedef struct { int32_t user; uint32_t slot, option; uint8_t reserved[28]; } MemorySync;
_Static_assert(sizeof(BackupRequest)==64 && sizeof(BackupCheck)==72 && sizeof(BackupRestore)==72,"backup layouts");
static const char *s_sz_copy_source, *s_sz_copy_target;
static int copy_entry(const char *sz_path, const struct stat *p_stat, int flag, struct FTW *p_ftw) {
    (void)p_stat; (void)p_ftw;
    char sz_target[1400];
    snprintf(sz_target,sizeof(sz_target),"%s%s",s_sz_copy_target,sz_path+strlen(s_sz_copy_source));
    if (flag==FTW_D) { const int e=ps_host_fs_make_dir(sz_target,0755); return e && e!=EEXIST ? -1 : 0; }
    if (flag!=FTW_F) return 0;
    FILE *p_in=fopen(sz_path,"rb"), *p_out=p_in ? fopen(sz_target,"wb") : NULL;
    int is_copied=p_out!=NULL;
    char sz_buffer[65536];
    for (size_t cb_read; is_copied && (cb_read=fread(sz_buffer,1,sizeof(sz_buffer),p_in));)
        is_copied=fwrite(sz_buffer,1,cb_read,p_out)==cb_read;
    if (p_out) is_copied=!fclose(p_out) && is_copied && !ferror(p_in);
    if (p_in) fclose(p_in);
    return is_copied ? 0 : -1;
}
/* Replaces sz_target with a copy of the folder sz_source; requires lock. */
static int copy_tree_locked(const char *sz_source, const char *sz_target) {
    nftw(sz_target,remove_entry,16,FTW_DEPTH|FTW_PHYS);
    s_sz_copy_source=sz_source; s_sz_copy_target=sz_target;
    return nftw(sz_source,copy_entry,16,FTW_PHYS);
}
static void save_paths(int32_t user, const TitleId *p_title, const char *sz_dir, int is_backup, char *sz_out_host,
                       char *sz_out_meta) {
    char sz_base[600];
    root(user,p_title ? p_title->data : NULL,sz_base,sizeof(sz_base));
    snprintf(sz_out_host,640,"%s%s/%s",sz_base,is_backup ? "/.backup" : "",sz_dir);
    snprintf(sz_out_meta,680,"%s%s/%s.sce_sys",sz_base,is_backup ? "/.backup" : "",sz_dir);
}
static int is_folder(const char *sz_path) { PsHostStat st; return !ps_host_fs_stat(sz_path,&st) && PS_HOST_MODE_IS_DIRECTORY(st.mode); }
static int32_t copy_save_locked(int32_t user, const TitleId *p_title, const char *sz_dir, int is_restore) {
    char sz_host[640], sz_meta[680], sz_backup_host[640], sz_backup_meta[680];
    save_paths(user,p_title,sz_dir,0,sz_host,sz_meta);
    save_paths(user,p_title,sz_dir,1,sz_backup_host,sz_backup_meta);
    const char *sz_from_host=is_restore ? sz_backup_host : sz_host, *sz_from_meta=is_restore ? sz_backup_meta : sz_meta;
    if (!is_folder(sz_from_host)) return ERR_NOT_FOUND;
    if (!is_restore && make_dirs(sz_backup_host)) return ERR_INTERNAL;
    const int has_failed=copy_tree_locked(sz_from_host,is_restore ? sz_host : sz_backup_host) ||
                         (is_folder(sz_from_meta) && copy_tree_locked(sz_from_meta,is_restore ? sz_meta : sz_backup_meta));
    printf("Runtime: save data '%s' %s%s\n",sz_dir,is_restore ? "restored from its backup" : "backed up",
           has_failed ? " (failed)" : "");
    return has_failed ? ERR_INTERNAL : 0;
}
static void queue_event_locked(uint32_t type, int32_t user, const TitleId *p_title, const DirName *p_dir) {
    if (s_c_events>=SLOTS) return;
    Event event={.type=type,.user=user};
    if (p_title) event.title=*p_title;
    else snprintf(event.title.data,sizeof(event.title.data),"%.9s",s_sz_title_id);
    if (p_dir) event.dir=*p_dir;
    s_events[s_c_events++]=event;
}
static ABI int32_t save_backup(const BackupRequest *p_request) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (!p_request || !p_request->dir || !valid_name(p_request->dir->data,sizeof(p_request->dir->data))) return ERR_PARAMETER;
    pthread_mutex_lock(&s_lock);
    const int32_t result=copy_save_locked(p_request->user,p_request->title,p_request->dir->data,0);
    if (!result) queue_event_locked(EVENT_BACKUP,p_request->user,p_request->title,p_request->dir);
    pthread_mutex_unlock(&s_lock);
    return result;
}
static ABI int32_t save_check_backup(const BackupCheck *p_check) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (!p_check || !p_check->p_dir || !valid_name(p_check->p_dir->data,sizeof(p_check->p_dir->data))) return ERR_PARAMETER;
    char sz_host[640], sz_meta[680];
    save_paths(p_check->user,p_check->p_title,p_check->p_dir->data,1,sz_host,sz_meta);
    if (!is_folder(sz_host)) return ERR_NOT_FOUND;
    if (p_check->p_param) read_param(sz_meta,p_check->p_param);
    if (p_check->p_icon && p_check->p_icon->buffer) {
        char sz_icon[700];
        snprintf(sz_icon,sizeof(sz_icon),"%s/icon0.png",sz_meta);
        FILE *p_file=fopen(sz_icon,"rb");
        uint64_t cb_icon=p_file ? fread((void *)p_check->p_icon->buffer,1,p_check->p_icon->buffer_size,p_file) : 0;
        if (p_file) fclose(p_file);
        p_check->p_icon->data_size=cb_icon;
    }
    return 0;
}
static ABI int32_t save_restore_backup(const BackupRestore *p_restore) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (!p_restore || !p_restore->p_dir || !valid_name(p_restore->p_dir->data,sizeof(p_restore->p_dir->data))) return ERR_PARAMETER;
    pthread_mutex_lock(&s_lock);
    const int32_t result=copy_save_locked(p_restore->user,p_restore->p_title,p_restore->p_dir->data,1);
    pthread_mutex_unlock(&s_lock);
    return result;
}
static ABI int32_t save_progress(float *p_out_progress) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (!p_out_progress) return ERR_PARAMETER;
    *p_out_progress=1.0f;
    return 0;
}
static ABI int32_t save_load_icon(const MountPoint *p_point, Icon *p_icon) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (!p_icon || !p_icon->buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&s_lock);
    const int i_slot=slot_of(p_point);
    int32_t result=ERR_NOT_MOUNTED;
    if (i_slot>=0) {
        char sz_icon[700];
        snprintf(sz_icon,sizeof(sz_icon),"%s/icon0.png",s_slots[i_slot].sz_meta);
        FILE *p_file=fopen(sz_icon,"rb");
        result=p_file ? 0 : ERR_NOT_FOUND;
        if (p_file) { p_icon->data_size=fread((void *)p_icon->buffer,1,p_icon->buffer_size,p_file); fclose(p_file); }
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}
/* Save data memory is written to its file on every set; syncing makes it durable. */
static ABI int32_t memory_sync(const MemorySync *p_sync) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (!p_sync || p_sync->slot>=SLOTS) return ERR_PARAMETER;
    pthread_mutex_lock(&s_lock);
    int32_t result=ERR_MEMORY_NOT_READY;
    if (s_memories[p_sync->slot].cb_memory) {
        char sz_path[700];
        snprintf(sz_path,sizeof(sz_path),"%s/memory.dat",s_memories[p_sync->slot].sz_dir);
        FILE *p_file=fopen(sz_path,"r+b");
        result=p_file && !ps_host_fs_sync(fileno(p_file)) ? 0 : ERR_INTERNAL;
        if (p_file) fclose(p_file);
        if (!result) queue_event_locked(EVENT_MEMORY_SYNC,p_sync->user,NULL,NULL);
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}
static ABI int32_t save_clear_progress(void) { return s_is_initialized ? 0 : ERR_NOT_INITIALIZED; }

static const RuntimeExport s_exports[]={
    {"sceSaveDataInitialize",save_initialize}, {"sceSaveDataInitialize2",save_initialize},
    {"sceSaveDataInitialize3",save_initialize}, {"sceSaveDataTerminate",save_terminate},
    {"sceSaveDataMount",save_mount}, {"sceSaveDataMount2",save_mount2}, {"sceSaveDataUmount",save_umount},
    {"sceSaveDataUmountWithBackup",save_umount_with_backup}, {"sceSaveDataGetEventResult",save_event_result},
    {"sceSaveDataSetParam",save_set_param}, {"sceSaveDataGetParam",save_get_param},
    {"sceSaveDataSaveIcon",save_icon}, {"sceSaveDataDelete",save_delete},
    {"sceSaveDataDirNameSearch",save_search}, {"sceSaveDataTransferringMount",save_transferring_mount},
    {"sceSaveDataSetupSaveDataMemory",memory_setup}, {"sceSaveDataGetSaveDataMemory",memory_get},
    {"sceSaveDataSetSaveDataMemory",memory_set}, {"sceSaveDataSetupSaveDataMemory2",memory_setup2},
    {"sceSaveDataGetSaveDataMemory2",memory_get2}, {"sceSaveDataSetSaveDataMemory2",memory_set2},
    {"sceSaveDataClearProgress",save_clear_progress}, {"sceSaveDataGetMountInfo",save_mount_info},
    {"sceSaveDataBackup",save_backup}, {"sceSaveDataCheckBackupData",save_check_backup},
    {"sceSaveDataRestoreBackupData",save_restore_backup}, {"sceSaveDataGetProgress",save_progress},
    {"sceSaveDataLoadIcon",save_load_icon}, {"sceSaveDataSyncSaveDataMemory",memory_sync},
};
uintptr_t runtime_savedata_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
int runtime_savedata_param(int32_t user, const char *sz_title, const char *sz_dir, void *p_param) {
    char sz_base[600], sz_path[680];
    PsHostStat st;
    if (!valid_name(sz_dir,32)) return -1;
    root(user,sz_title,sz_base,sizeof(sz_base));
    snprintf(sz_path,sizeof(sz_path),"%s/%s",sz_base,sz_dir);
    if (ps_host_fs_stat(sz_path,&st) || !PS_HOST_MODE_IS_DIRECTORY(st.mode)) return -1;
    snprintf(sz_path,sizeof(sz_path),"%s/%s.sce_sys",sz_base,sz_dir);
    read_param(sz_path,p_param);
    return 0;
}
void runtime_savedata_report(void) { printf("Runtime: save data mounts=%zu, memory writes=%zu\n",s_c_mounts_done,s_c_memory_writes); }
#else
void runtime_savedata_configure(const char *title) { (void)title; }
uintptr_t runtime_savedata_resolve(const char *name) { (void)name; return 0; }
int runtime_savedata_param(int32_t user, const char *title, const char *dir, void *param) { (void)user; (void)title; (void)dir; (void)param; return -1; }
void runtime_savedata_report(void) {}
#endif
