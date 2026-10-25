/* Online play (PS_ONLINE=1): the game's network and PSN imports go to the online module
 * (gpu/net: libpsport-net.so beside libpsport-gpu.so), loaded here on the first such import. Offline,
 * or when the module is missing or fails to start, they stay with the runtime's offline
 * versions (runtime_services.c). Trophies, voice and the store/profile dialogs always stay here. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/net/psport_net.h"
#include "gpu/psport_gpu.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char s_sz_game_folder[PATH_MAX], s_sz_game_serial[16], s_sz_game_title[128], s_sz_game_version[16];

void runtime_net_configure(const char *sz_app0, const char *sz_serial, const char *sz_title, const char *sz_version) {
    snprintf(s_sz_game_folder,sizeof(s_sz_game_folder),"%s",sz_app0 ? sz_app0 : "");
    snprintf(s_sz_game_serial,sizeof(s_sz_game_serial),"%s",sz_serial ? sz_serial : "");
    snprintf(s_sz_game_title,sizeof(s_sz_game_title),"%s",sz_title ? sz_title : "");
    snprintf(s_sz_game_version,sizeof(s_sz_game_version),"%s",sz_version ? sz_version : "");
}

int runtime_net_enabled(void) {
    static int s_is_enabled=-1;
    if (s_is_enabled<0) { const char *sz_value=runtime_env("PS_ONLINE",NULL); s_is_enabled=sz_value && sz_value[0]=='1'; }
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

static PsHostLibrary *s_p_module;

/* The title's extension (Game::OnlineExtension), when the player turned it on with its switch. */
static const char *title_extension(const PsTitleOnlineExtension *p_extension) {
    if (!p_extension->sz_name) return NULL;
    const char *sz_value=p_extension->sz_switch ? runtime_env(p_extension->sz_switch,NULL) : NULL;
    if (sz_value && sz_value[0]=='1') {
        printf("Online: the title's extension %s is on (%s=1)\n",p_extension->sz_name,p_extension->sz_switch);
        return p_extension->sz_name;
    }
    printf("Online: the title's extension %s is off (%s=1 turns it on)\n",p_extension->sz_name,
           p_extension->sz_switch ? p_extension->sz_switch : "its switch");
    return NULL;
}

void runtime_net_image_ready(uintptr_t va_image, uint64_t cb_image) {
    if (!s_p_module) return;
    const PsNetImageReadyFn pfn_ready=(PsNetImageReadyFn)ps_host_dl_sym(s_p_module,"psport_net_image_ready");
    if (pfn_ready) pfn_ready((uint64_t)va_image,cb_image);
}

/* The module: PS_NET_MODULE, else gpu/libpsport-net.so beside psport (where libpsport-gpu.so is). */
static int find_module(char *sz_out_path, size_t cb_path) {
    const char *sz_forced=runtime_env("PS_NET_MODULE",NULL);
    if (sz_forced && sz_forced[0]) return snprintf(sz_out_path,cb_path,"%s",sz_forced)>0;
    char sz_exe[PATH_MAX-32];
    if (ps_host_exe_path(sz_exe,sizeof(sz_exe))<0) return 0;
    char *sz_slash=strrchr(sz_exe,'/');
    if (sz_slash) *sz_slash=0;
    return snprintf(sz_out_path,cb_path,"%s/gpu/libpsport-net.so",sz_exe)>0;
}

/* Without loading the module, which would start its TLS library: a build may have none (the public
 * binary cache and bundle leave it out until kb #850), and a launcher then hides online play. */
void runtime_net_probe_module(void) {
    static char s_sz_result[PATH_MAX+64];
    char sz_path[PATH_MAX];
    if (!find_module(sz_path,sizeof(sz_path))) {
        ps_host_feature_probed(PS_HOST_FEATURE_online_module,0,"the module's path could not be found");
        return;
    }
    const int is_present=!ps_host_fs_access(sz_path,PS_HOST_ACCESS_READ);
    snprintf(s_sz_result,sizeof(s_sz_result),is_present ? "%s" : "no %s in this build: offline play only",sz_path);
    ps_host_feature_probed(PS_HOST_FEATURE_online_module,is_present,s_sz_result);
}

static PsNetResolveFn module_resolve(void) {
    static int s_has_tried;
    static PsNetResolveFn s_pfn_resolve;
    if (s_has_tried) return s_pfn_resolve;
    s_has_tried=1;
    char sz_path[PATH_MAX];
    if (!find_module(sz_path,sizeof(sz_path))) return NULL;
    /* RTLD_LOCAL: its C++ (network and NP libraries) stays out of the process's symbol scope. */
    char sz_error[512];
    PsHostLibrary *p_module=ps_host_dl_open(sz_path,sz_error,sizeof(sz_error));
    if (!p_module) {
        printf("Online: the online module is not available (%s): offline play\n",sz_error);
        return NULL;
    }
    const PsNetInitFn pfn_init=(PsNetInitFn)ps_host_dl_sym(p_module,"psport_net_init");
    const PsNetResolveFn pfn_found=(PsNetResolveFn)ps_host_dl_sym(p_module,"psport_net_resolve");
    const PsTitleInfo title=psport_gpu_title(s_sz_game_serial);
    const PsNetHost host={
        .abi=PSPORT_NET_ABI_VERSION, .sz_app0=s_sz_game_folder, .sz_serial=s_sz_game_serial, .sz_title=s_sz_game_title,
        .sz_app_ver=s_sz_game_version, .pfn_thread_spawn=runtime_thread_spawn,
        .pfn_thread_join=runtime_thread_join_spawned, .sz_summon_route=title.online.sz_summon_route,
        .sz_extension=title_extension(&title.online.extension),
    };
    if (!pfn_init || !pfn_found || pfn_init(&host)!=0) {
        printf("Online: %s did not start: offline play\n",sz_path);
        return NULL;
    }
    printf("Online: module %s\n",sz_path);
    s_p_module=p_module;
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
