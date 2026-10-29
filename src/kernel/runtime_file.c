// SPDX-License-Identifier: GPL-2.0-or-later
/* Guest file system: PS4 mount points mapped onto host directories.
 *   /app0, /hostapp  -> game package root (read-only by convention)
 *   /temp0, /download0, /data, and mounts added by SaveData -> user directory
 * Guest descriptors are small integers in our own table; stdio 0-2 pass through.
 * Paths containing ".." components are rejected rather than normalized. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define MAX_FILES RUNTIME_MAX_FILES
#define MAX_MOUNTS 16

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct {
    uint32_t dev, ino;
    uint16_t mode, nlink;
    uint32_t uid, gid, rdev;
    GuestTimespec atime, mtime, ctime;
    int64_t size, blocks;
    uint32_t blksize, flags, gen;
    int32_t lspare;
    GuestTimespec birthtime;
} GuestStat;
_Static_assert(sizeof(GuestStat)==120,"FreeBSD stat layout");

typedef struct { char *sz_names; size_t c_names, *p_offsets; unsigned char *p_types; } Listing;
/* commit: a save file opened for writing is written to a temporary copy (temp), which replaces
 * the file (commit) in one rename when closed: a crash or a kill mid-save leaves the old file. */
/* serial: which open this entry is, so a kqueue filter knows its descriptor was closed (and the
 * number taken again) since it was added. */
typedef struct { int used, host, is_dirty, is_socket; Listing *p_dir; size_t position; char sz_path[512]; char *sz_commit, *sz_temp; uint64_t serial; } File;
typedef struct { char guest[64]; char host[512]; } Mount;
static File s_files[MAX_FILES];
static Mount s_mounts[MAX_MOUNTS];
static size_t s_c_mounts, s_c_opens, s_c_reads, s_c_writes, s_c_missing;
static uint64_t s_cb_read;
static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_last_serial;
#define MAX_UNMOUNTED 32
static struct { char sz_root[64]; size_t c_hits; } s_unmounted[MAX_UNMOUNTED];
static size_t s_c_unmounted;

/* Where relative guest paths start: the game package until the game changes it (chdir). */
static char s_sz_cwd[512]="/app0";
static pthread_mutex_t s_cwd_lock=PTHREAD_MUTEX_INITIALIZER;
static void absolute_path(const char *sz_guest,char *sz_out,size_t cb_out) {
    if (!*sz_guest || sz_guest[0]=='/') { snprintf(sz_out,cb_out,"%s",sz_guest); return; }
    pthread_mutex_lock(&s_cwd_lock);
    snprintf(sz_out,cb_out,"%s/%s",s_sz_cwd,sz_guest);
    pthread_mutex_unlock(&s_cwd_lock);
}
static int save_path(const char *sz_path) {
    if (!sz_path || !*sz_path) return 0;
    char sz_absolute[1024];
    absolute_path(sz_path,sz_absolute,sizeof(sz_absolute));
    return !strncmp(sz_absolute,"/savedata",9);
}
static int temp_name(const char *sz_name) {
    size_t cb_name=strlen(sz_name);
    return cb_name>6 && !strcmp(sz_name+cb_name-6,".bbtmp");
}
/* Copies left by a crash (none of them open): removed when their directory is mounted. */
static void remove_stale_temps(const char *sz_dir,int depth) {
    PsHostDir *p_dir=ps_host_dir_open(sz_dir);
    if (!p_dir) return;
    for (PsHostDirEntry entry; ps_host_dir_next(p_dir,&entry);) {
        if (entry.sz_name[0]=='.') continue;
        char sz_path[1024];
        if ((size_t)snprintf(sz_path,sizeof(sz_path),"%s/%s",sz_dir,entry.sz_name)>=sizeof(sz_path)) continue;
        if (temp_name(entry.sz_name)) {
            int is_open_now=0;
            pthread_mutex_lock(&s_lock);
            for (int i=3;i<MAX_FILES;++i) if (s_files[i].used && s_files[i].sz_temp && !strcmp(s_files[i].sz_temp,sz_path)) is_open_now=1;
            const int is_removed=!is_open_now && !ps_host_fs_remove(sz_path);
            pthread_mutex_unlock(&s_lock);
            if (is_removed) printf("Runtime: removed %s (a save interrupted earlier; the file it would have replaced is intact)\n",sz_path);
        } else if (depth<4 && entry.kind==PS_HOST_ENTRY_DIRECTORY) remove_stale_temps(sz_path,depth+1);
    }
    ps_host_dir_close(p_dir);
}

int runtime_file_mount(const char *sz_guest,const char *sz_host) {
    pthread_mutex_lock(&s_lock);
    for (size_t i=0;i<s_c_mounts;++i) if (!strcmp(s_mounts[i].guest,sz_guest)) {
        snprintf(s_mounts[i].host,sizeof(s_mounts[i].host),"%s",sz_host);
        pthread_mutex_unlock(&s_lock); return 0;
    }
    if (s_c_mounts==MAX_MOUNTS || strlen(sz_guest)>=64 || strlen(sz_host)>=512) { pthread_mutex_unlock(&s_lock); return -1; }
    snprintf(s_mounts[s_c_mounts].guest,64,"%s",sz_guest);
    snprintf(s_mounts[s_c_mounts].host,512,"%s",sz_host);
    ++s_c_mounts;
    pthread_mutex_unlock(&s_lock);
    /* The walk is not under the file table's lock, which every open and close of the game takes. */
    if (save_path(sz_guest)) remove_stale_temps(sz_host,0);
    return 0;
}
void runtime_file_unmount(const char *sz_guest) {
    pthread_mutex_lock(&s_lock);
    for (size_t i=0;i<s_c_mounts;++i) if (!strcmp(s_mounts[i].guest,sz_guest)) {
        s_mounts[i]=s_mounts[--s_c_mounts]; break;
    }
    pthread_mutex_unlock(&s_lock);
}
static char s_sz_user_root[512]="user";
const char *runtime_file_user_dir(void) { return s_sz_user_root; }
void runtime_file_configure(const char *sz_app0,const char *sz_user) {
    char sz_path[600];
    snprintf(s_sz_user_root,sizeof(s_sz_user_root),"%s",sz_user);
    runtime_file_mount("/app0",sz_app0);
    runtime_file_mount("/hostapp",sz_app0);
    /* Games seed their RNGs from the console's random devices; the host's give the same stream. */
    runtime_file_mount("/dev/urandom","/dev/urandom");
    runtime_file_mount("/dev/random","/dev/random");
    const char *writable[]={"temp0","download0","data"};
    ps_host_fs_make_dir(sz_user,0755);
    for (int i=0;i<3;++i) {
        snprintf(sz_path,sizeof(sz_path),"%s/%s",sz_user,writable[i]);
        ps_host_fs_make_dir(sz_path,0755);
        char sz_guest[32]; snprintf(sz_guest,sizeof(sz_guest),"/%s",writable[i]);
        runtime_file_mount(sz_guest,sz_path);
    }
}
/* Resolve a guest path to a host path; returns 0 or a host errno. */
static int translate(const char *sz_guest,char *sz_out,size_t cb_out) {
    if (!sz_guest || !*sz_guest) return ENOENT;
    char sz_buffer[1024];
    absolute_path(sz_guest,sz_buffer,sizeof(sz_buffer));
    for (const char *sz_cursor=sz_buffer;(sz_cursor=strstr(sz_cursor,".."));sz_cursor+=2)
        if ((sz_cursor==sz_buffer || sz_cursor[-1]=='/') && (sz_cursor[2]==0 || sz_cursor[2]=='/')) {
            static uint64_t s_c_parent_refusals;
            const uint64_t c_before=__atomic_fetch_add(&s_c_parent_refusals,1,__ATOMIC_RELAXED);
            if (c_before<8) printf("Runtime: guest path %s refused (EACCES): \"..\" components are not resolved\n",sz_guest);
            if (!c_before) runtime_calls_hle_stub("guest path with \"..\" refused (EACCES)",&s_c_parent_refusals);
            return EACCES;
        }
    pthread_mutex_lock(&s_lock);
    size_t best=0; const Mount *p_mount=NULL;
    for (size_t i=0;i<s_c_mounts;++i) {
        size_t cb_prefix=strlen(s_mounts[i].guest);
        if (!strncmp(sz_buffer,s_mounts[i].guest,cb_prefix) && (sz_buffer[cb_prefix]=='/' || !sz_buffer[cb_prefix]) && cb_prefix>best) { best=cb_prefix; p_mount=&s_mounts[i]; }
    }
    int result=0, first=1, counted=0;
    size_t root=1+strcspn(sz_buffer+1,"/");
    if (root>=sizeof(s_unmounted[0].sz_root)) root=sizeof(s_unmounted[0].sz_root)-1;
    if (!p_mount) {
        result=ENOENT;
        size_t i=0;
        while (i<s_c_unmounted && (strlen(s_unmounted[i].sz_root)!=root || strncmp(s_unmounted[i].sz_root,sz_buffer,root))) ++i;
        if (i==s_c_unmounted && i<MAX_UNMOUNTED) { memcpy(s_unmounted[i].sz_root,sz_buffer,root); __atomic_store_n(&s_c_unmounted,i+1,__ATOMIC_RELEASE); }
        if (i<s_c_unmounted) { first=!__atomic_fetch_add(&s_unmounted[i].c_hits,1,__ATOMIC_RELAXED); counted=1; }
    }
    else if ((size_t)snprintf(sz_out,cb_out,"%s%s",p_mount->host,sz_buffer+best)>=cb_out) result=ENAMETOOLONG;
    pthread_mutex_unlock(&s_lock);
    if (!p_mount && first && counted)
        fprintf(stderr,"Runtime: no mount for guest path %s (further misses under %.*s are counted for the exit report)\n",sz_guest,(int)root,sz_buffer);
    else if (!p_mount && first) fprintf(stderr,"Runtime: no mount for guest path %s\n",sz_guest);
    /* PS_FILE_TRACE=1: every guest path the game opens, stats or checks (mods, missing files). */
    static int s_trace=-1;
    if (s_trace<0) { const char *sz_value=runtime_env("PS_FILE_TRACE",NULL); s_trace=sz_value && sz_value[0]=='1'; }
    if (s_trace) printf("File trace: %s -> %s\n",sz_guest,result ? "(no mount)" : sz_out);
    return result;
}
/* The PS4's file system ignores case, and the game asks for lower-case names
 * (adhoc/font/dbgfont14h.ccm): files from mods or dumps made elsewhere keep their own case.
 * After a miss, the host path is matched component by component ignoring case; true when it was
 * corrected (same length: only case differs). Only on misses: no cost for found files. */
