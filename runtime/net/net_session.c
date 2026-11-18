// One local wireless session over UDP: host or station (net_session.h).
//
// Timing: a station says HELLO every 250 ms until challenged and repeats PROOF + JOIN until welcomed, up to 5 s; every
// joined peer is pinged every 500 ms and dropped after 5 s of silence (ldn's SignalLost); the host sends the whole
// NetworkInfo (SYNC) on every change and once a second, so a lost SYNC costs at most a second and no message needs
// acknowledging. The game's own datagrams ride unreliable, as over the console's radio: its PIA layer resends what it
// needs.
#include "net_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_lobby.h"

enum {
    HELLO_EVERY_MS = 250, JOIN_GIVE_UP_MS = 5000, RELAY_AFTER_MS = 2000, PING_EVERY_MS = 500, SILENT_MS = 5000,
    SYNC_EVERY_MS = 1000, HANDSHAKE_MS = 10000, PEERS = 16, PENDING_MAX = 8,
    // per address, before a key: messages a second (burst twice), wrong proofs before a 60 s block
    CLEAR_RATE = 20, PROOF_TRIES = 5, BLOCK_MS = 60000, ADDRS = 64,
    // per station, game datagrams: 1 MiB/s and 1000 datagrams/s (bursts twice); PIA sends far less (docs/ps5/multiplayer.md)
    DATA_BYTES = 1 << 20, DATA_DGRAMS = 1000,
};

typedef enum { P_FREE, P_CHALLENGED, P_AUTHED, P_JOINED } PeerState;

typedef struct {
    PeerState st;
    NetAddr addr;
    int via_relay;            // addr.port is the relay's flow, addr.ip 0
    uint8_t node;             // 1-7 once joined
    uint8_t pk[32], sk[32], nonce[32], shared[32];
    uint8_t hello[NET_HEADER + L_HELLO], challenge[NET_HEADER + L_CHALLENGE];
    NetKeys keys;
    uint64_t born, last_rx, last_ping;
    uint32_t rtt;
    double tok_bytes, tok_dgrams;   // DATA budget
    uint64_t tok_at;
} Peer;

typedef struct { NetAddr addr; double tokens; uint64_t at, blocked_until; int bad_proofs; uint64_t first_bad; } AddrRate;

struct NetSession {
    NetConfig cfg;
    int sock, state, reason, refused, locked;
    uint16_t port;
    uint8_t info[LDN_NETWORK_INFO];
    uint8_t subnet;                 // X of 169.254.X.0/24
    // host: peers[] (handshakes and stations); station: peers[0] is the host
    Peer peers[PEERS];
    AddrRate rates[ADDRS];
    // station
    uint8_t my_node, my_sk[32], my_pk[32], session_id[16];
    uint8_t join_body[L_JOIN];
    uint64_t join_started, join_sent;
    NetAddr join_direct;            // the host's direct address while trying it before the relay
    uint32_t relay_session;
    uint8_t security[LDN_SECURITY_CONFIG];   // host: the game's passphrase
    int has_security;
    NetFound found[NET_FOUND_MAX];
    int nfound;
    uint64_t last_sync;
    void (*lobby_fn)(void *ctx, int type, const uint8_t *body, int len);
    void *lobby_ctx;
    NetStats stats;
};

static uint64_t now(NetSession *s) { return s->cfg.now_ms(); }

#define LOG(...) do { if (s->cfg.log) s->cfg.log(__VA_ARGS__); } while (0)

uint32_t net_session_mask(void) { return 0xffffff00u; }
static uint32_t vip_of(NetSession *s, int node) { return 0xa9fe0000u | (uint32_t)s->subnet << 8 | (uint32_t)(node + 1); }
static uint32_t broadcast_of(NetSession *s) { return 0xa9fe0000u | (uint32_t)s->subnet << 8 | 0xff; }
static int is_host(const NetSession *s) { return s->state == NET_HOSTING; }

NetSession *net_session_new(const NetConfig *cfg) {
    NetSession *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->cfg = *cfg;
    s->sock = net_udp_open(cfg->bind_ip, cfg->port, NET_UDP_BROADCAST, &s->port);
    if (s->sock < 0) { free(s); return NULL; }
    return s;
}

