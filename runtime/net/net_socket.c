// nn::socket for MKW: the game's sockets carried inside the local wireless session (net_ldn.c), never the host's
// network directly. MKW's own traffic over local wireless is PIA's (nn::pia) UDP: Socket, Bind, SetSockOpt
// (SO_REUSEADDR 4, SO_BROADCAST 0x20, SO_REUSEPORT 0x200, IP option 4; main+0x15182c0..0x1518550), SendTo and RecvFrom
// with MSG_DONTWAIT (0x80, main+0x15186a8) on the addresses nn::ldn hands out. Each socket here is virtual: a port in
// this node's 169.254.X.0/24 session address, its datagrams sent through the session (net_game_send) and delivered
// into its queue (net_sock_deliver). Everything else is refused as a console without a network would: no name lookup
// (GetAddrInfo fails, so the game's online client never resolves Nintendo's servers), no address outside the session.
// Before this file, every nn::socket call reached the console's "bsd:u" service, which the runtime does not serve.
//
// Errors are nn::socket::Errno, the Linux numbers (Ryujinx's BSD service answers with LinuxError for the same reason);
// structures are BSD's (sockaddr_in with sin_len, pollfd). Blocking calls give the virtual core up while they wait.
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "net_game.h"
#include "platform/platform.h"

enum {
    E_INTR = 4, E_BADF = 9, E_AGAIN = 11, E_NOMEM = 12, E_FAULT = 14, E_INVAL = 22, E_MFILE = 24, E_MSGSIZE = 90,
    E_PROTOTYPE = 91, E_PROTONOSUPPORT = 93, E_AFNOSUPPORT = 97, E_ADDRINUSE = 98, E_ADDRNOTAVAIL = 99,
    E_NETUNREACH = 101,
};
enum { AF_INET_ = 2, SOCK_DGRAM_ = 2, SOCK_TYPE_MASK = 0xffff, SOCK_NONBLOCK_ = 0x20000000, MSG_DONTWAIT_ = 0x80,
       SOL_SOCKET_ = 0xffff, SO_REUSEADDR_ = 4, SO_BROADCAST_ = 0x20, SO_REUSEPORT_ = 0x200 };
enum { POLLIN_ = 1, POLLOUT_ = 4, POLLNVAL_ = 0x20 };
enum { FDS = 64, FD_BASE = 3, QUEUE_DGRAMS = 256, QUEUE_BYTES = 512 * 1024, PORT_FIRST = 49152 };

typedef struct Dgram { struct Dgram *next; uint32_t src_ip; uint16_t src_port; int n; uint8_t p[]; } Dgram;

typedef struct {
    int kind;                 // 0 free, 1 UDP socket, 2 event fd
    int nonblock, reuse, broadcast, shut;
    uint32_t bind_ip;         // host order, 0 any
    uint16_t port;            // 0 not bound
    Dgram *head, *tail;
    int queued, queued_bytes;
    uint64_t counter;         // event fd
    int semaphore;
} Fd;

static PlatMutex lock = PLAT_MUTEX_INIT;
static PlatCond ready;
static int ready_init;
static Fd fds[FDS];
static uint16_t next_port = PORT_FIRST;
static __thread int last_errno;
static uint64_t dropped;   // datagrams a full queue refused (the first one is logged)

typedef struct { uint8_t len, family; uint16_t port; uint32_t addr; uint8_t zero[8]; } SockAddrIn;   // BSD, network order
typedef struct { int32_t fd; int16_t events, revents; } PollFd;
_Static_assert(sizeof(SockAddrIn) == 16 && sizeof(PollFd) == 8, "nn::socket ABI");

static uint16_t bswap16(uint16_t v) { return (uint16_t)(v >> 8 | v << 8); }
static uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }

static int64_t fail(int e) { last_errno = e; return -1; }

static void init_once(void) {
    if (!ready_init) plat_cond_init(&ready), ready_init = 1;
}

static Fd *get(int fd, int kind) {
    int i = fd - FD_BASE;
    return i >= 0 && i < FDS && fds[i].kind && (!kind || fds[i].kind == kind) ? &fds[i] : NULL;
}

static void drain(Fd *f) {
    for (Dgram *d = f->head, *n; d; d = n) n = d->next, free(d);
    f->head = f->tail = NULL, f->queued = f->queued_bytes = 0;
}

static int alloc_fd(int kind) {
    for (int i = 0; i < FDS; i++)
        if (!fds[i].kind) {
            memset(&fds[i], 0, sizeof fds[i]);
            fds[i].kind = kind;
            return i + FD_BASE;
        }
    return -1;
}

