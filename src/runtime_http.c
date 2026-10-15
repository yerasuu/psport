/* libSceHttp and libSceHttp2 on an unplugged console (decision #207): templates, connections and
 * requests are created and tracked so setup code proceeds; any transfer fails as unplugged, so no
 * response, header, redirect or TLS handshake ever arrives. The URI and header parsers work on the
 * game's own strings and behave as shadPS4's reverse-engineered ones (gpu/net/shadps4/.../http.cpp). */
#define _GNU_SOURCE
#include "runtime_np.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>
#ifndef _WIN32
#include <time.h>

#define HTTP_OUT_OF_MEMORY ((int32_t)0x80431022)
#define HTTP_INVALID_ID ((int32_t)0x80431100)
#define HTTP_INVALID_VALUE ((int32_t)0x804311fe)
#define HTTP_INVALID_URL ((int32_t)0x80433060)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define HTTP_BEFORE_SEND ((int32_t)0x80431065)
#define HTTP_ABORTED ((int32_t)0x80431080)
#define HTTP_PARSE_NOT_FOUND ((int32_t)0x80432025)
#define HTTP_PARSE_INVALID_RESPONSE ((int32_t)0x80432060)
#define HTTP_PARSE_INVALID_VALUE ((int32_t)0x804321fe)
#define URI_PART_MAX 0x3fff

typedef struct {
    bool is_opaque;
    char *sz_scheme, *sz_username, *sz_password, *sz_hostname, *sz_path, *sz_query, *sz_fragment;
    uint16_t port;
    uint8_t reserved[10];
} HttpUriElement;
_Static_assert(sizeof(HttpUriElement)==80,"OrbisHttpUriElement layout");

static int next_id=1;
static int new_id(void) { return __atomic_fetch_add(&next_id,1,__ATOMIC_RELAXED); }

static ABI int32_t lib_init_id(void) { return new_id(); }
static ABI int32_t http_fail(void) { return HTTP_NETWORK; }
static ABI int32_t http_object(void) { return new_id(); }
static ABI int32_t http_epoll(int32_t ctx,void **handle) {
    (void)ctx;
    if (!handle) return HTTP_INVALID_VALUE;
    *handle=(void *)(uintptr_t)(0x100+new_id()); return 0;
}
static ABI int32_t http_wait(void *handle,void *events,int32_t max,int64_t timeout) {
    (void)handle; (void)events; (void)max;
    if (timeout>0) { struct timespec t={timeout/1000000,(timeout%1000000)*1000}; nanosleep(&t,NULL); }
    return 0; /* no events: nothing is in flight */
}