static int fix_case(char *sz_path) {
    if (sz_path[0]!='/') return 0;
    char sz_out[1024]="", sz_trial[1024], sz_part[256];
    const char *sz_cursor=sz_path;
    while (*sz_cursor) {
        while (*sz_cursor=='/') ++sz_cursor;
        if (!*sz_cursor) break;
        const char *sz_end=strchr(sz_cursor,'/');
        size_t len=sz_end ? (size_t)(sz_end-sz_cursor) : strlen(sz_cursor);
        if (len>=sizeof(sz_part)) return 0;
        memcpy(sz_part,sz_cursor,len); sz_part[len]=0;
        if ((size_t)snprintf(sz_trial,sizeof(sz_trial),"%s/%s",sz_out,sz_part)>=sizeof(sz_trial)) return 0;
        if (ps_host_fs_access(sz_trial,0)) {
            PsHostDir *p_dir=ps_host_dir_open(sz_out[0] ? sz_out : "/");
            if (!p_dir) return 0;
            int is_found=0;
            for (PsHostDirEntry entry;ps_host_dir_next(p_dir,&entry);)
                if (!strcasecmp(entry.sz_name,sz_part)) { snprintf(sz_trial,sizeof(sz_trial),"%s/%s",sz_out,entry.sz_name); is_found=1; break; }
            ps_host_dir_close(p_dir);
            if (!is_found) return 0;
        }
        memcpy(sz_out,sz_trial,strlen(sz_trial)+1);
        sz_cursor+=len;
    }
    if (!strcmp(sz_out,sz_path) || strlen(sz_out)!=strlen(sz_path)) return 0;
    memcpy(sz_path,sz_out,strlen(sz_out)+1);
    return 1;
}
static unsigned host_flags(int flags) {
    unsigned r;
    switch (flags&3) { case 0: r=PS_HOST_OPEN_READ; break; case 1: r=PS_HOST_OPEN_WRITE; break; default: r=PS_HOST_OPEN_READ_WRITE; }
    if (flags&0x4) r|=PS_HOST_OPEN_NONBLOCK;
    if (flags&0x8) r|=PS_HOST_OPEN_APPEND;
    if (flags&0x80) r|=PS_HOST_OPEN_SYNC;
    if (flags&0x200) r|=PS_HOST_OPEN_CREATE;
    if (flags&0x400) r|=PS_HOST_OPEN_TRUNCATE;
    if (flags&0x800) r|=PS_HOST_OPEN_EXCLUSIVE;
    if (flags&0x20000) r|=PS_HOST_OPEN_DIRECTORY;
    return r;
}
static void free_listing(Listing *p_listing) { if (p_listing) { free(p_listing->sz_names); free(p_listing->p_offsets); free(p_listing->p_types); free(p_listing); } }
static Listing *list_directory(const char *sz_path) {
    PsHostDir *p_dir=ps_host_dir_open(sz_path);
    if (!p_dir) return NULL;
    Listing *p_listing=calloc(1,sizeof(*p_listing));
    size_t c_capacity=0,cb_names=0,cap_names=0;
    PsHostDirEntry entry;
    while (p_listing && ps_host_dir_next(p_dir,&entry)) {
        if (temp_name(entry.sz_name)) continue;
        size_t cb_name=strlen(entry.sz_name)+1;
        if (p_listing->c_names==c_capacity) {
            c_capacity=c_capacity ? c_capacity*2 : 64;
            p_listing->p_offsets=realloc(p_listing->p_offsets,c_capacity*sizeof(size_t));
            p_listing->p_types=realloc(p_listing->p_types,c_capacity);
        }
        if (cb_names+cb_name>cap_names) { cap_names=(cb_names+cb_name)*2; p_listing->sz_names=realloc(p_listing->sz_names,cap_names); }
        if (!p_listing->p_offsets || !p_listing->p_types || !p_listing->sz_names) { fputs("Out of memory listing directory\n",stderr); exit(1); }
        memcpy(p_listing->sz_names+cb_names,entry.sz_name,cb_name);
        p_listing->p_offsets[p_listing->c_names]=cb_names;
        PsHostEntryKind kind=entry.kind;
        if (kind==PS_HOST_ENTRY_LINK || kind==PS_HOST_ENTRY_UNKNOWN) {
            PsHostStat target;
            if (!ps_host_dir_stat(p_dir,entry.sz_name,&target))
                kind=PS_HOST_MODE_IS_DIRECTORY(target.mode) ? PS_HOST_ENTRY_DIRECTORY : PS_HOST_MODE_IS_REGULAR(target.mode) ? PS_HOST_ENTRY_REGULAR : kind;
        }
        p_listing->p_types[p_listing->c_names]=kind==PS_HOST_ENTRY_DIRECTORY ? 4 : kind==PS_HOST_ENTRY_REGULAR ? 8 : kind==PS_HOST_ENTRY_LINK ? 10 : 0;
        ++p_listing->c_names; cb_names+=cb_name;
    }
    ps_host_dir_close(p_dir);
    return p_listing;
}
static void convert_stat(const PsHostStat *p_host_stat,GuestStat *p_guest_stat) {
    memset(p_guest_stat,0,sizeof(*p_guest_stat));
    p_guest_stat->dev=(uint32_t)p_host_stat->dev; p_guest_stat->ino=(uint32_t)p_host_stat->ino;
    p_guest_stat->mode=(uint16_t)p_host_stat->mode; p_guest_stat->nlink=(uint16_t)p_host_stat->c_links;
    p_guest_stat->size=p_host_stat->cb_size; p_guest_stat->blocks=p_host_stat->c_blocks; p_guest_stat->blksize=p_host_stat->cb_block;
    p_guest_stat->atime=(GuestTimespec){p_host_stat->atime.tv_sec,p_host_stat->atime.tv_nsec};
    p_guest_stat->mtime=(GuestTimespec){p_host_stat->mtime.tv_sec,p_host_stat->mtime.tv_nsec};
    p_guest_stat->ctime=(GuestTimespec){p_host_stat->ctime.tv_sec,p_host_stat->ctime.tv_nsec};
    p_guest_stat->birthtime=p_guest_stat->ctime;
}
static File *get(int h_fd) {
    if (h_fd<3 || h_fd>=MAX_FILES || !s_files[h_fd].used) return NULL;
    return &s_files[h_fd];
}
/* PS_AUDIO_TRACE=1: sound file opens and failed reads (missing game sounds). */
static int audio_trace(void) { static int s_is_enabled=-1; if (s_is_enabled<0) { const char *sz_value=runtime_env("PS_AUDIO_TRACE",NULL); s_is_enabled=sz_value && sz_value[0]=='1'; } return s_is_enabled; }
/* PS_SAVE_TRACE=1: every operation on save files (/savedataN). */
static int save_trace(void) { static int s_is_enabled=-1; if (s_is_enabled<0) { const char *sz_value=runtime_env("PS_SAVE_TRACE",NULL); s_is_enabled=sz_value && sz_value[0]=='1'; } return s_is_enabled; }
/* Game mounts (including linked mod overlays) are read-only. Saves use other mounts. */
static int game_path(const char *sz_guest) {
    if (!sz_guest || !*sz_guest) return 0;
    char sz_path[1024];
    absolute_path(sz_guest,sz_path,sizeof(sz_path));
    return (!strncmp(sz_path,"/app0",5) && (!sz_path[5] || sz_path[5]=='/')) ||
           (!strncmp(sz_path,"/hostapp",8) && (!sz_path[8] || sz_path[8]=='/'));
}
/* Saves (/savedataN): see File. The copy starts as the old file unless the open truncates. */
static unsigned s_temp_serial;
/* 0, or the host's errno. */
static int copy_contents(int from,int to) {
    char sz_buffer[65536];
    for (;;) {
        int64_t cb_read=ps_host_fs_read(from,sz_buffer,sizeof(sz_buffer));
        if (cb_read==-EINTR) continue;
        if (cb_read<=0) return (int)-cb_read;
        for (int64_t done=0; done<cb_read;) {
            int64_t w=ps_host_fs_write(to,sz_buffer+done,(size_t)(cb_read-done));
            if (w==-EINTR) continue;
            if (w<0) return (int)-w;
            done+=w;
        }
    }
}
/* A save file open for writing is its copy to the operations by path too (the game stats the
 * file it is writing, which may not exist yet: it renames the old one to its backup first). */
