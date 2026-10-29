// SPDX-License-Identifier: GPL-2.0-or-later
/* Who the process is, as libkernel and the system libraries ask the kernel: a retail game with no
 * extra capabilities, inside its sandbox, with no title workaround and no VR capture. Layouts and
 * error codes are the ones the console's own modules use (kb #1248); the values a retail game gets
 * were not read from a console, so each answer is the one that takes the retail branch where the
 * modules test it, and the call report counts it as such. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define EFAULT_ ERR(14)
#define EINVAL_ ERR(22)
#define GUEST_PID 1000
/* No console's id for a game was read; any fixed one serves, because the guest only names things
 * with it (CommonDialog's shared memory "/<app id>CdlgShWk"). */
#define GUEST_APP_ID UINT32_C(0x60000100)

static int32_t fail_posix(int e) { *runtime_errno()=runtime_guest_errno(e); return -1; }
static void note_inferred(uint64_t *p_count, const char *sz_what) {
    if (!__atomic_fetch_add(p_count,1,__ATOMIC_RELAXED)) runtime_calls_hle_stub(sz_what,p_count);
}

/* paid 0x38000000100000xx marks a system process and caps[0] bits 62 and 60, caps[1] bit 57 and
 * attrs byte 0x2a grant more: zero takes every retail-game branch libkernel has. */
typedef struct { uint64_t paid, caps[4], attrs[4], unknown[8]; } GuestAuthinfo;
_Static_assert(sizeof(GuestAuthinfo)==0x88,"authinfo layout");
static uint64_t s_c_authinfo;
static ABI int32_t get_authinfo(int32_t pid, GuestAuthinfo *p_out_info) {
    if (pid!=GUEST_PID && pid!=0) return fail_posix(ESRCH);
    if (!runtime_memory_is_guest_range(p_out_info,sizeof(*p_out_info))) return fail_posix(EFAULT);
    *p_out_info=(GuestAuthinfo){0};
    note_inferred(&s_c_authinfo,"get_authinfo (zeroed: a retail game's paid and capabilities were not read)");
    return 0;
}

/* flags 0x02, 0x10, 0x20, 0x40 and 0x80 select system, JIT, SpZeroConf and SpcPlayerCore paths:
 * none is a game's, and type 0 is what sceKernelGetProcessType answers. */
typedef struct { uint64_t cb_info; uint32_t type, flags; } GuestProcTypeInfo;
_Static_assert(sizeof(GuestProcTypeInfo)==0x10,"proc_type_info layout");
static uint64_t s_c_proc_type_info;
static ABI int32_t get_proc_type_info(GuestProcTypeInfo *p_inout_info) {
    if (!runtime_memory_is_guest_range(p_inout_info,sizeof(*p_inout_info))) return fail_posix(EFAULT);
    if (p_inout_info->cb_info<sizeof(*p_inout_info)) return fail_posix(EINVAL);
    *p_inout_info=(GuestProcTypeInfo){.cb_info=sizeof(*p_inout_info)};
    note_inferred(&s_c_proc_type_info,"__sys_get_proc_type_info (type 0, no flags: the kernel's value was not read)");
    return 0;
}

/* kern.proc.appinfo. Proven: a negative rendering mode makes the caller fail with 0x80020001, and
 * TitleWorkaroundIsEnabled reads its bitmap at +8 of the block at 0x30. The app id at 0, the title
 * ID at 0x10 and preload flags at 0x20 are inferred. */
