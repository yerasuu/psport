/* The host's BSD socket calls and constants (bind, listen, getsockname, setsockopt, AF_INET,
 * SO_*, inet_pton...), which Linux and Winsock 2 spell alike; what differs is ps_host_net_* in
 * ps_host.h. */
#ifndef PS_HOST_SOCKETS_H
#define PS_HOST_SOCKETS_H
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define SHUT_RDWR SD_BOTH
/* Windows has no SO_REUSEPORT (SO_REUSEADDR already allows what it does): 0 marks an option the
 * host does not have, which the caller accepts without effect and says. */
#define SO_REUSEPORT 0
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif
#endif