static int32_t create(uint32_t kind) {
    const int64_t h_object=np_object_create(kind);
    return h_object>0 && h_object<=INT32_MAX ? (int32_t)h_object : HTTP_OUT_OF_MEMORY;
}
static bool is_request(uint32_t kind) { return kind==NP_HTTP_REQUEST || kind==NP_HTTP_REQUEST_SENT || kind==NP_HTTP_REQUEST_ABORTED; }
/* What the setting calls (timeouts, callbacks, transfer options) accept: a template, a connection or a request. */
static bool has_settings(int32_t h_object) {
    const uint32_t kind=np_object_kind(h_object);
    return kind==NP_HTTP_TEMPLATE || kind==NP_HTTP_CONNECTION || is_request(kind);
}
static ABI int32_t http_create_template(int32_t h_context, const char *sz_user_agent, int32_t version, int32_t is_auto_proxy) {
    (void)h_context; (void)sz_user_agent; (void)version; (void)is_auto_proxy;
    return create(NP_HTTP_TEMPLATE);
}
static ABI int32_t http_create_connection_url(int32_t h_template, const char *sz_url, int32_t is_keepalive) {
    (void)is_keepalive;
    if (!np_object_exists(NP_HTTP_TEMPLATE,h_template)) return HTTP_INVALID_ID;
    if (!sz_url) return HTTP_INVALID_URL;
    return create(NP_HTTP_CONNECTION);
}
static ABI int32_t http_create_connection(int32_t h_template, const char *sz_server, const char *sz_scheme, uint16_t port,
                                          int32_t is_keepalive) {
    (void)port; (void)is_keepalive;
    if (!np_object_exists(NP_HTTP_TEMPLATE,h_template)) return HTTP_INVALID_ID;
    if (!sz_server || !sz_scheme) return HTTP_INVALID_VALUE;
    return create(NP_HTTP_CONNECTION);
}
static ABI int32_t http_create_request_url(int32_t h_connection, int32_t method, const char *sz_url, uint64_t cb_content) {
    (void)method; (void)cb_content;
    if (!np_object_exists(NP_HTTP_CONNECTION,h_connection)) return HTTP_INVALID_ID;
    if (!sz_url) return HTTP_INVALID_URL;
    return create(NP_HTTP_REQUEST);
}
/* A method outside the standard ones is sent as given (shadPS4's CUSTOM slot), so only its presence is checked. */
static ABI int32_t http_create_request_url2(int32_t h_connection, const char *sz_method, const char *sz_url, uint64_t cb_content) {
    (void)cb_content;
    if (!np_object_exists(NP_HTTP_CONNECTION,h_connection)) return HTTP_INVALID_ID;
    if (!sz_method) return HTTP_INVALID_VALUE;
    if (!sz_url) return HTTP_INVALID_URL;
    return create(NP_HTTP_REQUEST);
}
static ABI int32_t http_create_request2(int32_t h_connection, const char *sz_method, const char *sz_path, uint64_t cb_content) {
    (void)cb_content;
    if (!np_object_exists(NP_HTTP_CONNECTION,h_connection)) return HTTP_INVALID_ID;
    if (!sz_method || !sz_path || strpbrk(sz_path,"\r\n")) return HTTP_INVALID_VALUE;
    return create(NP_HTTP_REQUEST);
}
static ABI int32_t http_delete_template(int32_t h_template) { return np_object_delete(NP_HTTP_TEMPLATE,h_template) ? 0 : HTTP_INVALID_ID; }
static ABI int32_t http_delete_connection(int32_t h_connection) {
    return np_object_delete(NP_HTTP_CONNECTION,h_connection) ? 0 : HTTP_INVALID_ID;
}
static ABI int32_t http_delete_request(int32_t h_request) {
    const uint32_t kind=np_object_kind(h_request);
    return is_request(kind) && np_object_delete(kind,h_request) ? 0 : HTTP_INVALID_ID;
}
static ABI int32_t http_send_request(int32_t h_request, const void *p_post, uint64_t cb_post) {
    (void)p_post; (void)cb_post;
    const uint32_t kind=np_object_kind(h_request);
    if (!is_request(kind)) return HTTP_INVALID_ID;
    if (kind==NP_HTTP_REQUEST_ABORTED) return HTTP_ABORTED;
    np_object_change(h_request,NP_HTTP_REQUEST_SENT);
    return HTTP_NETWORK;
}
/* Once sent, the request has already failed, so there is nothing left to abort. */
static ABI int32_t http_abort_request(int32_t h_request) {
    const uint32_t kind=np_object_kind(h_request);
    if (!is_request(kind)) return HTTP_INVALID_ID;
    if (kind==NP_HTTP_REQUEST) np_object_change(h_request,NP_HTTP_REQUEST_ABORTED);
    return 0;
}
static ABI int32_t http_set_timeout(int32_t h_object, uint32_t timeout_us) {
    (void)timeout_us;
    return has_settings(h_object) ? 0 : HTTP_INVALID_ID;
}
static ABI int32_t http_set_enabled(int32_t h_object, int32_t is_enabled) {
    (void)is_enabled;
    return has_settings(h_object) ? 0 : HTTP_INVALID_ID;
}
/* No transfer reaches a redirect or a TLS handshake, so the callback is never due. */
static ABI int32_t http_set_callback(int32_t h_object, void *pfn_callback, void *p_user) {
    (void)pfn_callback; (void)p_user;
    return has_settings(h_object) ? 0 : HTTP_INVALID_ID;
}
/* No response ever set a cookie, so the jar has nothing to write out. */
static ABI int32_t http_cookie_flush(int32_t h_context) {
    (void)h_context;
    return 0;
}
static ABI int32_t http_all_response_headers(int32_t h_request, char **pp_out_header, uint64_t *p_out_cb_header) {
    if (!pp_out_header || !p_out_cb_header) return HTTP_INVALID_VALUE;
    switch (np_object_kind(h_request)) {
    case NP_HTTP_REQUEST: return HTTP_BEFORE_SEND;
    case NP_HTTP_REQUEST_SENT: return HTTP_NETWORK;
    case NP_HTTP_REQUEST_ABORTED: return HTTP_ABORTED;
    default: return HTTP_INVALID_ID;
    }
}

