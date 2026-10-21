/* libSceNet and POSIX sockets offline, as on a console with its cable unplugged: sockets, binds and traffic
 * over the loopback interface work (host sockets bound to 127.0.0.1), anything addressed beyond it
 * is unreachable. Game libraries that open their sockets at startup (Demonware's bdNet) need that
 * much to come up for single player. */
#define _GNU_SOURCE
#include "runtime.h"
#include "ps_host_sockets.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define NET_ERR(e) ((int32_t)(UINT32_C(0x80410100)|(uint32_t)(e)))
#define GUEST_EBADF 9
#define GUEST_EFAULT 14
#define GUEST_EMFILE 24
#define GUEST_EINVAL 22
#define GUEST_ENOSPC 28
#define GUEST_EAFNOSUPPORT 47
#define GUEST_EADDRNOTAVAIL 49
#define GUEST_ENETUNREACH 51
#define GUEST_ENOPROTOOPT 42
#define GUEST_EPROTONOSUPPORT 43
#define GUEST_EOPNOTSUPP 45
#define GUEST_AF_INET6 28
#define GUEST_SOCK_CLOEXEC 0x10000000
#define GUEST_SOCK_NONBLOCK 0x20000000
#define GUEST_FD_SETSIZE 1024

typedef struct { uint8_t len, family; uint16_t port; uint32_t addr; uint16_t vport; uint8_t zero[6]; } GuestSockaddr;
_Static_assert(sizeof(GuestSockaddr)==16,"SceNetSockaddrIn layout");
typedef struct {
    GuestSockaddr *p_name;
    uint32_t cb_name;
    PsHostIoVector *p_iov;
    int32_t c_iov;
    void *p_control;
    uint32_t cb_control;
    int32_t flags;
} GuestMsghdr;
_Static_assert(sizeof(GuestMsghdr)==48,"FreeBSD msghdr layout");
typedef struct { int64_t sec, usec; } GuestTimeval;

static int32_t s_net_errno;

int32_t *runtime_net_errno(void) { return &s_net_errno; }
static ABI int32_t *net_errno_loc(void) { return &s_net_errno; }
static int32_t fail(int guest_errno) { s_net_errno=guest_errno; return NET_ERR(guest_errno); }
/* Every operation below returns >=0 or -(FreeBSD errno); libSceNet turns that into its codes and
 * sets sceNetErrnoLoc's value, the POSIX exports into -1 and errno. */
static int64_t sce(int64_t result) { return result<0 ? fail((int)-result) : result; }
static int64_t posix(int64_t result) {
    if (result>=0) return result;
    *runtime_errno()=(int32_t)-result;
    return -1;
}
static int64_t host_failure(void) { return -runtime_guest_errno(ps_host_net_error()); }
/* Sockets live in the runtime's file table (runtime_file.c), as on the console where a socket is a
 * kernel descriptor like a file; 0-2 are the host's stdio, never a socket. */
static int host_socket(int32_t h_socket) { return h_socket<3 ? -1 : runtime_file_host(h_socket); }
static int64_t adopt(int h_host) {
    if (h_host<0) return host_failure();
    int h_socket=runtime_file_adopt(h_host);
    if (h_socket<0) {
        ps_host_net_close(h_host);
        return -GUEST_EMFILE;
    }
    return h_socket;
}

static int host_address(const GuestSockaddr *p_sockaddr, uint32_t len, struct sockaddr_in *p_out) {
    if (!p_sockaddr || len<8 || p_sockaddr->family!=AF_INET) return 0;
    *p_out=(struct sockaddr_in){.sin_family=AF_INET,.sin_port=p_sockaddr->port,.sin_addr.s_addr=p_sockaddr->addr};
    return 1;
}
static void guest_address(const struct sockaddr_in *p_address, GuestSockaddr *p_out, uint32_t *p_len) {
    if (!p_out || !p_len) return;
    GuestSockaddr g={.len=sizeof(g),.family=AF_INET,.port=p_address->sin_port,.addr=p_address->sin_addr.s_addr};
    memcpy(p_out,&g,*p_len<sizeof(g) ? *p_len : sizeof(g));
    *p_len=sizeof(g);
}
static int loopback(const struct sockaddr_in *p_address) { return (ntohl(p_address->sin_addr.s_addr)>>24)==127; }

