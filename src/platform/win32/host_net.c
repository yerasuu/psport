// SPDX-License-Identifier: GPL-2.0-or-later
/* Windows: what sockets need beyond the BSD socket calls, on Winsock 2 (ps_host.h). Socket
 * descriptors are SOCKET values, which fit an int since 64-bit Windows keeps handle values within
 * 32 bits for 32-bit processes to share them. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include "ps_host.h"
#include "host_win32.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

static SOCKET to_socket(int h_socket) { return (SOCKET)(unsigned)h_socket; }

static INIT_ONCE s_startup_once = INIT_ONCE_STATIC_INIT;
static int s_startup_error;

static int net_errno(int error);

static BOOL CALLBACK start_winsock(PINIT_ONCE p_once, void *p_parameter, void **pp_context) {
    (void)p_once;
    (void)p_parameter;
    (void)pp_context;
    WSADATA data;
    s_startup_error = net_errno(WSAStartup(MAKEWORD(2, 2), &data));
    return TRUE;
}

int ps_host_net_init(void) {
    InitOnceExecuteOnce(&s_startup_once, start_winsock, NULL, NULL);
    return s_startup_error;
}

/* Winsock's codes as the errno values Linux gives for the same failures. WSAEWOULDBLOCK reads as
 * EAGAIN, which Linux's EWOULDBLOCK equals and the generic code tests. */
static int net_errno(int error) {
    switch (error) {
    case 0: return 0;
    case WSAEINTR: return EINTR;
    case WSAEBADF:
    case WSA_INVALID_HANDLE: return EBADF;
    case WSAEACCES: return EACCES;
    case WSAEFAULT: return EFAULT;
    case WSAEINVAL:
    case WSA_INVALID_PARAMETER: return EINVAL;
    case WSAEMFILE: return EMFILE;
    case WSAEWOULDBLOCK: return EAGAIN;
    case WSAEINPROGRESS: return EINPROGRESS;
    case WSAEALREADY: return EALREADY;
    case WSAENOTSOCK: return ENOTSOCK;
    case WSAEDESTADDRREQ: return EDESTADDRREQ;
    case WSAEMSGSIZE: return EMSGSIZE;
    case WSAEPROTOTYPE: return EPROTOTYPE;
    case WSAENOPROTOOPT: return ENOPROTOOPT;
    case WSAEPROTONOSUPPORT:
    case WSAESOCKTNOSUPPORT: return EPROTONOSUPPORT;
    case WSAEOPNOTSUPP: return EOPNOTSUPP;
    case WSAEPFNOSUPPORT:
    case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
    case WSAEADDRINUSE: return EADDRINUSE;
    case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
    case WSAENETDOWN:
    case WSANOTINITIALISED:
    case WSASYSNOTREADY: return ENETDOWN;
    case WSAENETUNREACH: return ENETUNREACH;
    case WSAENETRESET: return ENETRESET;
    case WSAECONNABORTED: return ECONNABORTED;
    case WSAECONNRESET: return ECONNRESET;
    case WSAENOBUFS:
    case WSA_NOT_ENOUGH_MEMORY: return ENOBUFS;
    case WSAEISCONN: return EISCONN;
    case WSAENOTCONN: return ENOTCONN;
    case WSAESHUTDOWN: return EPIPE;
    case WSAETIMEDOUT: return ETIMEDOUT;
    case WSAECONNREFUSED: return ECONNREFUSED;
    case WSAELOOP: return ELOOP;
    case WSAENAMETOOLONG: return ENAMETOOLONG;
    case WSAEHOSTDOWN:
    case WSAEHOSTUNREACH: return EHOSTUNREACH;
    case WSAENOTEMPTY: return ENOTEMPTY;
    case WSAEPROCLIM:
    case WSAEUSERS: return EAGAIN;
    case WSAEDQUOT: return ENOSPC;
    case WSAEDISCON: return EPIPE;
    }
    return EIO;
}

int ps_host_net_close(int h_socket) { return closesocket(to_socket(h_socket)) ? net_errno(WSAGetLastError()) : 0; }

int ps_host_net_error(void) { return net_errno(WSAGetLastError()); }

