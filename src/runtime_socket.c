/* libSceNet and POSIX sockets offline, as on a console with its cable unplugged: sockets, binds and traffic
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
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>

#define NET_ERR(e) ((int32_t)(UINT32_C(0x80410100)|(uint32_t)(e)))
#define GUEST_EBADF 9
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
    struct iovec *p_iov;
    int32_t c_iov;
    void *p_control;
    uint32_t cb_control;
    int32_t flags;
} GuestMsghdr;
_Static_assert(sizeof(GuestMsghdr)==48,"FreeBSD msghdr layout");
typedef struct { int64_t sec, usec; } GuestTimeval;

static int32_t net_errno;

int32_t *runtime_net_errno(void) { return &net_errno; }
static ABI int32_t *net_errno_loc(void) { return &net_errno; }
static int32_t fail(int guest_errno) { net_errno=guest_errno; return NET_ERR(guest_errno); }
/* Every operation below returns >=0 or -(FreeBSD errno); libSceNet turns that into its codes and
 * sets sceNetErrnoLoc's value, the POSIX exports into -1 and errno. */
static int64_t sce(int64_t result) { return result<0 ? fail((int)-result) : result; }
static int64_t posix(int64_t result) {
    if (result>=0) return result;
    *runtime_errno()=(int32_t)-result;
    return -1;
}
static int64_t host_failure(void) { return -runtime_guest_errno(errno); }
/* Sockets live in the runtime's file table (runtime_file.c), as on the console where a socket is a
 * kernel descriptor like a file; 0-2 are the host's stdio, never a socket. */
static int host_socket(int32_t h_socket) { return h_socket<3 ? -1 : runtime_file_host(h_socket); }
static int64_t adopt(int h_host) {
    if (h_host<0) return host_failure();
    int h_socket=runtime_file_adopt(h_host);
    if (h_socket<0) {
        close(h_host);
        return -GUEST_EMFILE;
    }
    return h_socket;
}

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