static int64_t do_socket(int family, int type, int protocol) {
    if (family!=AF_INET) return -GUEST_EAFNOSUPPORT;
    const int is_nonblocking=(type&GUEST_SOCK_NONBLOCK)!=0;
    type&=~(GUEST_SOCK_CLOEXEC|GUEST_SOCK_NONBLOCK);
    /* The P2P types (6 datagram, 10 stream) carry a virtual port the loopback path has no use for. */
    int host_type=type==1 || type==10 ? SOCK_STREAM : type==2 || type==6 ? SOCK_DGRAM : -1;
    if (host_type<0) return -GUEST_EPROTONOSUPPORT;
    int e=ps_host_net_init();
    if (e) return -runtime_guest_errno(e);
    int h_host=ps_host_net_socket(AF_INET,host_type,protocol,is_nonblocking);
    return h_host<0 ? -runtime_guest_errno(-h_host) : adopt(h_host);
}
static int64_t do_close(int32_t s) {
    if (host_socket(s)<0) return -GUEST_EBADF;
    int64_t result=runtime_file_close(s);
    return result<0 ? -runtime_guest_errno((int)-result) : 0;
}
static int64_t do_shutdown(int32_t s, int how) {
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    return shutdown(h,how) ? host_failure() : 0;
}
/* A wildcard bind lands on loopback, the only interface an unplugged console has. */
static int64_t do_bind(int32_t s, const GuestSockaddr *p_addr, uint32_t len) {
    struct sockaddr_in a;
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    if (!host_address(p_addr,len,&a)) return -GUEST_EINVAL;
    if (a.sin_addr.s_addr==htonl(INADDR_ANY)) a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    else if (!loopback(&a)) return -GUEST_EADDRNOTAVAIL;
    return bind(h,(struct sockaddr *)&a,sizeof(a)) ? host_failure() : 0;
}
static int64_t do_connect(int32_t s, const GuestSockaddr *p_addr, uint32_t len) {
    struct sockaddr_in a;
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    if (!host_address(p_addr,len,&a)) return -GUEST_EINVAL;
    if (!loopback(&a)) return -GUEST_ENETUNREACH;
    int e=ps_host_net_connect(h,&a,sizeof(a));
    return e ? -runtime_guest_errno(e) : 0;
}
static int64_t do_listen(int32_t s, int backlog) {
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    return listen(h,backlog) ? host_failure() : 0;
}
static int64_t do_accept(int32_t s, GuestSockaddr *p_addr, uint32_t *p_len) {
    struct sockaddr_in a;
    uint32_t cb_address=sizeof(a);
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    int h_accepted=ps_host_net_accept(h,&a,&cb_address);
    if (h_accepted<0) return -runtime_guest_errno(-h_accepted);
    int64_t accepted=adopt(h_accepted);
    if (accepted>=0) guest_address(&a,p_addr,p_len);
    return accepted;
}
static int64_t do_getsockname(int32_t s, GuestSockaddr *p_addr, uint32_t *p_len) {
    struct sockaddr_in a;
    socklen_t cb_address=sizeof(a);
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    if (getsockname(h,(struct sockaddr *)&a,&cb_address)) return host_failure();
    guest_address(&a,p_addr,p_len);
    return 0;
}
static int64_t do_getpeername(int32_t h_socket, GuestSockaddr *p_out_address, uint32_t *p_cb_address) {
    struct sockaddr_in peer;
    socklen_t cb_peer=sizeof(peer);
    int h_host=host_socket(h_socket);
    if (h_host<0) return -GUEST_EBADF;
    if (getpeername(h_host,(struct sockaddr *)&peer,&cb_peer)) return host_failure();
    guest_address(&peer,p_out_address,p_cb_address);
    return 0;
}
/* The console's BSD MSG_WAITALL (0x40) is Linux's MSG_DONTWAIT, so each flag is translated. */
static unsigned host_flags(int flags) {
    return (flags&0x1 ? PS_HOST_NET_OUT_OF_BAND : 0u)|(flags&0x2 ? PS_HOST_NET_PEEK : 0u)|
           (flags&0x40 ? PS_HOST_NET_WAIT_ALL : 0u)|(flags&0x80 ? PS_HOST_NET_DONT_WAIT : 0u);
}
static int64_t host_result(int64_t result) { return result<0 ? -runtime_guest_errno((int)-result) : result; }
static int64_t do_recvfrom(int32_t s, void *p_buf, uint64_t len, int flags, GuestSockaddr *p_from, uint32_t *p_fromlen) {
    struct sockaddr_in a={0};
    uint32_t cb_address=sizeof(a);
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    const PsHostIoVector buffer={p_buf,len};
    int64_t cb_received=ps_host_net_receive(h,&buffer,1,&a,&cb_address,host_flags(flags),NULL);
    if (cb_received<0) return host_result(cb_received);
    if (p_from && cb_address) guest_address(&a,p_from,p_fromlen);
    return cb_received;
}
static int64_t do_sendto(int32_t s, const void *p_buf, uint64_t len, int flags, const GuestSockaddr *p_sockaddr, uint32_t tolen) {
    struct sockaddr_in a;
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    if (p_sockaddr && !host_address(p_sockaddr,tolen,&a)) return -GUEST_EINVAL;
    if (p_sockaddr && !loopback(&a)) return -GUEST_ENETUNREACH;
    const PsHostIoVector buffer={(void *)p_buf,len};
    return host_result(ps_host_net_send(h,&buffer,1,p_sockaddr ? &a : NULL,p_sockaddr ? sizeof(a) : 0,host_flags(flags)));
}
/* Control messages (descriptor passing, credentials) have no use between the game's own loopback
 * sockets and FreeBSD lays them out differently, so a message carrying one is refused. */
