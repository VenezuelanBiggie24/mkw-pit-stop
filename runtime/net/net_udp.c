// UDP sockets of the host system for local wireless play (net_udp.h). Linux and the PS5 share the BSD calls (the PS5's
// libkernel exports socket, bind, sendto, recvfrom, poll and getsockname as FreeBSD does; runtime/platform/ps5_recv.c
// uses the same); Windows uses Winsock 2, started once on first use.
#include "net_udp.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define net_errno() WSAGetLastError()
#define WOULD_BLOCK(e) ((e) == WSAEWOULDBLOCK || (e) == WSAECONNRESET)   // ICMP port unreachable surfaces as a reset
static void wsa_start(void) {
    static volatile LONG started;
    if (InterlockedCompareExchange(&started, 1, 0)) return;
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define net_errno() errno
#define WOULD_BLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == ECONNREFUSED || (e) == EINTR)
#define closesocket close
static void wsa_start(void) {}
#endif
// Every read and send passes MSG_DONTWAIT and a read polls first: O_NONBLOCK alone did not keep recvfrom from waiting
// on the PS5 (17/11/2026: the network thread read the socket with the session lock held and never came back, so the
// first room search waited forever and the game showed a communication error). sdk/ov/ov_socket.c does the same.
#ifdef _WIN32
#define DONTWAIT 0
#else
#define DONTWAIT MSG_DONTWAIT
#endif

static __thread char err_text[96];

static void set_err(const char *what) {
    snprintf(err_text, sizeof err_text, "%s failed (error %d)", what, (int)net_errno());
}

const char *net_udp_error(void) { return err_text; }

static struct sockaddr_in sin_of(NetAddr a) {
    struct sockaddr_in s;
    memset(&s, 0, sizeof s);
    s.sin_family = AF_INET;
    s.sin_port = htons(a.port);
    s.sin_addr.s_addr = htonl(a.ip);
    return s;
}

int net_udp_open(uint32_t ip, uint16_t port, int flags, uint16_t *bound) {
    wsa_start();
    int s = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { set_err("socket"); return -1; }
    int one = 1;
    if (flags & NET_UDP_BROADCAST) setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    struct sockaddr_in a = sin_of((NetAddr){ip, port});
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { set_err("bind"); closesocket(s); return -1; }
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    // Without this a Windows UDP socket reports WSAECONNRESET on the next recv after an ICMP port unreachable.
    BOOL no = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &no, sizeof no, NULL, 0, &ret, NULL, NULL);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    if (bound) {
        socklen_t len = sizeof a;
        *bound = getsockname(s, (struct sockaddr *)&a, &len) == 0 ? ntohs(a.sin_port) : port;
    }
    return s;
}

void net_udp_close(int s) {
    if (s >= 0) closesocket(s);
}

int net_udp_send(int s, const void *p, int n, NetAddr to) {
    struct sockaddr_in a = sin_of(to);
    int k = (int)sendto(s, (const char *)p, n, DONTWAIT, (struct sockaddr *)&a, sizeof a);
    if (k >= 0) return k;
    int e = net_errno();
    if (WOULD_BLOCK(e)) return n;
    set_err("sendto");
    return -1;
}

int net_udp_recv(int s, void *p, int max, NetAddr *from) {
    for (;;) {
        if (!net_udp_wait(s, 0)) return 0;
        struct sockaddr_in a;
        socklen_t len = sizeof a;
        int k = (int)recvfrom(s, (char *)p, max, DONTWAIT, (struct sockaddr *)&a, &len);
        if (k >= 0) {
            if (from) *from = (NetAddr){ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)};
            return k;
        }
        int e = net_errno();
#ifdef _WIN32
        if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;   // an ICMP error, or a datagram over max: the next one
#else
        if (e == ECONNREFUSED) continue;
#endif
        if (WOULD_BLOCK(e)) return 0;
        set_err("recvfrom");
        return -1;
    }
}

int net_udp_wait(int s, int ms) {
#ifdef _WIN32
    WSAPOLLFD f = {(SOCKET)s, POLLRDNORM, 0};
    return WSAPoll(&f, 1, ms) > 0;
#else
    struct pollfd f = {s, POLLIN, 0};
    return poll(&f, 1, ms) > 0;
#endif
}

uint32_t net_udp_local_ip(uint32_t toward) {
    wsa_start();
    int s = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return 0;
    struct sockaddr_in a = sin_of((NetAddr){toward ? toward : 0x01010101u, 9});   // 1.1.1.1: only routes, nothing sent
    uint32_t ip = 0;
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
        socklen_t len = sizeof a;
        if (getsockname(s, (struct sockaddr *)&a, &len) == 0) ip = ntohl(a.sin_addr.s_addr);
    }
    closesocket(s);
    return ip;
}

uint32_t net_resolve(const char *name) {
#ifdef __PROSPERO__
    (void)name;
    return 0;
#else
    wsa_start();
    struct addrinfo hint, *res = NULL;
    memset(&hint, 0, sizeof hint);
    hint.ai_family = AF_INET, hint.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(name, NULL, &hint, &res) || !res) return 0;
    uint32_t ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return ip;
#endif
}

uint32_t net_ip_parse(const char *s) {
    unsigned a, b, c, d;
    char tail;
    if (!s || sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    return a << 24 | b << 16 | c << 8 | d;
}

void net_ip_format(uint32_t ip, char out[16]) {
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
}
