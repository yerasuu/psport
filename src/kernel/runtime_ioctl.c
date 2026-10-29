// SPDX-License-Identifier: GPL-2.0-or-later
/* _ioctl on guest descriptors: sockets answer their requests in runtime_socket.c; anything else
 * is not a device that takes requests (ENOTTY), counted so that a request the runtime should serve
 * shows up. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#define GUEST_EBADF 9
#define GUEST_ENOTTY 25
#define GUEST_ENOTSOCK 38

static uint64_t s_c_refused;
static ABI int32_t guest_ioctl(int32_t h_fd, uint64_t command, void *p_argument) {
    int64_t result=runtime_file_host(h_fd)<0 ? -GUEST_EBADF : runtime_socket_ioctl(h_fd,(uint32_t)command,p_argument);
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
