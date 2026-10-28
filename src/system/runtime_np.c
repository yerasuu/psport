// SPDX-License-Identifier: GPL-2.0-or-later
/* PSN libraries offline: NpWebApi and NpWebApi2, NpInGameMessage, libSceNet's and libSceSsl's
 * initialization with libSceNpManager's network start, the memory pool statistics of Net/Ssl/Http/WebApi, libSceSsl's certificate calls,
 * NpGameIntent, the PS Store icon and the NpManager/NpAuth calls the toolkits make. Score, Tus/Tss,
 * WordFilter, BandwidthTest and Matching2 are in runtime_np_community.c. */
#define _GNU_SOURCE
#include "runtime_np.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USER_ID 1
#define NP_USER_NOT_FOUND ((int32_t)0x80550007)
#define NP_AUTH_INVALID_ARGUMENT ((int32_t)0x80550301)
#define WEBAPI_INVALID_ARGUMENT ((int32_t)0x80552902)
#define WEBAPI_USER_CONTEXT_NOT_FOUND ((int32_t)0x80552905)
#define WEBAPI_REQUEST_NOT_FOUND ((int32_t)0x80552906)
#define WEBAPI_NOT_SIGNED_IN ((int32_t)0x80552907)
#define WEBAPI_PUSH_EVENT_FILTER_NOT_FOUND ((int32_t)0x8055290B)
#define WEBAPI_PUSH_EVENT_CALLBACK_NOT_FOUND ((int32_t)0x8055290C)
#define WEBAPI_HANDLE_NOT_FOUND ((int32_t)0x8055290D)
#define WEBAPI_SIGNED_IN_USER_NOT_FOUND ((int32_t)0x80552910)
#define WEBAPI_EXTD_PUSH_EVENT_FILTER_NOT_FOUND ((int32_t)0x8055291C)
#define WEBAPI_EXTD_PUSH_EVENT_CALLBACK_NOT_FOUND ((int32_t)0x8055291D)
#define WEBAPI2_INVALID_ARGUMENT ((int32_t)0x80553402)
#define WEBAPI2_USER_CONTEXT_NOT_FOUND ((int32_t)0x80553405)
#define WEBAPI2_REQUEST_NOT_FOUND ((int32_t)0x80553406)
#define WEBAPI2_NOT_SIGNED_IN ((int32_t)0x80553407)
#define WEBAPI2_PUSH_EVENT_FILTER_NOT_FOUND ((int32_t)0x8055340B)
#define WEBAPI2_PUSH_EVENT_CALLBACK_NOT_FOUND ((int32_t)0x8055340C)
#define WEBAPI2_HANDLE_NOT_FOUND ((int32_t)0x8055340D)
#define WEBAPI2_SIGNED_IN_USER_NOT_FOUND ((int32_t)0x8055340E)
#define WEBAPI2_PUSH_CONTEXT_NOT_FOUND ((int32_t)0x8055341C)
#define IN_GAME_MESSAGE_INVALID_ARGUMENT ((int32_t)0x80552B02)
#define IN_GAME_MESSAGE_LIB_CONTEXT_NOT_FOUND ((int32_t)0x80552B03)
#define IN_GAME_MESSAGE_NOT_SIGNED_IN ((int32_t)0x80552B04)
#define IN_GAME_MESSAGE_HANDLE_NOT_FOUND ((int32_t)0x80552B05)
#define GAME_INTENT_ALREADY_INITIALIZED ((int32_t)0x80553801)
#define GAME_INTENT_NOT_INITIALIZED ((int32_t)0x80553802)
#define GAME_INTENT_INVALID_ARGUMENT ((int32_t)0x80553804)
#define GAME_INTENT_INTENT_NOT_FOUND ((int32_t)0x80553806)
#define GAME_INTENT_VALUE_NOT_FOUND ((int32_t)0x80553807)
#define NET_EINVAL ((int32_t)0x80410116)
#define HTTP_INVALID_VALUE ((int32_t)0x80431077)
#define SSL_INVALID_CERT ((int32_t)0x8095F005)
#define SSL_INVALID_VALUE ((int32_t)0x8095F007)

/* ---- Object table ---- */
typedef struct NpObject {
    int64_t h_object;
    uint32_t kind;
} NpObject;
static pthread_rwlock_t s_objects_lock = PTHREAD_RWLOCK_INITIALIZER;
static NpObject *s_p_objects;
static size_t s_c_objects, s_c_object_capacity;
static int64_t s_next_object_id;

static int64_t np_next_id(void) { return __atomic_add_fetch(&s_next_object_id, 1, __ATOMIC_RELAXED); }

int64_t np_object_create(uint32_t kind) {
    const int64_t h_object = np_next_id();
    pthread_rwlock_wrlock(&s_objects_lock);
    size_t i_slot = s_c_objects;
    for (size_t i = 0; i < s_c_objects; ++i)
        if (s_p_objects[i].kind == NP_OBJECT_FREE) {
            i_slot = i;
            break;
        }
    if (i_slot == s_c_objects) {
        if (s_c_objects == s_c_object_capacity) {
            const size_t c_grown = s_c_object_capacity ? s_c_object_capacity * 2 : 64;
            NpObject *p_grown = realloc(s_p_objects, c_grown * sizeof(*s_p_objects));
            if (!p_grown) {
                pthread_rwlock_unlock(&s_objects_lock);
                return 0;
            }
            s_p_objects = p_grown;
            s_c_object_capacity = c_grown;
        }
        s_c_objects++;
    }
    s_p_objects[i_slot] = (NpObject){h_object, kind};
    pthread_rwlock_unlock(&s_objects_lock);
    return h_object;
}

bool np_object_exists(uint32_t kind, int64_t h_object) {
    bool is_found = false;
    pthread_rwlock_rdlock(&s_objects_lock);
    for (size_t i = 0; i < s_c_objects && !is_found; ++i) is_found = s_p_objects[i].kind == kind && s_p_objects[i].h_object == h_object;
    pthread_rwlock_unlock(&s_objects_lock);
    return is_found;
}