static bool is_space(unsigned char c) { return c<0x80 && isspace(c); }
/* The value of a field in a raw header block: its start and length, and as the result the offset
 * past the line it ends on (folded continuation lines are part of the value). */
static ABI int32_t http_parse_response_header(const char *p_header, uint64_t cb_header, const char *sz_field,
                                              const char **pp_out_value, uint64_t *p_out_cb_value) {
    if (!p_header) return HTTP_PARSE_INVALID_RESPONSE;
    if (!sz_field || !pp_out_value || !p_out_cb_value) return HTTP_PARSE_INVALID_VALUE;
    const uint64_t cb_field=strnlen(sz_field,0xfff);
    uint64_t off_value=0;
    bool is_found=false, is_line_start=true;
    for (uint64_t off=0; off<cb_header; is_line_start=p_header[off]=='\n', ++off)
        if (is_line_start && !is_space((unsigned char)p_header[off]) && cb_field<cb_header-off &&
            !strncasecmp(sz_field,p_header+off,cb_field) && p_header[off+cb_field]==':') {
            off_value=off+cb_field+1;
            is_found=true;
            break;
        }
    if (!is_found) return HTTP_PARSE_NOT_FOUND;
    while (off_value<cb_header && is_space((unsigned char)p_header[off_value]))
        if (p_header[off_value++]=='\n') break;
    const uint64_t off_start=off_value;
    uint64_t off_line_end=cb_header, off_value_end=cb_header;
    for (uint64_t off=off_value; off<cb_header; ++off) {
        if (p_header[off]!='\n') continue;
        if (off+1<cb_header && (p_header[off+1]==' ' || p_header[off+1]=='\t')) continue;
        off_line_end=off+1;
        off_value_end=off>off_start && p_header[off-1]=='\r' ? off-1 : off;
        break;
    }
    if (off_value_end<=off_start) {
        *pp_out_value=NULL;
        *p_out_cb_value=0;
        return 0;
    }
    *pp_out_value=p_header+off_start;
    *p_out_cb_value=off_value_end-off_start;
    return (int32_t)off_line_end;
}

static bool is_unreserved(unsigned char c) { return isalnum(c) || c=='-' || c=='_' || c=='.' || c=='~'; }
static ABI int32_t http_uri_escape(char *sz_out, uint64_t *p_out_cb_required, uint64_t cb_out, const char *sz_in) {
    if (!sz_in) return HTTP_INVALID_VALUE;
    uint64_t cb_required=1;
    for (const unsigned char *p=(const unsigned char *)sz_in; *p; ++p) cb_required+=is_unreserved(*p) ? 1 : 3;
    if (p_out_cb_required) *p_out_cb_required=cb_required;
    if (!sz_out) return 0;
    if (cb_out<cb_required) return HTTP_OUT_OF_MEMORY;
    static const char hex[]="0123456789ABCDEF";
    for (const unsigned char *p=(const unsigned char *)sz_in; *p; ++p)
        if (is_unreserved(*p)) *sz_out++=(char)*p;
        else { *sz_out++='%'; *sz_out++=hex[*p>>4]; *sz_out++=hex[*p&15]; }
    *sz_out=0;
    return 0;
}