int net_session_fd(const NetSession *s) { return s->sock; }
uint16_t net_session_port(const NetSession *s) { return s->port; }
int net_session_state(const NetSession *s) { return s->state; }
int net_session_reason(const NetSession *s) { return s->reason; }
int net_session_refused(const NetSession *s) { return s->refused; }
uint8_t *net_session_info(NetSession *s) { return s->info; }
uint32_t net_session_vip(const NetSession *s) {
    return s->state == NET_HOSTING || s->state == NET_JOINED ? ld32(ni_node_c(s->info, s->my_node) + NODE_IP) : 0;
}
int net_session_found(const NetSession *s, const NetFound **out) { *out = s->found; return s->nfound; }
int net_session_found_put(NetSession *s, const NetFound *f) {
    for (int i = 0; i < s->nfound; i++)
        if (!memcmp(s->found[i].info + NI_SESSION_ID, f->info + NI_SESSION_ID, 16)) { s->found[i] = *f; return i; }
    if (s->nfound == NET_FOUND_MAX) return -1;
    s->found[s->nfound] = *f;
    return s->nfound++;
}
void net_session_password(NetSession *s, const char *password) {
    snprintf(s->cfg.password, sizeof s->cfg.password, "%s", password ? password : "");
}
void net_session_on_lobby(NetSession *s, void (*fn)(void *, int, const uint8_t *, int), void *ctx) {
    s->lobby_fn = fn, s->lobby_ctx = ctx;
}
void net_session_stats(const NetSession *s, NetStats *out) {
    *out = s->stats;
    for (int i = 0; i < PEERS; i++) {
        const Peer *p = &s->peers[i];
        if (p->st != P_JOINED) continue;
        out->rtt_ms[s->state == NET_HOSTING ? p->node : 0] = p->rtt;
    }
}

// ---- datagrams out: direct, or wrapped for the relay
static int xmit(NetSession *s, NetAddr to, int via_relay, const uint8_t *d, int n) {
    if (n <= 0) return 0;
    int k;
    if (via_relay) {
        uint8_t w[NET_WIRE_MAX], body[NET_RELAY_HEAD + NET_DGRAM_MAX];
        uint32_t sess = s->relay_session;
        body[0] = (uint8_t)(sess >> 24), body[1] = (uint8_t)(sess >> 16), body[2] = (uint8_t)(sess >> 8);
        body[3] = (uint8_t)sess, body[4] = (uint8_t)(to.port >> 8), body[5] = (uint8_t)to.port;
        memcpy(body + NET_RELAY_HEAD, d, (size_t)n);
        k = net_udp_send(s->sock, w, net_put(w, M_RELAY, body, NET_RELAY_HEAD + n), s->cfg.lobby);
    } else
        k = net_udp_send(s->sock, d, n, to);
    if (k > 0) s->stats.tx_bytes += (uint64_t)k, s->stats.tx_dgrams++;
    return k > 0;
}

int net_session_raw(NetSession *s, NetAddr to, const void *p, int n) { return xmit(s, to, 0, p, n); }

static int send_clear(NetSession *s, Peer *p, NetAddr to, int via_relay, int type, const void *body, int len) {
    uint8_t d[NET_DGRAM_MAX];
    int n = net_put(d, type, body, len);
    if (p && type == M_CHALLENGE) memcpy(p->challenge, d, (size_t)n);
    return xmit(s, to, via_relay, d, n);
}

static int send_sealed(NetSession *s, Peer *p, int type, const void *body, int len) {
    uint8_t d[NET_DGRAM_MAX];
    return xmit(s, p->addr, p->via_relay, d, net_seal(&p->keys, d, type, body, len));
}

static void refuse(NetSession *s, NetAddr to, int via_relay, int reason) {
    uint8_t r = (uint8_t)reason;
    s->stats.refused++;
    send_clear(s, NULL, to, via_relay, M_REFUSE, &r, 1);
}

// ---- per-address limits before a key exists
static AddrRate *rate_of(NetSession *s, NetAddr a, int via_relay) {
    if (via_relay) a.ip = 0xffffffffu;   // flows behind the relay share the relay's budget per flow
    AddrRate *free_slot = NULL, *oldest = &s->rates[0];
    for (int i = 0; i < ADDRS; i++) {
        AddrRate *r = &s->rates[i];
        if (r->addr.ip == a.ip && r->addr.port == a.port) return r;
        if (!r->addr.ip && !free_slot) free_slot = r;
        if (r->at < oldest->at && r->blocked_until < now(s)) oldest = r;
    }
    AddrRate *r = free_slot ? free_slot : oldest;
    *r = (AddrRate){.addr = a, .tokens = 2 * CLEAR_RATE, .at = now(s)};
    return r;
}

static int clear_allowed(NetSession *s, NetAddr a, int via_relay) {
    AddrRate *r = rate_of(s, a, via_relay);
    uint64_t t = now(s);
    if (r->blocked_until > t) return 0;
    r->tokens += (double)(t - r->at) * CLEAR_RATE / 1000.0;
    if (r->tokens > 2 * CLEAR_RATE) r->tokens = 2 * CLEAR_RATE;
    r->at = t;
    if (r->tokens < 1) { s->stats.dropped++; return 0; }
    r->tokens -= 1;
    return 1;
}