bool np_object_delete(uint32_t kind, int64_t h_object) {
    bool is_found = false;
    pthread_rwlock_wrlock(&s_objects_lock);
    for (size_t i = 0; i < s_c_objects && !is_found; ++i)
        if (s_p_objects[i].kind == kind && s_p_objects[i].h_object == h_object) {
            s_p_objects[i].kind = NP_OBJECT_FREE;
            is_found = true;
        }
    pthread_rwlock_unlock(&s_objects_lock);
    return is_found;
}

uint32_t np_object_kind(int64_t h_object) {
    uint32_t kind = NP_OBJECT_FREE;
    pthread_rwlock_rdlock(&s_objects_lock);
    for (size_t i = 0; i < s_c_objects && kind == NP_OBJECT_FREE; ++i)
        if (s_p_objects[i].kind != NP_OBJECT_FREE && s_p_objects[i].h_object == h_object) kind = s_p_objects[i].kind;
    pthread_rwlock_unlock(&s_objects_lock);
    return kind;
}

bool np_object_change(int64_t h_object, uint32_t kind) {
    bool is_found = false;
    pthread_rwlock_wrlock(&s_objects_lock);
    for (size_t i = 0; i < s_c_objects && !is_found; ++i)
        if (s_p_objects[i].kind != NP_OBJECT_FREE && s_p_objects[i].h_object == h_object) {
            s_p_objects[i].kind = kind;
            is_found = true;
        }
    pthread_rwlock_unlock(&s_objects_lock);
    return is_found;
}

static int32_t create_or(uint32_t kind, int32_t no_memory_error) {
    const int64_t h_object = np_object_create(kind);
    return h_object > 0 && h_object <= INT32_MAX ? (int32_t)h_object : no_memory_error;
}

/* ---- Memory pools: the size each library was initialized with; nothing is ever in use, since
 * no transfer starts. ---- */
enum { POOL_HTTP, POOL_WEBAPI, POOL_IN_GAME_MESSAGE };
typedef struct NpPool {
    uint32_t pool_kind;
    int32_t h_pool;
    uint64_t cb_pool;
} NpPool;
static NpPool s_pools[64];
static int s_c_pools;

static int32_t pool_create(uint32_t pool_kind, uint64_t cb_pool) {
    const int32_t h_pool = (int32_t)np_next_id();
    pthread_rwlock_wrlock(&s_objects_lock);
    if (s_c_pools < (int)(sizeof(s_pools) / sizeof(*s_pools))) s_pools[s_c_pools++] = (NpPool){pool_kind, h_pool, cb_pool};
    pthread_rwlock_unlock(&s_objects_lock);
    return h_pool;
}

/* Every library's statistics start with pool size, maximum and current use, as 64-bit values. */
static bool pool_stats(uint32_t pool_kind, int32_t h_pool, uint64_t *p_out_stats) {
    bool is_found = false;
    pthread_rwlock_rdlock(&s_objects_lock);
    for (int i = 0; i < s_c_pools && !is_found; ++i)
        if (s_pools[i].pool_kind == pool_kind && s_pools[i].h_pool == h_pool) {
            p_out_stats[0] = s_pools[i].cb_pool;
            is_found = true;
        }
    pthread_rwlock_unlock(&s_objects_lock);
    if (!is_found) p_out_stats[0] = 0;
    p_out_stats[1] = p_out_stats[2] = 0;
    return is_found;
}

/* ---- libSceNet and libSceSsl initialization, as their firmware does it (kb #735); the online
 * module (gpu/net) keeps the same ids and checks. Neither library's pool is mapped, since only
 * their own internal allocations use it and nothing here makes any. ---- */
#define NET_ERRNO_EBADF 9
#define NET_ERRNO_EINVAL 22
#define NET_ERRNO_ENFILE 23
#define NET_ERRNO_ENAMETOOLONG 63
#define NET_ERRNO_ENOTINIT 200
#define NET_ERROR_EBADF ((int32_t)0x80410109)
#define NET_ERROR_ENFILE ((int32_t)0x80410117)
#define NET_ERROR_ENAMETOOLONG ((int32_t)0x8041013F)
#define NET_ERROR_ENOTINIT ((int32_t)0x804101C8)
#define NET_POOL_ID_BASE 3000
#define CB_NET_POOL_MIN 0x1000
#define CB_NET_POOL_NAME 32
#define SSL_ERROR_INVALID_ID ((int32_t)0x8095F006)
#define SSL_ERROR_NO_FREE_CONTEXT ((int32_t)0x8095F008)

typedef struct LibraryPool {
    bool is_used;
    uint64_t cb_pool;
} LibraryPool;
static pthread_mutex_t s_library_pools_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_is_net_initialized;
static LibraryPool s_net_pools[32];
static LibraryPool s_ssl_contexts[128];
static size_t s_i_last_ssl_context = sizeof(s_ssl_contexts) / sizeof(*s_ssl_contexts) - 1;

enum { LOGGED_LIBRARY_FAILURES = 16 };
static size_t s_c_library_failures;
/* The call report counts every failure per call; the first ones also show here, in order. */
static int32_t log_library_failure(const char *sz_call, int32_t error) {
    const size_t c_failures = __atomic_add_fetch(&s_c_library_failures, 1, __ATOMIC_RELAXED);
    if (c_failures <= LOGGED_LIBRARY_FAILURES)
        printf("Runtime: %s failed with 0x%08X (libSceNet/libSceSsl failure %zu%s)\n", sz_call, (uint32_t)error,
               c_failures, c_failures == LOGGED_LIBRARY_FAILURES ? ", later ones only counted in the call report" : "");
    return error;
}
static int32_t net_fail(const char *sz_call, int32_t error, int32_t net_errno) {
    *runtime_net_errno() = net_errno;
    return log_library_failure(sz_call, error);
}
static bool find_pool(const LibraryPool *p_pools, size_t c_pools, int64_t i_pool, uint64_t *p_out_cb_pool) {
    pthread_mutex_lock(&s_library_pools_lock);
    const bool is_found = i_pool >= 0 && (uint64_t)i_pool < c_pools && p_pools[i_pool].is_used;
    if (is_found) *p_out_cb_pool = p_pools[i_pool].cb_pool;
    pthread_mutex_unlock(&s_library_pools_lock);
    return is_found;
}
static bool release_pool(LibraryPool *p_pools, size_t c_pools, int64_t i_pool) {
    pthread_mutex_lock(&s_library_pools_lock);
    const bool is_found = i_pool >= 0 && (uint64_t)i_pool < c_pools && p_pools[i_pool].is_used;
    if (is_found) p_pools[i_pool].is_used = false;
    pthread_mutex_unlock(&s_library_pools_lock);
    return is_found;
}
static void write_pool_stats(bool is_found, uint64_t cb_pool, uint64_t *p_out_stats) {
    p_out_stats[0] = is_found ? cb_pool : 0;
    p_out_stats[1] = p_out_stats[2] = 0;
}