/* Removes "." and ".." segments from an absolute path in place, as RFC 3986's remove_dot_segments. */
static void sweep_path(char *sz_path) {
    if (sz_path[0]!='/') return;
    char *p_out=sz_path;
    for (const char *p_in=sz_path; *p_in;) {
        const char *p_end=strchr(p_in+1,'/');
        const size_t cb_segment=p_end ? (size_t)(p_end-p_in) : strlen(p_in);
        if (cb_segment==2 && p_in[1]=='.') {
            if (!p_end) *p_out++='/';
        } else if (cb_segment==3 && p_in[1]=='.' && p_in[2]=='.') {
            while (p_out>sz_path && *--p_out!='/') {}
            if (!p_end) *p_out++='/';
        } else {
            memmove(p_out,p_in,cb_segment);
            p_out+=cb_segment;
        }
        p_in+=cb_segment;
    }
    if (p_out==sz_path) *p_out++='/';
    *p_out=0;
}
typedef struct { char *p_pool; uint64_t cb_pool, cb_used; bool should_write; } UriPool;
/* Copies cb_part bytes of the URI as the next NUL-terminated part of the pool; false when it does not fit. */
static bool pool_put(UriPool *p_pool, const char *p_part, uint64_t cb_part, char **pp_out_part) {
    if (p_pool->should_write) {
        if (p_pool->cb_pool-p_pool->cb_used<cb_part+1) return false;
        char *p_at=p_pool->p_pool+p_pool->cb_used;
        memcpy(p_at,p_part,cb_part);
        p_at[cb_part]=0;
        *pp_out_part=p_at;
    }
    p_pool->cb_used+=cb_part+1;
    return true;
}
static bool is_userinfo(unsigned char c) { return isalnum(c) || (c<0x80 && strchr("!$%&'()*+,-.:;=_~",c)); }
static bool is_host(unsigned char c, bool is_ipv6) { return isalnum(c) || (c<0x80 && strchr(is_ipv6 ? "-._:" : "-._",c)); }
/* The parts of a URI copied into the caller's pool, in order: scheme, user name, password, host,
 * path (dot segments removed), query and fragment (with their '?' and '#'). Without a pool only the
 * size it needs is computed. */
static ABI int32_t http_uri_parse(HttpUriElement *p_out, const char *sz_uri, void *p_pool, uint64_t *p_out_cb_required,
                                  uint64_t cb_pool) {
    if (!sz_uri) return HTTP_INVALID_URL;
    UriPool pool={.p_pool=p_pool,.cb_pool=cb_pool,.should_write=p_out && p_pool};
    if (!pool.should_write && !p_out_cb_required) return HTTP_INVALID_VALUE;
    HttpUriElement element={0};
    uint64_t cb_scheme=0;
    while (cb_scheme<0x20 && sz_uri[cb_scheme] && sz_uri[cb_scheme]!=':' &&
           (isalnum((unsigned char)sz_uri[cb_scheme]) || strchr("+-.",sz_uri[cb_scheme])))
        ++cb_scheme;
    const bool has_scheme=cb_scheme && sz_uri[cb_scheme]==':' && isalpha((unsigned char)sz_uri[0]);
    if (!has_scheme) cb_scheme=0;
    if (!pool_put(&pool,sz_uri,cb_scheme,&element.sz_scheme)) return HTTP_OUT_OF_MEMORY;
    const char *p=sz_uri+(has_scheme ? cb_scheme+1 : 0);
    if (p[0]=='/' && p[1]=='/') p+=2;
    else element.is_opaque=true;
    uint64_t cb_authority=0, off_colon=0;
    bool has_colon=false, has_at=false;
    for (;; ++cb_authority) {
        const unsigned char c=(unsigned char)p[cb_authority];
        if (!c) break;
        if (c=='@') { has_at=true; break; }
        if (c==':' && !has_colon) { has_colon=true; off_colon=cb_authority; }
        else if (!is_userinfo(c)) break;
    }
    if (has_at) {
        const uint64_t cb_user=has_colon ? off_colon : cb_authority;
        const char *p_password=has_colon ? p+off_colon+1 : p+cb_authority;
        if (!pool_put(&pool,p,cb_user,&element.sz_username) ||
            !pool_put(&pool,p_password,(uint64_t)(p+cb_authority-p_password),&element.sz_password))
            return HTTP_OUT_OF_MEMORY;
        p+=cb_authority+1;
    } else if (!pool_put(&pool,p,0,&element.sz_username) || !pool_put(&pool,p,0,&element.sz_password))
        return HTTP_OUT_OF_MEMORY;
    const bool is_ipv6=p[0]=='[';
    const char *p_host=p+is_ipv6;
    uint64_t cb_host=0;
    if (p[0]!='.')
        while (p_host[cb_host] && is_host((unsigned char)p_host[cb_host],is_ipv6))
            if (++cb_host>=(is_ipv6 ? 0xfeu : 0xffu)) return HTTP_INVALID_URL;
    if (is_ipv6 && p_host[cb_host]!=']') return HTTP_INVALID_URL;
    if (!pool_put(&pool,p_host,cb_host,&element.sz_hostname)) return HTTP_OUT_OF_MEMORY;
    p=p_host+cb_host+is_ipv6;
    bool has_port=false;
    if (*p==':') {
        uint32_t port=0;
        int c_digits=0;
        for (++p; c_digits<5 && isdigit((unsigned char)*p); ++p, ++c_digits) port=port*10+(uint32_t)(*p-'0');
        if (port>0x10000 || (*p && *p!='/')) return HTTP_INVALID_URL;
        has_port=c_digits>0;
        element.port=(uint16_t)port;
    }
    if (!has_port && has_scheme)
        element.port=!strncasecmp(sz_uri,"https",5) ? 443 : !strncasecmp(sz_uri,"http",4) ? 80 : 0;
    const uint64_t cb_path=strcspn(p,"?#");
    if (cb_path>URI_PART_MAX) return HTTP_INVALID_URL;
    if (!pool_put(&pool,p,cb_path,&element.sz_path)) return HTTP_OUT_OF_MEMORY;
    if (pool.should_write) sweep_path(element.sz_path);
    p+=cb_path;
    const uint64_t cb_query=*p=='?' ? strcspn(p,"#") : 0;
    if (cb_query>URI_PART_MAX) return HTTP_INVALID_URL;
    if (!pool_put(&pool,p,cb_query,&element.sz_query)) return HTTP_OUT_OF_MEMORY;
    p+=cb_query;
    const uint64_t cb_fragment=*p=='#' ? strlen(p) : 0;
    if (cb_fragment>URI_PART_MAX) return HTTP_INVALID_URL;
    if (!pool_put(&pool,p,cb_fragment,&element.sz_fragment)) return HTTP_OUT_OF_MEMORY;
    if (pool.should_write) *p_out=element;
    if (p_out_cb_required) *p_out_cb_required=pool.cb_used;
    return 0;
}

