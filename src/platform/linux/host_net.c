/* Linux: what sockets need beyond the BSD socket calls (ps_host.h). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

_Static_assert(sizeof(PsHostIoVector) == sizeof(struct iovec), "PsHostIoVector is struct iovec");

int ps_host_net_init(void) { return 0; }

int ps_host_net_close(int h_socket) { return close(h_socket) ? errno : 0; }

int ps_host_net_error(void) { return errno; }

int ps_host_net_socket(int family, int type, int protocol, int is_nonblocking) {
    const int h_socket = socket(family, type | SOCK_CLOEXEC | (is_nonblocking ? SOCK_NONBLOCK : 0), protocol);
    return h_socket < 0 ? -errno : h_socket;
}

int ps_host_net_socketpair(int type, int protocol, int is_nonblocking, int p_out_h_sockets[2]) {
    return socketpair(AF_UNIX, type | SOCK_CLOEXEC | (is_nonblocking ? SOCK_NONBLOCK : 0), protocol, p_out_h_sockets)
               ? errno
               : 0;
}

int ps_host_net_accept(int h_socket, void *p_out_address, uint32_t *p_cb_address) {
    socklen_t cb_address = p_cb_address ? *p_cb_address : 0;
    const int h_accepted = accept4(h_socket, p_out_address, p_cb_address ? &cb_address : NULL, SOCK_CLOEXEC);
    if (h_accepted < 0) return -errno;
    if (p_cb_address) *p_cb_address = cb_address;
    return h_accepted;
}

int ps_host_net_connect(int h_socket, const void *p_address, uint32_t cb_address) {
    return connect(h_socket, p_address, cb_address) ? errno : 0;
}

static int message_flags(unsigned flags) {
    return MSG_NOSIGNAL | (flags & PS_HOST_NET_OUT_OF_BAND ? MSG_OOB : 0) | (flags & PS_HOST_NET_PEEK ? MSG_PEEK : 0) |
           (flags & PS_HOST_NET_WAIT_ALL ? MSG_WAITALL : 0) | (flags & PS_HOST_NET_DONT_WAIT ? MSG_DONTWAIT : 0);
}

int64_t ps_host_net_send(int h_socket, const PsHostIoVector *p_vectors, int c_vectors, const void *p_to, uint32_t cb_to,
                         unsigned flags) {
    const struct msghdr message = {.msg_name = (void *)p_to, .msg_namelen = p_to ? cb_to : 0,
                                   .msg_iov = (struct iovec *)p_vectors, .msg_iovlen = (size_t)c_vectors};
    const ssize_t cb_sent = sendmsg(h_socket, &message, message_flags(flags));
    return cb_sent < 0 ? -errno : cb_sent;
}

int64_t ps_host_net_receive(int h_socket, const PsHostIoVector *p_vectors, int c_vectors, void *p_out_from,
                            uint32_t *p_cb_from, unsigned flags, unsigned *p_out_flags) {
    struct msghdr message = {.msg_name = p_out_from, .msg_namelen = p_out_from && p_cb_from ? *p_cb_from : 0,
                             .msg_iov = (struct iovec *)p_vectors, .msg_iovlen = (size_t)c_vectors};
    const ssize_t cb_received = recvmsg(h_socket, &message, message_flags(flags));
    if (cb_received < 0) return -errno;
    if (p_cb_from) *p_cb_from = p_out_from ? message.msg_namelen : 0;
    if (p_out_flags)
        *p_out_flags = (message.msg_flags & MSG_TRUNC ? PS_HOST_NET_TRUNCATED : 0u) |
                       (message.msg_flags & MSG_CTRUNC ? PS_HOST_NET_CONTROL_TRUNCATED : 0u) |
                       (message.msg_flags & MSG_OOB ? PS_HOST_NET_GOT_OUT_OF_BAND : 0u);
    return cb_received;
}

int ps_host_net_poll(PsHostPollEntry *p_entries, int c_entries, int64_t timeout_ns) {
    struct pollfd polled[c_entries ? c_entries : 1];
    for (int i = 0; i < c_entries; ++i)
        polled[i] = (struct pollfd){.fd = p_entries[i].h_socket,
                                    .events = (short)((p_entries[i].events & PS_HOST_NET_POLL_IN ? POLLIN : 0) |
                                                      (p_entries[i].events & PS_HOST_NET_POLL_OUT ? POLLOUT : 0) |
                                                      (p_entries[i].events & PS_HOST_NET_POLL_PRIORITY ? POLLPRI : 0))};
    const struct timespec timeout = {timeout_ns / 1000000000, timeout_ns % 1000000000};
    const int c_ready = ppoll(polled, (nfds_t)c_entries, timeout_ns < 0 ? NULL : &timeout, NULL);
    if (c_ready < 0) return -errno;
    for (int i = 0; i < c_entries; ++i) {
        const short revents = polled[i].revents;
        p_entries[i].revents = (revents & POLLIN ? PS_HOST_NET_POLL_IN : 0u) | (revents & POLLOUT ? PS_HOST_NET_POLL_OUT : 0u) |
                               (revents & POLLPRI ? PS_HOST_NET_POLL_PRIORITY : 0u) | (revents & POLLERR ? PS_HOST_NET_POLL_ERROR : 0u) |
                               (revents & POLLHUP ? PS_HOST_NET_POLL_HANG_UP : 0u) | (revents & POLLNVAL ? PS_HOST_NET_POLL_INVALID : 0u);
    }
    return c_ready;
}

int ps_host_net_set_nonblocking(int h_socket, int is_nonblocking) {
    const int flags = fcntl(h_socket, F_GETFL);
    if (flags < 0) return errno;
    return fcntl(h_socket, F_SETFL, is_nonblocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) ? errno : 0;
}

void ps_host_net_queued(int h_socket, int32_t *p_out_cb_receive, int32_t *p_out_cb_send) {
    int cb_queued = 0;
    if (!ioctl(h_socket, FIONREAD, &cb_queued)) *p_out_cb_receive = cb_queued;
    if (!ioctl(h_socket, TIOCOUTQ, &cb_queued)) *p_out_cb_send = cb_queued;
}

PsHostTcpState ps_host_net_tcp_state(int h_socket) {
    /* Linux's TCP_* states, 1 ESTABLISHED to 11 CLOSING, in RFC order. */
    static const PsHostTcpState s_states[12] = {
        PS_HOST_TCP_CLOSED,     PS_HOST_TCP_ESTABLISHED, PS_HOST_TCP_SYN_SENT,   PS_HOST_TCP_SYN_RECEIVED,
        PS_HOST_TCP_FIN_WAIT_1, PS_HOST_TCP_FIN_WAIT_2,  PS_HOST_TCP_TIME_WAIT,  PS_HOST_TCP_CLOSED,
        PS_HOST_TCP_CLOSE_WAIT, PS_HOST_TCP_LAST_ACK,    PS_HOST_TCP_LISTEN,     PS_HOST_TCP_CLOSING,
    };
    struct tcp_info tcp;
    socklen_t cb_tcp = sizeof(tcp);
    if (getsockopt(h_socket, IPPROTO_TCP, TCP_INFO, &tcp, &cb_tcp)) return PS_HOST_TCP_CLOSED;
    return tcp.tcpi_state < 12 ? s_states[tcp.tcpi_state] : PS_HOST_TCP_CLOSED;
}

int ps_host_net_ready(int h_socket, unsigned events, unsigned *p_out_events, int64_t *p_out_cb_ready) {
    return ps_host_fs_poll(h_socket, events, p_out_events, p_out_cb_ready);
}
