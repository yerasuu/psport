// SPDX-License-Identifier: GPL-2.0-or-later
/* PSN libraries on an unplugged console, signed out (decision #207): contexts, handles, requests and
 * callbacks are created and tracked locally; whatever needs the PSN servers fails with the
 * library's not-signed-in or network error. */
#ifndef PS_RUNTIME_NP_H
#define PS_RUNTIME_NP_H
#include "runtime.h"
#include <stdbool.h>

enum {
    NP_OBJECT_FREE,
    NP_WEBAPI_USER_CONTEXT,
    NP_WEBAPI_REQUEST,
    NP_WEBAPI_HANDLE,
    NP_WEBAPI_FILTER,
    NP_WEBAPI_CALLBACK,
    NP_WEBAPI2_USER_CONTEXT,
    NP_WEBAPI2_REQUEST,
    NP_WEBAPI2_HANDLE,
    NP_WEBAPI2_FILTER,
    NP_WEBAPI2_CALLBACK,
    NP_WEBAPI2_PUSH_CONTEXT,
    NP_IN_GAME_MESSAGE_LIBRARY,
    NP_IN_GAME_MESSAGE_HANDLE,
    NP_TUS_REQUEST,
    NP_WORD_FILTER_REQUEST,
    NP_HTTP_TEMPLATE,
    NP_HTTP_CONNECTION,
    NP_HTTP_REQUEST,
    NP_HTTP_REQUEST_SENT,
    NP_HTTP_REQUEST_ABORTED,
    NP_TROPHY_CONTEXT,
    NP_TROPHY_HANDLE,
};

#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)

/* Ids are positive, unique across kinds, and never reused while the process runs. */
int64_t np_object_create(uint32_t kind);
bool np_object_exists(uint32_t kind, int64_t h_object);
bool np_object_delete(uint32_t kind, int64_t h_object);
/* The kind of an existing object, or NP_OBJECT_FREE. */
uint32_t np_object_kind(int64_t h_object);
/* Moves an object to another kind (a request's state), keeping its id. */
bool np_object_change(int64_t h_object, uint32_t kind);

uintptr_t runtime_np_community_resolve(const char *sz_name);
#endif
