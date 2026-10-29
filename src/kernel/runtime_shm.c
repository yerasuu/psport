// SPDX-License-Identifier: GPL-2.0-or-later
/* POSIX shared memory objects (shm_open, shm_unlink): a table of names over anonymous host files
 * (ps_host_fs_open_anonymous). Each open is a descriptor of its own in the guest's file table, so
 * ftruncate, fstat, mmap(MAP_SHARED) and close take the path of regular files (runtime_file.c,
 * runtime_memory_kernel.c). An object lives while it has its name or an open descriptor.
 * CommonDialog keeps its work page in one ("/<app id>CdlgShWk", 0x1000 bytes). */
#define _GNU_SOURCE
#include "runtime.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define SHM_OBJECTS_MAX 64
/* NAME_MAX, the leading slash included. */
#define CB_SHM_NAME_MAX 256
#define LOGGED_FAILURES 8
/* FreeBSD's SHM_ANON: an object with no name, gone with its last descriptor. */
#define SHM_ANON ((const char *)(uintptr_t)1)
enum {
    GUEST_O_ACCMODE = 0x3,
    GUEST_O_RDONLY = 0x0,
    GUEST_O_RDWR = 0x2,
    GUEST_O_CREAT = 0x200,
    GUEST_O_TRUNC = 0x400,
    GUEST_O_EXCL = 0x800,
    GUEST_O_CLOEXEC = 0x100000,
    /* CommonDialog adds it from SDK 3.50; its meaning was not read from the firmware (kb #1248). */
    SHM_FLAG_UNREAD = 0x200000,
};

typedef struct {
    char sz_name[CB_SHM_NAME_MAX];
    int h_object;
    int is_used;
} ShmObject;

static ShmObject s_objects[SHM_OBJECTS_MAX];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_c_created, s_c_opened, s_c_anonymous, s_c_unlinked, s_c_failed, s_c_table_full, s_c_unread_flag;
static int s_has_summary;

/* Part of the call report: no stdio, no lock. */
static void summary(char *sz_out, size_t cb_out) {
    snprintf(sz_out, cb_out,
             "Runtime: summary shared memory objects: %llu created, %llu opened existing, %llu anonymous, %llu unlinked, "
             "%llu failed (%llu with the table of %d full)\n",
             (unsigned long long)__atomic_load_n(&s_c_created, __ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&s_c_opened, __ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&s_c_anonymous, __ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&s_c_unlinked, __ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&s_c_failed, __ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&s_c_table_full, __ATOMIC_RELAXED), SHM_OBJECTS_MAX);
}

static void count(uint64_t *p_counter) {
    __atomic_fetch_add(p_counter, 1, __ATOMIC_RELAXED);
    if (!__atomic_exchange_n(&s_has_summary, 1, __ATOMIC_ACQ_REL)) runtime_calls_add_summary(summary);
}

/* 0, or the errno the name gets: it starts with '/', has no other and fits NAME_MAX. */
static int check_name(const char *sz_name) {
    if (!sz_name) return EFAULT;
    if (sz_name == SHM_ANON || sz_name[0] != '/') return EINVAL;
    const size_t cb_name = strnlen(sz_name, CB_SHM_NAME_MAX);
    if (cb_name == CB_SHM_NAME_MAX) return ENAMETOOLONG;
    return cb_name < 2 || strchr(sz_name + 1, '/') ? EINVAL : 0;
}

static int64_t failed(const char *sz_call, const char *sz_name, int flags, int error) {
    const uint64_t c_before = __atomic_load_n(&s_c_failed, __ATOMIC_RELAXED);
    count(&s_c_failed);
    if (c_before < LOGGED_FAILURES)
        printf("Runtime: %s(%.*s, flags 0x%x) failed: %s%s\n", sz_call, sz_name == SHM_ANON ? 8 : CB_SHM_NAME_MAX - 1,
               sz_name == SHM_ANON ? "SHM_ANON" : check_name(sz_name) == EFAULT ? "NULL" : sz_name, (unsigned)flags,
               strerror(error), c_before + 1 == LOGGED_FAILURES ? " (further failures are only counted)" : "");
    return -error;
}

static ShmObject *find(const char *sz_name) {
    for (int i = 0; i < SHM_OBJECTS_MAX; ++i)
        if (s_objects[i].is_used && !strcmp(s_objects[i].sz_name, sz_name)) return &s_objects[i];
    return NULL;
}

/* A guest descriptor of its own on the object, or minus the host's errno. */
static int64_t open_descriptor(int h_object, unsigned access, const char *sz_name) {
    const int h_host = ps_host_fs_reopen(h_object, access);
    if (h_host < 0) return h_host;
    const int h_fd = runtime_file_adopt_shared_memory(h_host, sz_name);
    if (h_fd < 0) ps_host_fs_close(h_host);
    return h_fd;
}