static int64_t do_sendmsg(int32_t h_socket, const GuestMsghdr *p_message, int flags) {
    struct sockaddr_in to;
    int h_host=host_socket(h_socket);
    if (h_host<0) return -GUEST_EBADF;
    if (!p_message || p_message->c_iov<0) return -GUEST_EINVAL;
    if (p_message->p_control && p_message->cb_control) {
        fprintf(stderr,"Runtime: sendmsg with %u bytes of control data refused\n",p_message->cb_control);
        return -GUEST_EOPNOTSUPP;
    }
    if (p_message->p_name && !host_address(p_message->p_name,p_message->cb_name,&to)) return -GUEST_EINVAL;
    if (p_message->p_name && !loopback(&to)) return -GUEST_ENETUNREACH;
    return host_result(ps_host_net_send(h_host,p_message->p_iov,p_message->c_iov,p_message->p_name ? &to : NULL,
                                        p_message->p_name ? sizeof(to) : 0,host_flags(flags)));
}
static int64_t do_recvmsg(int32_t h_socket, GuestMsghdr *p_message, int flags) {
    struct sockaddr_in from={0};
    int h_host=host_socket(h_socket);
    if (h_host<0) return -GUEST_EBADF;
    if (!p_message || p_message->c_iov<0) return -GUEST_EINVAL;
    uint32_t cb_from=sizeof(from);
    unsigned received_flags=0;
    int64_t cb_received=ps_host_net_receive(h_host,p_message->p_iov,p_message->c_iov,&from,&cb_from,host_flags(flags),
                                            &received_flags);
    if (cb_received<0) return host_result(cb_received);
    if (p_message->p_name && cb_from) guest_address(&from,p_message->p_name,&p_message->cb_name);
    else p_message->cb_name=0;
    p_message->cb_control=0;
    p_message->flags=(received_flags&PS_HOST_NET_TRUNCATED ? 0x10 : 0)|(received_flags&PS_HOST_NET_CONTROL_TRUNCATED ? 0x20 : 0)|
                     (received_flags&PS_HOST_NET_GOT_OUT_OF_BAND ? 0x1 : 0);
    return cb_received;
}
/* The console's option numbers (BSD, plus SO_NBIO) for the host's; 0 when there is none. */
static int host_option(int level, int option, int *p_host_level) {
    *p_host_level=level==0xffff ? SOL_SOCKET : level;
    if (level==0xffff) switch (option) {
    case 0x4: return SO_REUSEADDR;
    case 0x8: return SO_KEEPALIVE;
    case 0x20: return SO_BROADCAST;
    case 0x80: return SO_LINGER;
    case 0x200: return SO_REUSEPORT;
    case 0x1001: return SO_SNDBUF;
    case 0x1002: return SO_RCVBUF;
    case 0x1005: return SO_SNDTIMEO;
    case 0x1006: return SO_RCVTIMEO;
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
static int64_t do_setsockopt(int32_t s, int level, int option, const void *p_value, uint32_t len) {
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    if (level==0xffff && option==0x1200) { /* SO_NBIO */
        if (!p_value || len<4) return -GUEST_EINVAL;
        int nbio;
        memcpy(&nbio,p_value,sizeof(nbio));
        int e=ps_host_net_set_nonblocking(h,nbio);
        return e ? -runtime_guest_errno(e) : 0;
    }
    int host_level, host=host_option(level,option,&host_level);
    if (!host) {
        fprintf(stderr,"Runtime: socket option %#x/%#x accepted without effect\n",level,option);
        return 0;
    }
    return setsockopt(h,host_level,host,p_value,len) ? host_failure() : 0;
}
static int64_t do_getsockopt(int32_t s, int level, int option, void *p_value, uint32_t *p_len) {
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    int host_level, host=host_option(level,option,&host_level);
    if (!host) return -GUEST_ENOPROTOOPT;
    socklen_t cb_value=p_len ? *p_len : 0;
    if (getsockopt(h,host_level,host,p_value,&cb_value)) return host_failure();
    if (host==SO_ERROR && host_level==SOL_SOCKET && p_value && cb_value>=(socklen_t)sizeof(int32_t)) {
        int32_t e;
        memcpy(&e,p_value,sizeof(e));
        e=runtime_guest_errno(e);
        memcpy(p_value,&e,sizeof(e));
    }
    if (p_len) *p_len=cb_value;
    return 0;
}
/* Over poll, so a host descriptor past the host's FD_SETSIZE still works; FreeBSD's fd_set is the
 * same bit array of longs as Linux's. A descriptor that hung up or failed counts as readable. */
static int64_t do_select(int c_fds, uint64_t *p_read, uint64_t *p_write, uint64_t *p_except, const GuestTimeval *p_timeout) {
    if (c_fds<0 || c_fds>GUEST_FD_SETSIZE) return -GUEST_EINVAL;
    if (p_timeout && (p_timeout->sec<0 || p_timeout->usec<0 || p_timeout->usec>=1000000)) return -GUEST_EINVAL;
    PsHostPollEntry polled[c_fds ? c_fds : 1];
    int h_guest[c_fds ? c_fds : 1], c_polled=0;
    for (int h_fd=0;h_fd<c_fds;++h_fd) {
        const uint64_t bit=UINT64_C(1)<<(h_fd&63);
        const int i_word=h_fd>>6;
        unsigned events=(p_read && p_read[i_word]&bit ? PS_HOST_NET_POLL_IN : 0u)|(p_write && p_write[i_word]&bit ? PS_HOST_NET_POLL_OUT : 0u)|
                        (p_except && p_except[i_word]&bit ? PS_HOST_NET_POLL_PRIORITY : 0u);
        if (!events) continue;
        int h_host=runtime_file_host(h_fd);
        if (h_host<0) return -GUEST_EBADF;
        polled[c_polled]=(PsHostPollEntry){.h_socket=h_host,.events=events};
        h_guest[c_polled++]=h_fd;
    }
    const int64_t timeout_ns=p_timeout ? p_timeout->sec*1000000000+p_timeout->usec*1000 : -1;
    int c_ready=ps_host_net_poll(polled,c_polled,timeout_ns);
    if (c_ready<0) return -runtime_guest_errno(-c_ready);
    const size_t cb_set=(size_t)(c_fds+63)/64*sizeof(uint64_t);
    if (p_read) memset(p_read,0,cb_set);
    if (p_write) memset(p_write,0,cb_set);
    if (p_except) memset(p_except,0,cb_set);
    int64_t c_bits=0;
    for (int i=0;i<c_polled;++i) {
        const unsigned events=polled[i].events, revents=polled[i].revents;
        const uint64_t bit=UINT64_C(1)<<(h_guest[i]&63);
        const int i_word=h_guest[i]>>6;
        if (revents&PS_HOST_NET_POLL_INVALID) return -GUEST_EBADF;
        if (events&PS_HOST_NET_POLL_IN && revents&(PS_HOST_NET_POLL_IN|PS_HOST_NET_POLL_HANG_UP|PS_HOST_NET_POLL_ERROR)) { p_read[i_word]|=bit; ++c_bits; }
        if (events&PS_HOST_NET_POLL_OUT && revents&(PS_HOST_NET_POLL_OUT|PS_HOST_NET_POLL_ERROR)) { p_write[i_word]|=bit; ++c_bits; }
        if (events&PS_HOST_NET_POLL_PRIORITY && revents&PS_HOST_NET_POLL_PRIORITY) { p_except[i_word]|=bit; ++c_bits; }
    }
    return c_bits;
}
/* Only local pairs exist (AF_LOCAL is 1 on both systems), stream, datagram or seqpacket. */
static int64_t do_socketpair(int family, int type, int protocol, int32_t *p_out_sockets) {
    if (!p_out_sockets) return -GUEST_EFAULT;
    if (family!=AF_UNIX) return -GUEST_EAFNOSUPPORT;
    const int is_nonblocking=(type&GUEST_SOCK_NONBLOCK)!=0;
    type&=~(GUEST_SOCK_CLOEXEC|GUEST_SOCK_NONBLOCK);
    if (type!=SOCK_STREAM && type!=SOCK_DGRAM && type!=SOCK_SEQPACKET) return -GUEST_EPROTONOSUPPORT;
    int h_hosts[2];
    const int e=ps_host_net_socketpair(type,protocol,is_nonblocking,h_hosts);
    if (e) return -runtime_guest_errno(e);
    const int64_t h_first=adopt(h_hosts[0]);
    if (h_first<0) { ps_host_net_close(h_hosts[1]); return h_first; }
    const int64_t h_second=adopt(h_hosts[1]);
    if (h_second<0) { runtime_file_close((int)h_first); return h_second; }
    p_out_sockets[0]=(int32_t)h_first; p_out_sockets[1]=(int32_t)h_second;
    return 0;
}
static int host_family(int family) { return family==AF_INET ? AF_INET : family==GUEST_AF_INET6 ? AF_INET6 : -1; }

static ABI int32_t net_socket(const char *sz_name, int family, int type, int protocol) {
    (void)sz_name;
    return (int32_t)sce(do_socket(family,type,protocol));
}
static ABI int32_t net_close(int32_t s) { return (int32_t)sce(do_close(s)); }
static ABI int32_t net_abort(int32_t s, int flags) {
    (void)flags;
    int h=host_socket(s);
    if (h<0) return fail(GUEST_EBADF);
    shutdown(h,SHUT_RDWR);
    return 0;
}
static ABI int32_t net_shutdown(int32_t s, int how) { return (int32_t)sce(do_shutdown(s,how)); }
static ABI int32_t net_bind(int32_t s, const GuestSockaddr *p_addr, uint32_t len) { return (int32_t)sce(do_bind(s,p_addr,len)); }
static ABI int32_t net_connect(int32_t s, const GuestSockaddr *p_addr, uint32_t len) { return (int32_t)sce(do_connect(s,p_addr,len)); }
static ABI int32_t net_listen(int32_t s, int backlog) { return (int32_t)sce(do_listen(s,backlog)); }
static ABI int32_t net_accept(int32_t s, GuestSockaddr *p_addr, uint32_t *p_len) { return (int32_t)sce(do_accept(s,p_addr,p_len)); }
static ABI int32_t net_getsockname(int32_t s, GuestSockaddr *p_addr, uint32_t *p_len) { return (int32_t)sce(do_getsockname(s,p_addr,p_len)); }
static ABI int64_t net_recvfrom(int32_t s, void *p_buf, uint64_t len, int flags, GuestSockaddr *p_from, uint32_t *p_fromlen) {
    return sce(do_recvfrom(s,p_buf,len,flags,p_from,p_fromlen));
}
static ABI int64_t net_recv(int32_t s, void *p_buf, uint64_t len, int flags) { return sce(do_recvfrom(s,p_buf,len,flags,NULL,NULL)); }
static ABI int64_t net_sendto(int32_t s, const void *p_buf, uint64_t len, int flags, const GuestSockaddr *p_sockaddr, uint32_t tolen) {
    return sce(do_sendto(s,p_buf,len,flags,p_sockaddr,tolen));
}
static ABI int64_t net_send(int32_t s, const void *p_buf, uint64_t len, int flags) { return sce(do_sendto(s,p_buf,len,flags,NULL,0)); }
static ABI int32_t net_setsockopt(int32_t s, int level, int option, const void *p_value, uint32_t len) {
    return (int32_t)sce(do_setsockopt(s,level,option,p_value,len));
}
static ABI int32_t net_getsockopt(int32_t s, int level, int option, void *p_value, uint32_t *p_len) {
    return (int32_t)sce(do_getsockopt(s,level,option,p_value,p_len));
}
/* The wired interface's address, which an unplugged console still reports: one in Sony's 00:D9:D1
 * block, the same for sceNetGetMacAddress and sceNetCtlGetInfo. */
void runtime_net_ether_address(uint8_t p_out_address[6]) {
    static const uint8_t s_address[6]={0x00,0xd9,0xd1,0x50,0x53,0x34};
    memcpy(p_out_address,s_address,sizeof(s_address));
}
static ABI int32_t net_get_mac_address(uint8_t *p_out_address, int flags) {
    (void)flags;
    if (!p_out_address) return fail(GUEST_EINVAL);
    runtime_net_ether_address(p_out_address);
    return 0;
}
/* SceNetSockInfo as the Vita's libnet lays it out, which the PS4's continues; the name a socket
 * was created with is not kept, so it stays empty. */
typedef struct {
    char name[32];
    int32_t pid, h_socket;
    int8_t socket_type, policy;
    int16_t reserved16;
    int32_t cb_recv_queue, cb_send_queue;
    uint32_t local_address, remote_address;
    uint16_t local_port, remote_port, local_vport, remote_vport;
    int32_t state, flags, reserved[8];
} GuestSockInfo;
_Static_assert(sizeof(GuestSockInfo)==108,"SceNetSockInfo layout");
static int describe_socket(int32_t h_socket, GuestSockInfo *p_out_info) {
    int h_host=host_socket(h_socket), type;
    socklen_t cb_type=sizeof(type);
    if (h_host<0 || getsockopt(h_host,SOL_SOCKET,SO_TYPE,(void *)&type,&cb_type)) return 0;
    memset(p_out_info,0,sizeof(*p_out_info));
    p_out_info->h_socket=h_socket;
    p_out_info->socket_type=type==SOCK_STREAM ? 1 : 2;
    ps_host_net_queued(h_host,&p_out_info->cb_recv_queue,&p_out_info->cb_send_queue);
    struct sockaddr_in address;
    socklen_t cb_address=sizeof(address);
    if (!getsockname(h_host,(struct sockaddr *)&address,&cb_address)) {
        p_out_info->local_address=address.sin_addr.s_addr;
        p_out_info->local_port=address.sin_port;
    }
    cb_address=sizeof(address);
    if (!getpeername(h_host,(struct sockaddr *)&address,&cb_address)) {
        p_out_info->remote_address=address.sin_addr.s_addr;
        p_out_info->remote_port=address.sin_port;
    }
    if (type==SOCK_STREAM) p_out_info->state=(int32_t)ps_host_net_tcp_state(h_host);
    return 1;
}
/* One socket, or every socket with h_socket -1; returns how many entries were written. */
static ABI int32_t net_get_sock_info(int32_t h_socket, GuestSockInfo *p_out_info, int32_t c_info, int32_t flags) {
    (void)flags;
    if (c_info<0 || (c_info && !p_out_info)) return fail(GUEST_EINVAL);
    if (h_socket!=-1) {
        GuestSockInfo info;
        if (!describe_socket(h_socket,&info)) return fail(GUEST_EBADF);
        if (c_info) p_out_info[0]=info;
        return c_info ? 1 : 0;
    }
    int32_t c_written=0;
    for (int32_t h=3; h<GUEST_FD_SETSIZE && c_written<c_info; ++h) c_written+=describe_socket(h,&p_out_info[c_written]);
    return c_written;
}

static ABI int32_t posix_socket(int family, int type, int protocol) { return (int32_t)posix(do_socket(family,type,protocol)); }
static ABI int32_t posix_socketpair(int family, int type, int protocol, int32_t *p_out_sockets) {
    return (int32_t)posix(do_socketpair(family,type,protocol,p_out_sockets));
}
static ABI int32_t posix_shutdown(int32_t h_socket, int how) { return (int32_t)posix(do_shutdown(h_socket,how)); }
static ABI int32_t posix_bind(int32_t h_socket, const GuestSockaddr *p_address, uint32_t cb_address) {
    return (int32_t)posix(do_bind(h_socket,p_address,cb_address));
}
static ABI int32_t posix_connect(int32_t h_socket, const GuestSockaddr *p_address, uint32_t cb_address) {
    return (int32_t)posix(do_connect(h_socket,p_address,cb_address));
}
static ABI int32_t posix_listen(int32_t h_socket, int backlog) { return (int32_t)posix(do_listen(h_socket,backlog)); }
static ABI int32_t posix_accept(int32_t h_socket, GuestSockaddr *p_out_address, uint32_t *p_cb_address) {
    return (int32_t)posix(do_accept(h_socket,p_out_address,p_cb_address));
}
static ABI int32_t posix_getsockname(int32_t h_socket, GuestSockaddr *p_out_address, uint32_t *p_cb_address) {
    return (int32_t)posix(do_getsockname(h_socket,p_out_address,p_cb_address));
}
static ABI int32_t posix_getpeername(int32_t h_socket, GuestSockaddr *p_out_address, uint32_t *p_cb_address) {
    return (int32_t)posix(do_getpeername(h_socket,p_out_address,p_cb_address));
}
static ABI int64_t posix_recvfrom(int32_t h_socket, void *p_buffer, uint64_t cb_buffer, int flags, GuestSockaddr *p_out_from,
                                  uint32_t *p_cb_from) {
    return posix(do_recvfrom(h_socket,p_buffer,cb_buffer,flags,p_out_from,p_cb_from));
}
static ABI int64_t posix_recv(int32_t h_socket, void *p_buffer, uint64_t cb_buffer, int flags) {
    return posix(do_recvfrom(h_socket,p_buffer,cb_buffer,flags,NULL,NULL));
}
static ABI int64_t posix_sendto(int32_t h_socket, const void *p_buffer, uint64_t cb_buffer, int flags, const GuestSockaddr *p_to,
                                uint32_t cb_to) {
    return posix(do_sendto(h_socket,p_buffer,cb_buffer,flags,p_to,cb_to));
}
static ABI int64_t posix_send(int32_t h_socket, const void *p_buffer, uint64_t cb_buffer, int flags) {
    return posix(do_sendto(h_socket,p_buffer,cb_buffer,flags,NULL,0));
}
static ABI int64_t posix_sendmsg(int32_t h_socket, const GuestMsghdr *p_message, int flags) {
    return posix(do_sendmsg(h_socket,p_message,flags));
}
static ABI int64_t posix_recvmsg(int32_t h_socket, GuestMsghdr *p_message, int flags) {
    return posix(do_recvmsg(h_socket,p_message,flags));
}
static ABI int32_t posix_setsockopt(int32_t h_socket, int level, int option, const void *p_value, uint32_t cb_value) {
    return (int32_t)posix(do_setsockopt(h_socket,level,option,p_value,cb_value));
}
static ABI int32_t posix_getsockopt(int32_t h_socket, int level, int option, void *p_out_value, uint32_t *p_cb_value) {
    return (int32_t)posix(do_getsockopt(h_socket,level,option,p_out_value,p_cb_value));
}
static ABI int32_t posix_select(int c_fds, uint64_t *p_read, uint64_t *p_write, uint64_t *p_except, const GuestTimeval *p_timeout) {
    return (int32_t)posix(do_select(c_fds,p_read,p_write,p_except,p_timeout));
}
static ABI int32_t posix_inet_pton(int family, const char *sz_address, void *p_out_address) {
    int family_host=host_family(family);
    if (family_host<0) return (int32_t)posix(-GUEST_EAFNOSUPPORT);
    return inet_pton(family_host,sz_address,p_out_address);
}
static ABI const char *posix_inet_ntop(int family, const void *p_address, char *sz_out, uint32_t cb_out) {
    int family_host=host_family(family);
    if (family_host<0) { posix(-GUEST_EAFNOSUPPORT); return NULL; }
    const char *sz_result=inet_ntop(family_host,p_address,sz_out,cb_out);
    if (!sz_result) posix(-GUEST_ENOSPC);
    return sz_result;
}

static int is_space(char c) { return c==' ' || (c>='\t' && c<='\r'); }
/* inet_aton's forms: a part is decimal, 0x hexadecimal or octal after a 0 ("09" stays decimal), parts
 * wrap at 32 bits unchecked, and the address ends at NUL or white space, after which anything may
 * follow. The strict form (sceNetInetPton) takes only four decimal parts. */
static int parse_inet4(const char *sz_address, int is_strict, uint8_t *p_out_address) {
    uint32_t parts[4];
    int c_parts=0;
    const char *p_cursor=sz_address;
    for (;;) {
        if (*p_cursor<'0' || *p_cursor>'9') return 0;
        uint32_t base=10, value=0;
        if (!is_strict && *p_cursor=='0') {
            if ((p_cursor[1]|0x20)=='x') base=16, p_cursor+=2;
            else if (p_cursor[1]>='0' && p_cursor[1]<='7') base=8, ++p_cursor;
        }
        for (;;++p_cursor) {
            uint32_t digit;
            if (*p_cursor>='0' && *p_cursor<='9') digit=(uint32_t)(*p_cursor-'0');
            else if (base==16 && (*p_cursor|0x20)>='a' && (*p_cursor|0x20)<='f') digit=(uint32_t)((*p_cursor|0x20)-'a'+10);
            else break;
            if (digit>=base) return 0;
            value=value*base+digit;
        }
        parts[c_parts++]=value;
        if (*p_cursor!='.') break;
        if (c_parts==4) return 0;
        ++p_cursor;
    }
    if (*p_cursor && !is_space(*p_cursor)) return 0;
    if (is_strict && c_parts!=4) return 0;
    static const uint32_t s_last_max[4]={UINT32_MAX,0xFFFFFF,0xFFFF,0xFF};
    uint32_t address=0;
    for (int i=0;i<c_parts-1;++i) {
        if (parts[i]>0xFF) return 0;
        address|=parts[i]<<(24-8*i);
    }
    if (parts[c_parts-1]>s_last_max[c_parts-1]) return 0;
    address|=parts[c_parts-1];
    for (int i=0;i<4;++i) p_out_address[i]=(uint8_t)(address>>(24-8*i));
    return 1;
}
/* 1, 0 for an address the family cannot read (errno untouched), or the error; the flags of
 * sceNetInetPtonEx change nothing. */
static int32_t inet_to_binary(int family, const char *sz_address, void *p_out_address, int is_strict) {
    if (!sz_address) return fail(GUEST_EINVAL);
    if (family!=AF_INET && family!=GUEST_AF_INET6) return fail(GUEST_EAFNOSUPPORT);
    uint8_t address[16];
    if (family==AF_INET ? !parse_inet4(sz_address,is_strict,address) : inet_pton(AF_INET6,sz_address,address)!=1)
        return 0;
    if (p_out_address) memcpy(p_out_address,address,family==AF_INET ? 4 : 16);
    return 1;
}
static ABI int32_t net_inet_pton(int family, const char *sz_address, void *p_out_address) {
    return inet_to_binary(family,sz_address,p_out_address,1);
}
static ABI int32_t net_inet_pton_ex(int family, const char *sz_address, void *p_out_address, int32_t flags) {
    (void)flags;
    return inet_to_binary(family,sz_address,p_out_address,0);
}
/* Lowercase groups with the longest run of two or more zero groups (the first of equals) as "::",
 * and no dotted IPv4 tail, as the console writes it. */
static size_t format_inet6(const uint8_t *p_address, char *sz_out) {
    uint16_t words[8];
    int i_best=-1, c_best=0;
    for (int i=0;i<8;++i) words[i]=(uint16_t)(p_address[2*i]<<8|p_address[2*i+1]);
    for (int i=0;i<8;) {
        int c_zeros=0;
        while (i+c_zeros<8 && !words[i+c_zeros]) ++c_zeros;
        if (c_zeros>=2 && c_zeros>c_best) i_best=i, c_best=c_zeros;
        i+=c_zeros ? c_zeros : 1;
    }
    char *p_cursor=sz_out;
    for (int i=0;i<8;++i) {
        if (i_best>=0 && i>=i_best && i<i_best+c_best) {
            if (i==i_best) *p_cursor++=':';
            continue;
        }
        if (i) *p_cursor++=':';
        p_cursor+=sprintf(p_cursor,"%x",words[i]);
    }
    if (i_best>=0 && i_best+c_best==8) *p_cursor++=':';
    *p_cursor=0;
    return (size_t)(p_cursor-sz_out);
}
/* The console copies all cb_out bytes of its scratch buffer, garbage after the NUL included; here
 * the bytes after the NUL are zeroed instead. */
static ABI const char *net_inet_ntop(int family, const void *p_address, char *sz_out, uint32_t cb_out) {
    if (family!=AF_INET && family!=GUEST_AF_INET6) { fail(GUEST_EAFNOSUPPORT); return NULL; }
    if (!p_address || !sz_out) { fail(GUEST_ENOSPC); return NULL; }
    const uint8_t *p_bytes=p_address;
    char sz_text[64];
    const size_t len=family==AF_INET ? (size_t)sprintf(sz_text,"%u.%u.%u.%u",p_bytes[0],p_bytes[1],p_bytes[2],p_bytes[3])
                                     : format_inet6(p_bytes,sz_text);
    if (len>=cb_out) { fail(GUEST_ENOSPC); return NULL; }
    memcpy(sz_out,sz_text,len+1);
    memset(sz_out+len+1,0,cb_out-len-1);
    return sz_out;
}

static const RuntimeExport s_exports[]={
    {"sceNetErrnoLoc",net_errno_loc}, {"sceNetSocket",net_socket}, {"sceNetSocketClose",net_close},
    {"sceNetSocketAbort",net_abort}, {"sceNetShutdown",net_shutdown}, {"sceNetBind",net_bind},
    {"sceNetConnect",net_connect}, {"sceNetListen",net_listen}, {"sceNetAccept",net_accept}, {"sceNetGetsockname",net_getsockname}, {"sceNetRecvfrom",net_recvfrom},
    {"sceNetRecv",net_recv}, {"sceNetSendto",net_sendto}, {"sceNetSend",net_send},
    {"sceNetSetsockopt",net_setsockopt}, {"sceNetGetsockopt",net_getsockopt},
    {"sceNetGetMacAddress",net_get_mac_address}, {"sceNetGetSockInfo",net_get_sock_info},
    {"sceNetInetPton",net_inet_pton}, {"sceNetInetPtonEx",net_inet_pton_ex}, {"sceNetInetNtop",net_inet_ntop},
    {"socket",posix_socket}, {"socketpair",posix_socketpair}, {"shutdown",posix_shutdown}, {"bind",posix_bind}, {"connect",posix_connect},
    {"listen",posix_listen}, {"accept",posix_accept}, {"getsockname",posix_getsockname}, {"getpeername",posix_getpeername},
    {"recvfrom",posix_recvfrom}, {"recv",posix_recv}, {"sendto",posix_sendto}, {"send",posix_send},
    {"sendmsg",posix_sendmsg}, {"recvmsg",posix_recvmsg}, {"setsockopt",posix_setsockopt}, {"getsockopt",posix_getsockopt},
    {"select",posix_select}, {"inet_pton",posix_inet_pton}, {"inet_ntop",posix_inet_ntop},
};
uintptr_t runtime_socket_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