int ps_host_net_set_nonblocking(int h_socket, int is_nonblocking) {
    u_long is_set = is_nonblocking != 0;
    return ioctlsocket(to_socket(h_socket), FIONBIO, &is_set) ? net_errno(WSAGetLastError()) : 0;
}

/* SIO_TCP_INFO (Windows 10 1703 and later) has the connection's state and the bytes sent and not
 * yet acknowledged, which stand in for Linux's unsent queue. */
static int tcp_info(int h_socket, TCP_INFO_v0 *p_out) {
    DWORD version = 0, cb_returned = 0;
    return !WSAIoctl(to_socket(h_socket), SIO_TCP_INFO, &version, sizeof(version), p_out, sizeof(*p_out), &cb_returned,
                     NULL, NULL) &&
           cb_returned >= sizeof(*p_out);
}

void ps_host_net_queued(int h_socket, int32_t *p_out_cb_receive, int32_t *p_out_cb_send) {
    u_long cb_queued = 0;
    if (!ioctlsocket(to_socket(h_socket), FIONREAD, &cb_queued))
        *p_out_cb_receive = cb_queued > INT32_MAX ? INT32_MAX : (int32_t)cb_queued;
    TCP_INFO_v0 tcp;
    if (tcp_info(h_socket, &tcp)) *p_out_cb_send = tcp.BytesInFlight > INT32_MAX ? INT32_MAX : (int32_t)tcp.BytesInFlight;
}

PsHostTcpState ps_host_net_tcp_state(int h_socket) {
    static const PsHostTcpState s_states[TCPSTATE_MAX] = {
        [TCPSTATE_CLOSED] = PS_HOST_TCP_CLOSED,
        [TCPSTATE_LISTEN] = PS_HOST_TCP_LISTEN,
        [TCPSTATE_SYN_SENT] = PS_HOST_TCP_SYN_SENT,
        [TCPSTATE_SYN_RCVD] = PS_HOST_TCP_SYN_RECEIVED,
        [TCPSTATE_ESTABLISHED] = PS_HOST_TCP_ESTABLISHED,
        [TCPSTATE_FIN_WAIT_1] = PS_HOST_TCP_FIN_WAIT_1,
        [TCPSTATE_FIN_WAIT_2] = PS_HOST_TCP_FIN_WAIT_2,
        [TCPSTATE_CLOSE_WAIT] = PS_HOST_TCP_CLOSE_WAIT,
        [TCPSTATE_CLOSING] = PS_HOST_TCP_CLOSING,
        [TCPSTATE_LAST_ACK] = PS_HOST_TCP_LAST_ACK,
        [TCPSTATE_TIME_WAIT] = PS_HOST_TCP_TIME_WAIT,
    };
    TCP_INFO_v0 tcp;
    if (!tcp_info(h_socket, &tcp)) return PS_HOST_TCP_CLOSED;
    return (unsigned)tcp.State < TCPSTATE_MAX ? s_states[tcp.State] : PS_HOST_TCP_CLOSED;
}