/* A second call answers 0 as well. */
static ABI int32_t net_init(void) {
    if (!__atomic_exchange_n(&s_is_net_initialized, 1, __ATOMIC_ACQ_REL)) printf("Runtime: libSceNet initialized (offline)\n");
    return 0;
}
/* libSceNet's own sceNetTerm does nothing and answers 0, so the library stays initialized. */
static ABI int32_t net_term(void) { return 0; }
static ABI int32_t net_pool_create(const char *sz_name, int32_t cb_pool, int32_t flags) {
    if (!__atomic_load_n(&s_is_net_initialized, __ATOMIC_ACQUIRE))
        return net_fail("sceNetPoolCreate", NET_ERROR_ENOTINIT, NET_ERRNO_ENOTINIT);
    if (!sz_name || cb_pool < CB_NET_POOL_MIN || flags != 0) return net_fail("sceNetPoolCreate", NET_EINVAL, NET_ERRNO_EINVAL);
    if (strnlen(sz_name, CB_NET_POOL_NAME) == CB_NET_POOL_NAME)
        return net_fail("sceNetPoolCreate", NET_ERROR_ENAMETOOLONG, NET_ERRNO_ENAMETOOLONG);
    pthread_mutex_lock(&s_library_pools_lock);
    for (size_t i_pool = 0; i_pool < sizeof(s_net_pools) / sizeof(*s_net_pools); ++i_pool)
        if (!s_net_pools[i_pool].is_used) {
            s_net_pools[i_pool] = (LibraryPool){true, (uint64_t)cb_pool};
            pthread_mutex_unlock(&s_library_pools_lock);
            const int32_t h_pool = NET_POOL_ID_BASE + (int32_t)i_pool;
            printf("Runtime: libSceNet pool %d '%s', %d bytes\n", h_pool, sz_name, cb_pool);
            return h_pool;
        }
    pthread_mutex_unlock(&s_library_pools_lock);
    return net_fail("sceNetPoolCreate", NET_ERROR_ENFILE, NET_ERRNO_ENFILE);
}
static ABI int32_t net_pool_destroy(int32_t h_pool) {
    if (!__atomic_load_n(&s_is_net_initialized, __ATOMIC_ACQUIRE))
        return net_fail("sceNetPoolDestroy", NET_ERROR_ENOTINIT, NET_ERRNO_ENOTINIT);
    if (!release_pool(s_net_pools, sizeof(s_net_pools) / sizeof(*s_net_pools), (int64_t)h_pool - NET_POOL_ID_BASE))
        return net_fail("sceNetPoolDestroy", NET_ERROR_EBADF, NET_ERRNO_EBADF);
    printf("Runtime: libSceNet pool %d destroyed\n", h_pool);
    return 0;
}
static ABI int32_t net_pool_stats(int32_t h_pool, uint64_t *p_out_stats) {
    if (!p_out_stats) return log_library_failure("sceNetGetMemoryPoolStats", NET_EINVAL);
    uint64_t cb_pool = 0;
    const bool is_found = find_pool(s_net_pools, sizeof(s_net_pools) / sizeof(*s_net_pools), (int64_t)h_pool - NET_POOL_ID_BASE, &cb_pool);
    write_pool_stats(is_found, cb_pool, p_out_stats);
    return 0;
}
/* Fixed ids 1..128, handed out from the slot after the last one given and wrapping; there is no
 * already-initialized error, each call takes a new context. */
static ABI int32_t ssl_init(uint64_t cb_pool) {
    const size_t c_contexts = sizeof(s_ssl_contexts) / sizeof(*s_ssl_contexts);
    pthread_mutex_lock(&s_library_pools_lock);
    for (size_t c_tried = 1; c_tried <= c_contexts; ++c_tried) {
        const size_t i_context = (s_i_last_ssl_context + c_tried) % c_contexts;
        if (!s_ssl_contexts[i_context].is_used) {
            s_ssl_contexts[i_context] = (LibraryPool){true, cb_pool};
            s_i_last_ssl_context = i_context;
            pthread_mutex_unlock(&s_library_pools_lock);
            const int32_t h_context = (int32_t)i_context + 1;
            printf("Runtime: libSceSsl context %d, pool %llu bytes\n", h_context, (unsigned long long)cb_pool);
            return h_context;
        }
    }
    pthread_mutex_unlock(&s_library_pools_lock);
    return log_library_failure("sceSslInit", SSL_ERROR_NO_FREE_CONTEXT);
}
/* The firmware waits here for the context's connections to close; none is ever opened offline. */
static ABI int32_t ssl_term(int32_t h_context) {
    if (!release_pool(s_ssl_contexts, sizeof(s_ssl_contexts) / sizeof(*s_ssl_contexts), (int64_t)h_context - 1))
        return log_library_failure("sceSslTerm", SSL_ERROR_INVALID_ID);
    printf("Runtime: libSceSsl context %d terminated\n", h_context);
    return 0;
}
static ABI int32_t ssl_pool_stats(int32_t h_ssl, uint64_t *p_out_stats) {
    if (!p_out_stats) return log_library_failure("sceSslGetMemoryPoolStats", SSL_INVALID_VALUE);
    uint64_t cb_pool = 0;
    const bool is_found = find_pool(s_ssl_contexts, sizeof(s_ssl_contexts) / sizeof(*s_ssl_contexts), (int64_t)h_ssl - 1, &cb_pool);
    write_pool_stats(is_found, cb_pool, p_out_stats);
    return 0;
}
/* ---- libSceSsl certificates: offline no TLS connection is ever made, so the CA store handed to
 * the game's own TLS code is empty and no peer certificate (SceSslCert) or name taken from one
 * ever exists to ask about. With PS_ONLINE=1 the online module answers these instead. ---- */
