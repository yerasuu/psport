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
#include <dirent.h>
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
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint64_t blocks; uint32_t mode; uint8_t reserved[32]; } Mount1;
typedef struct { int32_t user; int32_t pad; const DirName *dir; uint64_t blocks; uint32_t mode;
                 uint8_t reserved[32]; int32_t pad2; } Mount2;
typedef struct { MountPoint point; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[28]; int32_t pad; } MountResult;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t unused;
                 uint8_t reserved[32]; int32_t pad2; } Delete;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t key, order;
                 uint8_t reserved[32]; } SearchCond;
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } SearchInfo;
typedef struct { uint32_t hits; int32_t pad; DirName *names; uint32_t names_capacity, set_count;
                 Param *params; SearchInfo *infos; uint8_t reserved[12]; int32_t pad2; } SearchResult;
typedef struct { const void *buffer; uint64_t buffer_size, data_size; uint8_t reserved[32]; } Icon;
typedef struct { uint32_t type; int32_t error, user, pad; TitleId title; DirName dir; uint8_t reserved[40]; } Event;
typedef struct { uint32_t option; int32_t user; uint64_t size, icon_size; const Param *param; const Icon *icon;
                 uint32_t slot; uint8_t reserved[20]; } MemorySetup2;
typedef struct { uint64_t existed_size; uint8_t reserved[16]; } MemorySetupResult;
typedef struct { void *buffer; uint64_t size; int64_t offset; uint8_t reserved[40]; } MemoryData;
typedef struct { int32_t user; int32_t pad; MemoryData *data; Param *param; Icon *icon; uint32_t slot;
                 uint8_t reserved[28]; } MemoryGet2;
typedef struct { int32_t user; int32_t pad; const MemoryData *data; const Param *param; const Icon *icon;
                 uint32_t data_count, slot; uint8_t reserved[24]; } MemorySet2;
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

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized;
static char title_id[16]="UNKNOWN";
static struct { int used; int32_t user; TitleId title; DirName dir; uint64_t c_blocks; char host[700], meta[700]; } slots[SLOTS];
static Event events[SLOTS];
static size_t event_count;
static struct { char dir[640]; uint64_t size; } memories[SLOTS];
static size_t mounts_done, memory_writes;

void runtime_savedata_configure(const char *sz_title) {
    if (sz_title && *sz_title) snprintf(title_id,sizeof(title_id),"%s",sz_title);
}
const char *runtime_savedata_title(void) { return title_id; }

static int make_dirs(const char *path) {
    char buffer[700];
    snprintf(buffer,sizeof(buffer),"%s",path);
    for (char *p=buffer+1;*p;++p) if (*p=='/') { *p=0; mkdir(buffer,0755); *p='/'; }
    return mkdir(buffer,0755) && errno!=EEXIST ? -1 : 0;
}
static void root(int32_t user, const char *title, char *out, size_t size) {
    snprintf(out,size,"%s/savedata/%d/%s",runtime_file_user_dir(),user,title && *title ? title : title_id);
}
static int valid_name(const char *name, size_t max) {
    size_t n=strnlen(name,max);
    if (!n || n==max) return 0;
    for (size_t i=0;i<n;++i) if (name[i]=='/' || name[i]=='\\' || (name[i]=='.' && (i==0 || name[i-1]=='.'))) return 0;
    return 1;
}
/* A whole file replaced in one rename (a crash mid-write keeps the old one). */
static int write_atomic(const char *path, const void *data, size_t size) {
    char temp[760]; snprintf(temp,sizeof(temp),"%s.bbtmp",path);
    FILE *f=fopen(temp,"wb");
    if (!f) return -1;
    int ok=fwrite(data,1,size,f)==size && !fflush(f) && !fsync(fileno(f));
    ok=!fclose(f) && ok;
    if (!ok || rename(temp,path)) { unlink(temp); return -1; }
    return 0;
}
static void write_param(const char *meta, const Param *p) {
    char path[700]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    Param copy=*p; copy.mtime=time(NULL);
    write_atomic(path,&copy,sizeof(copy));
}
static int read_param(const char *meta, Param *p) {
    char path[700]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    memset(p,0,sizeof(*p));
    FILE *f=fopen(path,"rb");
    if (!f) return -1;
    size_t n=fread(p,sizeof(*p),1,f); fclose(f);
    struct stat st;
    if (!stat(path,&st)) p->mtime=st.st_mtime;
    return n==1 ? 0 : -1;
}
static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw; return remove(path);
}