static int64_t open_anonymous(unsigned access) {
    if (access != PS_HOST_OPEN_READ_WRITE) return -EINVAL;
    const int h_object = ps_host_fs_open_anonymous("psport-shm-anonymous");
    if (h_object < 0) return h_object;
    const int64_t h_fd = open_descriptor(h_object, access, "(SHM_ANON)");
    ps_host_fs_close(h_object);
    if (h_fd >= 0) count(&s_c_anonymous);
    return h_fd;
}

static int64_t open_named(const char *sz_name, int flags, unsigned access) {
    pthread_mutex_lock(&s_lock);
    ShmObject *p_object = find(sz_name);
    const int is_created = !p_object;
    int64_t result = 0;
    if (p_object && (flags & GUEST_O_CREAT) && (flags & GUEST_O_EXCL))
        result = -EEXIST;
    else if (!p_object && !(flags & GUEST_O_CREAT))
        result = -ENOENT;
    else if (!p_object) {
        for (int i = 0; i < SHM_OBJECTS_MAX && !p_object; ++i)
            if (!s_objects[i].is_used) p_object = &s_objects[i];
        if (!p_object) {
            count(&s_c_table_full);
            result = -ENFILE;
        } else {
            char sz_label[64];
            snprintf(sz_label, sizeof(sz_label), "psport-shm-%.40s", sz_name + 1);
            const int h_object = ps_host_fs_open_anonymous(sz_label);
            if (h_object < 0)
                result = h_object;
            else {
                snprintf(p_object->sz_name, sizeof(p_object->sz_name), "%s", sz_name);
                p_object->h_object = h_object;
                p_object->is_used = 1;
            }
        }
    }
    if (!result && (flags & (GUEST_O_ACCMODE | GUEST_O_TRUNC)) == (GUEST_O_RDWR | GUEST_O_TRUNC))
        result = -ps_host_fs_truncate(p_object->h_object, 0);
    if (!result) result = open_descriptor(p_object->h_object, access, sz_name);
    if (result < 0 && is_created && p_object && p_object->is_used) {
        ps_host_fs_close(p_object->h_object);
        *p_object = (ShmObject){0};
    }
    pthread_mutex_unlock(&s_lock);
    if (result >= 0) count(is_created ? &s_c_created : &s_c_opened);
    return result;
}

static int64_t do_shm_open(const char *sz_name, int flags, unsigned mode) {
    /* One process holds every object: the permission bits restrict no one. */
    (void)mode;
    const int access_mode = flags & GUEST_O_ACCMODE;
    if ((access_mode != GUEST_O_RDONLY && access_mode != GUEST_O_RDWR) ||
        (flags & ~(GUEST_O_ACCMODE | GUEST_O_CREAT | GUEST_O_EXCL | GUEST_O_TRUNC | GUEST_O_CLOEXEC | SHM_FLAG_UNREAD)))
        return failed("shm_open", sz_name, flags, EINVAL);
    if ((flags & SHM_FLAG_UNREAD) && !__atomic_fetch_add(&s_c_unread_flag, 1, __ATOMIC_RELAXED)) {
        printf("Runtime: shm_open flag 0x%x ignored: its meaning is not known\n", SHM_FLAG_UNREAD);
        runtime_calls_hle_stub("shm_open flag 0x200000 (ignored)", &s_c_unread_flag);
    }
    const unsigned access = access_mode == GUEST_O_RDWR ? PS_HOST_OPEN_READ_WRITE : PS_HOST_OPEN_READ;
    int64_t result;
    if (sz_name == SHM_ANON)
        result = open_anonymous(access);
    else {
        const int error = check_name(sz_name);
        result = error ? -error : open_named(sz_name, flags, access);
    }
    return result < 0 ? failed("shm_open", sz_name, flags, (int)-result) : result;
}

static int64_t do_shm_unlink(const char *sz_name) {
    const int error = check_name(sz_name);
    if (error) return failed("shm_unlink", sz_name, 0, error);
    pthread_mutex_lock(&s_lock);
    ShmObject *p_object = find(sz_name);
    if (p_object) {
        ps_host_fs_close(p_object->h_object);
        *p_object = (ShmObject){0};
    }
    pthread_mutex_unlock(&s_lock);
    if (!p_object) return failed("shm_unlink", sz_name, 0, ENOENT);
    count(&s_c_unlinked);
    return 0;
}

static int64_t posix(int64_t result) {
    if (result >= 0) return result;
    *runtime_errno() = runtime_guest_errno((int)-result);
    return -1;
}

static ABI int64_t posix_shm_open(const char *sz_name, int flags, unsigned mode) {
    return posix(do_shm_open(sz_name, flags, mode));
}
static ABI int64_t posix_shm_unlink(const char *sz_name) { return posix(do_shm_unlink(sz_name)); }

static const RuntimeExport s_exports[] = {
    {"shm_open", posix_shm_open},
    {"shm_unlink", posix_shm_unlink},
};
uintptr_t runtime_shm_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports, sz_name); }