static const RuntimeExport exports[]={
    {"sceHttpTerm",runtime_stub},
    {"sceHttpCreateTemplate",http_create_template}, {"sceHttpDeleteTemplate",http_delete_template},
    {"sceHttpCreateConnectionWithURL",http_create_connection_url}, {"sceHttpCreateRequestWithURL",http_create_request_url},
    {"sceHttpCreateConnection",http_create_connection}, {"sceHttpCreateRequestWithURL2",http_create_request_url2},
    {"sceHttpCreateRequest2",http_create_request2},
    {"sceHttpSendRequest",http_send_request}, {"sceHttpAbortRequest",http_abort_request}, {"sceHttpCreateEpoll",http_epoll},
    {"sceHttpSetNonblock",runtime_stub}, {"sceHttpSetConnectTimeOut",runtime_stub},
    {"sceHttpSetSendTimeOut",http_set_timeout}, {"sceHttpSetRecvTimeOut",http_set_timeout},
    {"sceHttpSetChunkedTransferEnabled",http_set_enabled}, {"sceHttpSetAuthEnabled",http_set_enabled},
    {"sceHttpsSetSslCallback",http_set_callback}, {"sceHttpSetRedirectCallback",http_set_callback},
    {"sceHttpCookieFlush",http_cookie_flush},
    {"sceHttpsEnableOption",runtime_stub}, {"sceHttpsDisableOption",runtime_stub},
    {"sceHttpAddRequestHeader",runtime_stub}, {"sceHttpSetRequestContentLength",runtime_stub},
    {"sceHttpDeleteConnection",http_delete_connection}, {"sceHttpDeleteRequest",http_delete_request},
    {"sceHttpAbortWaitRequest",runtime_stub}, {"sceHttpDestroyEpoll",runtime_stub},
    {"sceHttpSetEpoll",runtime_stub}, {"sceHttpUnsetEpoll",runtime_stub}, {"sceHttpWaitRequest",http_wait},
    {"sceHttpGetStatusCode",http_fail}, {"sceHttpGetResponseContentLength",http_fail},
    {"sceHttpReadData",http_fail}, {"sceHttpGetAllResponseHeaders",http_all_response_headers},
    {"sceHttpParseResponseHeader",http_parse_response_header},
    {"sceHttpUriEscape",http_uri_escape}, {"sceHttpUriParse",http_uri_parse},
    {"sceHttp2Init",lib_init_id}, {"sceHttp2Term",runtime_stub},
    {"sceHttp2CreateTemplate",http_object}, {"sceHttp2DeleteTemplate",runtime_stub},
};
uintptr_t runtime_http_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
#else
uintptr_t runtime_http_resolve(const char *name) { (void)name; return 0; }
#endif