static ABI int32_t save_initialize(const void *param) { (void)param; initialized=1; return 0; }
static ABI int32_t save_terminate(void) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    initialized=0; return 0;
}
static int32_t mount(int32_t user, const char *title, const DirName *dir, uint32_t mode, uint64_t c_blocks,
                     MountResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user<0) return ERR_INVALID_USER;
    if (!dir || !result || !valid_name(dir->data,sizeof(dir->data))) return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(user,title,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,dir->data);
    struct stat st;
    int exists=!stat(host,&st) && S_ISDIR(st.st_mode);
    if ((mode & MODE_CREATE) && exists) return ERR_EXISTS;
    if (!(mode & (MODE_CREATE|MODE_CREATE2)) && !exists) return ERR_NOT_FOUND;
    pthread_mutex_lock(&lock);
    int slot=-1;
    for (int i=0;i<SLOTS;++i) {
        if (slots[i].used && !strcmp(slots[i].host,host)) { pthread_mutex_unlock(&lock); return ERR_BAD_MOUNTED; }
        if (!slots[i].used && slot<0) slot=i;
    }
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_MOUNT_FULL; }
    if (!exists && (make_dirs(host) || make_dirs(meta))) { pthread_mutex_unlock(&lock); return ERR_INTERNAL; }
    if (!exists) { Param empty={0}; write_param(meta,&empty); }
    slots[slot].used=1;
    slots[slot].user=user;
    memset(&slots[slot].title,0,sizeof(TitleId));
    snprintf(slots[slot].title.data,sizeof(slots[slot].title.data),"%.9s",title && *title ? title : title_id);
    slots[slot].dir=*dir;
    slots[slot].c_blocks=c_blocks;
    snprintf(slots[slot].host,sizeof(slots[slot].host),"%s",host);
    snprintf(slots[slot].meta,sizeof(slots[slot].meta),"%s",meta);
    memset(result,0,sizeof(*result));
    char point[32];
    snprintf(point,sizeof(point),"/savedata%d",slot&15);
    memcpy(result->point.data,point,strlen(point)+1);
    result->status=exists ? 0 : 1; /* CREATED */
    runtime_file_mount(result->point.data,host);
    ++mounts_done;
    pthread_mutex_unlock(&lock);
    printf("Runtime: save data '%s' mounted at %s (%s%s)\n",dir->data,result->point.data,
           exists ? "existing" : "created",(mode & MODE_RDONLY) ? ", read-only" : "");
    return 0;
}
static ABI int32_t save_mount(const Mount1 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    return mount(m->user,m->title ? m->title->data : NULL,m->dir,m->mode,m->blocks,result);
}
static ABI int32_t save_mount2(const Mount2 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    return mount(m->user,NULL,m->dir,m->mode,m->blocks,result);
}
static int slot_of(const MountPoint *point) {
    if (!point || strncmp(point->data,"/savedata",9)) return -1;
    int slot=atoi(point->data+9);
    return slot>=0 && slot<SLOTS && slots[slot].used ? slot : -1;
}
static int32_t copy_save_locked(int32_t user, const TitleId *p_title, const char *sz_dir, int is_restore);
static int32_t umount(const MountPoint *point, int backup) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot>=0) {
        runtime_file_unmount(point->data); slots[slot].used=0;
        if (backup) copy_save_locked(slots[slot].user,&slots[slot].title,slots[slot].dir.data,0);
        if (backup && event_count<SLOTS)
            events[event_count++]=(Event){.type=EVENT_UMOUNT_BACKUP,.user=slots[slot].user,.title=slots[slot].title,.dir=slots[slot].dir};
    }
    pthread_mutex_unlock(&lock);
    return slot>=0 ? 0 : ERR_NOT_FOUND;
}
static ABI int32_t save_umount(const MountPoint *point) { return umount(point,0); }
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
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!p_point || !p_out_info) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int i_slot=slot_of(p_point);
    if (i_slot<0) { pthread_mutex_unlock(&lock); return ERR_NOT_MOUNTED; }
    s_cb_used=0;
    nftw(slots[i_slot].host,add_used_size,16,FTW_PHYS);
    const uint64_t c_used_blocks=(s_cb_used+BLOCK_SIZE_BYTES-1)/BLOCK_SIZE_BYTES;
    memset(p_out_info,0,sizeof(*p_out_info));
    p_out_info->blocks=slots[i_slot].c_blocks;
    p_out_info->freeBlocks=c_used_blocks<slots[i_slot].c_blocks ? slots[i_slot].c_blocks-c_used_blocks : 0;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t save_umount_with_backup(const MountPoint *point) { return umount(point,1); }