static void current_copy(char *sz_path,size_t cb_path,int write) {
    pthread_mutex_lock(&s_lock);
    for (int i=3;i<MAX_FILES;++i)
        if (s_files[i].used && s_files[i].sz_commit && !strcmp(s_files[i].sz_commit,sz_path)) { snprintf(sz_path,cb_path,"%s",s_files[i].sz_temp); s_files[i].is_dirty|=write; break; }
    pthread_mutex_unlock(&s_lock);
}
/* Returns the descriptor of the copy, or -(errno). */
static int open_for_commit(const char *sz_path,const char *sz_source,int flags,int mode,char **pp_commit,char **pp_temp) {
    unsigned hf=host_flags(flags);
    PsHostStat s;
    int exists=!ps_host_fs_stat(sz_source,&s);
    if (exists && !PS_HOST_MODE_IS_REGULAR(s.mode)) return ps_host_fs_open(sz_path,hf,mode ? (unsigned)mode : 0644);
    if (exists && (hf&PS_HOST_OPEN_CREATE) && (hf&PS_HOST_OPEN_EXCLUSIVE)) return -EEXIST;
    if (!exists && !(hf&PS_HOST_OPEN_CREATE)) return -ENOENT;
    size_t cb_temp=strlen(sz_path)+32;
    char *sz_temp=malloc(cb_temp), *sz_commit=strdup(sz_path);
    if (!sz_temp || !sz_commit) { free(sz_temp); free(sz_commit); return -ENOMEM; }
    snprintf(sz_temp,cb_temp,"%s.%u.bbtmp",sz_path,__atomic_add_fetch(&s_temp_serial,1,__ATOMIC_RELAXED));
    int out=ps_host_fs_open(sz_temp,PS_HOST_OPEN_WRITE|PS_HOST_OPEN_CREATE|PS_HOST_OPEN_TRUNCATE,exists ? s.mode&07777 : mode ? (unsigned)mode : 0644);
    int e=out<0 ? -out : 0;
    if (!e && exists && !(hf&PS_HOST_OPEN_TRUNCATE)) {
        int in=ps_host_fs_open(sz_source,PS_HOST_OPEN_READ,0);
        e=in<0 ? -in : copy_contents(in,out);
        if (in>=0) ps_host_fs_close(in);
    }
    if (out>=0) ps_host_fs_close(out);
    int host=e ? -e : ps_host_fs_open(sz_temp,hf&~(unsigned)(PS_HOST_OPEN_CREATE|PS_HOST_OPEN_EXCLUSIVE|PS_HOST_OPEN_TRUNCATE),0);
    if (host<0) {
        e=-host;
        ps_host_fs_remove(sz_temp); free(sz_temp); free(sz_commit);
        return -e;
    }
    *pp_commit=sz_commit; *pp_temp=sz_temp;
    return host;
}
static void sync_directory(const char *sz_file) {
    char sz_dir[1024];
    snprintf(sz_dir,sizeof(sz_dir),"%s",sz_file);
    char *sz_slash=strrchr(sz_dir,'/');
    if (!sz_slash) return;
    *sz_slash=0;
    int d=ps_host_fs_open(sz_dir,PS_HOST_OPEN_READ|PS_HOST_OPEN_DIRECTORY,0);
    if (d>=0) { ps_host_fs_sync(d); ps_host_fs_close(d); }
}
static int commit_file(const File *p_file) {
    if (!p_file->is_dirty) { ps_host_fs_close(p_file->host); ps_host_fs_remove(p_file->sz_temp); return 0; } /* opened for writing, not written */
    int e=ps_host_fs_sync(p_file->host);
    ps_host_fs_close(p_file->host);
    if (!e) e=ps_host_fs_rename(p_file->sz_temp,p_file->sz_commit);
    if (e) {
        fprintf(stderr,"Runtime: save file %s not replaced (%s); the old one is kept\n",p_file->sz_path,strerror(e));
        ps_host_fs_remove(p_file->sz_temp);
        return -e;
    }
    sync_directory(p_file->sz_commit);
    return 0;
}
/* All operations return >=0 or -(host errno); wrappers adapt the convention. */
static int64_t do_open(const char *sz_guest,int flags,int mode) {
    char sz_absolute[1024];
    if (sz_guest) { absolute_path(sz_guest,sz_absolute,sizeof(sz_absolute)); sz_guest=sz_absolute; }
    if (game_path(sz_guest) && (flags & (3|0x8|0x200|0x400|0x800))) return -EROFS;
    char sz_path[1024];
    int e=translate(sz_guest,sz_path,sizeof(sz_path));
    if (e) return -e;
    char *sz_commit=NULL, *sz_temp=NULL, sz_current[1024];
    snprintf(sz_current,sizeof(sz_current),"%s",sz_path);
    if (save_path(sz_guest)) current_copy(sz_current,sizeof(sz_current),0);
    int host;
    if (save_path(sz_guest) && (flags&3)) host=open_for_commit(sz_path,sz_current,flags,mode,&sz_commit,&sz_temp);
    else {
        host=ps_host_fs_open(sz_current,host_flags(flags),mode ? (unsigned)mode : 0644);
        if (host==-ENOENT && !(flags&0x200) && !save_path(sz_guest) && fix_case(sz_current)) {
            host=ps_host_fs_open(sz_current,host_flags(flags),mode ? (unsigned)mode : 0644);
            snprintf(sz_path,sizeof(sz_path),"%s",sz_current); // the directory listing below uses it
        }
    }
    if (save_trace() && save_path(sz_guest))
        printf("Save trace: open(%s, flags 0x%x) -> host %d%s%s\n",sz_guest,flags,host<0 ? -1 : host,sz_temp ? ", copy " : "",sz_temp ? sz_temp : "");
    if (host<0) {
        if (host==-ENOENT) { ++s_c_missing; printf("Runtime: open(%s) -> not found\n",sz_guest); }
        return host;
    }
    PsHostStat s={0};
    Listing *p_dir=NULL;
    if (!ps_host_fs_stat_file(host,&s) && PS_HOST_MODE_IS_DIRECTORY(s.mode)) p_dir=list_directory(sz_path);
    pthread_mutex_lock(&s_lock);
    int h_fd=-1;
    for (int i=3;i<MAX_FILES;++i) if (!s_files[i].used) { h_fd=i; break; }
    if (h_fd<0) {
        pthread_mutex_unlock(&s_lock); ps_host_fs_close(host); free_listing(p_dir);
        if (sz_temp) ps_host_fs_remove(sz_temp);
        free(sz_commit); free(sz_temp); return -EMFILE;
    }
    s_files[h_fd]=(File){.used=1,.host=host,.p_dir=p_dir,.sz_commit=sz_commit,.sz_temp=sz_temp,.serial=++s_last_serial};
    absolute_path(sz_guest,s_files[h_fd].sz_path,sizeof(s_files[h_fd].sz_path));
    ++s_c_opens;
    pthread_mutex_unlock(&s_lock);
    if (audio_trace() && strstr(sz_guest,"sound/")) printf("Audio trace: open(%s) -> fd %d, %lld bytes\n",sz_guest,h_fd,(long long)s.cb_size);
    const char *sz_mod_trace=runtime_env("PS_MOD_TRACE",NULL), *sz_mod_root=runtime_env("PS_MODS_DIR",NULL);
    if (sz_mod_trace && sz_mod_trace[0]=='1' && sz_mod_root) {
        char sz_actual[PATH_MAX],sz_root[PATH_MAX];
        static unsigned s_c_traced;
        if (!ps_host_fs_real_path(sz_path,sz_actual,sizeof(sz_actual)) && !ps_host_fs_real_path(sz_mod_root,sz_root,sizeof(sz_root))) {
            size_t cb_root=strlen(sz_root);
            if (!strncmp(sz_actual,sz_root,cb_root) && sz_actual[cb_root]=='/' &&
                __atomic_fetch_add(&s_c_traced,1,__ATOMIC_RELAXED)<32)
                printf("Mods: open %s -> %s\n",sz_guest,sz_actual);
        }
    }
    if (save_trace() && save_path(sz_guest)) printf("Save trace: open(%s) -> fd %d\n",sz_guest,h_fd);
    return h_fd;
}
static int64_t release_file(int h_fd,File *p_closed);
static int64_t do_close(int h_fd) {
    if (h_fd>=0 && h_fd<3) return 0;
    pthread_mutex_lock(&s_lock);
    File *p_file=get(h_fd);
    if (!p_file) { pthread_mutex_unlock(&s_lock); return -EBADF; }
    File closed=*p_file;
    *p_file=(File){0};
    pthread_mutex_unlock(&s_lock);
    return release_file(h_fd,&closed);
}
/* What closing does once the entry left the table. */
static int64_t release_file(int h_fd,File *p_closed) {
    File closed=*p_closed;
    free_listing(closed.p_dir);
    int result=0;
    if (closed.sz_temp) result=commit_file(&closed);
    else if (closed.is_socket) ps_host_net_close(closed.host);
    else ps_host_fs_close(closed.host);
    if (save_trace() && save_path(closed.sz_path))
        printf("Save trace: close(fd %d, %s)%s -> %d\n",h_fd,closed.sz_path,closed.sz_temp ? closed.is_dirty ? " commit" : " unwritten" : "",result);
    free(closed.sz_commit); free(closed.sz_temp);
    return result;
}
static int host_fd(int h_fd) {
    if (h_fd>=0 && h_fd<3) return h_fd;
    File *p_file=get(h_fd);
    return p_file ? p_file->host : -1;
}
/* Sockets (runtime_socket.c) take descriptors from this table too, so close, read, write and fcntl
 * find them as the console's kernel would. The table owns the host descriptor once adopted. */
