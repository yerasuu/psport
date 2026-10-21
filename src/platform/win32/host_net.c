/* Windows: what sockets need beyond the BSD socket calls, on Winsock 2 (ps_host.h). Socket
 * descriptors are SOCKET values, which fit an int since 64-bit Windows keeps handle values within
 * 32 bits for 32-bit processes to share them. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include "ps_host.h"
#include "host_win32.h"
#include <errno.h>

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
