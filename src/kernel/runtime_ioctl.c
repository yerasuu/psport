// SPDX-License-Identifier: GPL-2.0-or-later
/* _ioctl on guest descriptors: sockets answer their requests in runtime_socket.c and the kernel's
 * virtual devices (runtime_file.c) here; anything else is not a device that takes requests
 * (ENOTTY), counted so that a request the runtime should serve shows up. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#define GUEST_EBADF 9
#define GUEST_EFAULT 14
#define GUEST_ENOTTY 25
#define GUEST_ENOTSOCK 38

/* /dev/dipsw, the development switches libSceSysmodule reads at start: index 6 (_IOR(0x88, 6, int))
 * is 1 on a devkit, and only then are 7, 8 and 9 read. A retail console has every switch off. */
#define DIPSW_IS_DEVKIT UINT32_C(0x40048806)
#define DIPSW_7 UINT32_C(0x40048807)
#define DIPSW_8 UINT32_C(0x40088808)
#define DIPSW_9 UINT32_C(0x40088809)
static int64_t dipsw_ioctl(uint32_t command, void *p_out_value) {
    if (command!=DIPSW_IS_DEVKIT && command!=DIPSW_7 && command!=DIPSW_8 && command!=DIPSW_9) return -GUEST_ENOTTY;
    const uint32_t cb_value=(command>>16)&0x1fff;
    if (!runtime_memory_is_guest_range(p_out_value,cb_value)) return -GUEST_EFAULT;
    memset(p_out_value,0,cb_value);
    return 0;
}

static uint64_t s_c_refused;
static ABI int32_t guest_ioctl(int32_t h_fd, uint64_t command, void *p_argument) {
    char sz_path[64];
    int64_t result;
    if (!runtime_file_guest_path(h_fd,sz_path,sizeof(sz_path)) && !strcmp(sz_path,"/dev/dipsw"))
        result=dipsw_ioctl((uint32_t)command,p_argument);
    else
        result=runtime_file_host(h_fd)<0 ? -GUEST_EBADF : runtime_socket_ioctl(h_fd,(uint32_t)command,p_argument);
    if (result==-GUEST_ENOTSOCK) result=-GUEST_ENOTTY;
    if (result==-GUEST_ENOTTY) {
        const uint64_t c_before=__atomic_fetch_add(&s_c_refused,1,__ATOMIC_RELAXED);
        if (c_before<8) printf("Runtime: _ioctl(fd %d, %#llx) not served: ENOTTY\n",h_fd,(unsigned long long)command);
        if (!c_before) runtime_calls_hle_stub("_ioctl request the runtime does not serve (ENOTTY)",&s_c_refused);
    }
    if (result>=0) return 0;
    *runtime_errno()=(int32_t)-result;
    return -1;
}

static const RuntimeExport s_exports[]={
    {"_ioctl",guest_ioctl},
};
uintptr_t runtime_ioctl_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
