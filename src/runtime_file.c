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
#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define MAX_FILES 1024
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
typedef struct { int used, host, is_dirty; Listing *p_dir; size_t position; char sz_path[512]; char *sz_commit, *sz_temp; } File;
typedef struct { char guest[64]; char host[512]; } Mount;
static File s_files[MAX_FILES];
static Mount s_mounts[MAX_MOUNTS];
static size_t s_c_mounts, s_opens, s_reads, s_writes, s_missing;
static uint64_t s_cb_read;
static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
#define MAX_UNMOUNTED 32
static struct { char sz_root[64]; size_t c_hits; } s_unmounted[MAX_UNMOUNTED];
static size_t s_c_unmounted;

static int save_path(const char *sz_path) { return sz_path && !strncmp(sz_path,"/savedata",9); }
static int temp_name(const char *sz_name) {
    size_t cb_name=strlen(sz_name);
    return cb_name>6 && !strcmp(sz_name+cb_name-6,".bbtmp");
}
/* Copies left by a crash (none of them open): removed when their directory is mounted. */
static void remove_stale_temps(const char *sz_dir,int depth) {
    DIR *p___dirstream=opendir(sz_dir);
    if (!p___dirstream) return;
    for (struct dirent *p_entry; (p_entry=readdir(p___dirstream));) {
        if (p_entry->d_name[0]=='.') continue;
        char sz_path[1024];
        if ((size_t)snprintf(sz_path,sizeof(sz_path),"%s/%s",sz_dir,p_entry->d_name)>=sizeof(sz_path)) continue;
        if (temp_name(p_entry->d_name)) {
            int is_open_now=0;
            for (int i=3;i<MAX_FILES;++i) if (s_files[i].used && s_files[i].sz_temp && !strcmp(s_files[i].sz_temp,sz_path)) is_open_now=1;
            if (!is_open_now && !unlink(sz_path)) printf("Runtime: removed %s (a save interrupted earlier; the file it would have replaced is intact)\n",sz_path);
        } else if (depth<4 && p_entry->d_type==DT_DIR) remove_stale_temps(sz_path,depth+1);
    }
    closedir(p___dirstream);
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
    if (save_path(sz_guest)) remove_stale_temps(sz_host,0);
    pthread_mutex_unlock(&s_lock);
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
    mkdir(sz_user,0755);
    for (int i=0;i<3;++i) {
        snprintf(sz_path,sizeof(sz_path),"%s/%s",sz_user,writable[i]);
        mkdir(sz_path,0755);
        char sz_guest[32]; snprintf(sz_guest,sizeof(sz_guest),"/%s",writable[i]);
        runtime_file_mount(sz_guest,sz_path);
    }
}
/* Resolve a guest path to a host path; returns 0 or a host errno. */
static int translate(const char *sz_guest,char *sz_out,size_t cb_out) {
    if (!sz_guest || !*sz_guest) return ENOENT;
    char sz_buffer[1024];
    if (sz_guest[0]!='/') snprintf(sz_buffer,sizeof(sz_buffer),"/app0/%s",sz_guest);
    else snprintf(sz_buffer,sizeof(sz_buffer),"%s",sz_guest);
    for (const char *sz_cursor=sz_buffer;(sz_cursor=strstr(sz_cursor,".."));sz_cursor+=2)
        if ((sz_cursor==sz_buffer || sz_cursor[-1]=='/') && (sz_cursor[2]==0 || sz_cursor[2]=='/')) return EACCES;
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
    if (s_trace<0) { const char *sz_value=getenv("PS_FILE_TRACE"); s_trace=sz_value && sz_value[0]=='1'; }
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
        if (access(sz_trial,F_OK)) {
            DIR *p___dirstream=opendir(sz_out[0] ? sz_out : "/");
            if (!p___dirstream) return 0;
            int is_found=0;
            for (struct dirent *p_entry;(p_entry=readdir(p___dirstream));)
                if (!strcasecmp(p_entry->d_name,sz_part)) { snprintf(sz_trial,sizeof(sz_trial),"%s/%s",sz_out,p_entry->d_name); is_found=1; break; }
            closedir(p___dirstream);
            if (!is_found) return 0;
        }
        memcpy(sz_out,sz_trial,strlen(sz_trial)+1);
        sz_cursor+=len;
    }
    if (!strcmp(sz_out,sz_path) || strlen(sz_out)!=strlen(sz_path)) return 0;
    memcpy(sz_path,sz_out,strlen(sz_out)+1);
    return 1;
}
static int host_flags(int flags) {
    int r;
    switch (flags&3) { case 0: r=O_RDONLY; break; case 1: r=O_WRONLY; break; default: r=O_RDWR; }
    if (flags&0x4) r|=O_NONBLOCK;
    if (flags&0x8) r|=O_APPEND;
    if (flags&0x80) r|=O_SYNC;
    if (flags&0x200) r|=O_CREAT;
    if (flags&0x400) r|=O_TRUNC;
    if (flags&0x800) r|=O_EXCL;
    if (flags&0x20000) r|=O_DIRECTORY;
    return r|O_CLOEXEC;
}
static void free_listing(Listing *p_listing) { if (p_listing) { free(p_listing->sz_names); free(p_listing->p_offsets); free(p_listing->p_types); free(p_listing); } }
static Listing *list_directory(const char *sz_path) {
    DIR *p___dirstream=opendir(sz_path);
    if (!p___dirstream) return NULL;
    Listing *p_listing=calloc(1,sizeof(*p_listing));
    size_t c_capacity=0,cb_names=0,cap_names=0;
    struct dirent *p_entry;
    while (p_listing && (p_entry=readdir(p___dirstream))) {
        if (temp_name(p_entry->d_name)) continue;
        size_t cb_name=strlen(p_entry->d_name)+1;
        if (p_listing->c_names==c_capacity) {
            c_capacity=c_capacity ? c_capacity*2 : 64;
            p_listing->p_offsets=realloc(p_listing->p_offsets,c_capacity*sizeof(size_t));
            p_listing->p_types=realloc(p_listing->p_types,c_capacity);
        }
        if (cb_names+cb_name>cap_names) { cap_names=(cb_names+cb_name)*2; p_listing->sz_names=realloc(p_listing->sz_names,cap_names); }
        if (!p_listing->p_offsets || !p_listing->p_types || !p_listing->sz_names) { fputs("Out of memory listing directory\n",stderr); exit(1); }
        memcpy(p_listing->sz_names+cb_names,p_entry->d_name,cb_name);
        p_listing->p_offsets[p_listing->c_names]=cb_names;
        unsigned char type=p_entry->d_type;
        if (type==DT_LNK || type==DT_UNKNOWN) {
            struct stat entry;
            if (!fstatat(dirfd(p___dirstream),p_entry->d_name,&entry,0))
                type=S_ISDIR(entry.st_mode) ? DT_DIR : S_ISREG(entry.st_mode) ? DT_REG : type;
        }
        p_listing->p_types[p_listing->c_names]=type==DT_DIR ? 4 : type==DT_REG ? 8 : type==DT_LNK ? 10 : 0;
        ++p_listing->c_names; cb_names+=cb_name;
    }
    closedir(p___dirstream);
    return p_listing;
}
static void convert_stat(const struct stat *p_host_stat,GuestStat *p_guest_stat) {
    memset(p_guest_stat,0,sizeof(*p_guest_stat));
    p_guest_stat->dev=(uint32_t)p_host_stat->st_dev; p_guest_stat->ino=(uint32_t)p_host_stat->st_ino;
    p_guest_stat->mode=(uint16_t)p_host_stat->st_mode; p_guest_stat->nlink=(uint16_t)p_host_stat->st_nlink;
    p_guest_stat->size=p_host_stat->st_size; p_guest_stat->blocks=p_host_stat->st_blocks; p_guest_stat->blksize=(uint32_t)p_host_stat->st_blksize;
    p_guest_stat->atime=(GuestTimespec){p_host_stat->st_atim.tv_sec,p_host_stat->st_atim.tv_nsec};
    p_guest_stat->mtime=(GuestTimespec){p_host_stat->st_mtim.tv_sec,p_host_stat->st_mtim.tv_nsec};
    p_guest_stat->ctime=(GuestTimespec){p_host_stat->st_ctim.tv_sec,p_host_stat->st_ctim.tv_nsec};
    p_guest_stat->birthtime=p_guest_stat->ctime;
}
static File *get(int h_fd) {
    if (h_fd<3 || h_fd>=MAX_FILES || !s_files[h_fd].used) return NULL;
    return &s_files[h_fd];
}
/* PS_AUDIO_TRACE=1: sound file opens and failed reads (missing game sounds). */
static int audio_trace(void) { static int s_is_enabled=-1; if (s_is_enabled<0) { const char *sz_value=getenv("PS_AUDIO_TRACE"); s_is_enabled=sz_value && sz_value[0]=='1'; } return s_is_enabled; }
/* PS_SAVE_TRACE=1: every operation on save files (/savedataN). */
static int save_trace(void) { static int s_is_enabled=-1; if (s_is_enabled<0) { const char *sz_value=getenv("PS_SAVE_TRACE"); s_is_enabled=sz_value && sz_value[0]=='1'; } return s_is_enabled; }
/* Game mounts (including linked mod overlays) are read-only. Saves use other mounts. */
static int game_path(const char *sz_path) {
    if (!sz_path || !*sz_path) return 0;
    if (*sz_path!='/') return 1;
    return (!strncmp(sz_path,"/app0",5) && (!sz_path[5] || sz_path[5]=='/')) ||
           (!strncmp(sz_path,"/hostapp",8) && (!sz_path[8] || sz_path[8]=='/'));
}
/* Saves (/savedataN): see File. The copy starts as the old file unless the open truncates. */
static unsigned s_temp_serial;
static int copy_contents(int from,int to) {
    char sz_buffer[65536];
    for (;;) {
        ssize_t cb_read=read(from,sz_buffer,sizeof(sz_buffer));
        if (cb_read<0 && errno==EINTR) continue;
        if (cb_read<=0) return cb_read<0 ? -1 : 0;
        for (ssize_t done=0; done<cb_read;) {
            ssize_t w=write(to,sz_buffer+done,(size_t)(cb_read-done));
            if (w<0 && errno==EINTR) continue;
            if (w<0) return -1;
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
    int hf=host_flags(flags);
    struct stat s;
    int exists=!stat(sz_source,&s);
    if (exists && !S_ISREG(s.st_mode)) { int h=open(sz_path,hf,mode ? mode : 0644); return h<0 ? -errno : h; }
    if (exists && (hf&O_CREAT) && (hf&O_EXCL)) return -EEXIST;
    if (!exists && !(hf&O_CREAT)) return -ENOENT;
    size_t cb_temp=strlen(sz_path)+32;
    char *sz_temp=malloc(cb_temp), *sz_commit=strdup(sz_path);
    if (!sz_temp || !sz_commit) { free(sz_temp); free(sz_commit); return -ENOMEM; }
    snprintf(sz_temp,cb_temp,"%s.%u.bbtmp",sz_path,__atomic_add_fetch(&s_temp_serial,1,__ATOMIC_RELAXED));
    int out=open(sz_temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,exists ? (int)(s.st_mode&07777) : mode ? mode : 0644);
    int e=out<0 ? errno : 0;
    if (!e && exists && !(hf&O_TRUNC)) {
        int in=open(sz_source,O_RDONLY|O_CLOEXEC);
        if (in<0 || copy_contents(in,out)) e=errno ? errno : EIO;
        if (in>=0) close(in);
    }
    if (out>=0) close(out);
    int host=e ? -1 : open(sz_temp,hf&~(O_CREAT|O_EXCL|O_TRUNC));
    if (host<0) {
        if (!e) e=errno;
        unlink(sz_temp); free(sz_temp); free(sz_commit);
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
    int d=open(sz_dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (d>=0) { fsync(d); close(d); }
}
static int commit_file(const File *p_file) {
    if (!p_file->is_dirty) { close(p_file->host); unlink(p_file->sz_temp); return 0; } /* opened for writing, not written */
    int e=fsync(p_file->host) ? errno : 0;
    close(p_file->host);
    if (!e && rename(p_file->sz_temp,p_file->sz_commit)) e=errno;
    if (e) {
        fprintf(stderr,"Runtime: save file %s not replaced (%s); the old one is kept\n",p_file->sz_path,strerror(e));
        unlink(p_file->sz_temp);
        return -e;
    }
    sync_directory(p_file->sz_commit);
    return 0;
}
/* All operations return >=0 or -(host errno); wrappers adapt the convention. */
static int64_t do_open(const char *sz_guest,int flags,int mode) {
    if (game_path(sz_guest) && (flags & (3|0x8|0x200|0x400|0x800))) return -EROFS;
    char sz_path[1024];
    int e=translate(sz_guest,sz_path,sizeof(sz_path));
    if (e) return -e;
    char *sz_commit=NULL, *sz_temp=NULL, sz_current[1024];
    snprintf(sz_current,sizeof(sz_current),"%s",sz_path);
    if (save_path(sz_guest)) current_copy(sz_current,sizeof(sz_current),0);
    int host;
    if (save_path(sz_guest) && (flags&3)) {
        host=open_for_commit(sz_path,sz_current,flags,mode,&sz_commit,&sz_temp);
        if (host<0) { errno=-host; host=-1; }
    } else {
        host=open(sz_current,host_flags(flags),mode ? mode : 0644);
        if (host<0 && errno==ENOENT && !(flags&0x200) && !save_path(sz_guest) && fix_case(sz_current)) {
            host=open(sz_current,host_flags(flags),mode ? mode : 0644);
            snprintf(sz_path,sizeof(sz_path),"%s",sz_current); // the directory listing below uses it
        }
    }
    if (save_trace() && save_path(sz_guest))
        printf("Save trace: open(%s, flags 0x%x) -> host %d%s%s\n",sz_guest,flags,host,sz_temp ? ", copy " : "",sz_temp ? sz_temp : "");
    if (host<0) {
        e=errno;
        if (e==ENOENT) { ++s_missing; printf("Runtime: open(%s) -> not found\n",sz_guest); }
        return -e;
    }
    struct stat s;
    Listing *p_dir=NULL;
    if (!fstat(host,&s) && S_ISDIR(s.st_mode)) p_dir=list_directory(sz_path);
    pthread_mutex_lock(&s_lock);
    int h_fd=-1;
    for (int i=3;i<MAX_FILES;++i) if (!s_files[i].used) { h_fd=i; break; }
    if (h_fd<0) {
        pthread_mutex_unlock(&s_lock); close(host); free_listing(p_dir);
        if (sz_temp) unlink(sz_temp);
        free(sz_commit); free(sz_temp); return -EMFILE;
    }
    s_files[h_fd]=(File){.used=1,.host=host,.p_dir=p_dir,.sz_commit=sz_commit,.sz_temp=sz_temp};
    snprintf(s_files[h_fd].sz_path,sizeof(s_files[h_fd].sz_path),"%s",sz_guest);
    ++s_opens;
    pthread_mutex_unlock(&s_lock);
    if (audio_trace() && strstr(sz_guest,"sound/")) printf("Audio trace: open(%s) -> fd %d, %lld bytes\n",sz_guest,h_fd,(long long)s.st_size);
    const char *sz_mod_trace=getenv("PS_MOD_TRACE"), *sz_mod_root=getenv("PS_MODS_DIR");
    if (sz_mod_trace && sz_mod_trace[0]=='1' && sz_mod_root) {
        char sz_actual[PATH_MAX],sz_root[PATH_MAX];
        static unsigned s_traced;
        if (realpath(sz_path,sz_actual) && realpath(sz_mod_root,sz_root)) {
            size_t cb_root=strlen(sz_root);
            if (!strncmp(sz_actual,sz_root,cb_root) && sz_actual[cb_root]=='/' &&
                __atomic_fetch_add(&s_traced,1,__ATOMIC_RELAXED)<32)
                printf("Mods: open %s -> %s\n",sz_guest,sz_actual);
        }
    }
    if (save_trace() && save_path(sz_guest)) printf("Save trace: open(%s) -> fd %d\n",sz_guest,h_fd);
    return h_fd;
}
static int64_t do_close(int h_fd) {
    if (h_fd>=0 && h_fd<3) return 0;
    pthread_mutex_lock(&s_lock);
    File *p_file=get(h_fd);
    if (!p_file) { pthread_mutex_unlock(&s_lock); return -EBADF; }
    File closed=*p_file;
    *p_file=(File){0};
    pthread_mutex_unlock(&s_lock);
    free_listing(closed.p_dir);
    int result=0;
    if (closed.sz_temp) result=commit_file(&closed);
    else close(closed.host);
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
int runtime_file_adopt(int h_host) {
    pthread_mutex_lock(&s_lock);
    int h_fd=-1;
    for (int i=3;i<MAX_FILES;++i) if (!s_files[i].used) { h_fd=i; break; }
    if (h_fd>=0) s_files[h_fd]=(File){.used=1,.host=h_host};
    pthread_mutex_unlock(&s_lock);
    return h_fd<0 ? -EMFILE : h_fd;
}
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
static ssize_t read_retrying(int h,void *p_buffer,uint64_t cb_buffer,int64_t off_file,int positional) {
    ssize_t cb_transferred=-1;
    for (int attempt=0; attempt<8; ++attempt) {
        cb_transferred=positional ? pread(h,p_buffer,cb_buffer,off_file) : read(h,p_buffer,cb_buffer);
        if (cb_transferred>=0 || errno!=EFAULT) break;
        touch_for_write(p_buffer,cb_buffer);
    }
    return cb_transferred;
}
static int64_t do_read(int h_fd,void *p_buffer,uint64_t cb_buffer) {
    int h=host_fd(h_fd);
    if (h<0) return -EBADF;
    runtime_memory_note_write((uintptr_t)p_buffer,cb_buffer);
    touch_for_write(p_buffer,cb_buffer);
    ssize_t cb_transferred=read_retrying(h,p_buffer,cb_buffer,0,0);
    if (cb_transferred<0) { if (audio_trace()) printf("Audio trace: read(fd %d, %llu) failed, errno %d\n",h_fd,(unsigned long long)cb_buffer,errno); return -errno; }
    if (cb_transferred>0) runtime_memory_note_write((uintptr_t)p_buffer,(uint64_t)cb_transferred); /* and once the data is there */
    __atomic_add_fetch(&s_reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&s_cb_read,(uint64_t)cb_transferred,__ATOMIC_RELAXED);
    return cb_transferred;
}
static int64_t do_pread(int h_fd,void *p_buffer,uint64_t cb_buffer,int64_t off_file) {
    int h=host_fd(h_fd);
    if (h<0) return -EBADF;
    runtime_memory_note_write((uintptr_t)p_buffer,cb_buffer);
    touch_for_write(p_buffer,cb_buffer);
    ssize_t cb_transferred=read_retrying(h,p_buffer,cb_buffer,off_file,1);
    if (cb_transferred<0) { if (audio_trace()) printf("Audio trace: pread(fd %d, %llu @%lld) failed, errno %d\n",h_fd,(unsigned long long)cb_buffer,(long long)off_file,errno); return -errno; }
    if (cb_transferred>0) runtime_memory_note_write((uintptr_t)p_buffer,(uint64_t)cb_transferred); /* and once the data is there */
    __atomic_add_fetch(&s_reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&s_cb_read,(uint64_t)cb_transferred,__ATOMIC_RELAXED);
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
    ssize_t cb_transferred=write(h,p_buffer,cb_buffer);
    if (cb_transferred<0) return -errno;
    __atomic_add_fetch(&s_writes,1,__ATOMIC_RELAXED);
    return cb_transferred;
}
static int64_t do_pwrite(int h_fd,const void *p_buffer,uint64_t cb_buffer,int64_t off_file) {
    int h=host_fd_written(h_fd);
    if (h<0) return -EBADF;
    touch_for_read(p_buffer,cb_buffer);
    ssize_t cb_transferred=pwrite(h,p_buffer,cb_buffer,off_file);
    return cb_transferred<0 ? -errno : cb_transferred;
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
    off_t r=lseek(p_file->host,off_file,whence);
    return r<0 ? -errno : r;
}
static int64_t do_fstat(int h_fd,GuestStat *p_out) {
    if (save_trace()) { File *p_file=get(h_fd); if (p_file && save_path(p_file->sz_path)) printf("Save trace: fstat(fd %d)\n",h_fd); }
    int h=host_fd(h_fd);
    if (h<0) return -EBADF;
    if (!p_out) return -EFAULT;
    struct stat s;
    if (fstat(h,&s)) return -errno;
    convert_stat(&s,p_out); return 0;
}
static int64_t do_stat(const char *sz_guest,GuestStat *p_out) {
    char sz_path[1024]; struct stat s;
    int e=translate(sz_guest,sz_path,sizeof(sz_path));
    if (e) return -e;
    if (save_path(sz_guest)) current_copy(sz_path,sizeof(sz_path),0);
    if (!p_out) return -EFAULT;
    int r=stat(sz_path,&s) ? -errno : 0;
    if (r==-ENOENT && !save_path(sz_guest) && fix_case(sz_path)) r=stat(sz_path,&s) ? -errno : 0;
    if (save_trace() && save_path(sz_guest)) printf("Save trace: stat(%s) -> %d, %lld bytes\n",sz_guest,r,r ? -1LL : (long long)s.st_size);
    if (r) return r;
    convert_stat(&s,p_out); return 0;
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
    if (game_path(sz_guest)) return -EROFS;
    char sz_path[1024];
    int e=translate(sz_guest,sz_path,sizeof(sz_path));
    if (e) return -e;
    int r= op==0 ? mkdir(sz_path,mode ? mode : 0755) : op==1 ? rmdir(sz_path) : unlink(sz_path);
    r=r ? -errno : 0;
    if (save_trace() && save_path(sz_guest)) printf("Save trace: %s(%s) -> %d\n",op==0 ? "mkdir" : op==1 ? "rmdir" : "unlink",sz_guest,r);
    return r;
}
static int64_t do_rename(const char *sz_from,const char *sz_to) {
    if (game_path(sz_from) || game_path(sz_to)) return -EROFS;
    char sz_a[1024],sz_b[1024];
    int e=translate(sz_from,sz_a,sizeof(sz_a));
    if (!e) e=translate(sz_to,sz_b,sizeof(sz_b));
    if (e) return -e;
    int r=rename(sz_a,sz_b) ? -errno : 0;
    if (save_trace() && (save_path(sz_from) || save_path(sz_to))) printf("Save trace: rename(%s, %s) -> %d\n",sz_from,sz_to,r);
    return r;
}
static int64_t do_ftruncate(int h_fd,int64_t length) {
    int h=host_fd_written(h_fd);
    if (h<0) return -EBADF;
    return ftruncate(h,length) ? -errno : 0;
}
static int64_t do_truncate(const char *sz_guest,int64_t length) {
    if (game_path(sz_guest)) return -EROFS;
    char sz_path[1024];
    int e=translate(sz_guest,sz_path,sizeof(sz_path));
    if (e) return -e;
    if (save_path(sz_guest)) current_copy(sz_path,sizeof(sz_path),1);
    if (save_trace() && save_path(sz_guest)) printf("Save trace: truncate(%s, %lld)\n",sz_guest,(long long)length);
    return truncate(sz_path,length) ? -errno : 0;
}
typedef struct { int64_t sec, usec; } GuestTimeval;
static int64_t do_utimes(const char *sz_guest,const GuestTimeval *p_times) {
    struct timeval host[2];
    for (int i=0;p_times && i<2;++i) {
        if (p_times[i].usec<0 || p_times[i].usec>=1000000) return -EINVAL;
        host[i].tv_sec=(time_t)p_times[i].sec; host[i].tv_usec=(suseconds_t)p_times[i].usec;
    }
    if (game_path(sz_guest)) return -EROFS;
    char sz_path[1024];
    int e=translate(sz_guest,sz_path,sizeof(sz_path));
    if (e) return -e;
    if (save_path(sz_guest)) current_copy(sz_path,sizeof(sz_path),1);
    return utimes(sz_path,p_times ? host : NULL) ? -errno : 0;
}
static int64_t do_futimes(int h_fd,const GuestTimeval *p_times) {
    struct timeval host_times[2];
    for (int i=0;p_times && i<2;++i) {
        if (p_times[i].usec<0 || p_times[i].usec>=1000000) return -EINVAL;
        host_times[i].tv_sec=(time_t)p_times[i].sec; host_times[i].tv_usec=(suseconds_t)p_times[i].usec;
    }
    const File *p_file=get(h_fd);
    if (p_file && game_path(p_file->sz_path)) return -EROFS;
    int h_host=host_fd_written(h_fd);
    if (h_host<0) return -EBADF;
    return futimes(h_host,p_times ? host_times : NULL) ? -errno : 0;
}
static int64_t do_chmod(const char *sz_guest,int mode) {
    if (game_path(sz_guest)) return -EROFS;
    char sz_path[1024];
    int error=translate(sz_guest,sz_path,sizeof(sz_path));
    if (error) return -error;
    if (save_path(sz_guest)) current_copy(sz_path,sizeof(sz_path),1);
    return chmod(sz_path,(mode_t)(mode&07777)) ? -errno : 0;
}
static int64_t do_fchmod(int h_fd,int mode) {
    const File *p_file=get(h_fd);
    if (p_file && game_path(p_file->sz_path)) return -EROFS;
    int h_host=host_fd_written(h_fd);
    if (h_host<0) return -EBADF;
    return fchmod(h_host,(mode_t)(mode&07777)) ? -errno : 0;
}
/* FreeBSD's commands and status flags (O_NONBLOCK 0x4, O_APPEND 0x8); FD_CLOEXEC is 1 on both. */
static int64_t do_fcntl(int h_fd,int command,int64_t argument) {
    int h_host=host_fd(h_fd);
    if (h_host<0) return -EBADF;
    int r;
    switch (command) {
    case 1: r=fcntl(h_host,F_GETFD); break;
    case 2: r=fcntl(h_host,F_SETFD,argument&1 ? FD_CLOEXEC : 0); break;
    case 3:
        r=fcntl(h_host,F_GETFL);
        if (r>=0) r=(r&O_ACCMODE)|(r&O_NONBLOCK ? 0x4 : 0)|(r&O_APPEND ? 0x8 : 0);
        break;
    case 4:
        r=fcntl(h_host,F_GETFL);
        if (r>=0) r=fcntl(h_host,F_SETFL,(r&~(O_NONBLOCK|O_APPEND))|(argument&0x4 ? O_NONBLOCK : 0)|(argument&0x8 ? O_APPEND : 0));
        break;
    default:
        fprintf(stderr,"Runtime: fcntl command %d on descriptor %d not supported\n",command,h_fd);
        return -EINVAL;
    }
    return r<0 ? -errno : r;
}
static int64_t do_fsync(int h_fd) { int h=host_fd(h_fd); if (h<0) return -EBADF; return fsync(h) ? -errno : 0; }
static int64_t do_access(const char *sz_guest,int mode) {
    char sz_path[1024];
    int e=translate(sz_guest,sz_path,sizeof(sz_path));
    if (e) return -e;
    if (save_path(sz_guest)) current_copy(sz_path,sizeof(sz_path),0);
    int r=access(sz_path,mode&7) ? -errno : 0;
    if (r==-ENOENT && !save_path(sz_guest) && fix_case(sz_path)) r=access(sz_path,mode&7) ? -errno : 0;
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
    {"sceKernelFstat",sce_fstat}, {"fstat",posix_fstat},
    {"sceKernelStat",sce_stat}, {"stat",posix_stat},
    {"sceKernelGetdirentries",sce_getdents}, {"getdirentries",posix_getdents},
    {"sceKernelGetdents",sce_getdents_plain}, {"getdents",posix_getdents_plain},
    {"sceKernelRename",sce_rename}, {"rename",posix_rename},
    {"sceKernelFtruncate",sce_ftruncate}, {"ftruncate",posix_ftruncate},
    {"sceKernelTruncate",sce_truncate}, {"truncate",posix_truncate},
    {"sceKernelFsync",sce_fsync}, {"fsync",posix_fsync},
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
void runtime_file_report(void) {
    printf("Runtime: files opened=%zu, reads=%zu (%llu bytes), writes=%zu, not found=%zu\n",
           s_opens,s_reads,(unsigned long long)s_cb_read,s_writes,s_missing);
}
/* Part of the call report, which runs from the shutdown and fault handlers: no stdio, no lock. */
void runtime_file_unmounted_report(void) {
    char sz_line[160];
    for (size_t i=0,c_unmounted=__atomic_load_n(&s_c_unmounted,__ATOMIC_ACQUIRE);i<c_unmounted;++i) {
        int used=snprintf(sz_line,sizeof(sz_line),"Runtime: no mount for %zu guest paths under %s\n",
                          __atomic_load_n(&s_unmounted[i].c_hits,__ATOMIC_RELAXED),s_unmounted[i].sz_root);
        if (used>0 && write(1,sz_line,(size_t)used)<0) return;
    }
}
#else
uintptr_t runtime_file_resolve(const char *name) { (void)name; return 0; }
void runtime_file_report(void) {}
void runtime_file_unmounted_report(void) {}
void runtime_file_configure(const char *a,const char *u) { (void)a; (void)u; }
#endif