static int port_taken(uint16_t port, const Fd *self) {
    for (int i = 0; i < FDS; i++)
        if (&fds[i] != self && fds[i].kind == 1 && fds[i].port == port) return fds[i].reuse && self->reuse ? 0 : 1;
    return 0;
}

static uint16_t ephemeral(const Fd *self) {
    for (int k = 0; k < 16384; k++) {
        uint16_t p = next_port++;
        if (next_port < PORT_FIRST) next_port = PORT_FIRST;
        if (!port_taken(p, self)) return p;
    }
    return 0;
}

// Waits on the game's thread, the virtual core given up, until ready is signalled or ms pass (ms < 0: no limit).
// The core is taken back with the lock released, as net_ldn.c's wait_for (a thread blocked on the lock keeps its core).
static void wait_ready(int ms) {
    int away = kern_core_away();
    // 1 ms sleeps with the lock released, as net_ldn.c's wait_for: the callers loop and look at their own deadline
    // (PIA reads with MSG_DONTWAIT, so this path is rare)
    (void)ms;
    plat_mutex_unlock(&lock);
    plat_sleep_ms(1);
    plat_mutex_lock(&lock);
    plat_mutex_unlock(&lock);
    kern_core_back(&away);
    plat_mutex_lock(&lock);
}

// ---- from the session
void net_sock_deliver(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t *p, int n) {
    plat_mutex_lock(&lock);
    init_once();
    int bc = (dst_ip & 0xff) == 0xff || dst_ip == 0xffffffffu, got = 0;
    for (int i = 0; i < FDS; i++) {
        Fd *f = &fds[i];
        if (f->kind != 1 || f->port != dst_port || f->shut) continue;
        if (f->bind_ip && f->bind_ip != dst_ip && !bc) continue;
        if (bc && !f->broadcast && !f->reuse) {}   // a console delivers broadcasts to every bound socket anyway
        if (f->queued >= QUEUE_DGRAMS || f->queued_bytes + n > QUEUE_BYTES) { dropped++; continue; }
        Dgram *d = malloc(sizeof *d + (size_t)n);
        if (!d) { dropped++; continue; }
        d->next = NULL, d->src_ip = src_ip, d->src_port = src_port, d->n = n;
        memcpy(d->p, p, (size_t)n);
        if (f->tail) f->tail->next = d; else f->head = d;
        f->tail = d, f->queued++, f->queued_bytes += n, got = 1;
        if (!bc) break;   // unicast: the first socket on the port (SO_REUSEPORT's last binder gets it on BSD; PIA binds one)
    }
    if (got) plat_cond_broadcast(&ready);
    uint64_t lost = dropped;
    plat_mutex_unlock(&lock);
    static volatile int32_t said;
    if (lost && !plat_xchg32(&said, 1)) rt_log("net: a game socket's queue is full (the game reads too slowly): datagrams dropped");
}

void net_sock_session_ended(void) {
    plat_mutex_lock(&lock);
    init_once();
    for (int i = 0; i < FDS; i++) if (fds[i].kind == 1) drain(&fds[i]);
    plat_cond_broadcast(&ready);
    plat_mutex_unlock(&lock);
}

// ---- nn::socket
void net_ov_ok(Cpu *c) { ret_x(c, 0); }
void net_ov_nothing(Cpu *c) { (void)c; }
void net_ov_true(Cpu *c) { ret_x(c, 1); }

// int Socket(int domain, int type, int protocol)
void net_ov_socket(Cpu *c) {
    int domain = (int)arg_x(c, 0), type = (int)arg_x(c, 1), proto = (int)arg_x(c, 2);
    if (domain != AF_INET_) { ret_x(c, (uint64_t)fail(E_AFNOSUPPORT)); return; }
    if ((type & SOCK_TYPE_MASK) != SOCK_DGRAM_) { ret_x(c, (uint64_t)fail(E_PROTOTYPE)); return; }   // no TCP: nothing uses it
    if (proto != 0 && proto != 17) { ret_x(c, (uint64_t)fail(E_PROTONOSUPPORT)); return; }
    plat_mutex_lock(&lock);
    init_once();
    int fd = alloc_fd(1);
    if (fd >= 0) get(fd, 1)->nonblock = (type & SOCK_NONBLOCK_) != 0;
    plat_mutex_unlock(&lock);
    if (fd < 0) { ret_x(c, (uint64_t)fail(E_MFILE)); return; }
    static volatile int32_t said;
    if (!plat_xchg32(&said, 1)) rt_log("net: the game opened its first socket (UDP, carried in the local wireless session)");
    ret_x(c, (uint64_t)(int64_t)fd);
}