typedef struct {
    uint32_t app_id;
    uint8_t reserved0[12];
    char title_id[13];
    int8_t rendering_mode;
    uint8_t reserved1[2];
    uint64_t preload_flags;
    uint8_t reserved2[8];
    uint64_t workaround_reserved, workaround_bits;
    uint8_t reserved3[8];
} GuestAppInfo;
_Static_assert(sizeof(GuestAppInfo)==0x48,"appinfo layout");
_Static_assert(offsetof(GuestAppInfo,rendering_mode)==0x1d,"appinfo rendering mode");
_Static_assert(offsetof(GuestAppInfo,workaround_bits)==0x38,"appinfo title workaround bitmap");
static uint64_t s_c_app_info;
int runtime_process_app_info(int32_t pid, void *p_old, uint64_t *p_inout_cb) {
    if (!runtime_memory_is_guest_range(p_inout_cb,sizeof(*p_inout_cb))) return EFAULT;
    if (pid!=GUEST_PID) return ESRCH;
    if (!p_old) { *p_inout_cb=sizeof(GuestAppInfo); return 0; }
    const uint64_t cb_copy=*p_inout_cb<sizeof(GuestAppInfo) ? *p_inout_cb : sizeof(GuestAppInfo);
    if (!runtime_memory_is_guest_range(p_old,cb_copy)) return EFAULT;
    GuestAppInfo info={.app_id=GUEST_APP_ID};
    snprintf(info.title_id,sizeof(info.title_id),"%s",runtime_content_title_id());
    note_inferred(&s_c_app_info,"kern.proc.appinfo (app id and title ID only: the rest of a retail game's was not read)");
    memcpy(p_old,&info,cb_copy);
    *p_inout_cb=cb_copy;
    return cb_copy<sizeof(GuestAppInfo) ? ENOMEM : 0;
}
static ABI int32_t get_app_info(int32_t pid, GuestAppInfo *p_out_info) {
    if (!p_out_info) return EFAULT_;
    uint64_t cb_info=sizeof(*p_out_info);
    const int e=runtime_process_app_info(pid,p_out_info,&cb_info);
    return e ? ERR(runtime_guest_errno(e)) : 0;
}
/* p_block is appinfo's title-workaround block (its offset 0x30). */
static ABI int32_t title_workaround_is_enabled(const void *p_block, uint32_t id, int32_t *p_out_is_enabled) {
    if (!p_block || !p_out_is_enabled) return EFAULT_;
    if (id>0x38) return EINVAL_;
    const uint64_t *p_bits=(const uint64_t *)((const unsigned char *)p_block+8);
    if (!runtime_memory_is_guest_range(p_bits,sizeof(*p_bits)) ||
        !runtime_memory_is_guest_range(p_out_is_enabled,sizeof(*p_out_is_enabled)))
        return EFAULT_;
    *p_out_is_enabled=(int32_t)((*p_bits>>id)&1);
    return 0;
}

/* Every game runs in a sandbox; the system libraries build their paths from its random word. */
static ABI int32_t is_in_sandbox(void) { return 1; }
static const char s_sz_sandbox_word[]="psportsbox";
const char *runtime_process_sandbox_word(void) { return s_sz_sandbox_word; }
static uint64_t s_c_sandbox_word;
static ABI const char *sandbox_random_word(void) {
    note_inferred(&s_c_sandbox_word,"sceKernelGetFsSandboxRandomWord (a fixed word; nothing is mounted under it)");
    return s_sz_sandbox_word;
}

/* Two libkernel globals of 0x28 bytes, both zero on a console that never captured for VR:
 * libSceCamera skips VR capture on a size of 0. */
#define CB_VR_CAPTURE 0x28
static uint64_t s_c_vr_capture_size;
static ABI int32_t vr_capture_size(void *p_out_first, void *p_out_second) {
    if (!runtime_memory_is_guest_range(p_out_first,CB_VR_CAPTURE) ||
        !runtime_memory_is_guest_range(p_out_second,CB_VR_CAPTURE))
        return EFAULT_;
    memset(p_out_first,0,CB_VR_CAPTURE);
    memset(p_out_second,0,CB_VR_CAPTURE);
    note_inferred(&s_c_vr_capture_size,"sceKernelGetVrCaptureSize (zero: no VR capture)");
    return 0;
}