typedef struct SslCaCerts {
    void *p_cert_data;
    uint64_t c_cert_data;
    void *p_pool;
} SslCaCerts;
static ABI int32_t ssl_get_ca_certs(int32_t h_ssl, SslCaCerts *p_out_certs) {
    (void)h_ssl;
    if (!p_out_certs) return SSL_INVALID_VALUE;
    static int s_is_logged;
    if (!__atomic_exchange_n(&s_is_logged, 1, __ATOMIC_RELAXED)) printf("Runtime: libSceSsl: no CA certificates offline\n");
    *p_out_certs = (SslCaCerts){0};
    return 0;
}
static ABI int32_t ssl_free_ca_certs(int32_t h_ssl, SslCaCerts *p_certs) {
    (void)h_ssl;
    if (!p_certs) return SSL_INVALID_VALUE;
    *p_certs = (SslCaCerts){0};
    return 0;
}
/* A NULL result is the failure, which the call report cannot see in a pointer: logged here. */
static ABI void *ssl_get_cert_name(int32_t h_ssl, void *p_cert) {
    (void)h_ssl;
    static int s_is_logged;
    if (!__atomic_exchange_n(&s_is_logged, 1, __ATOMIC_RELAXED))
        printf("Runtime: libSceSsl: certificate name asked of %p, no certificate exists offline: NULL\n", p_cert);
    return NULL;
}
static ABI int32_t ssl_no_cert_name(int32_t h_ssl, const void *p_name) {
    (void)h_ssl; (void)p_name;
    return SSL_INVALID_VALUE;
}
static ABI int32_t ssl_get_name_entry_info(int32_t h_ssl, const void *p_name, int32_t i_entry, char *sz_out_oid,
                                           uint64_t cb_oid, uint8_t *p_out_value, uint64_t cb_value,
                                           uint64_t *p_cb_out_value) {
    (void)h_ssl; (void)p_name; (void)i_entry; (void)sz_out_oid; (void)cb_oid; (void)p_out_value; (void)cb_value;
    if (p_cb_out_value) *p_cb_out_value = 0;
    return SSL_INVALID_VALUE;
}
static ABI int32_t ssl_get_serial_number(int32_t h_ssl, const void *p_cert, const uint8_t **pp_out_data,
                                         uint64_t *p_cb_out_data) {
    (void)h_ssl; (void)p_cert;
    if (pp_out_data) *pp_out_data = NULL;
    if (p_cb_out_data) *p_cb_out_data = 0;
    return SSL_INVALID_CERT;
}

static ABI int32_t http_init(int32_t h_net_pool, int32_t h_ssl, uint64_t cb_pool) {
    (void)h_net_pool; (void)h_ssl;
    return pool_create(POOL_HTTP, cb_pool);
}
/* libSceNpManager's module_start (sceNpManagerPrxStart's network init, kb #869), which on the
 * console runs before any game code in every process that links it: NpToolkit2's Core::init then
 * finds libSceNet initialized, and its own pool and SSL context take ids 3001 and 2. */
static void start_np_manager(void) {
    net_init();
    const int32_t h_pool = net_pool_create("SceNpSdkClient", 0x4000, 0);
    const int32_t h_ssl = ssl_init(0x40000);
    const int32_t h_http = http_init(h_pool, h_ssl, 0xC000);
    printf("Runtime: libSceNpManager started: net pool %d, SSL context %d, HTTP context %d\n", h_pool, h_ssl, h_http);
}
void runtime_np_manager_start(void) {
    static pthread_once_t s_once = PTHREAD_ONCE_INIT;
    pthread_once(&s_once, start_np_manager);
}
static ABI int32_t http_pool_stats(int32_t h_http, uint64_t *p_out_stats) {
    if (!p_out_stats) return HTTP_INVALID_VALUE;
    pool_stats(POOL_HTTP, h_http, p_out_stats);
    return 0;
}