int ps_host_net_socket(int family, int type, int protocol, int is_nonblocking) {
    const SOCKET h_socket = WSASocketW(family, type, protocol, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (h_socket == INVALID_SOCKET) return -net_errno(WSAGetLastError());
    const int error = is_nonblocking ? ps_host_net_set_nonblocking((int)h_socket, 1) : 0;
    if (error) {
        closesocket(h_socket);
        return -error;
    }
    return (int)h_socket;
}

int ps_host_net_accept(int h_socket, void *p_out_address, uint32_t *p_cb_address) {
    int cb_address = p_cb_address ? (int)*p_cb_address : 0;
    const SOCKET h_accepted = accept(to_socket(h_socket), p_out_address, p_cb_address ? &cb_address : NULL);
    if (h_accepted == INVALID_SOCKET) return -net_errno(WSAGetLastError());
    SetHandleInformation((HANDLE)h_accepted, HANDLE_FLAG_INHERIT, 0);
    if (p_cb_address) *p_cb_address = (uint32_t)cb_address;
    return (int)h_accepted;
}

/* Winsock answers a connection still being made on a non-blocking socket with WSAEWOULDBLOCK where
 * BSD sockets answer EINPROGRESS, which the guest waits on. */
int ps_host_net_connect(int h_socket, const void *p_address, uint32_t cb_address) {
    if (!connect(to_socket(h_socket), p_address, (int)cb_address)) return 0;
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK ? EINPROGRESS : net_errno(error);
}

/* Linux's UIO_MAXIOV, past which sendmsg and recvmsg refuse a message. */
#define MAX_VECTORS 1024

static int to_buffers(const PsHostIoVector *p_vectors, int c_vectors, WSABUF *p_out) {
    if (c_vectors < 0 || c_vectors > MAX_VECTORS) return EMSGSIZE;
    for (int i = 0; i < c_vectors; ++i) {
        if (p_vectors[i].cb_length > ULONG_MAX) return EINVAL;
        p_out[i] = (WSABUF){.len = (ULONG)p_vectors[i].cb_length, .buf = p_vectors[i].p_base};
    }
    return 0;
}

/* Winsock has no MSG_DONTWAIT: a socket that is not ready now answers EAGAIN, as it would. */
static int would_block(int h_socket, short events) {
    WSAPOLLFD polled = {.fd = to_socket(h_socket), .events = events};
    return WSAPoll(&polled, 1, 0) == 0;
}

static DWORD message_flags(unsigned flags) {
    return (flags & PS_HOST_NET_OUT_OF_BAND ? MSG_OOB : 0u) | (flags & PS_HOST_NET_PEEK ? MSG_PEEK : 0u) |
           (flags & PS_HOST_NET_WAIT_ALL ? MSG_WAITALL : 0u);
}

int64_t ps_host_net_send(int h_socket, const PsHostIoVector *p_vectors, int c_vectors, const void *p_to, uint32_t cb_to,
                         unsigned flags) {
    WSABUF buffers[MAX_VECTORS];
    const int error = to_buffers(p_vectors, c_vectors, buffers);
    if (error) return -error;
    if (flags & PS_HOST_NET_DONT_WAIT && would_block(h_socket, POLLWRNORM)) return -EAGAIN;
    DWORD cb_sent = 0;
    if (WSASendTo(to_socket(h_socket), buffers, (DWORD)c_vectors, &cb_sent, message_flags(flags), p_to,
                  p_to ? (int)cb_to : 0, NULL, NULL))
        return -net_errno(WSAGetLastError());
    return cb_sent;
}

int64_t ps_host_net_receive(int h_socket, const PsHostIoVector *p_vectors, int c_vectors, void *p_out_from,
                            uint32_t *p_cb_from, unsigned flags, unsigned *p_out_flags) {
    WSABUF buffers[MAX_VECTORS];
    const int error = to_buffers(p_vectors, c_vectors, buffers);
    if (error) return -error;
    if (flags & PS_HOST_NET_DONT_WAIT && would_block(h_socket, POLLRDNORM)) return -EAGAIN;
    DWORD cb_received = 0, received_flags = message_flags(flags);
    int cb_from = p_out_from && p_cb_from ? (int)*p_cb_from : 0;
    unsigned found = 0;
    if (WSARecvFrom(to_socket(h_socket), buffers, (DWORD)c_vectors, &cb_received, &received_flags,
                    p_out_from && p_cb_from ? p_out_from : NULL, p_out_from && p_cb_from ? &cb_from : NULL, NULL, NULL)) {
        const int receive_error = WSAGetLastError();
        if (receive_error != WSAEMSGSIZE) return -net_errno(receive_error);
        /* The buffers hold the datagram's start, as MSG_TRUNC reports it on Linux. */
        cb_received = 0;
        for (int i = 0; i < c_vectors; ++i) cb_received += buffers[i].len;
        found |= PS_HOST_NET_TRUNCATED;
    }
    if (received_flags & MSG_OOB) found |= PS_HOST_NET_GOT_OUT_OF_BAND;
    if (p_cb_from) *p_cb_from = p_out_from ? (uint32_t)cb_from : 0;
    if (p_out_flags) *p_out_flags = found;
    return cb_received;
}

/* WSAPoll refuses POLLPRI, so out-of-band data is asked as POLLRDBAND. It polls sockets only. */
int ps_host_net_poll(PsHostPollEntry *p_entries, int c_entries, int64_t timeout_ns) {
    if (c_entries < 0 || c_entries > MAX_VECTORS) return -EINVAL;
    const INT timeout_ms = timeout_ns < 0                       ? -1
                           : timeout_ns / 1000000 >= INT32_MAX ? INT32_MAX
                                                               : (INT)((timeout_ns + 999999) / 1000000);
    if (!c_entries) {
        if (timeout_ms < 0) return -EINVAL;
        Sleep((DWORD)timeout_ms);
        return 0;
    }
    WSAPOLLFD polled[MAX_VECTORS];
    for (int i = 0; i < c_entries; ++i)
        polled[i] = (WSAPOLLFD){.fd = to_socket(p_entries[i].h_socket),
                                .events = (SHORT)((p_entries[i].events & PS_HOST_NET_POLL_IN ? POLLRDNORM : 0) |
                                                  (p_entries[i].events & PS_HOST_NET_POLL_OUT ? POLLWRNORM : 0) |
                                                  (p_entries[i].events & PS_HOST_NET_POLL_PRIORITY ? POLLRDBAND : 0))};
    const int c_ready = WSAPoll(polled, (ULONG)c_entries, timeout_ms);
    if (c_ready < 0) return -net_errno(WSAGetLastError());
    for (int i = 0; i < c_entries; ++i) {
        const SHORT revents = polled[i].revents;
        p_entries[i].revents = (revents & POLLRDNORM ? PS_HOST_NET_POLL_IN : 0u) | (revents & POLLWRNORM ? PS_HOST_NET_POLL_OUT : 0u) |
                               (revents & POLLRDBAND ? PS_HOST_NET_POLL_PRIORITY : 0u) | (revents & POLLERR ? PS_HOST_NET_POLL_ERROR : 0u) |
                               (revents & POLLHUP ? PS_HOST_NET_POLL_HANG_UP : 0u) | (revents & POLLNVAL ? PS_HOST_NET_POLL_INVALID : 0u);
    }
    return c_ready;
}

/* Winsock's AF_UNIX has no socketpair, and building one from a listener on a temporary path would
 * give a stream pair only: the guest gets EOPNOTSUPP, counted. */
int ps_host_net_socketpair(int type, int protocol, int is_nonblocking, int p_out_h_sockets[2]) {
    (void)p_out_h_sockets;
    static volatile LONG s_c_refused;
    const LONG c_refused = InterlockedIncrement(&s_c_refused);
    if (!(c_refused & (c_refused - 1)))
        fprintf(stderr, "Host: socketpair(type %d, protocol %d, nonblocking %d) unavailable on Windows (%ld refused)\n",
                type, protocol, is_nonblocking, (long)c_refused);
    return EOPNOTSUPP;
}

int ps_host_net_ready(int h_socket, unsigned events, unsigned *p_out_events, int64_t *p_out_cb_ready) {
    WSAPOLLFD polled = {.fd = to_socket(h_socket),
                        .events = (SHORT)((events & PS_HOST_POLL_IN ? POLLRDNORM : 0) |
                                          (events & PS_HOST_POLL_OUT ? POLLWRNORM : 0))};
    if (WSAPoll(&polled, 1, 0) < 0) return net_errno(WSAGetLastError());
    if (polled.revents & POLLNVAL) return EBADF;
    *p_out_events = (polled.revents & POLLRDNORM ? PS_HOST_POLL_IN : 0u) |
                    (polled.revents & POLLWRNORM ? PS_HOST_POLL_OUT : 0u) |
                    (polled.revents & POLLHUP ? PS_HOST_POLL_HANGUP : 0u) |
                    (polled.revents & POLLERR ? PS_HOST_POLL_ERROR : 0u);
    *p_out_cb_ready = 0;
    u_long cb_queued = 0;
    if (events & PS_HOST_POLL_IN && !ioctlsocket(to_socket(h_socket), FIONREAD, &cb_queued)) *p_out_cb_ready = (int64_t)cb_queued;
    return 0;
}
