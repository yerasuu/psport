/* psport online module (libpsport-net.so, gpu/net): the PS4 network and PSN libraries with the
 * shadNet client. A separate library the runtime loads only for online play (PS_ONLINE=1,
 * src/net/runtime_net.c); offline the port never touches it, and a build without it runs offline.
 * Its C++ inside is private (hidden symbols, -Bsymbolic): these two calls are all it exports. */
#ifndef PSPORT_NET_H
#define PSPORT_NET_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSPORT_NET_ABI_VERSION 2

/* What the runtime gives the module: the game's identity (param.sfo, its folder for
 * sce_sys/npbind.dat), guest threads for the NP libraries' callbacks into the game and what its
 * title declares for online play (gpu/games, PsTitleOnline). */
typedef struct PsNetHost {
    uint32_t abi;           /* PSPORT_NET_ABI_VERSION */
    const char *sz_app0;       /* the game's folder */
    const char *sz_serial;     /* TITLE_ID */
    const char *sz_title;      /* TITLE */
    const char *sz_app_ver;    /* APP_VER */
    /* 0 or a positive errno value; entry has the System V ABI (the game's code calls it). */
    int32_t (*pfn_thread_spawn)(void **pp_thread, void *(__attribute__((sysv_abi)) *pfn_entry)(void *),
                            void *p_argument, uint64_t cb_stack, const char *sz_name);
    int32_t (*pfn_thread_join)(void *pp_thread, void **pp_result);
    /* The path of the title's summon calls (no trailing slash), NULL when it declares none: the
     * summon helpers of http.cpp act only on it. Since ABI 2. */
    const char *sz_summon_route;
} PsNetHost;

/* Registers the libraries; 0 on success, else the module cannot run (the game stays offline). */
typedef int (*PsNetInitFn)(const PsNetHost *p_host);
int psport_net_init(const PsNetHost *p_host);

/* The module's function for an imported NID ("NID#lib#mod"), or 0. */
typedef uintptr_t (*PsNetResolveFn)(const char *sz_scoped_nid);
uintptr_t psport_net_resolve(const char *sz_scoped_nid);

#ifdef __cplusplus
}
#endif
#endif