/* ---- NpWebApi ---- */
static ABI int32_t webapi_initialize(int32_t h_http, uint64_t cb_pool) {
    (void)h_http;
    return pool_create(POOL_WEBAPI, cb_pool);
}
static ABI int32_t webapi_terminate(int32_t h_library) { (void)h_library; return 0; }
static ABI int32_t webapi_pool_stats(int32_t h_library, uint64_t *p_out_stats) {
    if (!p_out_stats) return WEBAPI_INVALID_ARGUMENT;
    pool_stats(POOL_WEBAPI, h_library, p_out_stats);
    return 0;
}
/* The user context of a signed-out user exists; its requests fail when sent. */
static ABI int32_t webapi_create_context_user(int32_t h_library, int32_t user_id) {
    (void)h_library;
    if (user_id != USER_ID) return user_id < 0 ? WEBAPI_INVALID_ARGUMENT : WEBAPI_SIGNED_IN_USER_NOT_FOUND;
    return create_or(NP_WEBAPI_USER_CONTEXT, (int32_t)0x80552901);
}
static ABI int32_t webapi_create_context_online_id(int32_t h_library, const void *p_online_id) {
    if (!p_online_id) return WEBAPI_INVALID_ARGUMENT;
    return webapi_create_context_user(h_library, USER_ID);
}
static ABI int32_t webapi_delete_context(int32_t h_context) {
    return np_object_delete(NP_WEBAPI_USER_CONTEXT, h_context) ? 0 : WEBAPI_USER_CONTEXT_NOT_FOUND;
}
static ABI int32_t webapi_create_request(int32_t h_context, const char *sz_api_group, const char *sz_path, const char *sz_method,
                                         const void *p_content_parameter, int64_t *p_out_request_id) {
    (void)p_content_parameter;
    if (!sz_api_group || !sz_path || !sz_method || !p_out_request_id) return WEBAPI_INVALID_ARGUMENT;
    if (!np_object_exists(NP_WEBAPI_USER_CONTEXT, h_context)) return WEBAPI_USER_CONTEXT_NOT_FOUND;
    const int64_t h_request = np_object_create(NP_WEBAPI_REQUEST);
    if (h_request <= 0) return (int32_t)0x80552901;
    *p_out_request_id = h_request;
    return 0;
}
static ABI int32_t webapi_request_local(int64_t h_request) {
    return np_object_exists(NP_WEBAPI_REQUEST, h_request) ? 0 : WEBAPI_REQUEST_NOT_FOUND;
}
static ABI int32_t webapi_add_header(int64_t h_request, const char *sz_field_name, const char *sz_field_value) {
    if (!sz_field_name || !sz_field_value) return WEBAPI_INVALID_ARGUMENT;
    return webapi_request_local(h_request);
}
static ABI int32_t webapi_request_remote(int64_t h_request) {
    return np_object_exists(NP_WEBAPI_REQUEST, h_request) ? WEBAPI_NOT_SIGNED_IN : WEBAPI_REQUEST_NOT_FOUND;
}
static ABI int32_t webapi_delete_request(int64_t h_request) {
    return np_object_delete(NP_WEBAPI_REQUEST, h_request) ? 0 : WEBAPI_REQUEST_NOT_FOUND;
}
static ABI int32_t webapi_create_handle(int32_t h_library) {
    (void)h_library;
    return create_or(NP_WEBAPI_HANDLE, (int32_t)0x80552901);
}
static ABI int32_t webapi_delete_handle(int32_t h_library, int32_t h_handle) {
    (void)h_library;
    return np_object_delete(NP_WEBAPI_HANDLE, h_handle) ? 0 : WEBAPI_HANDLE_NOT_FOUND;
}
/* A filter on another service's events needs the signed-in user's PSN identity. */
static int32_t webapi_create_filter_for(int32_t h_handle, const char *sz_service_name, const void *p_filter_parameter,
                                        uint64_t c_filter_parameters) {
    if (!p_filter_parameter || !c_filter_parameters) return WEBAPI_INVALID_ARGUMENT;
    if (!np_object_exists(NP_WEBAPI_HANDLE, h_handle)) return WEBAPI_HANDLE_NOT_FOUND;
    if (sz_service_name) return WEBAPI_SIGNED_IN_USER_NOT_FOUND;
    return create_or(NP_WEBAPI_FILTER, (int32_t)0x80552901);
}
static ABI int32_t webapi_create_filter(int32_t h_library, int32_t h_handle, const char *sz_service_name,
                                        uint32_t service_label, const void *p_filter_parameter, uint64_t c_filter_parameters) {
    (void)h_library; (void)service_label;
    return webapi_create_filter_for(h_handle, sz_service_name, p_filter_parameter, c_filter_parameters);
}
static ABI int32_t webapi_delete_filter(int32_t h_library, int32_t h_filter) {
    (void)h_library;
    return np_object_delete(NP_WEBAPI_FILTER, h_filter) ? 0 : WEBAPI_PUSH_EVENT_FILTER_NOT_FOUND;
}
static ABI int32_t webapi_delete_extd_filter(int32_t h_library, int32_t h_filter) {
    (void)h_library;
    return np_object_delete(NP_WEBAPI_FILTER, h_filter) ? 0 : WEBAPI_EXTD_PUSH_EVENT_FILTER_NOT_FOUND;
}
/* Registered callbacks never run: no push event reaches an unplugged console. */
static int32_t webapi_register_for(int32_t h_context, int32_t h_filter, const void *pfn_callback, int32_t filter_missing_error) {
    if (!pfn_callback) return WEBAPI_INVALID_ARGUMENT;
    if (!np_object_exists(NP_WEBAPI_USER_CONTEXT, h_context)) return WEBAPI_USER_CONTEXT_NOT_FOUND;
    if (!np_object_exists(NP_WEBAPI_FILTER, h_filter)) return filter_missing_error;
    return create_or(NP_WEBAPI_CALLBACK, (int32_t)0x80552901);
}
static ABI int32_t webapi_register_callback(int32_t h_context, int32_t h_filter, const void *pfn_callback, void *p_user_argument) {
    (void)p_user_argument;
    return webapi_register_for(h_context, h_filter, pfn_callback, WEBAPI_PUSH_EVENT_FILTER_NOT_FOUND);
}
static ABI int32_t webapi_register_extd_callback(int32_t h_context, int32_t h_filter, const void *pfn_callback, void *p_user_argument) {
    (void)p_user_argument;
    return webapi_register_for(h_context, h_filter, pfn_callback, WEBAPI_EXTD_PUSH_EVENT_FILTER_NOT_FOUND);
}
static ABI int32_t webapi_unregister_callback(int32_t h_context, int32_t h_callback) {
    (void)h_context;
    return np_object_delete(NP_WEBAPI_CALLBACK, h_callback) ? 0 : WEBAPI_PUSH_EVENT_CALLBACK_NOT_FOUND;
}
static ABI int32_t webapi_unregister_extd_callback(int32_t h_context, int32_t h_callback) {
    (void)h_context;
    return np_object_delete(NP_WEBAPI_CALLBACK, h_callback) ? 0 : WEBAPI_EXTD_PUSH_EVENT_CALLBACK_NOT_FOUND;
}