static void proof_failed(NetSession *s, NetAddr a, int via_relay) {
    AddrRate *r = rate_of(s, a, via_relay);
    uint64_t t = now(s);
    if (!r->bad_proofs || t - r->first_bad > BLOCK_MS) r->bad_proofs = 0, r->first_bad = t;
    if (++r->bad_proofs >= PROOF_TRIES) {
        r->blocked_until = t + BLOCK_MS;
        LOG("net: %d wrong passwords from one address in a minute: blocked for 60 s", r->bad_proofs);
    }
}

static int data_allowed(NetSession *s, Peer *p, int bytes) {
    uint64_t t = now(s);
    double dt = (double)(t - p->tok_at) / 1000.0;
    p->tok_at = t;
    p->tok_bytes += dt * DATA_BYTES, p->tok_dgrams += dt * DATA_DGRAMS;
    if (p->tok_bytes > 2.0 * DATA_BYTES) p->tok_bytes = 2.0 * DATA_BYTES;
    if (p->tok_dgrams > 2.0 * DATA_DGRAMS) p->tok_dgrams = 2.0 * DATA_DGRAMS;
    if (p->tok_bytes < bytes || p->tok_dgrams < 1) { s->stats.dropped++; return 0; }
    p->tok_bytes -= bytes, p->tok_dgrams -= 1;
    return 1;
}

// ---- host: node table
static void node_set(NetSession *s, int node, const Peer *p, const uint8_t *user, uint16_t lcv) {
    uint8_t *n = ni_node(s->info, node);
    memset(n, 0, LDN_NODE_INFO);
    uint32_t vip = vip_of(s, node);
    st32(n + NODE_IP, vip);
    uint8_t mac[6] = {0x02, 0x00, (uint8_t)(vip >> 24), (uint8_t)(vip >> 16), (uint8_t)(vip >> 8), (uint8_t)vip};
    memcpy(n + NODE_MAC, mac, 6);
    n[NODE_ID] = (uint8_t)node, n[NODE_CONNECTED] = 1;
    if (user) memcpy(n + NODE_NAME, user, LDN_USERNAME), n[NODE_NAME + LDN_USERNAME - 1] = 0;
    st16(n + NODE_LCV, lcv);
    (void)p;
}

static void count_nodes(NetSession *s) {
    int c = 0;
    for (int i = 0; i < LDN_NODES; i++) c += ni_node_c(s->info, i)[NODE_CONNECTED] != 0;
    s->info[NI_NODE_COUNT] = (uint8_t)c;
}

static void sync_all(NetSession *s) {
    s->last_sync = now(s);
    for (int i = 0; i < PEERS; i++)
        if (s->peers[i].st == P_JOINED) send_sealed(s, &s->peers[i], M_SYNC, s->info, L_SYNC);
}

static void drop_peer(NetSession *s, Peer *p, int reason, int tell) {
    if (p->st == P_JOINED) {
        if (tell) { uint8_t r = (uint8_t)reason; send_sealed(s, p, M_LEAVE, &r, 1); }
        memset(ni_node(s->info, p->node), 0, LDN_NODE_INFO);
        count_nodes(s);
        LOG("net: node %d left (reason %d)", p->node, reason);
        net_wipe(p, sizeof *p);
        sync_all(s);
        if (s->cfg.on_change) s->cfg.on_change(s->cfg.ctx);
        return;
    }
    net_wipe(p, sizeof *p);
}

int net_session_host(NetSession *s, const uint8_t info[LDN_NETWORK_INFO]) {
    if (s->state != NET_IDLE && s->state != NET_SCANNING) return 0;
    memcpy(s->info, info, LDN_NETWORK_INFO);
    uint8_t r;
    do s->cfg.random(&r, 1); while (r == 0 || r == 255);
    s->subnet = r;
    s->state = NET_HOSTING, s->my_node = 0, s->last_sync = now(s);
    uint8_t name[LDN_USERNAME];
    memcpy(name, ni_node(s->info, 0) + NODE_NAME, LDN_USERNAME);
    uint16_t lcv = ld16(ni_node(s->info, 0) + NODE_LCV);
    for (int i = 0; i < LDN_NODES; i++) memset(ni_node(s->info, i), 0, LDN_NODE_INFO);
    node_set(s, 0, NULL, name, lcv);
    memcpy(s->info + NI_MAC, ni_node(s->info, 0) + NODE_MAC, 6);
    count_nodes(s);
    LOG("net: hosting on UDP port %u, session 169.254.%u.0/24, up to %d nodes", s->port, s->subnet, s->info[NI_NODE_MAX]);
    return 1;
}

void net_session_info_changed(NetSession *s) {
    if (is_host(s)) sync_all(s);
}

void net_session_lock(NetSession *s, int locked) { s->locked = locked; }

int net_session_kick(NetSession *s, uint32_t vip) {
    for (int i = 0; i < PEERS; i++) {
        Peer *p = &s->peers[i];
        if (p->st == P_JOINED && vip_of(s, p->node) == vip) { drop_peer(s, p, LDN_DR_REJECTED, 1); return 1; }
    }
    return 0;
}