static ABI int32_t save_event_result(const void *param, Event *event) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!event) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    const int found=event_count>0;
    if (found) { *event=events[0]; memmove(events,events+1,--event_count*sizeof(Event)); }
    pthread_mutex_unlock(&lock);
    return found ? 0 : ERR_NOT_FOUND;
}
static ABI int32_t save_set_param(const MountPoint *point, uint32_t type, const void *buffer, uint64_t size) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_NOT_MOUNTED; }
    Param p; read_param(slots[slot].meta,&p);
    switch (type) {
    case 0: if (size<sizeof(Param)) goto bad; memcpy(&p,buffer,sizeof(p)); break;     /* ALL */
    case 1: snprintf(p.title,sizeof(p.title),"%.*s",(int)size,(const char *)buffer); break;
    case 2: snprintf(p.subtitle,sizeof(p.subtitle),"%.*s",(int)size,(const char *)buffer); break;
    case 3: snprintf(p.detail,sizeof(p.detail),"%.*s",(int)size,(const char *)buffer); break;
    case 4: if (size<4) goto bad; memcpy(&p.user_param,buffer,4); break;
    default: goto bad;
    }
    write_param(slots[slot].meta,&p);
    pthread_mutex_unlock(&lock);
    return 0;
bad:
    pthread_mutex_unlock(&lock);
    return ERR_PARAMETER;
}
static uint64_t copy_text(void *buffer, uint64_t size, const char *text, size_t capacity) {
    size_t length=strnlen(text,capacity-1);
    if (length>=size) length=size-1;
    memcpy(buffer,text,length); ((char *)buffer)[length]=0;
    return length+1;
}
static ABI int32_t save_get_param(const MountPoint *point, uint32_t type, void *buffer, uint64_t size, uint64_t *got) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer || !size) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    Param p;
    if (slot>=0) read_param(slots[slot].meta,&p);
    pthread_mutex_unlock(&lock);
    if (slot<0) return ERR_NOT_MOUNTED;
    uint64_t written;
    switch (type) {
    case 0: if (size<sizeof(p)) return ERR_PARAMETER; memcpy(buffer,&p,written=sizeof(p)); break;
    case 1: written=copy_text(buffer,size,p.title,sizeof(p.title)); break;
    case 2: written=copy_text(buffer,size,p.subtitle,sizeof(p.subtitle)); break;
    case 3: written=copy_text(buffer,size,p.detail,sizeof(p.detail)); break;
    case 4: if (size<sizeof(p.user_param)) return ERR_PARAMETER; memcpy(buffer,&p.user_param,written=sizeof(p.user_param)); break;
    case 5: if (size<sizeof(p.mtime)) return ERR_PARAMETER; memcpy(buffer,&p.mtime,written=sizeof(p.mtime)); break;
    default: return ERR_PARAMETER;
    }
    if (got) *got=written;
    return 0;
}
static ABI int32_t save_icon(const MountPoint *point, const Icon *icon) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!icon || !icon->buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    int32_t r=ERR_NOT_MOUNTED;
    if (slot>=0) {
        char path[700]; snprintf(path,sizeof(path),"%s/icon0.png",slots[slot].meta);
        r=write_atomic(path,icon->buffer,icon->data_size) ? ERR_INTERNAL : 0;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t save_transferring_mount(const void *mount, void *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!mount || !result) return ERR_PARAMETER;
    return ERR_NOT_FOUND;
}
static ABI int32_t save_delete(const Delete *d) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!d || !d->dir || !valid_name(d->dir->data,sizeof(d->dir->data))) return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(d->user,d->title ? d->title->data : NULL,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,d->dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,d->dir->data);
    struct stat st;
    if (stat(host,&st)) return ERR_NOT_FOUND;
    nftw(host,remove_entry,16,FTW_DEPTH|FTW_PHYS);
    nftw(meta,remove_entry,16,FTW_DEPTH|FTW_PHYS);
    printf("Runtime: save data '%s' deleted\n",d->dir->data);
    return 0;
}
/* SQL-LIKE pattern: % any run, _ one character. */
static int like(const char *s, const char *p) {
    if (!*p) return !*s;
    if (*p=='%') { for (;;++s) { if (like(s,p+1)) return 1; if (!*s) return 0; } }
    return *s && (*p=='_' || *p==*s) && like(s+1,p+1);
}
typedef struct { char name[32]; Param param; } Entry;
static uint32_t sort_key, sort_order;
static int compare(const void *a, const void *b) {
    const Entry *x=a, *y=b;
    int c = sort_key==1 ? (x->param.user_param>y->param.user_param)-(x->param.user_param<y->param.user_param)
          : sort_key==3 ? (x->param.mtime>y->param.mtime)-(x->param.mtime<y->param.mtime)
          : strcmp(x->name,y->name);
    return sort_order ? -c : c;
}
static ABI int32_t save_search(const SearchCond *cond, SearchResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!cond || !result) return ERR_PARAMETER;
    char base[600];
    root(cond->user,cond->title ? cond->title->data : NULL,base,sizeof(base));
    Entry *entries=NULL; size_t count=0, capacity=0;
    DIR *d=opendir(base);
    for (struct dirent *e; d && (e=readdir(d));) {
        size_t n=strlen(e->d_name);
        if (e->d_name[0]=='.' || n>=32 || (n>8 && !strcmp(e->d_name+n-8,".sce_sys"))) continue;
        if (cond->dir && cond->dir->data[0] && !like(e->d_name,cond->dir->data)) continue;
        if (count==capacity) { capacity=capacity ? capacity*2 : 16; entries=realloc(entries,capacity*sizeof(*entries)); }
        snprintf(entries[count].name,32,"%s",e->d_name);
        char meta[680]; snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,e->d_name);
        read_param(meta,&entries[count].param);
        ++count;
    }
    if (d) closedir(d);
    sort_key=cond->key; sort_order=cond->order;
    if (count) qsort(entries,count,sizeof(*entries),compare);
    result->hits=(uint32_t)count;
    uint32_t set=count<result->names_capacity ? (uint32_t)count : result->names_capacity;
    for (uint32_t i=0;i<set;++i) {
        if (result->names) memcpy(result->names[i].data,entries[i].name,32);
        if (result->params) result->params[i]=entries[i].param;
        if (result->infos) { memset(&result->infos[i],0,sizeof(SearchInfo)); result->infos[i].blocks=32768; result->infos[i].free_blocks=16384; }
    }
    result->set_count=set;
    free(entries);
    return 0;
}
/* SaveDataMemory: one fixed-size blob per user/title and memory slot. */
static int32_t memory_open(int32_t user, uint32_t slot, uint64_t size, uint64_t *existed) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user<0) return ERR_INVALID_USER;
    if (!size || slot>=SLOTS) return ERR_PARAMETER;
    char base[600], dir[640], path[700];
    root(user,NULL,base,sizeof(base));
    if (slot) snprintf(dir,sizeof(dir),"%s.memory%u",base,slot);
    else snprintf(dir,sizeof(dir),"%s.memory",base);
    if (make_dirs(dir)) return ERR_INTERNAL;
    snprintf(path,sizeof(path),"%s/memory.dat",dir);
    pthread_mutex_lock(&lock);
    FILE *f=fopen(path,"r+b");
    if (!f) f=fopen(path,"w+b");
    int32_t r=ERR_INTERNAL;
    uint64_t old=0;
    if (f) {
        fseek(f,0,SEEK_END);
        old=(uint64_t)ftell(f);
        if (old<size && ftruncate(fileno(f),(off_t)size)) goto done;
        *existed=old;
        snprintf(memories[slot].dir,sizeof(memories[slot].dir),"%s",dir);
        memories[slot].size=size; r=0;
    done:
        fclose(f);
    }
    pthread_mutex_unlock(&lock);
    if (!r) printf("Runtime: save data memory %u ready (%llu bytes, %llu existed)\n",slot,(unsigned long long)size,
                   (unsigned long long)old);
    return r;
}
/* memory_ready, memory_io and memory_meta run under the lock. */
static int32_t memory_ready(uint32_t slot) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (slot>=SLOTS) return ERR_PARAMETER;
    return memories[slot].size ? 0 : ERR_MEMORY_NOT_READY;
}
static int32_t memory_io(uint32_t slot, void *buffer, uint64_t size, int64_t offset, int write) {
    if (!buffer || offset<0) return ERR_PARAMETER;
    if ((uint64_t)offset+size>memories[slot].size) return ERR_PARAMETER;
    char path[700]; snprintf(path,sizeof(path),"%s/memory.dat",memories[slot].dir);
    FILE *f=fopen(path,"r+b");
    int32_t r=ERR_INTERNAL;
    if (f && !fseek(f,offset,SEEK_SET) &&
        (write ? fwrite(buffer,1,size,f) : fread(buffer,1,size,f))==size) r=0;
    if (f) fclose(f);
    if (!r && write) ++memory_writes;
    return r;
}
static int32_t memory_meta(uint32_t slot, const Param *param, const Icon *icon) {
    if (param) write_param(memories[slot].dir,param);
    if (!icon || !icon->buffer) return 0;
    char path[700]; snprintf(path,sizeof(path),"%s/icon0.png",memories[slot].dir);
    return write_atomic(path,icon->buffer,icon->data_size) ? ERR_INTERNAL : 0;
}
static int32_t memory_transfer(void *buffer, uint64_t size, int64_t offset, int write) {
    pthread_mutex_lock(&lock);
    int32_t r=memory_ready(0);
    if (!r) r=memory_io(0,buffer,size,offset,write);
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t memory_setup(int32_t user, uint64_t size, const Param *param) {
    uint64_t existed;
    int32_t r=memory_open(user,0,size,&existed);
    if (r) return r;
    pthread_mutex_lock(&lock);
    r=memory_meta(0,param,NULL);
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t memory_get(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_transfer(buffer,size,offset,0); }
static ABI int32_t memory_set(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_transfer(buffer,size,offset,1); }
/* initParam and initIcon describe a new memory; MEMORY_SET_PARAM also replaces an existing one's param. */
static ABI int32_t memory_setup2(const MemorySetup2 *setup, MemorySetupResult *result) {
    if (!setup) return ERR_PARAMETER;
    uint64_t existed;
    int32_t r=memory_open(setup->user,setup->slot,setup->size,&existed);
    if (r) return r;
    pthread_mutex_lock(&lock);
    r=memory_meta(setup->slot,!existed || setup->option&MEMORY_SET_PARAM ? setup->param : NULL,
                  existed ? NULL : setup->icon);
    pthread_mutex_unlock(&lock);
    if (!r && result) { memset(result,0,sizeof(*result)); result->existed_size=existed; }
    return r;
}
static ABI int32_t memory_get2(const MemoryGet2 *get) {
    if (!get) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int32_t r=memory_ready(get->slot);
    if (!r && get->data) r=memory_io(get->slot,get->data->buffer,get->data->size,get->data->offset,0);
    if (!r && get->param) read_param(memories[get->slot].dir,get->param);
    if (!r && get->icon && get->icon->buffer) {
        char path[700]; snprintf(path,sizeof(path),"%s/icon0.png",memories[get->slot].dir);
        FILE *f=fopen(path,"rb");
        get->icon->data_size=f ? fread((void *)get->icon->buffer,1,get->icon->buffer_size,f) : 0;
        if (f) fclose(f);
    }
    pthread_mutex_unlock(&lock);
    return r;
}
/* A zero dataNum with data set is one element, since callers built that way leave the field zeroed. */
static ABI int32_t memory_set2(const MemorySet2 *set) {
    if (!set) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int32_t r=memory_ready(set->slot);
    for (uint32_t i=0;!r && set->data && i<(set->data_count ? set->data_count : 1);++i)
        r=memory_io(set->slot,set->data[i].buffer,set->data[i].size,set->data[i].offset,1);
    if (!r) r=memory_meta(set->slot,set->param,set->icon);
    pthread_mutex_unlock(&lock);
    return r;
}

/* Backups: <saves>/.backup/<dir> and its .sce_sys beside it, out of DirNameSearch's sight (it skips
 * names starting with a dot). Each request completes before it returns, so the progress is always
 * full and the event the game waits for is already queued. */
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *param;
                 uint8_t reserved[32]; } BackupRequest;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; Param *param; Icon *icon;
                 uint8_t reserved[32]; } BackupCheck;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint32_t unused; uint8_t reserved[32]; int32_t pad2; } BackupRestore;
