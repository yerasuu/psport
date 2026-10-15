/* Online play (PS_ONLINE=1): the game's network and PSN imports go to the online module
 * (gpu/net: libpsport-net.so beside libpsport-gpu.so), loaded here on the first such import. Offline,
 * or when the module is missing or fails to start, they stay with the runtime's offline
 * versions (runtime_services.c). Trophies, voice and the store/profile dialogs always stay here. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/net/psport_net.h"
#include "gpu/psport_gpu.h"
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char s_sz_game_folder[PATH_MAX], s_sz_game_serial[16], s_sz_game_title[128], s_sz_game_version[16];

void runtime_net_configure(const char *sz_app0, const char *sz_serial, const char *sz_title, const char *sz_version) {
    snprintf(s_sz_game_folder,sizeof(s_sz_game_folder),"%s",sz_app0 ? sz_app0 : "");
    snprintf(s_sz_game_serial,sizeof(s_sz_game_serial),"%s",sz_serial ? sz_serial : "");
    snprintf(s_sz_game_title,sizeof(s_sz_game_title),"%s",sz_title ? sz_title : "");
    snprintf(s_sz_game_version,sizeof(s_sz_game_version),"%s",sz_version ? sz_version : "");
}

int runtime_net_enabled(void) {
    static int s_is_enabled=-1;
    if (s_is_enabled<0) { const char *sz_value=getenv("PS_ONLINE"); s_is_enabled=sz_value && sz_value[0]=='1'; }
    return s_is_enabled;
}

/* Imports the module answers: network, HTTP/SSL and NP, except what stays in the runtime. */
int runtime_net_symbol(const char *sz_symbol) {
    static const char *const s_online[]={"sceNet","sceNp","sceHttp","sceSsl"};
    static const char *const s_local[]={"sceNpTrophy","sceNpCommerce","sceNpProfileDialog"};
    if (!sz_symbol) return 0;
    int match=0;
    for (size_t i=0;i<sizeof(s_online)/sizeof(*s_online);++i)
        if (!strncmp(sz_symbol,s_online[i],strlen(s_online[i]))) match=1;
    for (size_t i=0;i<sizeof(s_local)/sizeof(*s_local);++i)
        if (!strncmp(sz_symbol,s_local[i],strlen(s_local[i]))) match=0;
    return match;
}

/* The module: PS_NET_MODULE, else gpu/libpsport-net.so beside psport (where libpsport-gpu.so is). */
static PsNetResolveFn module_resolve(void) {
    static int s_has_tried;
    static PsNetResolveFn s_pfn_resolve;
    if (s_has_tried) return s_pfn_resolve;
    s_has_tried=1;
    char sz_path[PATH_MAX];
    const char *sz_forced=getenv("PS_NET_MODULE");
    if (sz_forced && sz_forced[0]) {
        snprintf(sz_path,sizeof(sz_path),"%s",sz_forced);
    } else {
        char sz_exe[PATH_MAX-32];
        const ssize_t cb_path=readlink("/proc/self/exe",sz_exe,sizeof(sz_exe)-1);
        if (cb_path<=0) return NULL;
        sz_exe[cb_path]=0;
        char *sz_slash=strrchr(sz_exe,'/');
        if (sz_slash) *sz_slash=0;
        snprintf(sz_path,sizeof(sz_path),"%s/gpu/libpsport-net.so",sz_exe);
    }
    /* RTLD_LOCAL: its C++ (network and NP libraries) stays out of the process's symbol scope. */
    void *p_module=dlopen(sz_path,RTLD_NOW|RTLD_LOCAL);
    if (!p_module) {
        printf("Online: the online module is not available (%s): offline play\n",dlerror());
        return NULL;
    }
    const PsNetInitFn pfn_init=(PsNetInitFn)dlsym(p_module,"psport_net_init");
    const PsNetResolveFn pfn_found=(PsNetResolveFn)dlsym(p_module,"psport_net_resolve");
    const PsTitleInfo title=psport_gpu_title(s_sz_game_serial);
    const PsNetHost host={
        .abi=PSPORT_NET_ABI_VERSION, .sz_app0=s_sz_game_folder, .sz_serial=s_sz_game_serial, .sz_title=s_sz_game_title,
        .sz_app_ver=s_sz_game_version, .pfn_thread_spawn=runtime_thread_spawn,
        .pfn_thread_join=runtime_thread_join_spawned, .sz_summon_route=title.online.sz_summon_route,
    };
    if (!pfn_init || !pfn_found || pfn_init(&host)!=0) {
        printf("Online: %s did not start: offline play\n",sz_path);
        return NULL;
    }
    printf("Online: module %s\n",sz_path);
    s_pfn_resolve=pfn_found;
    return s_pfn_resolve;
}

uintptr_t runtime_net_resolve(const char *sz_name) {
    if (!runtime_net_enabled()) return 0;
    const char *sz_symbol=runtime_symbol(sz_name);
    if (!runtime_net_symbol(sz_symbol)) return 0;
    const PsNetResolveFn pfn_resolve=module_resolve();
    const uintptr_t address=pfn_resolve ? pfn_resolve(sz_name) : 0;
    if (pfn_resolve && !address) printf("Online: %s has no online version; the offline one is used\n",sz_symbol);
    return address;
}