static Peer *peer_at(NetSession *s, NetAddr a, int via_relay) {
    for (int i = 0; i < PEERS; i++) {
        Peer *p = &s->peers[i];
        if (p->st != P_FREE && p->via_relay == via_relay && p->addr.ip == a.ip && p->addr.port == a.port) return p;
    }
    return NULL;
}

static uint8_t adv_flags(NetSession *s) {
    int full = s->info[NI_NODE_COUNT] >= (s->info[NI_NODE_MAX] ? s->info[NI_NODE_MAX] : LDN_NODES);
    return (uint8_t)((s->cfg.password[0] ? NET_ADV_PASSWORD : 0) |
                     (s->locked || s->info[NI_ACCEPT_POLICY] == LDN_REJECT_ALL ? NET_ADV_LOCKED : 0) |
                     (full ? NET_ADV_FULL : 0));
}

static void host_clear(NetSession *s, NetAddr from, int via_relay, int type, const uint8_t *b, int len,
                       const uint8_t *dgram, int dlen) {
    if (type == M_DISCOVER) {
        if (memcmp(b, s->cfg.compat, NET_COMPAT)) return;   // another build: not answered at all
        uint8_t body[L_ADVERT];
        memcpy(body, s->cfg.compat, NET_COMPAT);
        body[NET_COMPAT] = adv_flags(s);
        memcpy(body + NET_COMPAT + 1, s->info, LDN_NETWORK_INFO);
        send_clear(s, NULL, from, via_relay, M_ADVERT, body, L_ADVERT);
        return;
    }
    if (type == M_HELLO) {
        if (memcmp(b, s->cfg.compat, NET_COMPAT)) { refuse(s, from, via_relay, NET_REFUSE_VERSION); return; }
        if (memcmp(b + NET_COMPAT + 32, s->info + NI_SESSION_ID, 16)) { refuse(s, from, via_relay, NET_REFUSE_SESSION); return; }
        Peer *p = peer_at(s, from, via_relay);
        if (p && p->st == P_CHALLENGED && !memcmp(p->pk, b + NET_COMPAT, 32)) {   // a repeated HELLO: the same challenge
            xmit(s, from, via_relay, p->challenge, NET_HEADER + L_CHALLENGE);
            return;
        }
        if (p && p->st != P_CHALLENGED) return;   // already through: a stale HELLO
        if (adv_flags(s) & (NET_ADV_LOCKED | NET_ADV_FULL)) {
            refuse(s, from, via_relay, adv_flags(s) & NET_ADV_LOCKED ? NET_REFUSE_LOCKED : NET_REFUSE_FULL);
            return;
        }
        int pending = 0;
        for (int i = 0; i < PEERS; i++) pending += s->peers[i].st == P_CHALLENGED || s->peers[i].st == P_AUTHED;
        if (!p) {
            for (int i = 0; i < PEERS && !p; i++) if (s->peers[i].st == P_FREE) p = &s->peers[i];
            if (!p || pending >= PENDING_MAX) { refuse(s, from, via_relay, NET_REFUSE_BUSY); return; }
        }
        memset(p, 0, sizeof *p);
        p->st = P_CHALLENGED, p->addr = from, p->via_relay = via_relay, p->born = now(s);
        memcpy(p->pk, b + NET_COMPAT, 32);
        memcpy(p->hello, dgram, (size_t)dlen);
        uint8_t rnd[64];
        if (!s->cfg.random(rnd, sizeof rnd)) { net_wipe(p, sizeof *p); return; }
        memcpy(p->nonce, rnd, 32);
        uint8_t pk[32];
        net_keypair(rnd + 32, p->sk, pk);
        net_wipe(rnd, sizeof rnd);
        if (!net_shared(p->sk, p->pk, p->shared)) { net_wipe(p, sizeof *p); return; }
        uint8_t ch[L_CHALLENGE];
        memcpy(ch, p->nonce, 32);
        memcpy(ch + 32, pk, 32);
        send_clear(s, p, from, via_relay, M_CHALLENGE, ch, L_CHALLENGE);
        return;
    }
    if (type == M_PROOF) {
        Peer *p = peer_at(s, from, via_relay);
        if (!p || p->st != P_CHALLENGED) return;
        uint8_t want[32], hpk[32];
        memcpy(hpk, p->challenge + NET_HEADER + 32, 32);
        net_proof(s->cfg.password, p->nonce, p->pk, hpk, p->shared, want);
        if (!net_proof_equal(want, b)) {
            LOG("net: a station gave the wrong password");
            proof_failed(s, from, via_relay);
            refuse(s, from, via_relay, NET_REFUSE_PASSWORD);
            net_wipe(p, sizeof *p);
            return;
        }
        net_keys(&p->keys, 1, p->shared, s->cfg.password, p->hello, NET_HEADER + L_HELLO, p->challenge,
                 NET_HEADER + L_CHALLENGE);
        net_wipe(p->sk, 32);
        net_wipe(p->shared, 32);
        p->st = P_AUTHED, p->last_rx = now(s);
    }
}