// int Bind(int, const sockaddr *, socklen_t)
void net_ov_bind(Cpu *c) {
    int fd = (int)arg_x(c, 0);
    const SockAddrIn *a = arg_p(c, 1);
    uint32_t len = (uint32_t)arg_x(c, 2);
    if (!a || len < sizeof *a) { ret_x(c, (uint64_t)fail(E_INVAL)); return; }
    if (a->family != AF_INET_) { ret_x(c, (uint64_t)fail(E_AFNOSUPPORT)); return; }
    uint32_t ip = bswap32(a->addr), vip = net_game_vip();
    if (ip && ip != vip) { ret_x(c, (uint64_t)fail(E_ADDRNOTAVAIL)); return; }
    plat_mutex_lock(&lock);
    Fd *f = get(fd, 1);
    int64_t r = 0;
    if (!f) r = fail(E_BADF);
    else if (f->port) r = fail(E_INVAL);
    else {
        uint16_t port = bswap16(a->port);
        if (!port) port = ephemeral(f);
        if (!port || port_taken(port, f)) r = fail(E_ADDRINUSE);
        else f->port = port, f->bind_ip = ip;
    }
    plat_mutex_unlock(&lock);
    ret_x(c, (uint64_t)r);
}

// int SetSockOpt(int, Level, Option, const void *, socklen_t): the options PIA sets are kept or accepted
void net_ov_setsockopt(Cpu *c) {
    int fd = (int)arg_x(c, 0), level = (int)arg_x(c, 1), opt = (int)arg_x(c, 2);
    const int32_t *v = arg_p(c, 3);
    plat_mutex_lock(&lock);
    Fd *f = get(fd, 1);
    int64_t r = f ? 0 : fail(E_BADF);
    if (f && level == SOL_SOCKET_ && v) {
        if (opt == SO_REUSEADDR_ || opt == SO_REUSEPORT_) f->reuse = *v != 0;
        else if (opt == SO_BROADCAST_) f->broadcast = *v != 0;
    }
    plat_mutex_unlock(&lock);
    ret_x(c, (uint64_t)r);
}

// ssize_t SendTo(int, const void *, size_t, int flags, const sockaddr *, socklen_t)
void net_ov_sendto(Cpu *c) {
    int fd = (int)arg_x(c, 0);
    const void *p = arg_p(c, 1);
    uint64_t n = arg_x(c, 2);
    const SockAddrIn *a = arg_p(c, 4);
    if (!a || arg_x(c, 5) < sizeof *a || (n && !p)) { ret_x(c, (uint64_t)fail(E_INVAL)); return; }
    if (n > 65507) { ret_x(c, (uint64_t)fail(E_MSGSIZE)); return; }
    uint32_t dst = bswap32(a->addr), vip = net_game_vip();
    uint16_t dport = bswap16(a->port);
    plat_mutex_lock(&lock);
    Fd *f = get(fd, 1);
    int64_t r = (int64_t)n;
    if (!f) r = fail(E_BADF);
    else if (!vip || (dst != 0xffffffffu && (dst & 0xffffff00u) != (vip & 0xffffff00u))) r = fail(E_NETUNREACH);
    else if (!f->port && !(f->port = ephemeral(f))) r = fail(E_ADDRINUSE);
    uint16_t sport = f ? f->port : 0;
    plat_mutex_unlock(&lock);
    if (r >= 0 && !net_game_send(sport, dst == 0xffffffffu ? (vip | 0xff) : dst, dport, p, (int)n)) {
        static volatile int32_t said;
        if (n > 1428 && !plat_xchg32(&said, 1)) rt_log("net: a game datagram of %llu bytes is over the session's 1428: dropped", (unsigned long long)n);
        r = n > 1428 ? fail(E_MSGSIZE) : fail(E_NETUNREACH);
    }
    ret_x(c, (uint64_t)r);
}

