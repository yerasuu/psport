/* Linux: what sockets need beyond the BSD socket calls (ps_host.h). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

int ps_host_net_init(void) { return 0; }

int ps_host_net_close(int h_socket) { return close(h_socket) ? errno : 0; }

int ps_host_net_error(void) { return errno; }

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
