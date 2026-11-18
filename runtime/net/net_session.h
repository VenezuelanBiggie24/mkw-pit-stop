// One local wireless session over UDP (docs/ps5/multiplayer.md): the host (the game's access point) or a station, its
// node table and the game's datagrams. No threads, no runtime: the caller owns the lock, calls net_session_poll when
// the socket is readable (net_session_fd) or every few tens of milliseconds, and gets the game's datagrams through
// on_data. The nn::ldn and nn::socket replacements (net_ldn.c, net_socket.c) drive it; tests/test_net.py drives two of
// them over loopback.
//
// Star topology: every station talks to the host only, the host forwards datagrams between stations. Only the host
// needs to be reachable (one UDP port), the relay carries a session unchanged, and the host checks every source.
// Addresses inside the session are the virtual ones the ldn session hands out: 169.254.X.(node + 1)/24, X random.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "ldn_types.h"
#include "net_proto.h"
#include "net_udp.h"

typedef struct NetSession NetSession;

typedef struct {
    uint8_t compat[NET_COMPAT];
    char password[NET_PASSWORD_MAX + 1];   // "" for an open session
    uint16_t port;                         // host: the listening port (NET_PORT_DEFAULT); station: 0, any
    uint32_t bind_ip;                      // 0: every interface
    NetAddr lobby;                         // the lobby/relay (net_lobby.h), ip 0 none: only then is M_RELAY accepted
    int (*random)(void *out, size_t n);    // 1 on success
    uint64_t (*now_ms)(void);
    void (*log)(const char *fmt, ...);
    // Under the caller's lock, from net_session_poll/net_session_send: a game datagram for this node (dst_ip this
    // node's address or the session's broadcast), and a change of the node table or of the session's state.
    void (*on_data)(void *ctx, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
                    const uint8_t *p, int n);
    void (*on_change)(void *ctx);
    void *ctx;
} NetConfig;

enum { NET_IDLE, NET_SCANNING, NET_HOSTING, NET_JOINING, NET_JOINED, NET_GONE };

// A host found by a scan.
typedef struct {
    NetAddr addr;      // the host's UDP address (pseudo address for one reached through the relay)
    int via_relay;
    uint8_t flags;     // NET_ADV_*
    uint8_t info[LDN_NETWORK_INFO];
} NetFound;
enum { NET_FOUND_MAX = 16 };

NetSession *net_session_new(const NetConfig *cfg);   // NULL if its socket cannot be opened (net_udp_error)
void net_session_free(NetSession *s);                // says goodbye (LEAVE) first when in a session
int net_session_fd(const NetSession *s);
uint16_t net_session_port(const NetSession *s);
int net_session_state(const NetSession *s);
int net_session_reason(const NetSession *s);         // ldn DisconnectReason once NET_GONE
void net_session_poll(NetSession *s);                // reads every waiting datagram, runs timers

// Host: the NetworkInfo to serve; the engine owns its node table and NodeCount from now on and fills node 0 (the
// host) but its name. Returns 0 if not idle.
int net_session_host(NetSession *s, const uint8_t info[LDN_NETWORK_INFO]);
uint8_t *net_session_info(NetSession *s);           // current NetworkInfo (the host may change advertise data, policy)
void net_session_info_changed(NetSession *s);       // host: send it to every station now
void net_session_lock(NetSession *s, int locked);   // host: refuse new stations (also the game's RejectAll policy)
int net_session_kick(NetSession *s, uint32_t vip);  // host: remove that station (LEAVE Rejected); 0 if none

// Station: discovery on the LAN (DISCOVER to every address in to, a broadcast or a host), then the hosts that answered.
void net_session_scan(NetSession *s, const NetAddr *to, int n);
int net_session_found(const NetSession *s, const NetFound **out);
int net_session_found_put(NetSession *s, const NetFound *f);   // a host the lobby listed; its index, -1 if full
void net_session_password(NetSession *s, const char *password);   // the join password (station: before joining)
// Joins the host at addr (a scan's NetFound.addr): HELLO, the password proof, JOIN. Then poll until NET_JOINED or
// NET_GONE (reason: net_session_refused).
int net_session_join(NetSession *s, NetAddr addr, int via_relay, const uint8_t session_id[16],
                     const uint8_t user_config[LDN_USER_CONFIG], uint16_t lcv, const uint8_t security[LDN_SECURITY_CONFIG]);
int net_session_refused(const NetSession *s);       // NET_REFUSE_* the host gave, 0 none
uint32_t net_session_vip(const NetSession *s);       // this node's virtual address, 0 before joining
uint32_t net_session_mask(void);                    // 255.255.255.0

// A game datagram from this node's port src_port to dst_ip:dst_port (a node or the broadcast address). 0 if it cannot
// go (no session, too long, unknown address).
int net_session_send(NetSession *s, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const void *p, int n);

// For the lobby client (net_lobby.c): send a raw datagram from the session's socket (the lobby sees this port), and a
// lobby message the engine received and does not handle itself.
int net_session_raw(NetSession *s, NetAddr to, const void *p, int n);
void net_session_on_lobby(NetSession *s, void (*fn)(void *ctx, int type, const uint8_t *body, int len), void *ctx);
// Host: the lobby introduces a station at addr (its public address): a few clear DISCOVER-sized punches open the
// host's router for it.
void net_session_punch(NetSession *s, NetAddr addr);
// The lobby's session number this node's relayed traffic belongs to (host: its registration; station: the session it
// connects to). Set before net_session_join for a station that may fall back to the relay.
void net_session_relay(NetSession *s, uint32_t session);
// Host: the game's SecurityConfig (passphrase) a station's JOIN must carry, as the console's ldn checks it.
void net_session_security(NetSession *s, const uint8_t security[LDN_SECURITY_CONFIG]);

typedef struct {
    uint64_t tx_bytes, rx_bytes, tx_dgrams, rx_dgrams, dropped, refused;
    uint32_t rtt_ms[LDN_NODES];   // host: per node; station: [0] the host
} NetStats;
void net_session_stats(const NetSession *s, NetStats *out);