static void deliver(NetSession *s, const uint8_t *b, int len) {
    if (s->cfg.on_data)
        s->cfg.on_data(s->cfg.ctx, ld32(b), (uint16_t)(b[4] | b[5] << 8), ld32(b + 6), (uint16_t)(b[10] | b[11] << 8),
                       b + NET_DATA_HEAD, len - NET_DATA_HEAD);
}

// Host: a datagram (DATA body) from node src (0 the host itself) to its destination.
static void route(NetSession *s, int src, const uint8_t *b, int len) {
    uint32_t dst = ld32(b + 6), bc = broadcast_of(s);
    int all = dst == bc || dst == 0xffffffffu;
    if (src && (all || dst == vip_of(s, 0))) deliver(s, b, len);
    for (int i = 0; i < PEERS; i++) {
        Peer *p = &s->peers[i];
        if (p->st != P_JOINED || p->node == src) continue;
        if (all || dst == vip_of(s, p->node)) send_sealed(s, p, M_DATA, b, len);
    }
}

static void host_sealed(NetSession *s, Peer *p, int type, const uint8_t *b, int len) {
    if (type == M_JOIN) {
        if (p->st == P_JOINED) {   // the WELCOME was lost
            uint8_t w[L_WELCOME];
            w[0] = p->node;
            memcpy(w + 1, s->info, LDN_NETWORK_INFO);
            send_sealed(s, p, M_WELCOME, w, L_WELCOME);
            return;
        }
        if (adv_flags(s) & (NET_ADV_LOCKED | NET_ADV_FULL)) {
            uint8_t r = adv_flags(s) & NET_ADV_LOCKED ? LDN_DR_REJECTED : LDN_DR_SYSTEM;
            send_sealed(s, p, M_LEAVE, &r, 1);
            net_wipe(p, sizeof *p);
            return;
        }
        // the game's own checks, as the console's ldn makes them: its passphrase and its local communication version
        const uint8_t *sec = b + LDN_USER_CONFIG + 2;
        uint16_t pass_len = ld16(sec + SC_PASS_SIZE), want_len = ld16(s->security + SC_PASS_SIZE);
        if (s->has_security && (pass_len != want_len || pass_len > LDN_PASSPHRASE_MAX ||
                                memcmp(sec + SC_PASS, s->security + SC_PASS, pass_len))) {
            LOG("net: a station's game passphrase differs: refused");
            uint8_t r = LDN_DR_REJECTED;
            send_sealed(s, p, M_LEAVE, &r, 1);
            net_wipe(p, sizeof *p);
            return;
        }
        if (ld16(b + LDN_USER_CONFIG) != ld16(ni_node_c(s->info, 0) + NODE_LCV)) {
            LOG("net: a station's local communication version %u differs from ours (%u): refused",
                ld16(b + LDN_USER_CONFIG), ld16(ni_node_c(s->info, 0) + NODE_LCV));
            uint8_t r = LDN_DR_REJECTED;
            send_sealed(s, p, M_LEAVE, &r, 1);
            net_wipe(p, sizeof *p);
            return;
        }
        int used[LDN_NODES] = {1};
        for (int i = 0; i < PEERS; i++) if (s->peers[i].st == P_JOINED) used[s->peers[i].node] = 1;
        int max = s->info[NI_NODE_MAX] && s->info[NI_NODE_MAX] <= LDN_NODES ? s->info[NI_NODE_MAX] : LDN_NODES, node = 0;
        for (int i = 1; i < max && !node; i++) if (!used[i]) node = i;
        if (!node) { uint8_t r = LDN_DR_SYSTEM; send_sealed(s, p, M_LEAVE, &r, 1); net_wipe(p, sizeof *p); return; }
        p->st = P_JOINED, p->node = (uint8_t)node, p->tok_at = now(s);
        p->tok_bytes = DATA_BYTES, p->tok_dgrams = DATA_DGRAMS;
        node_set(s, node, p, b, ld16(b + LDN_USER_CONFIG));
        count_nodes(s);
        LOG("net: node %d joined%s", node, p->via_relay ? " through the relay" : "");
        uint8_t w[L_WELCOME];
        w[0] = (uint8_t)node;
        memcpy(w + 1, s->info, LDN_NETWORK_INFO);
        send_sealed(s, p, M_WELCOME, w, L_WELCOME);
        sync_all(s);
        if (s->cfg.on_change) s->cfg.on_change(s->cfg.ctx);
        return;
    }
    if (p->st != P_JOINED) return;
    if (type == M_DATA) {
        if (!data_allowed(s, p, len)) return;
        uint8_t fixed[NET_DATA_HEAD + NET_PAYLOAD_MAX];
        memcpy(fixed, b, (size_t)len);
        st32(fixed, vip_of(s, p->node));   // the source is the station's own address, whatever it wrote
        route(s, p->node, fixed, len);
    } else if (type == M_PING) {
        send_sealed(s, p, M_PONG, b, L_PING);
    } else if (type == M_PONG) {
        p->rtt = (uint32_t)(now(s) - ld64(b));
    } else if (type == M_LEAVE) {
        drop_peer(s, p, LDN_DR_USER, 0);
    }
}