/* LncAppStatus: callers read only the app id in its first word; 16 bytes at most. */
#define CB_LNC_APP_STATUS 16
static uint64_t s_c_app_status;
static ABI int32_t lnc_app_status(void *p_out_status) {
    /* libSceLncUtil's own code for a bad pointer was not read; the kernel's EFAULT stands in. */
    if (!runtime_memory_is_guest_range(p_out_status,CB_LNC_APP_STATUS)) return EFAULT_;
    memset(p_out_status,0,CB_LNC_APP_STATUS);
    const uint32_t app_id=GUEST_APP_ID;
    memcpy(p_out_status,&app_id,sizeof(app_id));
    note_inferred(&s_c_app_status,"sceLncUtilGetAppStatus (fixed app id, other fields zero)");
    return 0;
}

/* The registry (libSceRegMgr's system call): GetInt is op 2 with the key as id and a 4-byte value,
 * NonSysGetInt op 0x19 with {key, index, value} in the buffer. No key's console value has been read
 * yet, so each one answers 0, the value an unset setting has, and the first ones are logged with
 * their key so that the real values can be read and listed. */
#define REGMGR_GET_INT 2
#define REGMGR_NON_SYS_GET_INT 0x19
typedef struct { uint64_t key; uint32_t index, value; } GuestRegmgrQuery;
_Static_assert(sizeof(GuestRegmgrQuery)==0x10,"regmgr NonSysGetInt buffer");
static uint64_t s_c_regmgr_defaults, s_c_regmgr_unknown_ops;
static ABI int32_t regmgr_call(uint32_t op, uint64_t id, int32_t *p_out_status, void *p_inout_buffer, uint64_t cb_buffer) {
    uint64_t key=id;
    if (op!=REGMGR_GET_INT && op!=REGMGR_NON_SYS_GET_INT) {
        const uint64_t c_before=__atomic_fetch_add(&s_c_regmgr_unknown_ops,1,__ATOMIC_RELAXED);
        if (c_before<8) printf("Runtime: __sys_regmgr_call op %#x (id %#llx, %llu bytes) is not served: EINVAL\n",op,
                               (unsigned long long)id,(unsigned long long)cb_buffer);
        if (!c_before) runtime_calls_hle_stub("__sys_regmgr_call operation the runtime does not serve (EINVAL)",&s_c_regmgr_unknown_ops);
        return fail_posix(EINVAL);
    }
    const uint64_t cb_expected=op==REGMGR_GET_INT ? sizeof(int32_t) : sizeof(GuestRegmgrQuery);
    if (cb_buffer!=cb_expected) return fail_posix(EINVAL);
    if (!runtime_memory_is_guest_range(p_out_status,sizeof(*p_out_status)) ||
        !runtime_memory_is_guest_range(p_inout_buffer,cb_buffer))
        return fail_posix(EFAULT);
    if (op==REGMGR_NON_SYS_GET_INT) {
        GuestRegmgrQuery *p_query=p_inout_buffer;
        key=p_query->key;
        p_query->value=0;
    } else {
        memset(p_inout_buffer,0,sizeof(int32_t));
    }
    *p_out_status=0;
    const uint64_t c_before=__atomic_fetch_add(&s_c_regmgr_defaults,1,__ATOMIC_RELAXED);
    if (c_before<16) printf("Runtime: __sys_regmgr_call op %#x key %#llx answered 0 (the console's value is not known)\n",op,
                            (unsigned long long)key);
    if (!c_before) runtime_calls_hle_stub("__sys_regmgr_call registry key answered 0 (console value not known)",&s_c_regmgr_defaults);
    return 0;
}

static const RuntimeExport s_exports[]={
    {"get_authinfo",get_authinfo}, {"__sys_get_proc_type_info",get_proc_type_info},
    {"sceKernelGetAppInfo",get_app_info},
    {"sceKernelTitleWorkaroundIsEnabled",title_workaround_is_enabled},
    {"sceKernelTitleWorkdaroundIsEnabled",title_workaround_is_enabled},
    {"sceKernelIsInSandbox",is_in_sandbox}, {"sceKernelGetFsSandboxRandomWord",sandbox_random_word},
    {"sceKernelGetVrCaptureSize",vr_capture_size}, {"sceLncUtilGetAppStatus",lnc_app_status},
    {"__sys_regmgr_call",regmgr_call},
};
uintptr_t runtime_process_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