static int adopt(int h_host,int is_socket) {
    pthread_mutex_lock(&s_lock);
    int h_fd=-1;
    for (int i=3;i<MAX_FILES;++i) if (!s_files[i].used) { h_fd=i; break; }
    if (h_fd>=0) s_files[h_fd]=(File){.used=1,.host=h_host,.is_socket=is_socket,.serial=++s_last_serial};
    pthread_mutex_unlock(&s_lock);
    return h_fd<0 ? -EMFILE : h_fd;
}
int runtime_file_adopt(int h_host) { return adopt(h_host,1); }
int runtime_file_host(int h_fd) { return host_fd(h_fd); }
static int host_fd_written(int h_fd) {
    if (h_fd>=0 && h_fd<3) return h_fd;
    File *p_file=get(h_fd);
    if (!p_file) return -1;
    if (!p_file->is_dirty && save_trace() && save_path(p_file->sz_path)) printf("Save trace: first write to fd %d (%s)\n",h_fd,p_file->sz_path);
    p_file->is_dirty=1;
    return p_file->host;
}
/* The GPU side is told of the write first (runtime_memory_note_write: its tracking unprotects the
 * range). Pages protected again meanwhile would make the kernel's copy fail with EFAULT instead
 * of faulting to our handler: a user-mode write to each page first goes through the handler. */
static void touch_for_write(void *p_buffer,uint64_t cb_buffer) {
    if (!cb_buffer) return;
    uintptr_t p=(uintptr_t)p_buffer & ~(uintptr_t)4095, end=(uintptr_t)p_buffer+cb_buffer;
    for (; p<end; p+=4096) {
        volatile unsigned char *p_bytes=(volatile unsigned char *)(p<(uintptr_t)p_buffer ? (uintptr_t)p_buffer : p);
        *p_bytes=*p_bytes;
    }
}
/* A page protected again between the touch and the kernel's copy (the GPU side re-arms its write
 * traps): EFAULT with nothing read. Touched again, the read is tried again a few times. */
static int64_t read_retrying(int h,void *p_buffer,uint64_t cb_buffer,int64_t off_file,int positional) {
    int64_t cb_transferred=-EFAULT;
    for (int attempt=0; attempt<8; ++attempt) {
        cb_transferred=positional ? ps_host_fs_pread(h,p_buffer,cb_buffer,off_file) : ps_host_fs_read(h,p_buffer,cb_buffer);
        if (cb_transferred!=-EFAULT) break;
        touch_for_write(p_buffer,cb_buffer);
    }
    return cb_transferred;
}
/* The console's kernel never reads a regular file short of its end, and games take a short read
 * for the end of their data. Here one happens when the kernel's copy stops at a page the GPU side
 * protected again (Linux returns what it copied before the fault) or the host moves less per call
 * (Windows' ReadFile): the rest is read until the end, an error or SHORT_READ_RESUMES tries. */
#define SHORT_READ_RESUMES 64
static uint64_t s_c_short_reads_completed, s_c_short_reads_given_up;
static int s_has_short_read_summary;
/* Part of the call report: no stdio, no lock. */
static void short_read_summary(char *sz_out,size_t cb_out) {
    snprintf(sz_out,cb_out,"Runtime: summary short reads of regular files: %llu completed, %llu given up\n",
             (unsigned long long)__atomic_load_n(&s_c_short_reads_completed,__ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&s_c_short_reads_given_up,__ATOMIC_RELAXED));
}
static void note_short_read(int h_fd,int h,int64_t off_file,int positional,uint64_t cb_buffer,int64_t cb_first,int64_t cb_total,int64_t result) {
    const int is_given_up=result!=0 && (uint64_t)cb_total<cb_buffer;
    if (__atomic_fetch_add(is_given_up ? &s_c_short_reads_given_up : &s_c_short_reads_completed,1,__ATOMIC_RELAXED)) return;
    if (!__atomic_exchange_n(&s_has_short_read_summary,1,__ATOMIC_ACQ_REL)) runtime_calls_add_summary(short_read_summary);
    char sz_path[sizeof(((File *)0)->sz_path)]="(stdio)";
    pthread_mutex_lock(&s_lock);
    const File *p_file=get(h_fd);
    if (p_file) snprintf(sz_path,sizeof(sz_path),"%s",p_file->sz_path);
    pthread_mutex_unlock(&s_lock);
    const int64_t off_start=positional ? off_file : ps_host_fs_seek(h,0,1)-cb_total;
    if (is_given_up)
        printf("Runtime: short read of %s (fd %d) at %lld: %llu bytes asked, %lld read, then %lld (%s); "
               "returned short, further ones are only counted\n",sz_path,h_fd,(long long)off_start,(unsigned long long)cb_buffer,
               (long long)cb_first,(long long)result,result<0 ? strerror((int)-result) : "resume limit");
    else
        printf("Runtime: short read of %s (fd %d) at %lld: %llu bytes asked, %lld read; the rest was read again, "
               "further ones are only counted\n",sz_path,h_fd,(long long)off_start,(unsigned long long)cb_buffer,(long long)cb_first);
}
static int64_t read_completely(int h_fd,int h,void *p_buffer,uint64_t cb_buffer,int64_t off_file,int positional) {
    const int64_t cb_first=read_retrying(h,p_buffer,cb_buffer,off_file,positional);
    PsHostStat s;
    if (cb_first<=0 || (uint64_t)cb_first>=cb_buffer || ps_host_fs_stat_file(h,&s) || !PS_HOST_MODE_IS_REGULAR(s.mode)) return cb_first;
    int64_t cb_total=cb_first, result=-EAGAIN;
    for (int resume=0; resume<SHORT_READ_RESUMES && (uint64_t)cb_total<cb_buffer; ++resume) {
        result=read_retrying(h,(unsigned char *)p_buffer+cb_total,cb_buffer-(uint64_t)cb_total,off_file+cb_total,positional);
        if (result==-EINTR || result==-EAGAIN) continue;
        if (result<=0) break;
        cb_total+=result;
    }
    if (result!=0 || cb_total>cb_first) note_short_read(h_fd,h,off_file,positional,cb_buffer,cb_first,cb_total,result);
    return cb_total;
}
static int64_t do_read(int h_fd,void *p_buffer,uint64_t cb_buffer) {
    int h=host_fd(h_fd);
    if (h<0) return -EBADF;
    runtime_memory_note_write((uintptr_t)p_buffer,cb_buffer);
    touch_for_write(p_buffer,cb_buffer);
    int64_t cb_transferred=read_completely(h_fd,h,p_buffer,cb_buffer,0,0);
    if (cb_transferred<0) { if (audio_trace()) printf("Audio trace: read(fd %d, %llu) failed, errno %d\n",h_fd,(unsigned long long)cb_buffer,(int)-cb_transferred); return cb_transferred; }
    if (cb_transferred>0) runtime_memory_note_write((uintptr_t)p_buffer,(uint64_t)cb_transferred); /* and once the data is there */
    __atomic_add_fetch(&s_c_reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&s_cb_read,(uint64_t)cb_transferred,__ATOMIC_RELAXED);
    return cb_transferred;
}
static int64_t do_pread(int h_fd,void *p_buffer,uint64_t cb_buffer,int64_t off_file) {
    int h=host_fd(h_fd);
    if (h<0) return -EBADF;
    runtime_memory_note_write((uintptr_t)p_buffer,cb_buffer);
    touch_for_write(p_buffer,cb_buffer);
    int64_t cb_transferred=read_completely(h_fd,h,p_buffer,cb_buffer,off_file,1);
    if (cb_transferred<0) { if (audio_trace()) printf("Audio trace: pread(fd %d, %llu @%lld) failed, errno %d\n",h_fd,(unsigned long long)cb_buffer,(long long)off_file,(int)-cb_transferred); return cb_transferred; }
    if (cb_transferred>0) runtime_memory_note_write((uintptr_t)p_buffer,(uint64_t)cb_transferred); /* and once the data is there */
    __atomic_add_fetch(&s_c_reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&s_cb_read,(uint64_t)cb_transferred,__ATOMIC_RELAXED);
    return cb_transferred;
}
/* Pages the GPU side keeps without access (data newer in VRAM, runtime_memory_trap reasons from
 * 16 up) would make the kernel's copy fail with EFAULT: a user-mode read of each page first goes
 * through the handler, which copies the data back. */
