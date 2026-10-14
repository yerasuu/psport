/* libSceHttp and libSceHttp2 on an unplugged console (decision #207): templates, connections and
 * requests are created so setup code proceeds; any transfer fails as unplugged. */
#define _GNU_SOURCE
#include "runtime.h"
#ifndef _WIN32
#include <time.h>

#define HTTP_NETWORK ((int32_t)0x80431063)

static int next_id=1;
static int new_id(void) { return __atomic_fetch_add(&next_id,1,__ATOMIC_RELAXED); }

static ABI int32_t lib_init_id(void) { return new_id(); }
static ABI int32_t http_fail(void) { return HTTP_NETWORK; }
/* Objects are created so setup code proceeds; any transfer fails as unplugged. */
static ABI int32_t http_object(void) { return new_id(); }
static ABI int32_t http_epoll(int32_t ctx,void **handle) {
    (void)ctx;
    if (!handle) return (int32_t)0x80431077; /* HTTP INVALID_VALUE */
    *handle=(void *)(uintptr_t)(0x100+new_id()); return 0;
}
static ABI int32_t http_wait(void *handle,void *events,int32_t max,int64_t timeout) {
    (void)handle; (void)events; (void)max;
    if (timeout>0) { struct timespec t={timeout/1000000,(timeout%1000000)*1000}; nanosleep(&t,NULL); }
    return 0; /* no events: nothing is in flight */
}

static const RuntimeExport exports[]={
    {"sceHttpTerm",runtime_stub},
    {"sceHttpCreateTemplate",http_object}, {"sceHttpDeleteTemplate",runtime_stub},
    {"sceHttpCreateConnectionWithURL",http_object}, {"sceHttpCreateRequestWithURL",http_object},
    {"sceHttpSendRequest",http_fail}, {"sceHttpCreateEpoll",http_epoll},
    {"sceHttpSetNonblock",runtime_stub}, {"sceHttpSetConnectTimeOut",runtime_stub},
    {"sceHttpsEnableOption",runtime_stub}, {"sceHttpsDisableOption",runtime_stub},
    {"sceHttpAddRequestHeader",runtime_stub}, {"sceHttpSetRequestContentLength",runtime_stub},
    {"sceHttpDeleteConnection",runtime_stub}, {"sceHttpDeleteRequest",runtime_stub},
    {"sceHttpAbortWaitRequest",runtime_stub}, {"sceHttpDestroyEpoll",runtime_stub},
    {"sceHttpSetEpoll",runtime_stub}, {"sceHttpUnsetEpoll",runtime_stub}, {"sceHttpWaitRequest",http_wait},
    {"sceHttpGetStatusCode",http_fail}, {"sceHttpGetResponseContentLength",http_fail},
    {"sceHttpReadData",http_fail},
    {"sceHttp2Init",lib_init_id}, {"sceHttp2Term",runtime_stub},
    {"sceHttp2CreateTemplate",http_object}, {"sceHttp2DeleteTemplate",runtime_stub},
};
uintptr_t runtime_http_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
#else
uintptr_t runtime_http_resolve(const char *name) { (void)name; return 0; }
#endif