// ---- station
int net_session_join(NetSession *s, NetAddr addr, int via_relay, const uint8_t session_id[16],
                     const uint8_t user_config[LDN_USER_CONFIG], uint16_t lcv, const uint8_t security[LDN_SECURITY_CONFIG]) {
    if (s->state == NET_HOSTING || s->state == NET_JOINED || s->state == NET_JOINING) return 0;
    Peer *h = &s->peers[0];
    memset(h, 0, sizeof *h);
    h->addr = addr, h->via_relay = via_relay;
    memcpy(s->session_id, session_id, 16);
    memcpy(s->join_body, user_config, LDN_USER_CONFIG);
    st16(s->join_body + LDN_USER_CONFIG, lcv);
    memcpy(s->join_body + LDN_USER_CONFIG + 2, security, LDN_SECURITY_CONFIG);
    uint8_t rnd[32];
    if (!s->cfg.random(rnd, 32)) return 0;
    net_keypair(rnd, s->my_sk, s->my_pk);
    net_wipe(rnd, 32);
    s->state = NET_JOINING, s->refused = 0, s->reason = 0, s->join_started = now(s), s->join_sent = 0;
    s->join_direct = via_relay ? (NetAddr){0, 0} : addr;
    return 1;
}

static void station_hello(NetSession *s) {
    Peer *h = &s->peers[0];
    uint8_t b[L_HELLO], d[NET_DGRAM_MAX];
    memcpy(b, s->cfg.compat, NET_COMPAT);
    memcpy(b + NET_COMPAT, s->my_pk, 32);
    memcpy(b + NET_COMPAT + 32, s->session_id, 16);
    int n = net_put(d, M_HELLO, b, L_HELLO);
    memcpy(h->hello, d, (size_t)n);
    xmit(s, h->addr, h->via_relay, d, n);
    s->join_sent = now(s);
}

static void station_gone(NetSession *s, int reason) {
    int was = s->state;
    s->state = NET_GONE, s->reason = reason;
    net_wipe(&s->peers[0], sizeof s->peers[0]);
    if (was == NET_JOINED || was == NET_JOINING) LOG("net: left the session (reason %d)", reason);
    if (s->cfg.on_change) s->cfg.on_change(s->cfg.ctx);
}

static void station_clear(NetSession *s, NetAddr from, int via_relay, int type, const uint8_t *b, int len,
                          const uint8_t *dgram, int dlen) {
    (void)len;
    if (type == M_ADVERT && s->state != NET_JOINED) {
        if (memcmp(b, s->cfg.compat, NET_COMPAT)) return;
        NetFound *f = NULL;
        for (int i = 0; i < s->nfound && !f; i++)
            if (!memcmp(s->found[i].info + NI_SESSION_ID, b + NET_COMPAT + 1 + NI_SESSION_ID, 16)) f = &s->found[i];
        if (!f && s->nfound < NET_FOUND_MAX) f = &s->found[s->nfound++];
        if (!f) return;
        f->addr = from, f->via_relay = via_relay, f->flags = b[NET_COMPAT];
        memcpy(f->info, b + NET_COMPAT + 1, LDN_NETWORK_INFO);
        return;
    }
    Peer *h = &s->peers[0];
    if (s->state != NET_JOINING || h->via_relay != via_relay || h->addr.ip != from.ip || h->addr.port != from.port) return;
    if (type == M_REFUSE) {
        s->refused = b[0];
        LOG("net: the host refused to let us in (reason %d)", b[0]);
        station_gone(s, b[0] == NET_REFUSE_KICKED || b[0] == NET_REFUSE_LOCKED ? LDN_DR_REJECTED : LDN_DR_SYSTEM);
        return;
    }
    if (type == M_CHALLENGE && h->st == P_FREE) {
        memcpy(h->challenge, dgram, (size_t)dlen);
        uint8_t shared[32], proof[32];
        if (!net_shared(s->my_sk, b + 32, shared)) return;
        net_proof(s->cfg.password, b, s->my_pk, b + 32, shared, proof);
        net_keys(&h->keys, 0, shared, s->cfg.password, h->hello, NET_HEADER + L_HELLO, h->challenge,
                 NET_HEADER + L_CHALLENGE);
        net_wipe(shared, 32);
        memcpy(h->nonce, proof, 32);   // kept to repeat the PROOF while waiting
        h->st = P_AUTHED, h->last_rx = now(s);
        send_clear(s, NULL, h->addr, h->via_relay, M_PROOF, proof, L_PROOF);
        send_sealed(s, h, M_JOIN, s->join_body, L_JOIN);
        s->join_sent = now(s);
    }
}

