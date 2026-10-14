/* libSceNpTrophy: trophies are accepted locally and recorded in the log. The game's trophy pack
 * (sce_sys/trophy/trophy00.trp) is not read, so the set looks as on a console without the title's
 * trophy data installed: no groups, no icons. */
#define _GNU_SOURCE
#include "runtime_np.h"
#include <stdio.h>
#include <string.h>
#ifndef _WIN32

#define TROPHY_INVALID ((int32_t)0x80551604)
#define TROPHY_OUT_OF_MEMORY ((int32_t)0x80551603)
#define TROPHY_INVALID_HANDLE ((int32_t)0x80551608)
#define TROPHY_INVALID_CONTEXT ((int32_t)0x80551609)
#define TROPHY_INVALID_TROPHY_ID ((int32_t)0x8055160A)
#define TROPHY_INVALID_GROUP_ID ((int32_t)0x8055160B)
#define TROPHY_ICON_FILE_NOT_FOUND ((int32_t)0x80551614)
#define TROPHY_NUM_MAX 128
#define BASE_GAME_GROUP (-1)

static int32_t new_object(uint32_t kind, int32_t *p_out_id) {
    const int64_t h_object=np_object_create(kind);
    if (h_object<=0 || h_object>INT32_MAX) return TROPHY_OUT_OF_MEMORY;
    *p_out_id=(int32_t)h_object;
    return 0;
}
static int32_t check(int32_t h_context, int32_t h_handle) {
    if (!np_object_exists(NP_TROPHY_CONTEXT,h_context)) return TROPHY_INVALID_CONTEXT;
    return np_object_exists(NP_TROPHY_HANDLE,h_handle) ? 0 : TROPHY_INVALID_HANDLE;
}

static ABI int32_t trophy_context(int32_t *ctx,int32_t user,uint32_t label,uint64_t options) {
    (void)user; (void)label; (void)options;
    if (!ctx) return TROPHY_INVALID;
    return new_object(NP_TROPHY_CONTEXT,ctx);
}
static ABI int32_t trophy_handle(int32_t *handle) { if (!handle) return TROPHY_INVALID; return new_object(NP_TROPHY_HANDLE,handle); }
static ABI int32_t trophy_destroy_context(int32_t h_context) {
    return np_object_delete(NP_TROPHY_CONTEXT,h_context) ? 0 : TROPHY_INVALID_CONTEXT;
}
static ABI int32_t trophy_destroy_handle(int32_t h_handle) {
    return np_object_delete(NP_TROPHY_HANDLE,h_handle) ? 0 : TROPHY_INVALID_HANDLE;
}
/* Every call on a handle completes before it returns, so an abort finds nothing in progress. */
static ABI int32_t trophy_abort_handle(int32_t h_handle) {
    return np_object_exists(NP_TROPHY_HANDLE,h_handle) ? 0 : TROPHY_INVALID_HANDLE;
}
static ABI int32_t trophy_unlock(int32_t ctx,int32_t handle,int32_t id,int32_t *platinum) {
    (void)ctx; (void)handle;
    printf("Runtime: trophy %d unlocked\n",id);
    if (platinum) *platinum=-1;
    return 0;
}
static ABI int32_t trophy_game_info(int32_t ctx,int32_t handle,void *details,void *data) {
    (void)ctx; (void)handle;
    if (details) { uint64_t size; memcpy(&size,details,8); memset((char *)details+8,0,size>8 && size<4096 ? size-8 : 0); }
    if (data) { uint64_t size; memcpy(&size,data,8); memset((char *)data+8,0,size>8 && size<4096 ? size-8 : 0); }
    return 0;
}
static ABI int32_t trophy_info(int32_t ctx,int32_t handle,int32_t id,void *details,void *data) {
    (void)id; return trophy_game_info(ctx,handle,details,data);
}
static ABI int32_t trophy_unlock_state(int32_t ctx,int32_t handle,uint32_t *flags,uint32_t *count) {
    (void)ctx; (void)handle;
    if (!flags || !count) return TROPHY_INVALID;
    memset(flags,0,16); *count=0;
    return 0;
}
static ABI int32_t trophy_game_icon(int32_t h_context, int32_t h_handle, void *p_buffer, uint64_t *p_cb_icon) {
    (void)p_buffer;
    const int32_t result=check(h_context,h_handle);
    if (result) return result;
    return p_cb_icon ? TROPHY_ICON_FILE_NOT_FOUND : TROPHY_INVALID;
}
static ABI int32_t trophy_group_icon(int32_t h_context, int32_t h_handle, int32_t group, void *p_buffer, uint64_t *p_cb_icon) {
    (void)group; (void)p_buffer;
    const int32_t result=check(h_context,h_handle);
    if (result) return result;
    return p_cb_icon ? TROPHY_ICON_FILE_NOT_FOUND : TROPHY_INVALID;
}
static ABI int32_t trophy_icon(int32_t h_context, int32_t h_handle, int32_t trophy, void *p_buffer, uint64_t *p_cb_icon) {
    (void)p_buffer;
    if (!p_cb_icon) return TROPHY_INVALID;
    if (trophy<0 || trophy>=TROPHY_NUM_MAX) return TROPHY_INVALID_TROPHY_ID;
    const int32_t result=check(h_context,h_handle);
    return result ? result : TROPHY_ICON_FILE_NOT_FOUND;
}
/* OrbisNpTrophyGroupDetails and OrbisNpTrophyGroupData both start with their size and the group id.
 * Only the base game's group exists in an empty set, with nothing in it. */
