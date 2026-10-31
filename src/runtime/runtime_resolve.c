// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime.h"
#include "gpu/psport_gpu.h"
#include <string.h>

/* First match wins, so a name two libraries export goes to the earlier one; the shadPS4 ports last. */
static uintptr_t (*const s_pfn_resolvers[])(const char *sz_name)={
    runtime_mutex_resolve, runtime_thread_resolve, runtime_sched_resolve, runtime_content_resolve,
    runtime_sema_resolve, runtime_eventflag_resolve, runtime_ulobjmgr_resolve, runtime_ult_resolve,
    runtime_rwlock_resolve, runtime_barrier_resolve, runtime_memory_resolve, runtime_mempool_resolve,
    runtime_exception_resolve, runtime_kernel_resolve, runtime_module_resolve, runtime_process_resolve, runtime_playgo_resolve,
    runtime_voice_resolve, runtime_services_resolve, runtime_ajm_resolve, runtime_audio_resolve,
    runtime_audio3d_resolve, runtime_pad_resolve, runtime_gesture_resolve, runtime_devices_resolve,
    runtime_capture_resolve, runtime_vr_resolve, runtime_mouse_resolve, runtime_keyboard_resolve,
    runtime_rtc_resolve, runtime_savedata_resolve, runtime_file_resolve, runtime_shm_resolve, runtime_ioctl_resolve, runtime_aio_resolve, runtime_guest_output_resolve,
    psport_gpu_resolve,
};
uintptr_t runtime_library_resolve(const char *sz_name) {
    for (size_t i=0;i<sizeof(s_pfn_resolvers)/sizeof(*s_pfn_resolvers);++i) {
        const uintptr_t va_found=s_pfn_resolvers[i](sz_name);
        if (va_found) return va_found;
    }
    return 0;
}
int g_is_runtime_net_held;
int runtime_library_serves(const char *sz_nid) {
    g_is_runtime_net_held=1;
    const int is_served=runtime_library_resolve(sz_nid)!=0;
    g_is_runtime_net_held=0;
    return is_served;
}
/* The NIDs before any "#<library>#<module>" scope are equal. */
int runtime_same_nid(const char *sz_a, const char *sz_b) {
    while (*sz_a && *sz_a!='#' && *sz_a==*sz_b) ++sz_a, ++sz_b;
    return (!*sz_a || *sz_a=='#') && (!*sz_b || *sz_b=='#');
}
/* Symbol name for a NID in any scope, or NULL when no listed title imports it. */
static const struct { char sz_nid[12]; const char *sz_symbol; } s_all_names[]={
#include "import_names_all.inc"
};
const char *runtime_symbol(const char *sz_nid) {
    size_t low=0, high=sizeof(s_all_names)/sizeof(*s_all_names);
    while (low<high) {
        size_t mid=low+(high-low)/2;
        int order=strncmp(sz_nid,s_all_names[mid].sz_nid,11);
        if (!order) return s_all_names[mid].sz_symbol;
        if (order<0) high=mid; else low=mid+1;
    }
    /* Exports aerolib does not name, called as shadPS4 calls them. */
    static const struct { const char *sz_nid, *sz_symbol; } s_extra[]={
        {"BG26hBGiNlw","_sceUlobjmgrRegisterObject"}, {"Smf+fUNblPc","_sceUlobjmgrUnregisterObject"},
        {"HZ9Q2c+4BU4","ulobjmgr Func_1D9F50D9CFB8054E"}, {"SweJO7t3pkk","ulobjmgr Func_4B07893BBB77A649"},
        {"fJgP+wqifno","sceDiscMap_7C980FFB0AA27E7A"}, {"ioKMruft1ek","sceDiscMap_8A828CAEE7EDD5E9"},
        {"mkgXxsoxWHg","sceKernelClearVirtualRangeName"},
    };
    for (size_t i=0;i<sizeof(s_extra)/sizeof(*s_extra);++i)
        if (runtime_same_nid(sz_nid,s_extra[i].sz_nid)) return s_extra[i].sz_symbol;
    return NULL;
}
/* Same symbol name means same contract whether imported from libkernel or
 * libScePosix, so newer modules resolve by name through runtime_symbol. */
/* Arguments are ignored: under the System V ABI the caller removes them, so one function stands
 * for every signature returning a status. */
ABI int32_t runtime_stub(void) { return 0; }
int g_runtime_last_stub;
uintptr_t runtime_lookup(const RuntimeExport *p_table,size_t c_exports,const char *sz_nid) {
    const char *sz_symbol=runtime_symbol(sz_nid);
    if (!sz_symbol) return 0;
    for (size_t i=0;i<c_exports;++i)
        if (!strcmp(sz_symbol,p_table[i].sz_name)) {
            g_runtime_last_stub=p_table[i].p_function==(void *)runtime_stub;
            return (uintptr_t)p_table[i].p_function;
        }
    return 0;
}
const char *runtime_import_name(const char *sz_name) {
    const char *sz_symbol=runtime_symbol(sz_name);
    return sz_symbol ? sz_symbol : "name not resolved";
}
static const struct { char sz_nid[12]; const char *sz_function; unsigned char outputs; } s_output_arguments[]={
#include "import_outparams.inc"
};
unsigned runtime_import_outputs(const char *sz_nid) {
    size_t low=0, high=sizeof(s_output_arguments)/sizeof(*s_output_arguments);
    while (low<high) {
        size_t mid=low+(high-low)/2;
        int order=strncmp(sz_nid,s_output_arguments[mid].sz_nid,11);
        if (!order) return s_output_arguments[mid].outputs;
        if (order<0) high=mid; else low=mid+1;
    }
    return 0;
}
static const struct { char sz_nid[12]; unsigned char kind; } s_failure_kinds[]={
#include "import_returns.inc"
};
unsigned runtime_import_failure(const char *sz_nid) {
    size_t low=0, high=sizeof(s_failure_kinds)/sizeof(*s_failure_kinds);
    while (low<high) {
        size_t mid=low+(high-low)/2;
        int order=strncmp(sz_nid,s_failure_kinds[mid].sz_nid,11);
        if (!order) return s_failure_kinds[mid].kind;
        if (order<0) high=mid; else low=mid+1;
    }
    return 0;
}
typedef struct { char sz_nid[12]; const char *sz_function; uint32_t code; } ImportResult;
static const ImportResult s_status_results[]={
#include "import_statuses.inc"
};
static const ImportResult s_offline_results[]={
#include "import_offline.inc"
};
static int has_import_result(const ImportResult *p_results, size_t c_results, const char *sz_nid, uint32_t code) {
    size_t low=0, high=c_results;
    while (low<high) {
        size_t mid=low+(high-low)/2;
        int order=strncmp(sz_nid,p_results[mid].sz_nid,11);
        if (!order) order=code<p_results[mid].code ? -1 : code>p_results[mid].code;
        if (!order) return 1;
        if (order<0) high=mid; else low=mid+1;
    }
    return 0;
}
int runtime_import_status(const char *sz_nid, uint32_t code) {
    return has_import_result(s_status_results,sizeof(s_status_results)/sizeof(*s_status_results),sz_nid,code);
}
int runtime_import_offline(const char *sz_nid, uint32_t code) {
    return has_import_result(s_offline_results,sizeof(s_offline_results)/sizeof(*s_offline_results),sz_nid,code);
}