/* ---- NpWebApi2 ---- */
static ABI int32_t webapi2_terminate(int32_t h_library) { (void)h_library; return 0; }
static ABI int32_t webapi2_create_user_context(int32_t h_library, int32_t user_id) {
    (void)h_library;
    if (user_id != USER_ID) return user_id < 0 ? WEBAPI2_INVALID_ARGUMENT : WEBAPI2_SIGNED_IN_USER_NOT_FOUND;
    return create_or(NP_WEBAPI2_USER_CONTEXT, (int32_t)0x80553401);
}
static ABI int32_t webapi2_delete_user_context(int32_t h_context) {
    return np_object_delete(NP_WEBAPI2_USER_CONTEXT, h_context) ? 0 : WEBAPI2_USER_CONTEXT_NOT_FOUND;
}
static ABI int32_t webapi2_create_request(int32_t h_context, const char *sz_api_group, const char *sz_path, const char *sz_method,
                                          const void *p_content_parameter, int64_t *p_out_request_id) {
    (void)p_content_parameter;
    if (!sz_api_group || !sz_path || !sz_method || !p_out_request_id) return WEBAPI2_INVALID_ARGUMENT;
    if (!np_object_exists(NP_WEBAPI2_USER_CONTEXT, h_context)) return WEBAPI2_USER_CONTEXT_NOT_FOUND;
    const int64_t h_request = np_object_create(NP_WEBAPI2_REQUEST);
    if (h_request <= 0) return (int32_t)0x80553401;
    *p_out_request_id = h_request;
    return 0;
}
static ABI int32_t webapi2_request_local(int64_t h_request) {
    return np_object_exists(NP_WEBAPI2_REQUEST, h_request) ? 0 : WEBAPI2_REQUEST_NOT_FOUND;
}
static ABI int32_t webapi2_add_header(int64_t h_request, const char *sz_field_name, const char *sz_field_value) {
    if (!sz_field_name || !sz_field_value) return WEBAPI2_INVALID_ARGUMENT;
    return webapi2_request_local(h_request);
}
static ABI int32_t webapi2_request_remote(int64_t h_request) {
    return np_object_exists(NP_WEBAPI2_REQUEST, h_request) ? WEBAPI2_NOT_SIGNED_IN : WEBAPI2_REQUEST_NOT_FOUND;
}
static ABI int32_t webapi2_delete_request(int64_t h_request) {
    return np_object_delete(NP_WEBAPI2_REQUEST, h_request) ? 0 : WEBAPI2_REQUEST_NOT_FOUND;
}
static ABI int32_t webapi2_create_handle(int32_t h_library) {
    (void)h_library;
    return create_or(NP_WEBAPI2_HANDLE, (int32_t)0x80553401);
}
static ABI int32_t webapi2_delete_handle(int32_t h_library, int32_t h_handle) {
    (void)h_library;
    return np_object_delete(NP_WEBAPI2_HANDLE, h_handle) ? 0 : WEBAPI2_HANDLE_NOT_FOUND;
}
static ABI int32_t webapi2_create_filter(int32_t h_library, int32_t h_handle, const char *sz_service_name,
                                         uint32_t service_label, const void *p_filter_parameter, uint64_t c_filter_parameters) {
    (void)h_library; (void)service_label;
    if (!p_filter_parameter || !c_filter_parameters) return WEBAPI2_INVALID_ARGUMENT;
    if (!np_object_exists(NP_WEBAPI2_HANDLE, h_handle)) return WEBAPI2_HANDLE_NOT_FOUND;
    if (sz_service_name) return WEBAPI2_SIGNED_IN_USER_NOT_FOUND;
    return create_or(NP_WEBAPI2_FILTER, (int32_t)0x80553401);
}
static ABI int32_t webapi2_register_callback(int32_t h_context, int32_t h_filter, const void *pfn_callback, void *p_user_argument) {
    (void)p_user_argument;
    if (!pfn_callback) return WEBAPI2_INVALID_ARGUMENT;
    if (!np_object_exists(NP_WEBAPI2_USER_CONTEXT, h_context)) return WEBAPI2_USER_CONTEXT_NOT_FOUND;
    if (!np_object_exists(NP_WEBAPI2_FILTER, h_filter)) return WEBAPI2_PUSH_EVENT_FILTER_NOT_FOUND;
    return create_or(NP_WEBAPI2_CALLBACK, (int32_t)0x80553401);
}
static ABI int32_t webapi2_unregister_callback(int32_t h_context, int32_t h_callback) {
    (void)h_context;
    return np_object_delete(NP_WEBAPI2_CALLBACK, h_callback) ? 0 : WEBAPI2_PUSH_EVENT_CALLBACK_NOT_FOUND;
}
/* A push context is named by a version 4 UUID string (37 bytes with its terminator); this port
 * writes the context's id into the node field so the name leads back to it. */
static void push_context_name(int64_t h_push_context, char *p_out_uuid) {
    snprintf(p_out_uuid, 37, "00000000-0000-4000-8000-%012llx", (unsigned long long)h_push_context & 0xFFFFFFFFFFFFull);
}
static int64_t find_push_context(const char *sz_uuid) {
    char sz_expected[37];
    if (!sz_uuid || strnlen(sz_uuid, 37) != 36) return 0;
    const int64_t h_push_context = (int64_t)strtoull(sz_uuid + 24, NULL, 16);
    push_context_name(h_push_context, sz_expected);
    return memcmp(sz_expected, sz_uuid, 36) ? 0 : h_push_context;
}
static ABI int32_t webapi2_create_push_context(int32_t h_context, char *p_out_uuid) {
    if (!p_out_uuid) return WEBAPI2_INVALID_ARGUMENT;
    if (!np_object_exists(NP_WEBAPI2_USER_CONTEXT, h_context)) return WEBAPI2_USER_CONTEXT_NOT_FOUND;
    const int64_t h_push_context = np_object_create(NP_WEBAPI2_PUSH_CONTEXT);
    if (h_push_context <= 0) return (int32_t)0x80553401;
    push_context_name(h_push_context, p_out_uuid);
    return 0;
}
static ABI int32_t webapi2_delete_push_context(int32_t h_context, const char *sz_uuid) {
    (void)h_context;
    if (!sz_uuid) return WEBAPI2_INVALID_ARGUMENT;
    return np_object_delete(NP_WEBAPI2_PUSH_CONTEXT, find_push_context(sz_uuid)) ? 0 : WEBAPI2_PUSH_CONTEXT_NOT_FOUND;
}
/* Starting delivery registers the context with the push server. */
static ABI int32_t webapi2_start_push_context(int32_t h_context, const char *sz_uuid) {
    if (!sz_uuid) return WEBAPI2_INVALID_ARGUMENT;
    if (!np_object_exists(NP_WEBAPI2_USER_CONTEXT, h_context)) return WEBAPI2_USER_CONTEXT_NOT_FOUND;
    if (!np_object_exists(NP_WEBAPI2_PUSH_CONTEXT, find_push_context(sz_uuid))) return WEBAPI2_PUSH_CONTEXT_NOT_FOUND;
    return WEBAPI2_NOT_SIGNED_IN;
}

