/* libSceNet sockets offline, as on a console with its cable unplugged: sockets, binds and traffic
 * over the loopback interface work (host sockets bound to 127.0.0.1), anything addressed beyond it
 * is unreachable. Game libraries that open their sockets at startup (Demonware's bdNet) need that
 * much to come up for single player. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define NET_ERR(e) ((int32_t)(UINT32_C(0x80410100)|(uint32_t)(e)))
#define GUEST_EBADF 9
#define GUEST_EINVAL 22
#define GUEST_EAFNOSUPPORT 47
#define GUEST_ENETUNREACH 51
#define GUEST_EPROTONOSUPPORT 43
#define SOCKETS 1024

typedef struct { uint8_t len, family; uint16_t port; uint32_t addr; uint16_t vport; uint8_t zero[6]; } GuestSockaddr;
_Static_assert(sizeof(GuestSockaddr)==16,"SceNetSockaddrIn layout");

static int32_t net_errno;
static _Atomic unsigned char ours[SOCKETS];

int32_t *runtime_net_errno(void) { return &net_errno; }
static ABI int32_t *net_errno_loc(void) { return &net_errno; }
static int32_t fail(int guest_errno) { net_errno=guest_errno; return NET_ERR(guest_errno); }
static int32_t fail_host(void) { return fail(runtime_guest_errno(errno)); }
static int owned(int32_t s) { return s>=0 && s<SOCKETS && atomic_load(&ours[s]); }

static int host_address(const GuestSockaddr *in, uint32_t len, struct sockaddr_in *out) {
    if (!in || len<8 || in->family!=AF_INET) return 0;
    *out=(struct sockaddr_in){.sin_family=AF_INET,.sin_port=in->port,.sin_addr.s_addr=in->addr};
    return 1;
}
static void guest_address(const struct sockaddr_in *in, GuestSockaddr *out, uint32_t *len) {
    if (!out || !len) return;
    GuestSockaddr g={.len=sizeof(g),.family=AF_INET,.port=in->sin_port,.addr=in->sin_addr.s_addr};
    memcpy(out,&g,*len<sizeof(g) ? *len : sizeof(g));
    *len=sizeof(g);
}
static int loopback(const struct sockaddr_in *a) { return (ntohl(a->sin_addr.s_addr)>>24)==127; }

static ABI int32_t net_socket(const char *name, int family, int type, int protocol) {
    (void)name;
    if (family!=AF_INET) return fail(GUEST_EAFNOSUPPORT);
    /* The P2P types (6 datagram, 10 stream) carry a virtual port the loopback path has no use for. */
    int host_type=type==1 || type==10 ? SOCK_STREAM : type==2 || type==6 ? SOCK_DGRAM : -1;
    if (host_type<0) return fail(GUEST_EPROTONOSUPPORT);
    int fd=socket(AF_INET,host_type|SOCK_CLOEXEC,protocol);
    if (fd<0) return fail_host();
    if (fd>=SOCKETS) { close(fd); return fail(24); }
    atomic_store(&ours[fd],1);
    return fd;
}
static ABI int32_t net_close(int32_t s) {
    if (!owned(s)) return fail(GUEST_EBADF);
    atomic_store(&ours[s],0);
    return close(s) ? fail_host() : 0;
}
static ABI int32_t net_abort(int32_t s, int flags) {
    (void)flags;
    if (!owned(s)) return fail(GUEST_EBADF);
    shutdown(s,SHUT_RDWR);
    return 0;
}
static ABI int32_t net_shutdown(int32_t s, int how) {
    if (!owned(s)) return fail(GUEST_EBADF);
    return shutdown(s,how) ? fail_host() : 0;
}
/* A wildcard bind lands on loopback, the only interface an unplugged console has. */
static ABI int32_t net_bind(int32_t s, const GuestSockaddr *addr, uint32_t len) {
    struct sockaddr_in a;
    if (!owned(s)) return fail(GUEST_EBADF);
    if (!host_address(addr,len,&a)) return fail(GUEST_EINVAL);
    if (a.sin_addr.s_addr==htonl(INADDR_ANY)) a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    else if (!loopback(&a)) return fail(49); /* EADDRNOTAVAIL */
    return bind(s,(struct sockaddr *)&a,sizeof(a)) ? fail_host() : 0;
}
static ABI int32_t net_connect(int32_t s, const GuestSockaddr *addr, uint32_t len) {
    struct sockaddr_in a;
    if (!owned(s)) return fail(GUEST_EBADF);
    if (!host_address(addr,len,&a)) return fail(GUEST_EINVAL);
    if (!loopback(&a)) return fail(GUEST_ENETUNREACH);
    return connect(s,(struct sockaddr *)&a,sizeof(a)) ? fail_host() : 0;
}
static ABI int32_t net_listen(int32_t s, int backlog) {
    if (!owned(s)) return fail(GUEST_EBADF);
    return listen(s,backlog) ? fail_host() : 0;
}
static ABI int32_t net_accept(int32_t s, GuestSockaddr *addr, uint32_t *len) {
    struct sockaddr_in a;
    socklen_t size=sizeof(a);
    if (!owned(s)) return fail(GUEST_EBADF);
    int fd=accept4(s,(struct sockaddr *)&a,&size,SOCK_CLOEXEC);
    if (fd<0) return fail_host();
    if (fd>=SOCKETS) { close(fd); return fail(24); }
    atomic_store(&ours[fd],1);
    guest_address(&a,addr,len);
    return fd;
}
static ABI int32_t net_getsockname(int32_t s, GuestSockaddr *addr, uint32_t *len) {
    struct sockaddr_in a;
    socklen_t size=sizeof(a);
    if (!owned(s)) return fail(GUEST_EBADF);
    if (getsockname(s,(struct sockaddr *)&a,&size)) return fail_host();
    guest_address(&a,addr,len);
    return 0;
}
/* The console's BSD MSG_WAITALL (0x40) is Linux's MSG_DONTWAIT, so each flag is translated. */
static int host_flags(int flags) {
    return (flags&0x1 ? MSG_OOB : 0)|(flags&0x2 ? MSG_PEEK : 0)|(flags&0x40 ? MSG_WAITALL : 0)|(flags&0x80 ? MSG_DONTWAIT : 0);
}
static ABI int64_t net_recvfrom(int32_t s, void *buf, uint64_t len, int flags, GuestSockaddr *from, uint32_t *fromlen) {
    struct sockaddr_in a={0};
    socklen_t size=sizeof(a);
    if (!owned(s)) return fail(GUEST_EBADF);
    ssize_t n=recvfrom(s,buf,len,host_flags(flags)|MSG_NOSIGNAL,(struct sockaddr *)&a,&size);
    if (n<0) return fail_host();
    if (from && size) guest_address(&a,from,fromlen);
    return n;
}
static ABI int64_t net_recv(int32_t s, void *buf, uint64_t len, int flags) { return net_recvfrom(s,buf,len,flags,NULL,NULL); }
static ABI int64_t net_sendto(int32_t s, const void *buf, uint64_t len, int flags, const GuestSockaddr *to, uint32_t tolen) {
    struct sockaddr_in a;
    if (!owned(s)) return fail(GUEST_EBADF);
    if (to && !host_address(to,tolen,&a)) return fail(GUEST_EINVAL);
    if (to && !loopback(&a)) return fail(GUEST_ENETUNREACH);
    ssize_t n=sendto(s,buf,len,host_flags(flags)|MSG_NOSIGNAL,to ? (struct sockaddr *)&a : NULL,to ? sizeof(a) : 0);
    return n<0 ? fail_host() : n;
}
static ABI int64_t net_send(int32_t s, const void *buf, uint64_t len, int flags) { return net_sendto(s,buf,len,flags,NULL,0); }
/* The console's option numbers (BSD, plus SO_NBIO) for the host's; 0 when there is none. */
static int host_option(int level, int option, int *host_level) {
    *host_level=level==0xffff ? SOL_SOCKET : level;
    if (level==0xffff) switch (option) {
    case 0x4: return SO_REUSEADDR;
    case 0x8: return SO_KEEPALIVE;
    case 0x20: return SO_BROADCAST;
    case 0x80: return SO_LINGER;
    case 0x200: return SO_REUSEPORT;
    case 0x1001: return SO_SNDBUF;
    case 0x1002: return SO_RCVBUF;
    case 0x1007: return SO_ERROR;
    case 0x1008: return SO_TYPE;
    default: return 0;
    }
    if (level==IPPROTO_TCP && option==1) return TCP_NODELAY;
    if (level==IPPROTO_IP) switch (option) {
    case 3: return IP_TOS;
    case 4: return IP_TTL;
    default: return 0;
    }
    return 0;
}
static ABI int32_t net_setsockopt(int32_t s, int level, int option, const void *value, uint32_t len) {
    if (!owned(s)) return fail(GUEST_EBADF);
    if (level==0xffff && option==0x1200) { /* SO_NBIO */
        if (!value || len<4) return fail(GUEST_EINVAL);
        int nbio;
        memcpy(&nbio,value,sizeof(nbio));
        int f=fcntl(s,F_GETFL);
        return f<0 || fcntl(s,F_SETFL,nbio ? f|O_NONBLOCK : f&~O_NONBLOCK) ? fail_host() : 0;
    }
    int host_level, host=host_option(level,option,&host_level);
    if (!host) {
        fprintf(stderr,"Runtime: socket option %#x/%#x accepted without effect\n",level,option);
        return 0;
    }
    return setsockopt(s,host_level,host,value,len) ? fail_host() : 0;
}
static ABI int32_t net_getsockopt(int32_t s, int level, int option, void *value, uint32_t *len) {
    if (!owned(s)) return fail(GUEST_EBADF);
    int host_level, host=host_option(level,option,&host_level);
    if (!host) return fail(42); /* ENOPROTOOPT */
    socklen_t size=len ? *len : 0;
    if (getsockopt(s,host_level,host,value,&size)) return fail_host();
    if (host==SO_ERROR && host_level==SOL_SOCKET && value && size>=sizeof(int32_t)) {
        int32_t e;
        memcpy(&e,value,sizeof(e));
        e=runtime_guest_errno(e);
        memcpy(value,&e,sizeof(e));
    }
    if (len) *len=size;
    return 0;
}

static const RuntimeExport exports[]={
    {"sceNetErrnoLoc",net_errno_loc}, {"sceNetSocket",net_socket}, {"sceNetSocketClose",net_close},
    {"sceNetSocketAbort",net_abort}, {"sceNetShutdown",net_shutdown}, {"sceNetBind",net_bind},
    {"sceNetConnect",net_connect}, {"sceNetListen",net_listen}, {"sceNetAccept",net_accept}, {"sceNetGetsockname",net_getsockname}, {"sceNetRecvfrom",net_recvfrom},
    {"sceNetRecv",net_recv}, {"sceNetSendto",net_sendto}, {"sceNetSend",net_send},
    {"sceNetSetsockopt",net_setsockopt}, {"sceNetGetsockopt",net_getsockopt},
};
uintptr_t runtime_socket_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
#else
int32_t *runtime_net_errno(void) { static int32_t e; return &e; }
uintptr_t runtime_socket_resolve(const char *name) { (void)name; return 0; }
#endif