// ssize_t RecvFrom(int, void *, size_t, int flags, sockaddr *, socklen_t *)
void net_ov_recvfrom(Cpu *c) {
    int fd = (int)arg_x(c, 0), flags = (int)arg_x(c, 3);
    uint8_t *p = arg_p(c, 1);
    uint64_t max = arg_x(c, 2);
    SockAddrIn *from = arg_p(c, 4);
    uint32_t *fromlen = arg_p(c, 5);
    plat_mutex_lock(&lock);
    init_once();
    int64_t r;
    for (;;) {
        Fd *f = get(fd, 1);
        if (!f) { r = fail(E_BADF); break; }
        if (f->head) {
            Dgram *d = f->head;
            f->head = d->next;
            if (!f->head) f->tail = NULL;
            f->queued--, f->queued_bytes -= d->n;
            uint64_t k = (uint64_t)d->n < max ? (uint64_t)d->n : max;   // a longer datagram is cut, as UDP does
            if (k && p) memcpy(p, d->p, k);
            if (from && fromlen && *fromlen >= sizeof *from) {
                *from = (SockAddrIn){sizeof *from, AF_INET_, bswap16(d->src_port), bswap32(d->src_ip), {0}};
                *fromlen = sizeof *from;
            }
            free(d);
            r = (int64_t)k;
            break;
        }
        if (f->nonblock || (flags & MSG_DONTWAIT_) || f->shut) { r = fail(E_AGAIN); break; }
        wait_ready(-1);
    }
    plat_mutex_unlock(&lock);
    ret_x(c, (uint64_t)r);
}

static int revents_of(const PollFd *q) {
    Fd *f = get(q->fd, 0);
    if (!f) return POLLNVAL_;
    int r = 0;
    if (f->kind == 1) r = (f->head ? POLLIN_ : 0) | POLLOUT_;
    else r = (f->counter ? POLLIN_ : 0) | POLLOUT_;
    return r & (q->events | ~(POLLIN_ | POLLOUT_));
}

// int Poll(pollfd *, nfds_t, int timeout_ms)
void net_ov_poll(Cpu *c) {
    PollFd *q = arg_p(c, 0);
    uint64_t n = arg_x(c, 1);
    int timeout = (int)arg_x(c, 2);
    if (n > 256 || (n && !q)) { ret_x(c, (uint64_t)fail(E_INVAL)); return; }
    uint64_t end = plat_now_ms() + (timeout > 0 ? (uint64_t)timeout : 0);
    plat_mutex_lock(&lock);
    init_once();
    int count;
    for (;;) {
        count = 0;
        for (uint64_t i = 0; i < n; i++) {
            q[i].revents = (int16_t)(q[i].fd < 0 ? 0 : revents_of(&q[i]));
            count += q[i].revents != 0;
        }
        if (count || timeout == 0) break;
        uint64_t t = plat_now_ms();
        if (timeout > 0 && t >= end) break;
        wait_ready(timeout < 0 ? -1 : (int)(end - t));
    }
    plat_mutex_unlock(&lock);
    ret_x(c, (uint64_t)(int64_t)count);
}

void net_ov_close(Cpu *c) {
    int fd = (int)arg_x(c, 0);
    plat_mutex_lock(&lock);
    Fd *f = get(fd, 0);
    int64_t r = 0;
    if (!f) r = fail(E_BADF);
    else drain(f), f->kind = 0;
    if (ready_init) plat_cond_broadcast(&ready);
    plat_mutex_unlock(&lock);
    ret_x(c, (uint64_t)r);
}

// int Shutdown(int, int how): a waiting RecvFrom returns
void net_ov_shutdown(Cpu *c) {
    plat_mutex_lock(&lock);
    Fd *f = get((int)arg_x(c, 0), 1);
    int64_t r = f ? 0 : fail(E_BADF);
    if (f) f->shut = 1;
    if (ready_init) plat_cond_broadcast(&ready);
    plat_mutex_unlock(&lock);
    ret_x(c, (uint64_t)r);
}

// int EventFd(uint64_t initval, EventFdFlags): bit 0 is taken as semaphore mode and any other bit as non-blocking
// (the flag values are not in the executable's symbols; the only caller, main+0x1021070, is a library's wakeup).
void net_ov_eventfd(Cpu *c) {
    plat_mutex_lock(&lock);
    init_once();
    int fd = alloc_fd(2);
    if (fd >= 0) {
        Fd *f = get(fd, 2);
        uint32_t flags = (uint32_t)arg_x(c, 1);
        f->counter = arg_x(c, 0), f->semaphore = flags & 1, f->nonblock = (flags & ~1u) != 0;
    }
    plat_mutex_unlock(&lock);
    ret_x(c, (uint64_t)(fd >= 0 ? fd : fail(E_MFILE)));
}

// int EventFdRead(int, uint64_t *)
void net_ov_eventfd_read(Cpu *c) {
    uint64_t *out = arg_p(c, 1);
    plat_mutex_lock(&lock);
    int64_t r;
    for (;;) {
        Fd *f = get((int)arg_x(c, 0), 2);
        if (!f || !out) { r = fail(f ? E_FAULT : E_BADF); break; }
        if (f->counter) {
            *out = f->semaphore ? 1 : f->counter;
            f->counter = f->semaphore ? f->counter - 1 : 0;
            r = 0;
            break;
        }
        if (f->nonblock) { r = fail(E_AGAIN); break; }
        wait_ready(-1);
    }
    plat_mutex_unlock(&lock);
    ret_x(c, (uint64_t)r);
}