/* ---- NpInGameMessage ---- */
static ABI int32_t in_game_message_initialize(uint64_t cb_pool, const void *p_option) {
    (void)p_option;
    return pool_create(POOL_IN_GAME_MESSAGE, cb_pool);
}
static ABI int32_t in_game_message_terminate(int32_t h_library) {
    uint64_t stats[3];
    return pool_stats(POOL_IN_GAME_MESSAGE, h_library, stats) ? 0 : IN_GAME_MESSAGE_LIB_CONTEXT_NOT_FOUND;
}
static ABI int32_t in_game_message_create_handle(int32_t h_library) {
    uint64_t stats[3];
    if (!pool_stats(POOL_IN_GAME_MESSAGE, h_library, stats)) return IN_GAME_MESSAGE_LIB_CONTEXT_NOT_FOUND;
    return create_or(NP_IN_GAME_MESSAGE_HANDLE, (int32_t)0x80552B01);
}
static ABI int32_t in_game_message_delete_handle(int32_t h_library, int32_t h_handle) {
    (void)h_library;
    return np_object_delete(NP_IN_GAME_MESSAGE_HANDLE, h_handle) ? 0 : IN_GAME_MESSAGE_HANDLE_NOT_FOUND;
}
/* Preparing and sending both go through the PSN message server. */
static ABI int32_t in_game_message_remote(int32_t h_library, int32_t h_handle) {
    (void)h_library;
    if (!np_object_exists(NP_IN_GAME_MESSAGE_HANDLE, h_handle)) return IN_GAME_MESSAGE_HANDLE_NOT_FOUND;
    static int s_c_reported;
    if (__atomic_fetch_add(&s_c_reported, 1, __ATOMIC_RELAXED) < 4)
        printf("Runtime: NpInGameMessage request refused, signed out -> 0x%08X\n", (uint32_t)IN_GAME_MESSAGE_NOT_SIGNED_IN);
    return IN_GAME_MESSAGE_NOT_SIGNED_IN;
}
static ABI int32_t in_game_message_pool_stats(int32_t h_library, uint64_t *p_out_stats) {
    if (!p_out_stats) return IN_GAME_MESSAGE_INVALID_ARGUMENT;
    return pool_stats(POOL_IN_GAME_MESSAGE, h_library, p_out_stats) ? 0 : IN_GAME_MESSAGE_LIB_CONTEXT_NOT_FOUND;
}

/* ---- NpGameIntent: the game was started from its tile, so no intent is ever waiting. ---- */
static bool s_is_game_intent_initialized;
static ABI int32_t game_intent_initialize(const void *p_parameter) {
    (void)p_parameter;
    if (__atomic_exchange_n(&s_is_game_intent_initialized, true, __ATOMIC_RELAXED)) return GAME_INTENT_ALREADY_INITIALIZED;
    return 0;
}
static ABI int32_t game_intent_terminate(void) {
    return __atomic_exchange_n(&s_is_game_intent_initialized, false, __ATOMIC_RELAXED) ? 0 : GAME_INTENT_NOT_INITIALIZED;
}
static ABI int32_t game_intent_receive(void *p_out_intent) {
    if (!__atomic_load_n(&s_is_game_intent_initialized, __ATOMIC_RELAXED)) return GAME_INTENT_NOT_INITIALIZED;
    if (!p_out_intent) return GAME_INTENT_INVALID_ARGUMENT;
    return GAME_INTENT_INTENT_NOT_FOUND;
}
static ABI int32_t game_intent_property(const void *p_intent_data, const char *sz_key, char *p_out_value, uint64_t cb_value) {
    if (!__atomic_load_n(&s_is_game_intent_initialized, __ATOMIC_RELAXED)) return GAME_INTENT_NOT_INITIALIZED;
    if (!p_intent_data || !sz_key || !p_out_value || !cb_value) return GAME_INTENT_INVALID_ARGUMENT;
    return GAME_INTENT_VALUE_NOT_FOUND;
}

/* ---- PS Store icon: drawn by the system UI, which this port does not have. ---- */
static ABI int32_t store_icon_show(int32_t position) {
    static int s_is_reported;
    if (!__atomic_exchange_n(&s_is_reported, 1, __ATOMIC_RELAXED))
        printf("Runtime: PS Store icon shown at position %d (no system UI draws it)\n", position);
    return 0;
}
static ABI int32_t store_icon_hide(void) { return 0; }

/* ---- NpManager, NpAuth, NetCtl and UserService calls of the toolkits ---- */
static ABI int32_t np_user_by_account(uint64_t account_id, int32_t *p_out_user_id) {
    (void)account_id;
    if (!p_out_user_id) return NP_INVALID_ARGUMENT;
    return NP_USER_NOT_FOUND;
}
static ABI int32_t np_request_signed_out(int32_t h_request, int32_t user_id) {
    (void)h_request; (void)user_id;
    return NP_SIGNED_OUT;
}
static ABI int32_t np_account_country(int32_t user_id, void *p_out_country) {
    (void)user_id;
    return p_out_country ? NP_SIGNED_OUT : NP_INVALID_ARGUMENT;
}
static ABI int32_t np_parental_control(int32_t h_request, int32_t user_id, int8_t *p_out_age, void *p_out_info) {
    (void)h_request; (void)user_id;
    return p_out_age && p_out_info ? NP_SIGNED_OUT : NP_INVALID_ARGUMENT;
}
static ABI int32_t auth_code(int32_t h_request, const void *p_parameter, void *p_out_code, int32_t *p_out_issuer_id) {
    (void)h_request; (void)p_out_issuer_id;
    return p_parameter && p_out_code ? NP_SIGNED_OUT : NP_AUTH_INVALID_ARGUMENT;
}
/* Unregistering a pfnCallback that never fires (nothing is due offline) has nothing to undo. */
static ABI int32_t callback_unregistered(void) { return 0; }