static void touch_for_read(const void *p_buffer,uint64_t cb_buffer) {
    if (!cb_buffer) return;
    uintptr_t p=(uintptr_t)p_buffer & ~(uintptr_t)4095, end=(uintptr_t)p_buffer+cb_buffer;
    for (; p<end; p+=4096)
        (void)*(volatile const unsigned char *)(p<(uintptr_t)p_buffer ? (uintptr_t)p_buffer : p);
}
static int64_t do_write(int h_fd,const void *p_buffer,uint64_t cb_buffer) {
    int h=host_fd_written(h_fd);
    if (h<0) return -EBADF;
    touch_for_read(p_buffer,cb_buffer);
    int64_t cb_transferred=ps_host_fs_write(h,p_buffer,cb_buffer);
    if (cb_transferred<0) return cb_transferred;
    __atomic_add_fetch(&s_c_writes,1,__ATOMIC_RELAXED);
    return cb_transferred;
}
static int64_t do_pwrite(int h_fd,const void *p_buffer,uint64_t cb_buffer,int64_t off_file) {
    int h=host_fd_written(h_fd);
    if (h<0) return -EBADF;
    touch_for_read(p_buffer,cb_buffer);
    return ps_host_fs_pwrite(h,p_buffer,cb_buffer,off_file);
}
static int64_t do_lseek(int h_fd,int64_t off_file,int whence) {
    if (save_trace()) { File *p_file=get(h_fd); if (p_file && save_path(p_file->sz_path)) printf("Save trace: lseek(fd %d, %lld, %d)\n",h_fd,(long long)off_file,whence); }
    File *p_file=get(h_fd);
    if (!p_file) return -EBADF;
    if (whence<0 || whence>2) return -EINVAL;
    if (p_file->p_dir) {
        /* Directory offsets are entry indices for getdirentries. */
        if (whence==0 && off_file>=0) { p_file->position=(size_t)off_file; return off_file; }
        return -EINVAL;
    }
    return ps_host_fs_seek(p_file->host,off_file,whence);
}
static int64_t do_fstat(int h_fd,GuestStat *p_out) {
    if (save_trace()) { File *p_file=get(h_fd); if (p_file && save_path(p_file->sz_path)) printf("Save trace: fstat(fd %d)\n",h_fd); }
    int h=host_fd(h_fd);
    if (h<0) return -EBADF;
    if (!p_out) return -EFAULT;
    PsHostStat s;
    int e=ps_host_fs_stat_file(h,&s);
    if (e) return -e;
    convert_stat(&s,p_out); return 0;
}
static int64_t stat_path(const char *sz_guest,GuestStat *p_out,int is_link) {
    char sz_absolute[1024];
    if (sz_guest) { absolute_path(sz_guest,sz_absolute,sizeof(sz_absolute)); sz_guest=sz_absolute; }
    char sz_path[1024]; PsHostStat s;
    int e=translate(sz_guest,sz_path,sizeof(sz_path));
    if (e) return -e;
    if (save_path(sz_guest)) current_copy(sz_path,sizeof(sz_path),0);
    if (!p_out) return -EFAULT;
    int (*const pfn_stat)(const char *,PsHostStat *)=is_link ? ps_host_fs_stat_link : ps_host_fs_stat;
    int r=-pfn_stat(sz_path,&s);
    if (r==-ENOENT && !save_path(sz_guest) && fix_case(sz_path)) r=-pfn_stat(sz_path,&s);
    if (save_trace() && save_path(sz_guest)) printf("Save trace: stat(%s) -> %d, %lld bytes\n",sz_guest,r,r ? -1LL : (long long)s.cb_size);
    if (r) return r;
    convert_stat(&s,p_out); return 0;
}
static int64_t do_stat(const char *sz_guest,GuestStat *p_out) { return stat_path(sz_guest,p_out,0); }
static int64_t do_lstat(const char *sz_guest,GuestStat *p_out) { return stat_path(sz_guest,p_out,1); }
#define GUEST_AT_FDCWD (-100)
#define GUEST_AT_SYMLINK_NOFOLLOW 0x200
static int64_t do_fstatat(int h_dir,const char *sz_guest,GuestStat *p_out,int flags) {
    if (!sz_guest) return -EFAULT;
    if (flags&~GUEST_AT_SYMLINK_NOFOLLOW) return -EINVAL;
    const int is_link=(flags&GUEST_AT_SYMLINK_NOFOLLOW)!=0;
    if (sz_guest[0]=='/' || h_dir==GUEST_AT_FDCWD) return stat_path(sz_guest,p_out,is_link);
    char sz_path[1024];
    pthread_mutex_lock(&s_lock);
    const File *p_dir=get(h_dir);
    const int e=!p_dir ? EBADF : !p_dir->p_dir ? ENOTDIR :
                (size_t)snprintf(sz_path,sizeof(sz_path),"%s/%s",p_dir->sz_path,sz_guest)>=sizeof(sz_path) ? ENAMETOOLONG : 0;
    pthread_mutex_unlock(&s_lock);
    return e ? -e : stat_path(sz_path,p_out,is_link);
}
static int64_t do_chdir(const char *sz_guest) {
    if (!sz_guest) return -EFAULT;
    char sz_absolute[1024], sz_path[1024];
    absolute_path(sz_guest,sz_absolute,sizeof(sz_absolute));
    for (size_t cb_path=strlen(sz_absolute); cb_path>1 && sz_absolute[cb_path-1]=='/';) sz_absolute[--cb_path]=0;
    if (strlen(sz_absolute)>=sizeof(s_sz_cwd)) return -ENAMETOOLONG;
    int e=translate(sz_absolute,sz_path,sizeof(sz_path));
    PsHostStat s;
    if (!e) e=ps_host_fs_stat(sz_path,&s);
    if (!e && !PS_HOST_MODE_IS_DIRECTORY(s.mode)) e=ENOTDIR;
    if (e) return -e;
    pthread_mutex_lock(&s_cwd_lock);
    snprintf(s_sz_cwd,sizeof(s_sz_cwd),"%s",sz_absolute);
    pthread_mutex_unlock(&s_cwd_lock);
    printf("Runtime: guest working directory is now %s\n",sz_absolute);
    return 0;
}
static int64_t do_getdents(int h_fd,char *sz_buffer,uint64_t cb_buffer,int64_t *p_basep) {
    pthread_mutex_lock(&s_lock);
    File *p_file=get(h_fd);
    int64_t result=0;
    if (!p_file) result=-EBADF;
    else if (!p_file->p_dir) result=-EINVAL;
    else if (!sz_buffer) result=-EFAULT;
    else if (cb_buffer<512) result=-EINVAL;
    else {
        if (p_basep) *p_basep=(int64_t)p_file->position;
        uint64_t written=0;
        while (p_file->position<p_file->p_dir->c_names) {
            const char *sz_name=p_file->p_dir->sz_names+p_file->p_dir->p_offsets[p_file->position];
            size_t cb_name=strlen(sz_name); if (cb_name>255) cb_name=255;
            uint16_t reclen=(uint16_t)((8+cb_name+1+7)&~(size_t)7);
            if (written+reclen>cb_buffer) break;
            char *sz_entry=sz_buffer+written;
            memset(sz_entry,0,reclen);
            uint32_t ino=(uint32_t)p_file->position+1;
            memcpy(sz_entry,&ino,4); memcpy(sz_entry+4,&reclen,2);
            sz_entry[6]=(char)p_file->p_dir->p_types[p_file->position]; sz_entry[7]=(char)cb_name;
            memcpy(sz_entry+8,sz_name,cb_name);
            written+=reclen; ++p_file->position;
        }
        result=(int64_t)written;
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}
static int64_t path_op(const char *sz_guest,int op,int mode) {
    char sz_absolute[1024];
    if (sz_guest) { absolute_path(sz_guest,sz_absolute,sizeof(sz_absolute)); sz_guest=sz_absolute; }
    if (game_path(sz_guest)) return -EROFS;
    char sz_path[1024];
    int e=translate(sz_guest,sz_path,sizeof(sz_path));
    if (e) return -e;
    int r=-(op==0 ? ps_host_fs_make_dir(sz_path,mode ? (unsigned)mode : 0755) : op==1 ? ps_host_fs_remove_dir(sz_path) : ps_host_fs_remove(sz_path));
    if (save_trace() && save_path(sz_guest)) printf("Save trace: %s(%s) -> %d\n",op==0 ? "mkdir" : op==1 ? "rmdir" : "unlink",sz_guest,r);
    return r;
}
static int64_t do_rename(const char *sz_from,const char *sz_to) {
    char sz_absolute_from[1024], sz_absolute_to[1024];
    if (sz_from) { absolute_path(sz_from,sz_absolute_from,sizeof(sz_absolute_from)); sz_from=sz_absolute_from; }
    if (sz_to) { absolute_path(sz_to,sz_absolute_to,sizeof(sz_absolute_to)); sz_to=sz_absolute_to; }
    if (game_path(sz_from) || game_path(sz_to)) return -EROFS;
    char sz_a[1024],sz_b[1024];
    int e=translate(sz_from,sz_a,sizeof(sz_a));
    if (!e) e=translate(sz_to,sz_b,sizeof(sz_b));
    if (e) return -e;
    int r=-ps_host_fs_rename(sz_a,sz_b);
    if (save_trace() && (save_path(sz_from) || save_path(sz_to))) printf("Save trace: rename(%s, %s) -> %d\n",sz_from,sz_to,r);
    return r;
}
static int64_t do_ftruncate(int h_fd,int64_t length) {
    int h=host_fd_written(h_fd);
    if (h<0) return -EBADF;
    return -ps_host_fs_truncate(h,length);
}
static int64_t do_truncate(const char *sz_guest,int64_t length) {
    char sz_absolute[1024];
    if (sz_guest) { absolute_path(sz_guest,sz_absolute,sizeof(sz_absolute)); sz_guest=sz_absolute; }
    if (game_path(sz_guest)) return -EROFS;
    char sz_path[1024];
    int e=translate(sz_guest,sz_path,sizeof(sz_path));
    if (e) return -e;
    if (save_path(sz_guest)) current_copy(sz_path,sizeof(sz_path),1);
    if (save_trace() && save_path(sz_guest)) printf("Save trace: truncate(%s, %lld)\n",sz_guest,(long long)length);
    return -ps_host_fs_truncate_path(sz_path,length);
}
typedef struct { int64_t sec, usec; } GuestTimeval;
static int64_t do_utimes(const char *sz_guest,const GuestTimeval *p_times) {
    char sz_absolute[1024];
    if (sz_guest) { absolute_path(sz_guest,sz_absolute,sizeof(sz_absolute)); sz_guest=sz_absolute; }
    struct timespec host[2];
    for (int i=0;p_times && i<2;++i) {
        if (p_times[i].usec<0 || p_times[i].usec>=1000000) return -EINVAL;
        host[i]=(struct timespec){.tv_sec=(time_t)p_times[i].sec,.tv_nsec=(long)p_times[i].usec*1000};
    }
    if (game_path(sz_guest)) return -EROFS;
    char sz_path[1024];
    int e=translate(sz_guest,sz_path,sizeof(sz_path));
    if (e) return -e;
    if (save_path(sz_guest)) current_copy(sz_path,sizeof(sz_path),1);
    return -ps_host_fs_set_times(sz_path,p_times ? host : NULL);
}
static int64_t do_futimes(int h_fd,const GuestTimeval *p_times) {
    struct timespec host_times[2];
    for (int i=0;p_times && i<2;++i) {
        if (p_times[i].usec<0 || p_times[i].usec>=1000000) return -EINVAL;
        host_times[i]=(struct timespec){.tv_sec=(time_t)p_times[i].sec,.tv_nsec=(long)p_times[i].usec*1000};
    }
    const File *p_file=get(h_fd);
    if (p_file && game_path(p_file->sz_path)) return -EROFS;
    int h_host=host_fd_written(h_fd);
    if (h_host<0) return -EBADF;
    return -ps_host_fs_set_times_file(h_host,p_times ? host_times : NULL);
}
static int64_t do_chmod(const char *sz_guest,int mode) {
    char sz_absolute[1024];
    if (sz_guest) { absolute_path(sz_guest,sz_absolute,sizeof(sz_absolute)); sz_guest=sz_absolute; }
    if (game_path(sz_guest)) return -EROFS;
    char sz_path[1024];
    int error=translate(sz_guest,sz_path,sizeof(sz_path));
    if (error) return -error;
    if (save_path(sz_guest)) current_copy(sz_path,sizeof(sz_path),1);
    return -ps_host_fs_set_mode(sz_path,(unsigned)mode);
}
static int64_t do_fchmod(int h_fd,int mode) {
    const File *p_file=get(h_fd);
    if (p_file && game_path(p_file->sz_path)) return -EROFS;
    int h_host=host_fd_written(h_fd);
    if (h_host<0) return -EBADF;
    return -ps_host_fs_set_mode_file(h_host,(unsigned)mode);
}
/* FreeBSD's commands and status flags (O_NONBLOCK 0x4, O_APPEND 0x8); FD_CLOEXEC is 1 on both. */
static int64_t do_fcntl(int h_fd,int command,int64_t argument) {
    int h_host=host_fd(h_fd);
    if (h_host<0) return -EBADF;
    unsigned status;
    int e;
    switch (command) {
    case 1: return ps_host_fs_close_on_exec(h_host);
    case 2: return -ps_host_fs_set_close_on_exec(h_host,argument&1);
    case 3:
        e=ps_host_fs_status(h_host,&status);
        if (e) return -e;
        return (status&PS_HOST_OPEN_ACCESS_MASK)|(status&PS_HOST_OPEN_NONBLOCK ? 0x4 : 0)|(status&PS_HOST_OPEN_APPEND ? 0x8 : 0);
    case 4:
        return -ps_host_fs_set_status(h_host,(argument&0x4 ? PS_HOST_OPEN_NONBLOCK : 0u)|(argument&0x8 ? PS_HOST_OPEN_APPEND : 0u));
    default:
        fprintf(stderr,"Runtime: fcntl command %d on descriptor %d not supported\n",command,h_fd);
        return -EINVAL;
    }
}
static int64_t do_fsync(int h_fd) { int h=host_fd(h_fd); if (h<0) return -EBADF; return -ps_host_fs_sync(h); }
static int64_t do_fdatasync(int h_fd) { int h=host_fd(h_fd); if (h<0) return -EBADF; return -ps_host_fs_sync_data(h); }
/* Everything the game has open goes to disk; host files the runtime keeps for itself are not its. */
static int64_t do_sync(void) {
    int h_copies[MAX_FILES], c_copies=0;
    pthread_mutex_lock(&s_lock);
    for (int i=3;i<MAX_FILES;++i)
        if (s_files[i].used && !s_files[i].is_socket && !s_files[i].p_dir) {
            const int h_copy=ps_host_fs_duplicate(s_files[i].host,-1);
            if (h_copy>=0) h_copies[c_copies++]=h_copy;
        }
    pthread_mutex_unlock(&s_lock);
    /* The copies keep each file open while it is written back, outside the table's lock. */
    for (int i=0;i<c_copies;++i) { ps_host_fs_sync(h_copies[i]); ps_host_fs_close(h_copies[i]); }
    return 0;
}
/* struct iovec, the same on FreeBSD and Linux x86-64. A short transfer ends the vector, as it would
 * have ended the kernel's single one. */
typedef struct { void *p_base; uint64_t cb_base; } GuestIovec;
#define GUEST_IOV_MAX 1024
static int64_t do_vector(int h_fd,const GuestIovec *p_iov,int c_iov,int64_t off_file,int is_write,int is_positional) {
    if (c_iov<0 || c_iov>GUEST_IOV_MAX) return -EINVAL;
    if (c_iov && !p_iov) return -EFAULT;
    if (is_positional && off_file<0) return -EINVAL;
    int64_t cb_total=0;
    for (int i=0;i<c_iov;++i) {
        const int64_t off_at=off_file+cb_total;
        void *p_base=p_iov[i].p_base;
        const uint64_t cb_base=p_iov[i].cb_base;
        int64_t cb_done;
        if (is_write) cb_done=is_positional ? do_pwrite(h_fd,p_base,cb_base,off_at) : do_write(h_fd,p_base,cb_base);
        else cb_done=is_positional ? do_pread(h_fd,p_base,cb_base,off_at) : do_read(h_fd,p_base,cb_base);
        if (cb_done<0) return cb_total ? cb_total : cb_done;
        cb_total+=cb_done;
        if ((uint64_t)cb_done<cb_base) break;
    }
    return cb_total;
}
static int64_t do_readv(int h_fd,const GuestIovec *p_iov,int c_iov) { return do_vector(h_fd,p_iov,c_iov,0,0,0); }
static int64_t do_writev(int h_fd,const GuestIovec *p_iov,int c_iov) { return do_vector(h_fd,p_iov,c_iov,0,1,0); }
static int64_t do_preadv(int h_fd,const GuestIovec *p_iov,int c_iov,int64_t off_file) { return do_vector(h_fd,p_iov,c_iov,off_file,0,1); }
static int64_t do_pwritev(int h_fd,const GuestIovec *p_iov,int c_iov,int64_t off_file) { return do_vector(h_fd,p_iov,c_iov,off_file,1,1); }
/* flock's operations have the same numbers on FreeBSD and in ps_host.h. */
static int64_t do_flock(int h_fd,int operation) {
    const int kind=operation&(PS_HOST_LOCK_SHARED|PS_HOST_LOCK_EXCLUSIVE|PS_HOST_LOCK_UNLOCK);
    if (operation&~(kind|PS_HOST_LOCK_NONBLOCK) || (kind!=PS_HOST_LOCK_SHARED && kind!=PS_HOST_LOCK_EXCLUSIVE && kind!=PS_HOST_LOCK_UNLOCK))
        return -EINVAL;
    const File *p_file=get(h_fd);
    if (!p_file) return -EBADF;
    if (p_file->is_socket) return -EOPNOTSUPP;
    return -ps_host_fs_lock(p_file->host,(unsigned)operation);
}
/* FreeBSD's file flags (UF_*, SF_*) have no host counterpart: the file is checked as the kernel
 * would and the flags are not kept, so fstat keeps reporting none. */
static uint64_t s_c_dropped_flags;
static void note_dropped_flags(uint32_t flags) {
    if (flags && !__atomic_fetch_add(&s_c_dropped_flags,1,__ATOMIC_RELAXED)) {
        printf("Runtime: file flags 0x%x not kept (the host has no BSD file flags)\n",flags);
        runtime_calls_hle_stub("chflags/fchflags (BSD file flags not kept)",&s_c_dropped_flags);
    }
}
static int64_t do_chflags(const char *sz_guest,uint32_t flags) {
    if (!sz_guest) return -EFAULT;
    char sz_absolute[1024];
    absolute_path(sz_guest,sz_absolute,sizeof(sz_absolute));
    sz_guest=sz_absolute;
    if (game_path(sz_guest)) return -EROFS;
    char sz_path[1024];
    PsHostStat s;
    int e=translate(sz_guest,sz_path,sizeof(sz_path));
    if (!e && save_path(sz_guest)) current_copy(sz_path,sizeof(sz_path),0);
    if (!e) e=ps_host_fs_stat(sz_path,&s);
    if (e) return -e;
    note_dropped_flags(flags);
    return 0;
}
static int64_t do_fchflags(int h_fd,uint32_t flags) {
    const File *p_file=get(h_fd);
    if (!p_file) return -EBADF;
    if (game_path(p_file->sz_path)) return -EROFS;
    note_dropped_flags(flags);
    return 0;
}
static int64_t do_pipe(int32_t *p_out_fds) {
    if (!p_out_fds) return -EFAULT;
    int h_hosts[2];
    const int e=ps_host_fs_pipe(h_hosts);
    if (e) return -e;
    const int h_read=adopt(h_hosts[0],0);
    const int h_write=h_read<0 ? -EMFILE : adopt(h_hosts[1],0);
    if (h_write<0) {
        if (h_read>=0) do_close(h_read); else ps_host_fs_close(h_hosts[0]);
        ps_host_fs_close(h_hosts[1]);
        return -EMFILE;
    }
    p_out_fds[0]=h_read; p_out_fds[1]=h_write;
    return 0;
}
/* The copy shares the host file (offset and status) as dup's does. A save file being written keeps
 * its commit on the original descriptor, counted as written since the copy may write it; a
 * folder's copy lists it again from the start. dup2's target is taken over in one step under the
 * table's lock, its old file closed afterwards. Descriptors 0-2 are the host's own stdio, which the
 * game does not get to replace. */
static uint64_t s_c_stdio_replacements;
static int64_t do_dup(int h_fd,int h_target) {
    if (h_target<-1 || h_target>=MAX_FILES) return -EBADF;
    File copy={.used=1};
    pthread_mutex_lock(&s_lock);
    const File *p_file=get(h_fd);
    if (p_file) { copy.is_socket=p_file->is_socket; memcpy(copy.sz_path,p_file->sz_path,sizeof(copy.sz_path)); }
    const int is_open=p_file || (h_fd>=0 && h_fd<3), is_dir=p_file && p_file->p_dir;
    pthread_mutex_unlock(&s_lock);
    if (!is_open) return -EBADF;
    if (h_target==h_fd) return h_fd;
    if (h_target>=0 && h_target<3) {
        if (!__atomic_fetch_add(&s_c_stdio_replacements,1,__ATOMIC_RELAXED)) {
            printf("Runtime: dup2 onto descriptor %d refused: 0-2 are the host's stdio\n",h_target);
            runtime_calls_hle_stub("dup2 onto stdio (refused, EBADF)",&s_c_stdio_replacements);
        }
        return -EBADF;
    }
    if (is_dir) {
        char sz_host_path[1024];
        const int e=translate(copy.sz_path,sz_host_path,sizeof(sz_host_path));
        if (e) return -e;
        if (!(copy.p_dir=list_directory(sz_host_path))) return -ENOMEM;
    }
    File replaced={0};
    pthread_mutex_lock(&s_lock);
    File *p_source=get(h_fd);
    const int h_host=p_source ? p_source->host : h_fd<3 ? h_fd : -1;
    int h_new=h_target;
    if (h_new<0) for (int i=3;i<MAX_FILES;++i) if (!s_files[i].used) { h_new=i; break; }
    int64_t result=h_host<0 ? -EBADF : h_new<0 ? -EMFILE : 0;
    if (!result) {
        copy.host=ps_host_fs_duplicate(h_host,-1);
        if (copy.host<0) result=copy.host;
    }
    if (!result) {
        if (p_source && p_source->sz_temp) p_source->is_dirty=1;
        if (s_files[h_new].used) replaced=s_files[h_new];
        copy.serial=++s_last_serial;
        s_files[h_new]=copy;
        result=h_new;
    }
    pthread_mutex_unlock(&s_lock);
    if (result<0) free_listing(copy.p_dir);
    if (replaced.used) release_file(h_new,&replaced);
    return result;
}
static int64_t do_dup2(int h_fd,int h_target) { return h_target<0 ? -EBADF : do_dup(h_fd,h_target); }
static int64_t do_access(const char *sz_guest,int mode) {
    char sz_absolute[1024];
    if (sz_guest) { absolute_path(sz_guest,sz_absolute,sizeof(sz_absolute)); sz_guest=sz_absolute; }
    char sz_path[1024];
    int e=translate(sz_guest,sz_path,sizeof(sz_path));
    if (e) return -e;
    if (save_path(sz_guest)) current_copy(sz_path,sizeof(sz_path),0);
    int r=-ps_host_fs_access(sz_path,mode&7);
    if (r==-ENOENT && !save_path(sz_guest) && fix_case(sz_path)) r=-ps_host_fs_access(sz_path,mode&7);
    return r;
}

/* Convention adapters: sceKernel* -> Orbis error codes, POSIX -> -1 + errno. */
static int64_t sce(int64_t r) { return r<0 ? ERR(runtime_guest_errno((int)-r)) : r; }
static int64_t posix(int64_t r) { if (r<0) { *runtime_errno()=runtime_guest_errno((int)-r); return -1; } return r; }
#define PAIR(name,params,args) \
    static ABI int64_t sce_##name params { return sce(do_##name args); } \
    static ABI int64_t posix_##name params { return posix(do_##name args); }
PAIR(open,(const char *sz_path,int f,int m),(sz_path,f,m))
PAIR(close,(int h_fd),(h_fd))
PAIR(read,(int h_fd,void *p_buffer,uint64_t cb_buffer),(h_fd,p_buffer,cb_buffer))
PAIR(pread,(int h_fd,void *p_buffer,uint64_t cb_buffer,int64_t off_file),(h_fd,p_buffer,cb_buffer,off_file))
PAIR(write,(int h_fd,const void *p_buffer,uint64_t cb_buffer),(h_fd,p_buffer,cb_buffer))
PAIR(pwrite,(int h_fd,const void *p_buffer,uint64_t cb_buffer,int64_t off_file),(h_fd,p_buffer,cb_buffer,off_file))
PAIR(lseek,(int h_fd,int64_t o,int w),(h_fd,o,w))
PAIR(fstat,(int h_fd,GuestStat *p_stat),(h_fd,p_stat))
PAIR(stat,(const char *sz_path,GuestStat *p_stat),(sz_path,p_stat))
PAIR(getdents,(int h_fd,char *sz_buffer,uint64_t cb_buffer,int64_t *p_base),(h_fd,sz_buffer,cb_buffer,p_base))
PAIR(rename,(const char *sz_a,const char *sz_b),(sz_a,sz_b))
PAIR(ftruncate,(int h_fd,int64_t l),(h_fd,l))
PAIR(truncate,(const char *sz_path,int64_t l),(sz_path,l))
PAIR(fsync,(int h_fd),(h_fd))
PAIR(utimes,(const char *sz_path,const GuestTimeval *p_times),(sz_path,p_times))
PAIR(futimes,(int h_fd,const GuestTimeval *p_times),(h_fd,p_times))
PAIR(chmod,(const char *sz_path,int mode),(sz_path,mode))
PAIR(fchmod,(int h_fd,int mode),(h_fd,mode))
PAIR(fcntl,(int h_fd,int command,int64_t argument),(h_fd,command,argument))
PAIR(fdatasync,(int h_fd),(h_fd))
PAIR(readv,(int h_fd,const GuestIovec *p_iov,int c_iov),(h_fd,p_iov,c_iov))
PAIR(writev,(int h_fd,const GuestIovec *p_iov,int c_iov),(h_fd,p_iov,c_iov))
PAIR(preadv,(int h_fd,const GuestIovec *p_iov,int c_iov,int64_t off_file),(h_fd,p_iov,c_iov,off_file))
PAIR(pwritev,(int h_fd,const GuestIovec *p_iov,int c_iov,int64_t off_file),(h_fd,p_iov,c_iov,off_file))
PAIR(sync,(void),())
static ABI int64_t posix_lstat(const char *sz_path,GuestStat *p_stat) { return posix(do_lstat(sz_path,p_stat)); }
static ABI int64_t posix_fstatat(int h_dir,const char *sz_path,GuestStat *p_stat,int flags) { return posix(do_fstatat(h_dir,sz_path,p_stat,flags)); }
static ABI int64_t posix_chdir(const char *sz_path) { return posix(do_chdir(sz_path)); }
static ABI int64_t posix_flock(int h_fd,int operation) { return posix(do_flock(h_fd,operation)); }
static ABI int64_t posix_chflags(const char *sz_path,uint32_t flags) { return posix(do_chflags(sz_path,flags)); }
static ABI int64_t posix_fchflags(int h_fd,uint32_t flags) { return posix(do_fchflags(h_fd,flags)); }
static ABI int64_t posix_pipe(int32_t *p_out_fds) { return posix(do_pipe(p_out_fds)); }
static ABI int64_t posix_dup(int h_fd) { return posix(do_dup(h_fd,-1)); }
static ABI int64_t posix_dup2(int h_fd,int h_target) { return posix(do_dup2(h_fd,h_target)); }
/* getdirentries without the position out-parameter (Atelier Ryza lists its UI style folders with
 * it; answered by a stub, they read as empty and every UI image fell back to NO IMAGE). */
static ABI int64_t sce_getdents_plain(int h_fd,char *sz_buffer,uint64_t cb_buffer) { return sce(do_getdents(h_fd,sz_buffer,cb_buffer,NULL)); }
static ABI int64_t posix_getdents_plain(int h_fd,char *sz_buffer,uint64_t cb_buffer) { return posix(do_getdents(h_fd,sz_buffer,cb_buffer,NULL)); }
static ABI int64_t posix_access(const char *sz_path,int m) { return posix(do_access(sz_path,m)); }
static ABI int64_t sce_mkdir(const char *sz_path,int m) { return sce(path_op(sz_path,0,m)); }
static ABI int64_t posix_mkdir(const char *sz_path,int m) { return posix(path_op(sz_path,0,m)); }
static ABI int64_t sce_rmdir(const char *sz_path) { return sce(path_op(sz_path,1,0)); }
static ABI int64_t posix_rmdir(const char *sz_path) { return posix(path_op(sz_path,1,0)); }
static ABI int64_t sce_unlink(const char *sz_path) { return sce(path_op(sz_path,2,0)); }
static ABI int64_t posix_unlink(const char *sz_path) { return posix(path_op(sz_path,2,0)); }
static ABI int32_t sce_check_reachability(const char *sz_path) {
    GuestStat s; return (int32_t)sce(do_stat(sz_path,&s));
}

static const RuntimeExport s_exports[]={
    {"sceKernelOpen",sce_open}, {"open",posix_open}, {"_open",posix_open},
    {"sceKernelClose",sce_close}, {"close",posix_close}, {"_close",posix_close},
    {"sceKernelRead",sce_read}, {"read",posix_read}, {"_read",posix_read},
    {"sceKernelPread",sce_pread}, {"pread",posix_pread},
    {"sceKernelWrite",sce_write}, {"write",posix_write}, {"_write",posix_write},
    {"sceKernelPwrite",sce_pwrite}, {"pwrite",posix_pwrite},
    {"sceKernelLseek",sce_lseek}, {"lseek",posix_lseek},
    {"sceKernelFstat",sce_fstat}, {"fstat",posix_fstat}, {"_fstat",posix_fstat},
    {"sceKernelStat",sce_stat}, {"stat",posix_stat}, {"lstat",posix_lstat}, {"fstatat",posix_fstatat},
    {"sceKernelGetdirentries",sce_getdents}, {"getdirentries",posix_getdents}, {"_getdirentries",posix_getdents},
    {"sceKernelGetdents",sce_getdents_plain}, {"getdents",posix_getdents_plain},
    {"sceKernelRename",sce_rename}, {"rename",posix_rename},
    {"sceKernelFtruncate",sce_ftruncate}, {"ftruncate",posix_ftruncate},
    {"sceKernelTruncate",sce_truncate}, {"truncate",posix_truncate},
    {"sceKernelFsync",sce_fsync}, {"fsync",posix_fsync},
    {"sceKernelFdatasync",sce_fdatasync}, {"fdatasync",posix_fdatasync}, {"sceKernelSync",sce_sync}, {"sync",posix_sync},
    {"sceKernelReadv",sce_readv}, {"readv",posix_readv}, {"_readv",posix_readv},
    {"sceKernelWritev",sce_writev}, {"writev",posix_writev}, {"_writev",posix_writev},
    {"sceKernelPreadv",sce_preadv}, {"preadv",posix_preadv}, {"sceKernelPwritev",sce_pwritev}, {"pwritev",posix_pwritev},
    {"chdir",posix_chdir}, {"flock",posix_flock}, {"chflags",posix_chflags}, {"fchflags",posix_fchflags},
    {"pipe",posix_pipe}, {"dup",posix_dup}, {"dup2",posix_dup2}, {"_dup2",posix_dup2},
    {"sceKernelUtimes",sce_utimes}, {"utimes",posix_utimes},
    {"sceKernelFutimes",sce_futimes}, {"futimes",posix_futimes},
    {"sceKernelChmod",sce_chmod}, {"chmod",posix_chmod}, {"sceKernelFchmod",sce_fchmod}, {"fchmod",posix_fchmod},
    {"sceKernelFcntl",sce_fcntl}, {"fcntl",posix_fcntl}, {"_fcntl",posix_fcntl},
    {"access",posix_access}, {"sceKernelCheckReachability",sce_check_reachability},
    {"sceKernelMkdir",sce_mkdir}, {"mkdir",posix_mkdir},
    {"sceKernelRmdir",sce_rmdir}, {"rmdir",posix_rmdir},
    {"sceKernelUnlink",sce_unlink}, {"unlink",posix_unlink},
};
uintptr_t runtime_file_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
/* Host-side helpers for other modules (e.g. SaveData, Fios). */
int64_t runtime_file_open(const char *sz_path,int f,int m) { return do_open(sz_path,f,m); }
int64_t runtime_file_close(int h_fd) { return do_close(h_fd); }
int64_t runtime_file_read(int h_fd,void *p_buffer,uint64_t cb_buffer) { return do_read(h_fd,p_buffer,cb_buffer); }
int64_t runtime_file_pread(int h_fd,void *p_buffer,uint64_t cb_buffer,int64_t off_file) { return do_pread(h_fd,p_buffer,cb_buffer,off_file); }
/* A shared mapping may write the file at any time, so it counts as written from the start. */
int runtime_file_shared_host(int h_fd) { return host_fd_written(h_fd); }
int64_t runtime_file_write(int h_fd,const void *p_buffer,uint64_t cb_buffer) { return do_write(h_fd,p_buffer,cb_buffer); }
int64_t runtime_file_pwrite(int h_fd,const void *p_buffer,uint64_t cb_buffer,int64_t off_file) { return do_pwrite(h_fd,p_buffer,cb_buffer,off_file); }
int64_t runtime_file_lseek(int h_fd,int64_t o,int w) { return do_lseek(h_fd,o,w); }
int64_t runtime_file_stat(const char *sz_path,void *p_stat) { return do_stat(sz_path,p_stat); }
int64_t runtime_file_fstat(int h_fd,void *p_stat) { return do_fstat(h_fd,p_stat); }
int64_t runtime_file_getdents(int h_fd,char *sz_buffer,uint64_t cb_buffer,int64_t *p_base) { return do_getdents(h_fd,sz_buffer,cb_buffer,p_base); }
int runtime_file_translate(const char *sz_guest,char *sz_out,size_t cb_out) { return translate(sz_guest,sz_out,cb_out); }
uint64_t runtime_file_serial(int h_fd) {
    pthread_mutex_lock(&s_lock);
    const File *p_file=get(h_fd);
    const uint64_t serial=p_file ? p_file->serial : 0;
    pthread_mutex_unlock(&s_lock);
    return serial;
}
/* As FreeBSD's vnode filters: a regular file is readable while its offset is before its end and
 * always writable (with 0 room); pipes and sockets as the host polls them. Asked under the table's
 * lock, which a close takes before the host descriptor goes; none of it waits. */
int runtime_file_ready(int h_fd,uint64_t serial,int is_write,int64_t *p_out_data,int *p_out_is_eof) {
    *p_out_data=0; *p_out_is_eof=0;
    pthread_mutex_lock(&s_lock);
    const File *p_file=get(h_fd);
    const int is_open=p_file && p_file->serial==serial;
    PsHostStat s;
    const int is_file=is_open && !p_file->is_socket && !ps_host_fs_stat_file(p_file->host,&s) &&
                      (PS_HOST_MODE_IS_REGULAR(s.mode) || PS_HOST_MODE_IS_DIRECTORY(s.mode));
    int result=-EBADF;
    if (is_file) {
        const int64_t off_now=is_write ? 0 : ps_host_fs_seek(p_file->host,0,1);
        if (!is_write) *p_out_data=off_now<0 || off_now>=s.cb_size ? 0 : s.cb_size-off_now;
        result=is_write || *p_out_data!=0;
    } else if (is_open) {
        unsigned events=0;
        const unsigned wanted=is_write ? PS_HOST_POLL_OUT : PS_HOST_POLL_IN;
        const int e=p_file->is_socket ? ps_host_net_ready(p_file->host,wanted,&events,p_out_data)
                                       : ps_host_fs_poll(p_file->host,wanted,&events,p_out_data);
        *p_out_is_eof=!e && (events&PS_HOST_POLL_HANGUP)!=0;
        result=e ? -e : (events&(wanted|PS_HOST_POLL_HANGUP|PS_HOST_POLL_ERROR))!=0;
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}
/* EVFILT_VNODE: the NOTE_* changes (1 delete, 2 write, 4 extend, 8 attributes, 0x10 link count)
 * since p_inout_state, which the first call fills (p_inout_state[0] < 0) and each call updates. */
int runtime_file_changes(int h_fd,uint64_t serial,int64_t p_inout_state[4]) {
    pthread_mutex_lock(&s_lock);
    const File *p_file=get(h_fd);
    PsHostStat s;
    const int is_gone=!p_file || p_file->serial!=serial || p_file->is_socket || ps_host_fs_stat_file(p_file->host,&s);
    pthread_mutex_unlock(&s_lock);
    if (is_gone) return -EBADF;
    const int64_t now[4]={s.cb_size,(int64_t)s.mtime.tv_sec*1000000000+s.mtime.tv_nsec,
                          (int64_t)s.ctime.tv_sec*1000000000+s.ctime.tv_nsec,(int64_t)s.c_links};
    int notes=0;
    if (p_inout_state[0]>=0) {
        if (!now[3]) notes|=0x1;
        if (now[1]!=p_inout_state[1]) notes|=0x2;
        if (now[0]>p_inout_state[0]) notes|=0x4;
        if (now[2]!=p_inout_state[2]) notes|=0x8;
        if (now[3]!=p_inout_state[3]) notes|=0x10;
    }
    memcpy(p_inout_state,now,sizeof(now));
    return notes;
}
void runtime_file_report(void) {
    printf("Runtime: files opened=%zu, reads=%zu (%llu bytes), writes=%zu, not found=%zu\n",
           s_c_opens,s_c_reads,(unsigned long long)s_cb_read,s_c_writes,s_c_missing);
}
/* Part of the call report, which runs from the shutdown and fault handlers: no stdio, no lock. */
void runtime_file_unmounted_report(void) {
    char sz_line[160];
    for (size_t i=0,c_unmounted=__atomic_load_n(&s_c_unmounted,__ATOMIC_ACQUIRE);i<c_unmounted;++i) {
        int used=snprintf(sz_line,sizeof(sz_line),"Runtime: no mount for %zu guest paths under %s\n",
                          __atomic_load_n(&s_unmounted[i].c_hits,__ATOMIC_RELAXED),s_unmounted[i].sz_root);
        if (used>0 && ps_host_fs_write(1,sz_line,(size_t)used)<0) return;
    }
}