static void station_sealed(NetSession *s, int type, const uint8_t *b, int len) {
    Peer *h = &s->peers[0];
    if (type == M_WELCOME && s->state == NET_JOINING) {
        if (b[0] == 0 || b[0] >= LDN_NODES) return;
        s->my_node = b[0];
        memcpy(s->info, b + 1, LDN_NETWORK_INFO);
        s->subnet = (uint8_t)(ld32(ni_node_c(s->info, 0) + NODE_IP) >> 8);
        h->st = P_JOINED;
        s->state = NET_JOINED;
        LOG("net: joined as node %d%s", s->my_node, h->via_relay ? " through the relay" : "");
        if (s->cfg.on_change) s->cfg.on_change(s->cfg.ctx);
        return;
    }
    if (type == M_LEAVE && s->state == NET_JOINING) {   // refused after the handshake: the game's own checks
        station_gone(s, b[0] ? b[0] : LDN_DR_REJECTED);
        return;
    }
    if (s->state != NET_JOINED) return;
    if (type == M_SYNC) {
        memcpy(s->info, b, LDN_NETWORK_INFO);
        if (s->cfg.on_change) s->cfg.on_change(s->cfg.ctx);
    } else if (type == M_DATA) {
        deliver(s, b, len);
    } else if (type == M_PING) {
        send_sealed(s, h, M_PONG, b, L_PING);
    } else if (type == M_PONG) {
        h->rtt = (uint32_t)(now(s) - ld64(b));
    } else if (type == M_LEAVE) {
        station_gone(s, b[0] ? b[0] : LDN_DR_DESTROYED_SYSTEM);
    }
}

void net_session_scan(NetSession *s, const NetAddr *to, int n) {
    s->nfound = 0;
    if (s->state == NET_IDLE || s->state == NET_GONE) s->state = NET_SCANNING;
    for (int i = 0; i < n; i++) send_clear(s, NULL, to[i], 0, M_DISCOVER, s->cfg.compat, NET_COMPAT);
}

void net_session_punch(NetSession *s, NetAddr addr) {
    // DISCOVER-sized and never answered by a host of another build: it only opens this side's router
    for (int i = 0; i < 3; i++) send_clear(s, NULL, addr, 0, M_DISCOVER, s->cfg.compat, NET_COMPAT);
}

int net_session_send(NetSession *s, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const void *p, int n) {
    if (n < 0 || n > NET_PAYLOAD_MAX) { s->stats.dropped++; return 0; }
    uint8_t b[NET_DATA_HEAD + NET_PAYLOAD_MAX];
    st32(b, net_session_vip(s));
    b[4] = (uint8_t)src_port, b[5] = (uint8_t)(src_port >> 8);
    st32(b + 6, dst_ip);
    b[10] = (uint8_t)dst_port, b[11] = (uint8_t)(dst_port >> 8);
    memcpy(b + NET_DATA_HEAD, p, (size_t)n);
    if (is_host(s)) { route(s, 0, b, NET_DATA_HEAD + n); return 1; }
    if (s->state != NET_JOINED) return 0;
    return send_sealed(s, &s->peers[0], M_DATA, b, NET_DATA_HEAD + n);
}

