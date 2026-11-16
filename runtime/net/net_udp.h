// One UDP socket of the host system, IPv4 only: Linux and the PS5 (BSD sockets), Windows (Winsock). The session engine
// (net_session.c), the lobby client and the lobby server (server/lobby) use only this. Addresses are host byte order.
#pragma once
#include <stdint.h>

typedef struct { uint32_t ip; uint16_t port; } NetAddr;   // host order; ip 0 = none

enum { NET_UDP_BROADCAST = 1 };   // net_udp_open flags: may send to broadcast addresses

// A non-blocking socket bound to ip:port (0 any); -1 on failure (net_udp_error says why). *bound gets the port taken.
int net_udp_open(uint32_t ip, uint16_t port, int flags, uint16_t *bound);
void net_udp_close(int s);
int net_udp_send(int s, const void *p, int n, NetAddr to);          // bytes sent, -1 error (a full buffer counts as sent: UDP)
int net_udp_recv(int s, void *p, int max, NetAddr *from);           // bytes, 0 nothing waiting, -1 error
// Waits until s is readable or ms pass (ms < 0: no limit). 1 readable, 0 timeout.
int net_udp_wait(int s, int ms);
const char *net_udp_error(void);
// The local IPv4 address the system would use to reach `toward` (a UDP connect, nothing sent): the LAN interface. 0 if
// none. With toward 0, a public address is used to find the default route.
uint32_t net_udp_local_ip(uint32_t toward);
uint32_t net_ip_parse(const char *s);                  // "a.b.c.d" -> host order, 0 if not an address
// A host name's IPv4 address (getaddrinfo; blocking), 0 if none. Linux and Windows; the PS5 build takes addresses only.
uint32_t net_resolve(const char *name);
void net_ip_format(uint32_t ip, char out[16]);