static const RuntimeExport s_exports[] = {
    {"sceNetInit", net_init}, {"sceNetTerm", net_term},
    {"sceNetPoolCreate", net_pool_create}, {"sceNetPoolDestroy", net_pool_destroy}, {"sceNetGetMemoryPoolStats", net_pool_stats},
    {"sceSslInit", ssl_init}, {"sceSslTerm", ssl_term}, {"sceSslGetMemoryPoolStats", ssl_pool_stats},
    {"sceSslGetCaCerts", ssl_get_ca_certs}, {"sceSslFreeCaCerts", ssl_free_ca_certs},
    {"sceSslGetSubjectName", ssl_get_cert_name}, {"sceSslGetIssuerName", ssl_get_cert_name},
    {"sceSslGetNameEntryCount", ssl_no_cert_name}, {"sceSslFreeSslCertName", ssl_no_cert_name},
    {"sceSslGetNameEntryInfo", ssl_get_name_entry_info}, {"sceSslGetSerialNumber", ssl_get_serial_number},
    {"sceHttpInit", http_init}, {"sceHttpGetMemoryPoolStats", http_pool_stats},
    {"sceNpWebApiInitialize", webapi_initialize}, {"sceNpWebApiTerminate", webapi_terminate},
    {"sceNpWebApiGetMemoryPoolStats", webapi_pool_stats},
    {"sceNpWebApiCreateContext", webapi_create_context_online_id}, {"sceNpWebApiCreateContextA", webapi_create_context_user},
    {"sceNpWebApiDeleteContext", webapi_delete_context},
    {"sceNpWebApiCreateRequest", webapi_create_request}, {"sceNpWebApiDeleteRequest", webapi_delete_request},
    {"sceNpWebApiAddHttpRequestHeader", webapi_add_header}, {"sceNpWebApiAbortRequest", webapi_request_local},
    {"sceNpWebApiSetRequestTimeout", webapi_request_local},
    {"sceNpWebApiSendRequest", webapi_request_remote}, {"sceNpWebApiSendRequest2", webapi_request_remote},
    {"sceNpWebApiReadData", webapi_request_remote}, {"sceNpWebApiGetHttpStatusCode", webapi_request_remote},
    {"sceNpWebApiGetHttpResponseHeaderValue", webapi_request_remote},
    {"sceNpWebApiGetHttpResponseHeaderValueLength", webapi_request_remote},
    {"sceNpWebApiCreateHandle", webapi_create_handle}, {"sceNpWebApiDeleteHandle", webapi_delete_handle},
    {"sceNpWebApiCreatePushEventFilter", webapi_create_filter}, {"sceNpWebApiDeletePushEventFilter", webapi_delete_filter},
    {"sceNpWebApiCreateExtdPushEventFilter", webapi_create_filter},
    {"sceNpWebApiDeleteExtdPushEventFilter", webapi_delete_extd_filter},
    {"sceNpWebApiRegisterPushEventCallback", webapi_register_callback},
    {"sceNpWebApiUnregisterPushEventCallback", webapi_unregister_callback},
    {"sceNpWebApiRegisterExtdPushEventCallback", webapi_register_extd_callback},
    {"sceNpWebApiRegisterExtdPushEventCallbackA", webapi_register_extd_callback},
    {"sceNpWebApiUnregisterExtdPushEventCallback", webapi_unregister_extd_callback},
    {"sceNpWebApi2Terminate", webapi2_terminate},
    {"sceNpWebApi2CreateUserContext", webapi2_create_user_context}, {"sceNpWebApi2DeleteUserContext", webapi2_delete_user_context},
    {"sceNpWebApi2CreateRequest", webapi2_create_request}, {"sceNpWebApi2DeleteRequest", webapi2_delete_request},
    {"sceNpWebApi2AddHttpRequestHeader", webapi2_add_header}, {"sceNpWebApi2AbortRequest", webapi2_request_local},
    {"sceNpWebApi2SetRequestTimeout", webapi2_request_local},
    {"sceNpWebApi2SendRequest", webapi2_request_remote}, {"sceNpWebApi2ReadData", webapi2_request_remote},
    {"sceNpWebApi2GetHttpResponseHeaderValue", webapi2_request_remote},
    {"sceNpWebApi2GetHttpResponseHeaderValueLength", webapi2_request_remote},
    {"sceNpWebApi2PushEventCreateHandle", webapi2_create_handle}, {"sceNpWebApi2PushEventDeleteHandle", webapi2_delete_handle},
    {"sceNpWebApi2PushEventCreateFilter", webapi2_create_filter},
    {"sceNpWebApi2PushEventRegisterCallback", webapi2_register_callback},
    {"sceNpWebApi2PushEventUnregisterCallback", webapi2_unregister_callback},
    {"sceNpWebApi2PushEventRegisterPushContextCallback", webapi2_register_callback},
    {"sceNpWebApi2PushEventUnregisterPushContextCallback", webapi2_unregister_callback},
    {"sceNpWebApi2PushEventCreatePushContext", webapi2_create_push_context},
    {"sceNpWebApi2PushEventDeletePushContext", webapi2_delete_push_context},
    {"sceNpWebApi2PushEventStartPushContextCallback", webapi2_start_push_context},
    {"sceNpInGameMessageInitialize", in_game_message_initialize}, {"sceNpInGameMessageTerminate", in_game_message_terminate},
    {"sceNpInGameMessageCreateHandle", in_game_message_create_handle},
    {"sceNpInGameMessageDeleteHandle", in_game_message_delete_handle},
    {"sceNpInGameMessagePrepareA", in_game_message_remote}, {"sceNpInGameMessageSendDataA", in_game_message_remote},
    {"sceNpInGameMessageGetMemoryPoolStatistics", in_game_message_pool_stats},
    {"sceNpGameIntentInitialize", game_intent_initialize}, {"sceNpGameIntentTerminate", game_intent_terminate},
    {"sceNpGameIntentReceiveIntent", game_intent_receive},
    {"sceNpGameIntentGetPropertyValueString", game_intent_property},
    {"sceNpCommerceShowPsStoreIcon", store_icon_show}, {"sceNpCommerceHidePsStoreIcon", store_icon_hide},
    {"sceNpGetUserIdByAccountId", np_user_by_account}, {"sceNpCheckNpAvailabilityA", np_request_signed_out},
    {"sceNpGetAccountCountryA", np_account_country}, {"sceNpGetParentalControlInfoA", np_parental_control},
    {"sceNpAuthGetAuthorizationCodeA", auth_code}, {"sceNpAuthAbortRequest", callback_unregistered},
    {"sceNpUnregisterStateCallbackA", callback_unregistered},
    {"sceNpUnregisterStateCallbackForToolkit", callback_unregistered},
    {"sceNetCtlUnregisterCallbackForNpToolkit", callback_unregistered},
    {"sceUserServiceUnregisterEventCallback", callback_unregistered},
};
uintptr_t runtime_np_resolve(const char *sz_name) {
    const uintptr_t va_found = RUNTIME_LOOKUP(s_exports, sz_name);
    return va_found ? va_found : runtime_np_community_resolve(sz_name);
}