// ---- receive
static void handle(NetSession *s, NetAddr from, int via_relay, const uint8_t *d, int n) {
    int type, len, min, max, clear;
    const uint8_t *b;
    if (!net_parse(d, n, &type, &b, &len)) { s->stats.dropped++; return; }
    if (type >= 0x40) {
        if (via_relay) return;
        int is_lobby = s->cfg.lobby.ip && from.ip == s->cfg.lobby.ip && from.port == s->cfg.lobby.port;
        if (!is_lobby) { s->stats.dropped++; return; }
        if (type == M_RELAY) {
            if (len < NET_RELAY_HEAD + NET_HEADER) return;
            uint32_t sess = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
            uint16_t flow = (uint16_t)(b[4] << 8 | b[5]);
            if (sess != s->relay_session) return;
            handle(s, (NetAddr){0, flow}, 1, b + NET_RELAY_HEAD, len - NET_RELAY_HEAD);
            return;
        }
        int lmin, lmax;
        if (!net_lobby_limits(type, &lmin, &lmax) || len < lmin || len > lmax) return;
        if (s->lobby_fn) s->lobby_fn(s->lobby_ctx, type, b, len);
        return;
    }
    if (!net_msg_limits(type, &min, &max, &clear)) { s->stats.dropped++; return; }
    if (clear) {
        if (len < min || len > max) { s->stats.dropped++; return; }
        if (is_host(s)) {
            if (type != M_DISCOVER && type != M_HELLO && type != M_PROOF) return;
            if (!clear_allowed(s, from, via_relay)) return;
            host_clear(s, from, via_relay, type, b, len, d, n);
        } else
            station_clear(s, from, via_relay, type, b, len, d, n);
        return;
    }
    Peer *p = is_host(s) ? peer_at(s, from, via_relay) : &s->peers[0];
    if (!p || p->st < P_AUTHED || (!is_host(s) && (p->via_relay != via_relay || p->addr.ip != from.ip ||
                                                   p->addr.port != from.port))) {
        s->stats.dropped++;
        return;
    }
    uint8_t plain[NET_DGRAM_MAX];
    int pl = net_open(&p->keys, d, type, b, len, plain);
    if (pl < min || pl > max) { s->stats.dropped++; return; }
    p->last_rx = now(s);
    if (is_host(s)) host_sealed(s, p, type, plain, pl);
    else station_sealed(s, type, plain, pl);
}

static void timers(NetSession *s) {
    uint64_t t = now(s);
    if (is_host(s)) {
        for (int i = 0; i < PEERS; i++) {
            Peer *p = &s->peers[i];
            if (p->st == P_FREE) continue;
            if (p->st != P_JOINED) {
                if (t - p->born > HANDSHAKE_MS) net_wipe(p, sizeof *p);
                continue;
            }
            if (t - p->last_rx > SILENT_MS) { drop_peer(s, p, LDN_DR_SIGNAL_LOST, 0); continue; }
            if (t - p->last_ping >= PING_EVERY_MS) {
                uint8_t b[8];
                st64(b, t);
                send_sealed(s, p, M_PING, b, 8);
                p->last_ping = t;
            }
        }
        if (t - s->last_sync >= SYNC_EVERY_MS) sync_all(s);
        return;
    }
    Peer *h = &s->peers[0];
    if (s->state == NET_JOINING) {
        if (t - s->join_started > JOIN_GIVE_UP_MS) { s->refused = 0; station_gone(s, LDN_DR_SIGNAL_LOST); return; }
        // a direct attempt that got nothing for RELAY_AFTER_MS goes through the relay (the lobby introduced us)
        if (s->join_direct.ip && s->cfg.lobby.ip && s->relay_session && h->st == P_FREE &&
            t - s->join_started > RELAY_AFTER_MS) {
            LOG("net: no answer from the host directly: through the relay");
            h->via_relay = 1, h->addr = (NetAddr){0, 0}, s->join_direct = (NetAddr){0, 0};
            station_hello(s);
            return;
        }
        if (t - s->join_sent >= HELLO_EVERY_MS) {
            if (h->st == P_FREE) station_hello(s);
            else {   // PROOF and JOIN again
                send_clear(s, NULL, h->addr, h->via_relay, M_PROOF, h->nonce, L_PROOF);
                send_sealed(s, h, M_JOIN, s->join_body, L_JOIN);
                s->join_sent = t;
            }
        }
    } else if (s->state == NET_JOINED) {
        if (t - h->last_rx > SILENT_MS) { station_gone(s, LDN_DR_SIGNAL_LOST); return; }
        if (t - h->last_ping >= PING_EVERY_MS) {
            uint8_t b[8];
            st64(b, t);
            send_sealed(s, h, M_PING, b, 8);
            h->last_ping = t;
        }
    }
}

void net_session_poll(NetSession *s) {
    uint8_t d[NET_WIRE_MAX];
    NetAddr from;
    for (int i = 0; i < 256; i++) {   // bounded: timers run even under a flood
        int n = net_udp_recv(s->sock, d, sizeof d, &from);
        if (n <= 0) break;
        s->stats.rx_bytes += (uint64_t)n, s->stats.rx_dgrams++;
        handle(s, from, 0, d, n);
    }
    timers(s);
}

void net_session_free(NetSession *s) {
    if (!s) return;
    uint8_t r = is_host(s) ? LDN_DR_DESTROYED_USER : LDN_DR_USER;
    for (int i = 0; i < PEERS; i++)
        if (s->peers[i].st == P_JOINED) send_sealed(s, &s->peers[i], M_LEAVE, &r, 1);
    net_udp_close(s->sock);
    net_wipe(s, sizeof *s);
    free(s);
}

void net_session_relay(NetSession *s, uint32_t session) { s->relay_session = session; }
void net_session_security(NetSession *s, const uint8_t security[LDN_SECURITY_CONFIG]) {
    memcpy(s->security, security, LDN_SECURITY_CONFIG);
    s->has_security = 1;
}