static int64_t do_socket(int family, int type, int protocol) {
    if (family!=AF_INET) return -GUEST_EAFNOSUPPORT;
    int host_flags=SOCK_CLOEXEC|(type&GUEST_SOCK_NONBLOCK ? SOCK_NONBLOCK : 0);
    type&=~(GUEST_SOCK_CLOEXEC|GUEST_SOCK_NONBLOCK);
    /* The P2P types (6 datagram, 10 stream) carry a virtual port the loopback path has no use for. */
    int host_type=type==1 || type==10 ? SOCK_STREAM : type==2 || type==6 ? SOCK_DGRAM : -1;
    if (host_type<0) return -GUEST_EPROTONOSUPPORT;
    return adopt(socket(AF_INET,host_type|host_flags,protocol));
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
static int64_t do_bind(int32_t s, const GuestSockaddr *addr, uint32_t len) {
    struct sockaddr_in a;
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    if (!host_address(addr,len,&a)) return -GUEST_EINVAL;
    if (a.sin_addr.s_addr==htonl(INADDR_ANY)) a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    else if (!loopback(&a)) return -GUEST_EADDRNOTAVAIL;
    return bind(h,(struct sockaddr *)&a,sizeof(a)) ? host_failure() : 0;
}
static int64_t do_connect(int32_t s, const GuestSockaddr *addr, uint32_t len) {
    struct sockaddr_in a;
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    if (!host_address(addr,len,&a)) return -GUEST_EINVAL;
    if (!loopback(&a)) return -GUEST_ENETUNREACH;
    return connect(h,(struct sockaddr *)&a,sizeof(a)) ? host_failure() : 0;
}
static int64_t do_listen(int32_t s, int backlog) {
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    return listen(h,backlog) ? host_failure() : 0;
}
static int64_t do_accept(int32_t s, GuestSockaddr *addr, uint32_t *len) {
    struct sockaddr_in a;
    socklen_t size=sizeof(a);
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    int64_t accepted=adopt(accept4(h,(struct sockaddr *)&a,&size,SOCK_CLOEXEC));
    if (accepted>=0) guest_address(&a,addr,len);
    return accepted;
}
static int64_t do_getsockname(int32_t s, GuestSockaddr *addr, uint32_t *len) {
    struct sockaddr_in a;
    socklen_t size=sizeof(a);
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    if (getsockname(h,(struct sockaddr *)&a,&size)) return host_failure();
    guest_address(&a,addr,len);
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
static int host_flags(int flags) {
    return (flags&0x1 ? MSG_OOB : 0)|(flags&0x2 ? MSG_PEEK : 0)|(flags&0x40 ? MSG_WAITALL : 0)|(flags&0x80 ? MSG_DONTWAIT : 0);
}
static int64_t do_recvfrom(int32_t s, void *buf, uint64_t len, int flags, GuestSockaddr *from, uint32_t *fromlen) {
    struct sockaddr_in a={0};
    socklen_t size=sizeof(a);
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    ssize_t n=recvfrom(h,buf,len,host_flags(flags)|MSG_NOSIGNAL,(struct sockaddr *)&a,&size);
    if (n<0) return host_failure();
    if (from && size) guest_address(&a,from,fromlen);
    return n;
}
static int64_t do_sendto(int32_t s, const void *buf, uint64_t len, int flags, const GuestSockaddr *to, uint32_t tolen) {
    struct sockaddr_in a;
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    if (to && !host_address(to,tolen,&a)) return -GUEST_EINVAL;
    if (to && !loopback(&a)) return -GUEST_ENETUNREACH;
    ssize_t n=sendto(h,buf,len,host_flags(flags)|MSG_NOSIGNAL,to ? (struct sockaddr *)&a : NULL,to ? sizeof(a) : 0);
    return n<0 ? host_failure() : n;
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
    struct msghdr host_message={.msg_name=p_message->p_name ? &to : NULL,.msg_namelen=p_message->p_name ? sizeof(to) : 0,
                                .msg_iov=p_message->p_iov,.msg_iovlen=(size_t)p_message->c_iov};
    ssize_t cb_sent=sendmsg(h_host,&host_message,host_flags(flags)|MSG_NOSIGNAL);
    return cb_sent<0 ? host_failure() : cb_sent;
}
static int64_t do_recvmsg(int32_t h_socket, GuestMsghdr *p_message, int flags) {
    struct sockaddr_in from={0};
    int h_host=host_socket(h_socket);
    if (h_host<0) return -GUEST_EBADF;
    if (!p_message || p_message->c_iov<0) return -GUEST_EINVAL;
    struct msghdr host_message={.msg_name=&from,.msg_namelen=sizeof(from),.msg_iov=p_message->p_iov,
                                .msg_iovlen=(size_t)p_message->c_iov};
    ssize_t cb_received=recvmsg(h_host,&host_message,host_flags(flags)|MSG_NOSIGNAL);
    if (cb_received<0) return host_failure();
    if (p_message->p_name && host_message.msg_namelen) guest_address(&from,p_message->p_name,&p_message->cb_name);
    else p_message->cb_name=0;
    p_message->cb_control=0;
    p_message->flags=(host_message.msg_flags&MSG_TRUNC ? 0x10 : 0)|(host_message.msg_flags&MSG_CTRUNC ? 0x20 : 0)|
                     (host_message.msg_flags&MSG_OOB ? 0x1 : 0);
    return cb_received;
}
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
static int64_t do_setsockopt(int32_t s, int level, int option, const void *value, uint32_t len) {
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    if (level==0xffff && option==0x1200) { /* SO_NBIO */
        if (!value || len<4) return -GUEST_EINVAL;
        int nbio;
        memcpy(&nbio,value,sizeof(nbio));
        int f=fcntl(h,F_GETFL);
        return f<0 || fcntl(h,F_SETFL,nbio ? f|O_NONBLOCK : f&~O_NONBLOCK) ? host_failure() : 0;
    }
    int host_level, host=host_option(level,option,&host_level);
    if (!host) {
        fprintf(stderr,"Runtime: socket option %#x/%#x accepted without effect\n",level,option);
        return 0;
    }
    return setsockopt(h,host_level,host,value,len) ? host_failure() : 0;
}
static int64_t do_getsockopt(int32_t s, int level, int option, void *value, uint32_t *len) {
    int h=host_socket(s);
    if (h<0) return -GUEST_EBADF;
    int host_level, host=host_option(level,option,&host_level);
    if (!host) return -GUEST_ENOPROTOOPT;
    socklen_t size=len ? *len : 0;
    if (getsockopt(h,host_level,host,value,&size)) return host_failure();
    if (host==SO_ERROR && host_level==SOL_SOCKET && value && size>=sizeof(int32_t)) {
        int32_t e;
        memcpy(&e,value,sizeof(e));
        e=runtime_guest_errno(e);
        memcpy(value,&e,sizeof(e));
    }
    if (len) *len=size;
    return 0;
}
/* Over poll, so a host descriptor past the host's FD_SETSIZE still works; FreeBSD's fd_set is the
 * same bit array of longs as Linux's. A descriptor that hung up or failed counts as readable. */
static int64_t do_select(int c_fds, uint64_t *p_read, uint64_t *p_write, uint64_t *p_except, const GuestTimeval *p_timeout) {
    if (c_fds<0 || c_fds>GUEST_FD_SETSIZE) return -GUEST_EINVAL;
    if (p_timeout && (p_timeout->sec<0 || p_timeout->usec<0 || p_timeout->usec>=1000000)) return -GUEST_EINVAL;
    struct pollfd polled[c_fds ? c_fds : 1];
    int h_guest[c_fds ? c_fds : 1], c_polled=0;
    for (int h_fd=0;h_fd<c_fds;++h_fd) {
        const uint64_t bit=UINT64_C(1)<<(h_fd&63);
        const int i_word=h_fd>>6;
        short events=(p_read && p_read[i_word]&bit ? POLLIN : 0)|(p_write && p_write[i_word]&bit ? POLLOUT : 0)|
                     (p_except && p_except[i_word]&bit ? POLLPRI : 0);
        if (!events) continue;
        int h_host=runtime_file_host(h_fd);
        if (h_host<0) return -GUEST_EBADF;
        polled[c_polled]=(struct pollfd){.fd=h_host,.events=events};
        h_guest[c_polled++]=h_fd;
    }
    struct timespec timeout;
    if (p_timeout) timeout=(struct timespec){.tv_sec=p_timeout->sec,.tv_nsec=p_timeout->usec*1000};
    int c_ready=ppoll(polled,(nfds_t)c_polled,p_timeout ? &timeout : NULL,NULL);
    if (c_ready<0) return host_failure();
    const size_t cb_set=(size_t)(c_fds+63)/64*sizeof(uint64_t);
    if (p_read) memset(p_read,0,cb_set);
    if (p_write) memset(p_write,0,cb_set);
    if (p_except) memset(p_except,0,cb_set);
    int64_t c_bits=0;
    for (int i=0;i<c_polled;++i) {
        const short events=polled[i].events, revents=polled[i].revents;
        const uint64_t bit=UINT64_C(1)<<(h_guest[i]&63);
        const int i_word=h_guest[i]>>6;
        if (revents&POLLNVAL) return -GUEST_EBADF;
        if (events&POLLIN && revents&(POLLIN|POLLHUP|POLLERR)) { p_read[i_word]|=bit; ++c_bits; }
        if (events&POLLOUT && revents&(POLLOUT|POLLERR)) { p_write[i_word]|=bit; ++c_bits; }
        if (events&POLLPRI && revents&POLLPRI) { p_except[i_word]|=bit; ++c_bits; }
    }
    return c_bits;
}
static int host_family(int family) { return family==AF_INET ? AF_INET : family==GUEST_AF_INET6 ? AF_INET6 : -1; }

static ABI int32_t net_socket(const char *name, int family, int type, int protocol) {
    (void)name;
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
static ABI int32_t net_bind(int32_t s, const GuestSockaddr *addr, uint32_t len) { return (int32_t)sce(do_bind(s,addr,len)); }
static ABI int32_t net_connect(int32_t s, const GuestSockaddr *addr, uint32_t len) { return (int32_t)sce(do_connect(s,addr,len)); }
static ABI int32_t net_listen(int32_t s, int backlog) { return (int32_t)sce(do_listen(s,backlog)); }
static ABI int32_t net_accept(int32_t s, GuestSockaddr *addr, uint32_t *len) { return (int32_t)sce(do_accept(s,addr,len)); }
static ABI int32_t net_getsockname(int32_t s, GuestSockaddr *addr, uint32_t *len) { return (int32_t)sce(do_getsockname(s,addr,len)); }
static ABI int64_t net_recvfrom(int32_t s, void *buf, uint64_t len, int flags, GuestSockaddr *from, uint32_t *fromlen) {
    return sce(do_recvfrom(s,buf,len,flags,from,fromlen));
}
static ABI int64_t net_recv(int32_t s, void *buf, uint64_t len, int flags) { return sce(do_recvfrom(s,buf,len,flags,NULL,NULL)); }
static ABI int64_t net_sendto(int32_t s, const void *buf, uint64_t len, int flags, const GuestSockaddr *to, uint32_t tolen) {
    return sce(do_sendto(s,buf,len,flags,to,tolen));
}
static ABI int64_t net_send(int32_t s, const void *buf, uint64_t len, int flags) { return sce(do_sendto(s,buf,len,flags,NULL,0)); }
static ABI int32_t net_setsockopt(int32_t s, int level, int option, const void *value, uint32_t len) {
    return (int32_t)sce(do_setsockopt(s,level,option,value,len));
}
static ABI int32_t net_getsockopt(int32_t s, int level, int option, void *value, uint32_t *len) {
    return (int32_t)sce(do_getsockopt(s,level,option,value,len));
}

static ABI int32_t posix_socket(int family, int type, int protocol) { return (int32_t)posix(do_socket(family,type,protocol)); }
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

static const RuntimeExport exports[]={
    {"sceNetErrnoLoc",net_errno_loc}, {"sceNetSocket",net_socket}, {"sceNetSocketClose",net_close},
    {"sceNetSocketAbort",net_abort}, {"sceNetShutdown",net_shutdown}, {"sceNetBind",net_bind},
    {"sceNetConnect",net_connect}, {"sceNetListen",net_listen}, {"sceNetAccept",net_accept}, {"sceNetGetsockname",net_getsockname}, {"sceNetRecvfrom",net_recvfrom},
    {"sceNetRecv",net_recv}, {"sceNetSendto",net_sendto}, {"sceNetSend",net_send},
    {"sceNetSetsockopt",net_setsockopt}, {"sceNetGetsockopt",net_getsockopt},
    {"socket",posix_socket}, {"shutdown",posix_shutdown}, {"bind",posix_bind}, {"connect",posix_connect},
    {"listen",posix_listen}, {"accept",posix_accept}, {"getsockname",posix_getsockname}, {"getpeername",posix_getpeername},
    {"recvfrom",posix_recvfrom}, {"recv",posix_recv}, {"sendto",posix_sendto}, {"send",posix_send},
    {"sendmsg",posix_sendmsg}, {"recvmsg",posix_recvmsg}, {"setsockopt",posix_setsockopt}, {"getsockopt",posix_getsockopt},
    {"select",posix_select}, {"inet_pton",posix_inet_pton}, {"inet_ntop",posix_inet_ntop},
};
uintptr_t runtime_socket_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
#else
int32_t *runtime_net_errno(void) { static int32_t e; return &e; }
uintptr_t runtime_socket_resolve(const char *name) { (void)name; return 0; }
#endif
