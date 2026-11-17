// Between the nn::ldn replacements (net_ldn.c), which own the session, and the nn::socket replacements
// (net_socket.c), which carry the game's datagrams inside it.
#pragma once
#include <stdint.h>

// net_ldn.c. A game datagram out of this node (0 when no session carries it: the socket answers ENETUNREACH); this
// node's virtual address and the session's broadcast address, 0 outside a session.
int net_game_send(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const void *p, int n);
uint32_t net_game_vip(void);
uint32_t net_game_broadcast(void);
int net_game_enabled(void);   // MKW_LOCAL_WIRELESS is not off and ldn is initialized: the sockets are ours
void net_game_mode_reread(void);   // MKW_LOCAL_WIRELESS read again (probes)

// net_socket.c. A game datagram for this node from the session (any thread, never under a socket's lock), and the
// session's end: datagrams waiting are dropped, sockets stay open as on the console.
void net_sock_deliver(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t *p, int n);
void net_sock_session_ended(void);

// nn::socket inside the local wireless session (net_socket.c): sdk/ov/ov_socket.c owns the entry points and hands
// each call here while local wireless is on and ldn initialized (net_game_enabled), to the host's sockets (MKW_LAN)
// otherwise.
#include "cpu.h"
void net_ov_bind(Cpu *c);
void net_ov_close(Cpu *c);
void net_ov_eventfd(Cpu *c);
void net_ov_eventfd_read(Cpu *c);
void net_ov_eventfd_write(Cpu *c);
void net_ov_getaddrinfo(Cpu *c);
void net_ov_last_error(Cpu *c);
void net_ov_nothing(Cpu *c);
void net_ov_ok(Cpu *c);
void net_ov_poll(Cpu *c);
void net_ov_recvfrom(Cpu *c);
void net_ov_sendto(Cpu *c);
void net_ov_setsockopt(Cpu *c);
void net_ov_shutdown(Cpu *c);
void net_ov_socket(Cpu *c);
void net_ov_true(Cpu *c);