typedef struct { int32_t user; uint32_t slot, option; uint8_t reserved[28]; } MemorySync;
_Static_assert(sizeof(BackupRequest)==64 && sizeof(BackupCheck)==72 && sizeof(BackupRestore)==72,"backup layouts");
static const char *s_sz_copy_source, *s_sz_copy_target;
static int copy_entry(const char *sz_path, const struct stat *p_stat, int flag, struct FTW *p_ftw) {
    (void)p_stat; (void)p_ftw;
    char sz_target[1400];
    snprintf(sz_target,sizeof(sz_target),"%s%s",s_sz_copy_target,sz_path+strlen(s_sz_copy_source));
    if (flag==FTW_D) return mkdir(sz_target,0755) && errno!=EEXIST ? -1 : 0;
    if (flag!=FTW_F) return 0;
    FILE *p_in=fopen(sz_path,"rb"), *p_out=p_in ? fopen(sz_target,"wb") : NULL;
    int is_copied=p_out!=NULL;
    char buffer[65536];
    for (size_t cb_read; is_copied && (cb_read=fread(buffer,1,sizeof(buffer),p_in));)
        is_copied=fwrite(buffer,1,cb_read,p_out)==cb_read;
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
static int is_folder(const char *sz_path) { struct stat st; return !stat(sz_path,&st) && S_ISDIR(st.st_mode); }
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
    if (event_count>=SLOTS) return;
    Event event={.type=type,.user=user};
    if (p_title) event.title=*p_title;
    else snprintf(event.title.data,sizeof(event.title.data),"%.9s",title_id);
    if (p_dir) event.dir=*p_dir;
    events[event_count++]=event;
}
static ABI int32_t save_backup(const BackupRequest *p_request) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!p_request || !p_request->dir || !valid_name(p_request->dir->data,sizeof(p_request->dir->data))) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    const int32_t result=copy_save_locked(p_request->user,p_request->title,p_request->dir->data,0);
    if (!result) queue_event_locked(EVENT_BACKUP,p_request->user,p_request->title,p_request->dir);
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t save_check_backup(const BackupCheck *p_check) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!p_check || !p_check->dir || !valid_name(p_check->dir->data,sizeof(p_check->dir->data))) return ERR_PARAMETER;
    char sz_host[640], sz_meta[680];
    save_paths(p_check->user,p_check->title,p_check->dir->data,1,sz_host,sz_meta);
    if (!is_folder(sz_host)) return ERR_NOT_FOUND;
    if (p_check->param) read_param(sz_meta,p_check->param);
    if (p_check->icon && p_check->icon->buffer) {
        char sz_icon[700];
        snprintf(sz_icon,sizeof(sz_icon),"%s/icon0.png",sz_meta);
        FILE *p_file=fopen(sz_icon,"rb");
        uint64_t cb_icon=p_file ? fread((void *)p_check->icon->buffer,1,p_check->icon->buffer_size,p_file) : 0;
        if (p_file) fclose(p_file);
        p_check->icon->data_size=cb_icon;
    }
    return 0;
}
static ABI int32_t save_restore_backup(const BackupRestore *p_restore) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!p_restore || !p_restore->dir || !valid_name(p_restore->dir->data,sizeof(p_restore->dir->data))) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    const int32_t result=copy_save_locked(p_restore->user,p_restore->title,p_restore->dir->data,1);
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t save_progress(float *p_out_progress) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!p_out_progress) return ERR_PARAMETER;
    *p_out_progress=1.0f;
    return 0;
}
static ABI int32_t save_load_icon(const MountPoint *p_point, Icon *p_icon) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!p_icon || !p_icon->buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    const int i_slot=slot_of(p_point);
    int32_t result=ERR_NOT_MOUNTED;
    if (i_slot>=0) {
        char sz_icon[700];
        snprintf(sz_icon,sizeof(sz_icon),"%s/icon0.png",slots[i_slot].meta);
        FILE *p_file=fopen(sz_icon,"rb");
        result=p_file ? 0 : ERR_NOT_FOUND;
        if (p_file) { p_icon->data_size=fread((void *)p_icon->buffer,1,p_icon->buffer_size,p_file); fclose(p_file); }
    }
    pthread_mutex_unlock(&lock);
    return result;
}
/* Save data memory is written to its file on every set; syncing makes it durable. */
static ABI int32_t memory_sync(const MemorySync *p_sync) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!p_sync || p_sync->slot>=SLOTS) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int32_t result=ERR_MEMORY_NOT_READY;
    if (memories[p_sync->slot].size) {
        char sz_path[700];
        snprintf(sz_path,sizeof(sz_path),"%s/memory.dat",memories[p_sync->slot].dir);
        FILE *p_file=fopen(sz_path,"r+b");
        result=p_file && !fsync(fileno(p_file)) ? 0 : ERR_INTERNAL;
        if (p_file) fclose(p_file);
        if (!result) queue_event_locked(EVENT_MEMORY_SYNC,p_sync->user,NULL,NULL);
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t save_clear_progress(void) { return initialized ? 0 : ERR_NOT_INITIALIZED; }

static const RuntimeExport exports[]={
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
uintptr_t runtime_savedata_resolve(const char *sz_name) { return RUNTIME_LOOKUP(exports,sz_name); }
int runtime_savedata_param(int32_t user, const char *sz_title, const char *sz_dir, void *p_param) {
    char base[600], path[680];
    struct stat st;
    if (!valid_name(sz_dir,32)) return -1;
    root(user,sz_title,base,sizeof(base));
    snprintf(path,sizeof(path),"%s/%s",base,sz_dir);
    if (stat(path,&st) || !S_ISDIR(st.st_mode)) return -1;
    snprintf(path,sizeof(path),"%s/%s.sce_sys",base,sz_dir);
    read_param(path,p_param);
    return 0;
}
void runtime_savedata_report(void) { printf("Runtime: save data mounts=%zu, memory writes=%zu\n",mounts_done,memory_writes); }
#else
void runtime_savedata_configure(const char *title) { (void)title; }
uintptr_t runtime_savedata_resolve(const char *name) { (void)name; return 0; }
int runtime_savedata_param(int32_t user, const char *title, const char *dir, void *param) { (void)user; (void)title; (void)dir; (void)param; return -1; }
void runtime_savedata_report(void) {}
#endif