// int EventFdWrite(int, uint64_t)
void net_ov_eventfd_write(Cpu *c) {
    plat_mutex_lock(&lock);
    Fd *f = get((int)arg_x(c, 0), 2);
    int64_t r = f ? 0 : fail(E_BADF);
    if (f) f->counter += arg_x(c, 1);
    if (f) plat_cond_broadcast(&ready);
    plat_mutex_unlock(&lock);
    ret_x(c, (uint64_t)r);
}

void net_ov_last_error(Cpu *c) { ret_x(c, (uint64_t)(int64_t)last_errno); }

// int GetAddrInfo(...): no name is ever resolved (EAI_NONAME, 8 in BSD's numbering): the game's online client stops here
void net_ov_getaddrinfo(Cpu *c) {
    void **res = arg_p(c, 3);
    if (res) *res = NULL;
    static volatile int32_t said;
    if (!plat_xchg32(&said, 1)) rt_log("net: the game asked to resolve a server name: refused (no online service)");
    ret_x(c, 8);
}

static void ov_gai_strerror(Cpu *c) {
    static const char text[] = "name resolution is not available";
    ret_x(c, (uint64_t)(uintptr_t)text);
}

static void ov_htonl(Cpu *c) { ret_x(c, bswap32((uint32_t)arg_x(c, 0))); }
static void ov_htons(Cpu *c) { ret_x(c, bswap16((uint16_t)arg_x(c, 0))); }

// int InetPton(int af, const char *src, void *dst): 1, 0 for text that is not an address
static void ov_pton(Cpu *c) {
    const char *s = arg_p(c, 1);
    uint32_t *dst = arg_p(c, 2);
    if ((int)arg_x(c, 0) != AF_INET_) { ret_x(c, (uint64_t)fail(E_AFNOSUPPORT)); return; }
    unsigned v[4], k = 0, x = 0, digits = 0;
    for (const char *p = s; p && k < 4; p++) {
        if (*p >= '0' && *p <= '9' && digits < 3) x = x * 10 + (unsigned)(*p - '0'), digits++;
        else if ((*p == '.' || !*p) && digits && x < 256) { v[k++] = x, x = 0, digits = 0; if (!*p) break; }
        else { k = 99; break; }
    }
    if (k != 4 || !dst) { ret_x(c, 0); return; }
    *dst = bswap32(v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]);
    ret_x(c, 1);
}

// const char *InetNtop(int af, const void *src, char *dst, socklen_t size)
static void ov_ntop(Cpu *c) {
    const uint32_t *src = arg_p(c, 1);
    char *dst = arg_p(c, 2);
    uint32_t size = (uint32_t)arg_x(c, 3);
    if ((int)arg_x(c, 0) != AF_INET_ || !src || !dst || size < 16) { ret_x(c, (uint64_t)fail(E_AFNOSUPPORT)); ret_x(c, 0); return; }
    uint32_t ip = bswap32(*src);
    char t[16];
    int n = 0;
    for (int i = 0; i < 4; i++) {
        unsigned b = ip >> (24 - 8 * i) & 255;
        if (b >= 100) t[n++] = (char)('0' + b / 100);
        if (b >= 10) t[n++] = (char)('0' + b / 10 % 10);
        t[n++] = (char)('0' + b % 10);
        t[n++] = i < 3 ? '.' : 0;
    }
    memcpy(dst, t, (size_t)n);
    ret_x(c, (uint64_t)(uintptr_t)dst);
}

HOST_OVERRIDE("_ZN2nn6socket10InitializeEPvmmi", net_ov_ok);
HOST_OVERRIDE("_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE", ov_gai_strerror);
HOST_OVERRIDE("_ZN2nn6socket9InetHtonlEj", ov_htonl);
HOST_OVERRIDE("_ZN2nn6socket9InetNtohlEj", ov_htonl);
HOST_OVERRIDE("_ZN2nn6socket9InetHtonsEt", ov_htons);
HOST_OVERRIDE("_ZN2nn6socket9InetNtohsEt", ov_htons);
HOST_OVERRIDE("_ZN2nn6socket8InetPtonEiPKcPv", ov_pton);
HOST_OVERRIDE("_ZN2nn6socket8InetNtopEiPKvPcj", ov_ntop);