static ABI int32_t trophy_group_info(int32_t h_context, int32_t h_handle, int32_t group, void *p_details, void *p_data) {
    const int32_t result=check(h_context,h_handle);
    if (result) return result;
    if (!p_details || !p_data) return TROPHY_INVALID;
    if (group!=BASE_GAME_GROUP) return TROPHY_INVALID_GROUP_ID;
    trophy_game_info(h_context,h_handle,p_details,p_data);
    memcpy((char *)p_details+8,&group,sizeof(group));
    memcpy((char *)p_data+8,&group,sizeof(group));
    return 0;
}
/* The trophy list is the system UI's, which this port does not have. */
static ABI int32_t trophy_show_list(int32_t h_context, int32_t h_handle) {
    static uint64_t s_c_shown;
    const int32_t result=check(h_context,h_handle);
    if (result) return result;
    if (!__atomic_fetch_add(&s_c_shown,1,__ATOMIC_RELAXED)) {
        puts("Runtime: NpTrophy: the system's trophy list was asked for; there is no system UI to show it");
        runtime_calls_hle_stub("sceNpTrophyShowTrophyList (no system UI)",&s_c_shown);
    }
    return 0;
}
/* The screenshot the system would attach to the trophy: the port takes none. */
static ABI int32_t trophy_capture_screenshot(int32_t h_handle, const void *p_targets, uint32_t c_targets) {
    static uint64_t s_c_captures;
    (void)p_targets; (void)c_targets;
    if (!np_object_exists(NP_TROPHY_HANDLE,h_handle)) return TROPHY_INVALID_HANDLE;
    if (!__atomic_fetch_add(&s_c_captures,1,__ATOMIC_RELAXED)) {
        puts("Runtime: NpTrophy: trophy screenshot asked for; the port takes none");
        runtime_calls_hle_stub("sceNpTrophyCaptureScreenshot (no screenshot taken)",&s_c_captures);
    }
    return 0;
}

static const RuntimeExport exports[]={
    {"sceNpTrophyCreateContext",trophy_context}, {"sceNpTrophyCreateHandle",trophy_handle},
    {"sceNpTrophyRegisterContext",runtime_stub}, {"sceNpTrophyUnlockTrophy",trophy_unlock},
    {"sceNpTrophyDestroyHandle",trophy_destroy_handle}, {"sceNpTrophyDestroyContext",trophy_destroy_context},
    {"sceNpTrophyAbortHandle",trophy_abort_handle}, {"sceNpTrophyGetGameIcon",trophy_game_icon},
    {"sceNpTrophyGetGroupIcon",trophy_group_icon}, {"sceNpTrophyGetTrophyIcon",trophy_icon},
    {"sceNpTrophyGetGroupInfo",trophy_group_info}, {"sceNpTrophyShowTrophyList",trophy_show_list},
    {"sceNpTrophyCaptureScreenshot",trophy_capture_screenshot},
    {"sceNpTrophyGetGameInfo",trophy_game_info}, {"sceNpTrophyGetTrophyInfo",trophy_info},
    {"sceNpTrophyGetTrophyUnlockState",trophy_unlock_state},
};
uintptr_t runtime_trophy_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
#else
uintptr_t runtime_trophy_resolve(const char *name) { (void)name; return 0; }
#endif
