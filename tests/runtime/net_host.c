// Local wireless play without the game (tests/runtime/test_net.py): real session engines (runtime/net/net_session.c) on
// loopback UDP, and the lobby/relay (server/lobby/mkw_lobby.c) when a port is given. Each scenario prints "ok <name>"
// or "FAIL <name>: <why>" and the program exits 1 on a failure.
//
// Usage: net_host lan | password | version | fuzz | invite | lobby <lobby port>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "net_lobby.h"
#include "net_session.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s: ", scenario); printf(__VA_ARGS__); printf("\n"); failures++; return; } } while (0)
static const char *scenario;

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static int rnd(void *p, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t *)p)[i] = (uint8_t)rand();
    return 1;
}
static int verbose;
static void logf_(const char *fmt, ...) {
    if (!verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

// A node: its engine and the last game datagram it got.
typedef struct {
    NetSession *s;
    int got, changes;
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port;
    uint8_t data[2048];
    int n;
    // lobby client state (the lobby scenario)
    uint8_t cookie[NET_COOKIE], pending[NET_DGRAM_MAX];
    int pending_len, pending_type;
    uint32_t session;
    int registered, peer, peer_direct, scanned;
    NetAddr peer_host;
} Node;

static void on_data(void *ctx, uint32_t si, uint16_t sp, uint32_t di, uint16_t dp, const uint8_t *p, int n) {
    Node *x = ctx;
    x->got++, x->src_ip = si, x->src_port = sp, x->dst_ip = di, x->dst_port = dp, x->n = n;
    memcpy(x->data, p, (size_t)n);
}
static void on_change(void *ctx) { ((Node *)ctx)->changes++; }

static uint8_t compat_a[NET_COMPAT] = {1, 2, 3}, compat_b[NET_COMPAT] = {9, 9, 9};
static NetAddr lobby;

static Node *node_new(Node *x, uint16_t port, const uint8_t *compat, const char *password) {
    memset(x, 0, sizeof *x);
    NetConfig c = {.port = port, .random = rnd, .now_ms = now_ms, .log = logf_, .on_data = on_data,
                   .on_change = on_change, .ctx = x, .lobby = lobby};
    memcpy(c.compat, compat, NET_COMPAT);
    snprintf(c.password, sizeof c.password, "%s", password);
    x->s = net_session_new(&c);
    return x->s ? x : NULL;
}

static void pump(Node **xs, int n, int ms) {
    uint64_t end = now_ms() + (uint64_t)ms;
    while (now_ms() < end) {
        for (int i = 0; i < n; i++) if (xs[i] && xs[i]->s) net_session_poll(xs[i]->s);
        usleep(2000);
    }
}

static int pump_until(Node **xs, int n, int ms, int (*done)(Node **)) {
    uint64_t end = now_ms() + (uint64_t)ms;
    while (now_ms() < end) {
        for (int i = 0; i < n; i++) if (xs[i] && xs[i]->s) net_session_poll(xs[i]->s);
        if (done(xs)) return 1;
        usleep(2000);
    }
    return done(xs);
}

static void base_info(uint8_t *ni, int max) {
    memset(ni, 0, LDN_NETWORK_INFO);
    st64(ni + NI_LCID, 0x0100152000022000ull);
    for (int i = 0; i < 16; i++) ni[NI_SESSION_ID + i] = (uint8_t)(0xa0 + i);
    ni[NI_NODE_MAX] = (uint8_t)max;
    memcpy(ni_node(ni, 0) + NODE_NAME, "host", 5);
    st16(ni_node(ni, 0) + NODE_LCV, 3);
}

static uint8_t user[LDN_USER_CONFIG] = "station", security[LDN_SECURITY_CONFIG];

static int join_lan(Node *h, Node *st, uint16_t port, Node **all, int n) {
    NetAddr to = {0x7f000001, port};
    net_session_scan(st->s, &to, 1);
    pump(all, n, 100);
    const NetFound *f;
    if (net_session_found(st->s, &f) != 1) return -1;
    net_session_join(st->s, f[0].addr, 0, f[0].info + NI_SESSION_ID, user, 3, security);
    uint64_t end = now_ms() + 3000;
    while (now_ms() < end && net_session_state(st->s) == NET_JOINING) pump(all, n, 5);
    (void)h;
    return net_session_state(st->s);
}

static void t_lan(void) {
    scenario = "lan";
    Node h, a, b;
    CHECK(node_new(&h, 0, compat_a, ""), "host socket");
    uint16_t port = net_session_port(h.s);
    uint8_t ni[LDN_NETWORK_INFO];
    base_info(ni, 8);
    net_session_host(h.s, ni);
    net_session_security(h.s, security);
    CHECK(node_new(&a, 0, compat_a, "") && node_new(&b, 0, compat_a, ""), "station sockets");
    Node *all[] = {&h, &a, &b};
    CHECK(join_lan(&h, &a, port, all, 3) == NET_JOINED, "station a did not join (refused %d)", net_session_refused(a.s));
    CHECK(join_lan(&h, &b, port, all, 3) == NET_JOINED, "station b did not join");
    pump(all, 3, 50);
    CHECK(net_session_info(h.s)[NI_NODE_COUNT] == 3, "host counts %d nodes", net_session_info(h.s)[NI_NODE_COUNT]);
    CHECK(net_session_info(b.s)[NI_NODE_COUNT] == 3, "station b sees %d nodes", net_session_info(b.s)[NI_NODE_COUNT]);
    uint32_t vh = net_session_vip(h.s), va = net_session_vip(a.s), vb = net_session_vip(b.s);
    CHECK(vh && va && vb && va != vb && (va & 0xffffff00u) == (vh & 0xffffff00u) && (vh >> 16) == 0xa9fe,
          "addresses %08x %08x %08x", vh, va, vb);
    // station a to station b, through the host; the source is a's address whatever a writes
    net_session_send(a.s, 5000, vb, 6000, "hello b", 7);
    pump(all, 3, 50);
    CHECK(b.got == 1 && b.n == 7 && !memcmp(b.data, "hello b", 7) && b.src_ip == va && b.src_port == 5000 &&
          b.dst_port == 6000, "b got %d (src %08x)", b.got, b.src_ip);
    CHECK(h.got == 0 && a.got == 0, "a unicast reached others (%d %d)", h.got, a.got);
    // broadcast from the host reaches both stations, not the host
    net_session_send(h.s, 1, vh | 0xff, 2, "all", 3);
    pump(all, 3, 50);
    CHECK(a.got == 1 && b.got == 2 && h.got == 0, "broadcast %d %d %d", a.got, b.got, h.got);
    // a station to the host
    net_session_send(b.s, 7, vh, 8, "to host", 7);
    pump(all, 3, 50);
    CHECK(h.got == 1 && h.src_ip == vb, "host got %d", h.got);
    // the largest game datagram, and one too many
    uint8_t big[NET_PAYLOAD_MAX + 1];
    memset(big, 0x5a, sizeof big);
    CHECK(net_session_send(a.s, 1, vh, 1, big, NET_PAYLOAD_MAX), "max datagram refused");
    CHECK(!net_session_send(a.s, 1, vh, 1, big, NET_PAYLOAD_MAX + 1), "oversized datagram accepted");
    pump(all, 3, 50);
    CHECK(h.got == 2 && h.n == NET_PAYLOAD_MAX, "max datagram did not arrive (%d, %d)", h.got, h.n);
    // the host kicks b; b learns it was rejected
    CHECK(net_session_kick(h.s, vb), "kick");
    pump(all, 3, 100);
    CHECK(net_session_state(b.s) == NET_GONE && net_session_reason(b.s) == LDN_DR_REJECTED, "b state %d reason %d",
          net_session_state(b.s), net_session_reason(b.s));
    CHECK(net_session_info(h.s)[NI_NODE_COUNT] == 2, "after kick %d nodes", net_session_info(h.s)[NI_NODE_COUNT]);
    // the host locks: a new station is refused
    net_session_lock(h.s, 1);
    net_session_free(b.s);
    node_new(&b, 0, compat_a, "");
    all[2] = &b;
    CHECK(join_lan(&h, &b, port, all, 3) == NET_GONE && net_session_refused(b.s) == NET_REFUSE_LOCKED, "locked: refused %d",
          net_session_refused(b.s));
    net_session_lock(h.s, 0);
    // a leaves by itself: the host drops it at once
    net_session_free(a.s);
    a.s = NULL;
    pump(all, 3, 100);
    CHECK(net_session_info(h.s)[NI_NODE_COUNT] == 1, "after leave %d nodes", net_session_info(h.s)[NI_NODE_COUNT]);
    // the host goes away: b (joined again) sees the network destroyed
    net_session_free(b.s);
    node_new(&b, 0, compat_a, "");
    all[2] = &b;
    CHECK(join_lan(&h, &b, port, all, 3) == NET_JOINED, "rejoin");
    net_session_free(h.s);
    h.s = NULL;
    pump(all, 3, 100);
    CHECK(net_session_state(b.s) == NET_GONE && net_session_reason(b.s) == LDN_DR_DESTROYED_USER, "host gone: %d %d",
          net_session_state(b.s), net_session_reason(b.s));
    net_session_free(b.s);
    printf("ok lan\n");
}

static void t_password(void) {
    scenario = "password";
    Node h, a;
    CHECK(node_new(&h, 0, compat_a, "KQXT4MNP7R"), "host");
    uint8_t ni[LDN_NETWORK_INFO];
    base_info(ni, 4);
    net_session_host(h.s, ni);
    uint16_t port = net_session_port(h.s);
    for (int attempt = 0; attempt < 2; attempt++) {
        CHECK(node_new(&a, 0, compat_a, "WRONGPASS1"), "station");
        Node *all[] = {&h, &a};
        CHECK(join_lan(&h, &a, port, all, 2) == NET_GONE && net_session_refused(a.s) == NET_REFUSE_PASSWORD,
              "wrong password: state %d refused %d", net_session_state(a.s), net_session_refused(a.s));
        // a stranger without the password got nothing of the session but the advert (no node, no traffic)
        net_session_send(h.s, 1, net_session_vip(h.s) | 0xff, 1, "secret", 6);
        pump(all, 2, 30);
        CHECK(a.got == 0, "stranger received game data");
        net_session_free(a.s);
    }
    CHECK(node_new(&a, 0, compat_a, "KQXT4MNP7R"), "station");
    Node *all[] = {&h, &a};
    CHECK(join_lan(&h, &a, port, all, 2) == NET_JOINED, "right password refused (%d)", net_session_refused(a.s));
    // five wrong proofs from one address in a minute block it, even with the right password afterwards
    Node w;
    node_new(&w, 0, compat_a, "BADBADBAD2");
    Node *all2[] = {&h, &w};
    for (int i = 0; i < 5; i++) {
        join_lan(&h, &w, port, all2, 2);
        net_session_password(w.s, "BADBADBAD2");
    }
    net_session_password(w.s, "KQXT4MNP7R");
    CHECK(join_lan(&h, &w, port, all2, 2) != NET_JOINED, "an address with 5 wrong passwords got in");
    net_session_free(w.s), net_session_free(a.s), net_session_free(h.s);
    printf("ok password\n");
}

static void t_version(void) {
    scenario = "version";
    Node h, a;
    CHECK(node_new(&h, 0, compat_a, ""), "host");
    uint8_t ni[LDN_NETWORK_INFO];
    base_info(ni, 4);
    net_session_host(h.s, ni);
    CHECK(node_new(&a, 0, compat_b, ""), "station");
    Node *all[] = {&h, &a};
    NetAddr to = {0x7f000001, net_session_port(h.s)};
    net_session_scan(a.s, &to, 1);
    pump(all, 2, 100);
    const NetFound *f;
    CHECK(net_session_found(a.s, &f) == 0, "a host of another build was listed");
    net_session_join(a.s, to, 0, ni + NI_SESSION_ID, user, 3, security);
    pump(all, 2, 300);
    CHECK(net_session_state(a.s) == NET_GONE && net_session_refused(a.s) == NET_REFUSE_VERSION, "refused %d",
          net_session_refused(a.s));
    // the game's own version: another local communication version is refused by the host
    Node b;
    node_new(&b, 0, compat_a, "");
    Node *all2[] = {&h, &b};
    net_session_scan(b.s, &to, 1);
    pump(all2, 2, 100);
    net_session_join(b.s, to, 0, ni + NI_SESSION_ID, user, 4, security);
    uint64_t end = now_ms() + 2000;
    while (now_ms() < end && net_session_state(b.s) == NET_JOINING) pump(all2, 2, 5);
    CHECK(net_session_state(b.s) == NET_GONE && net_session_reason(b.s) == LDN_DR_REJECTED, "lcv 4 got in (%d)",
          net_session_state(b.s));
    net_session_free(a.s), net_session_free(b.s), net_session_free(h.s);
    printf("ok version\n");
}

// Random datagrams of every type and length at a host and at a joined station: nothing crashes, the session holds.
static void t_fuzz(void) {
    scenario = "fuzz";
    // the parser first: every length of every type
    for (int type = 0; type < 0x60; type++)
        for (int len = 0; len < NET_WIRE_MAX + 4; len += 7) {
            uint8_t d[NET_WIRE_MAX + 16] = {'M', 'K', 'W', 'N', NET_VERSION, (uint8_t)type, (uint8_t)len, (uint8_t)(len >> 8)};
            int t, l, min, max, clear;
            const uint8_t *b;
            int ok = net_parse(d, NET_HEADER + len, &t, &b, &l);
            if (ok && type <= M_SESSION_LAST && net_msg_limits(type, &min, &max, &clear))
                CHECK(l == len, "length");
            CHECK(!ok || NET_HEADER + len <= (type == M_RELAY ? NET_WIRE_MAX : NET_DGRAM_MAX), "oversized parse %d %d", type, len);
            CHECK(!net_parse(d, NET_HEADER + len + 1, &t, &b, &l), "trailing byte accepted");
        }
    Node h, a, z;
    CHECK(node_new(&h, 0, compat_a, "PASSWORD22"), "host");
    uint8_t ni[LDN_NETWORK_INFO];
    base_info(ni, 8);
    net_session_host(h.s, ni);
    uint16_t port = net_session_port(h.s);
    CHECK(node_new(&a, 0, compat_a, "PASSWORD22") && node_new(&z, 0, compat_a, ""), "stations");
    Node *all[] = {&h, &a, &z};
    CHECK(join_lan(&h, &a, port, all, 3) == NET_JOINED, "join");
    NetAddr th = {0x7f000001, port}, ta = {0x7f000001, net_session_port(a.s)};
    srand(1234);
    for (int i = 0; i < 20000; i++) {
        uint8_t d[NET_WIRE_MAX];
        int len = rand() % (NET_WIRE_MAX - NET_HEADER);
        int type = rand() % 0x58;
        if (i % 3 == 0) {   // exact lengths of the type, random bytes: deeper into the handlers
            int min, max, clear;
            if (net_msg_limits(type, &min, &max, &clear)) len = min + (max > min ? rand() % (max - min + 1) : 0);
            if (net_lobby_limits(type, &min, &max)) len = min + (max > min ? rand() % (max - min + 1) : 0);
        }
        for (int k = 0; k < len; k++) d[NET_HEADER + k] = (uint8_t)rand();
        if (type == M_DISCOVER || type == M_HELLO) memcpy(d + NET_HEADER, compat_a, NET_COMPAT);
        int n = net_put(d, type, d + NET_HEADER, len);
        if (rand() % 10 == 0 && n) d[rand() % n] ^= (uint8_t)(1 + rand() % 255);   // a broken header now and then
        if (n) net_session_raw(z.s, i & 1 ? th : ta, d, n);
        if (i % 64 == 0) pump(all, 3, 1);
    }
    pump(all, 3, 300);
    CHECK(net_session_state(a.s) == NET_JOINED && net_session_state(h.s) == NET_HOSTING, "session lost: %d %d",
          net_session_state(a.s), net_session_state(h.s));
    CHECK(net_session_info(h.s)[NI_NODE_COUNT] == 2, "fuzz added nodes: %d", net_session_info(h.s)[NI_NODE_COUNT]);
    int before = h.got;
    net_session_send(a.s, 1, net_session_vip(h.s), 1, "still", 5);
    pump(all, 3, 50);
    CHECK(h.got == before + 1, "data after the fuzz");
    // a replayed sealed datagram is dropped: capture by sending the same bytes twice through a raw socket is not
    // possible from here, so check the window directly
    NetKeys k1, k2;
    uint8_t shared[32] = {7};
    uint8_t hello[8] = {1}, ch[8] = {2};
    net_keys(&k1, 1, shared, "pw", hello, 8, ch, 8);
    net_keys(&k2, 0, shared, "pw", hello, 8, ch, 8);
    uint8_t d[NET_DGRAM_MAX], plain[NET_DGRAM_MAX];
    int n = net_seal(&k1, d, M_PING, "12345678", 8), t, l;
    const uint8_t *b;
    CHECK(net_parse(d, n, &t, &b, &l) && net_open(&k2, d, t, b, l, plain) == 8, "open");
    CHECK(net_open(&k2, d, t, b, l, plain) < 0, "replay accepted");
    d[NET_HEADER + 9] ^= 1;
    n = net_seal(&k1, d, M_PING, "12345678", 8);
    d[n - 1] ^= 1;
    CHECK(net_parse(d, n, &t, &b, &l) && net_open(&k2, d, t, b, l, plain) < 0, "forged tag accepted");
    n = net_seal(&k1, d, M_PING, "12345678", 8);
    d[5] = M_PONG;   // moved to another type
    CHECK(net_parse(d, n, &t, &b, &l) && net_open(&k2, d, t, b, l, plain) < 0, "a datagram moved to another type opened");
    for (int i = 0; i < 70; i++) net_seal(&k1, d, M_PING, "12345678", 8);
    n = net_seal(&k1, d, M_PING, "12345678", 8);
    CHECK(net_parse(d, n, &t, &b, &l) && net_open(&k2, d, t, b, l, plain) == 8, "a jump ahead refused");
    net_session_free(a.s), net_session_free(z.s), net_session_free(h.s);
    printf("ok fuzz\n");
}

static void t_invite(void) {
    scenario = "invite";
    char inv[NET_INVITE_LEN], pw[NET_PASSWORD_LETTERS + 1];
    uint32_t sess;
    CHECK(net_invite_make(0x89abcdefu, "KQXT4MNP7R", inv), "make");
    CHECK(strlen(inv) == 28 && inv[5] == '-', "format %s", inv);
    for (const char *p = inv; *p; p++) CHECK(!strchr("IO01", *p), "ambiguous letter in %s", inv);
    CHECK(net_invite_read(inv, &sess, pw) && sess == 0x89abcdefu && !strcmp(pw, "KQXT4MNP7R"), "read back");
    char loose[64];
    snprintf(loose, sizeof loose, " %s ", inv);
    for (char *p = loose; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    CHECK(net_invite_read(loose, &sess, pw), "lower case with spaces refused");
    for (int i = 0; i < (int)strlen(inv); i++) {   // every single changed letter is caught (bar a 2^-24 chance)
        if (inv[i] == '-') continue;
        char bad[NET_INVITE_LEN];
        memcpy(bad, inv, sizeof bad);
        bad[i] = bad[i] == 'A' ? 'B' : 'A';
        CHECK(!net_invite_read(bad, &sess, pw), "forged letter %d accepted", i);
    }
    CHECK(!net_invite_read("ABCDE-FGHJK", &sess, pw), "short accepted");
    CHECK(!net_invite_make(1, "short", inv) && !net_invite_make(1, "KQXT4MNP7I", inv), "bad password made an invite");
    char np[NET_PASSWORD_LETTERS + 1];
    uint8_t r[NET_PASSWORD_LETTERS] = {0, 1, 2, 3, 31, 30, 29, 8, 9, 10};
    net_password_new(r, np);
    CHECK(strlen(np) == 10 && net_invite_make(5, np, inv), "made password %s", np);
    char name[NET_NAME_MAX + 1] = "Rainbow Road fans  ";
    CHECK(net_name_ok(name) && !strcmp(name, "Rainbow Road fans"), "name");
    char n2[NET_NAME_MAX + 1] = "bad\x01name", n3[NET_NAME_MAX + 1] = "   ", n4[NET_NAME_MAX + 1] = "H1tl3r club",
         n5[NET_NAME_MAX + 1] = "caf\xc3\xa9", n6[NET_NAME_MAX + 1] = "x\xc3", n7[NET_NAME_MAX + 1] = "grapes";
    CHECK(!net_name_ok(n2) && !net_name_ok(n3) && !net_name_ok(n4) && net_name_ok(n5) && !net_name_ok(n6) && net_name_ok(n7),
          "name filter");
    printf("ok invite\n");
}

// ---- the lobby: a small client as net_ldn.c's
static void lobby_msg(void *ctx, int type, const uint8_t *b, int len) {
    Node *x = ctx;
    (void)len;
    if (type == L_COOKIE) {
        memcpy(x->cookie, b, NET_COOKIE);
        if (x->pending_len) {
            memcpy(x->pending, x->cookie, NET_COOKIE);
            uint8_t d[NET_DGRAM_MAX];
            net_session_raw(x->s, lobby, d, net_put(d, x->pending_type, x->pending, x->pending_len));
            x->pending_len = 0;
        }
    } else if (type == L_REGISTERED && b[4]) {
        x->session = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3], x->registered = 1;
    } else if (type == L_SCAN_REPLY) {
        NetFound f = {.via_relay = 1, .flags = b[4]};
        memcpy(f.info, b + 5, LDN_NETWORK_INFO);
        net_session_found_put(x->s, &f);
        x->scanned++;
    } else if (type == L_PEER) {
        x->peer = 1, x->peer_direct = b[4];
        x->peer_host = (NetAddr){(uint32_t)b[5] << 24 | b[6] << 16 | b[7] << 8 | b[8], (uint16_t)(b[9] << 8 | b[10])};
    } else if (type == L_INTRODUCE) {
        net_session_punch(x->s, (NetAddr){(uint32_t)b[4] << 24 | b[5] << 16 | b[6] << 8 | b[7], (uint16_t)(b[8] << 8 | b[9])});
    }
}

static void lsend(Node *x, int type, const uint8_t *body, int len, int needs_cookie) {
    uint8_t d[NET_DGRAM_MAX];
    if (needs_cookie) memcpy(x->pending, body, (size_t)len), x->pending_len = len, x->pending_type = type;
    net_session_raw(x->s, lobby, d, net_put(d, type, body, len));
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }

static void lregister(Node *h, int public_) {
    uint8_t b[LR_REGISTER] = {0};
    memcpy(b, compat_a, NET_COMPAT);
    put32(b + NET_COMPAT, h->session);
    memset(b + NET_COMPAT + 4, 0x42, NET_TOKEN);
    NetLobbyMeta m = {.flags = (uint8_t)(public_ ? NET_LOBBY_PUBLIC : NET_ADV_PASSWORD), .players = 1, .slots = 8};
    snprintf(m.name, sizeof m.name, "test room");
    net_meta_put(b + NET_COMPAT + 4 + NET_TOKEN, &m);
    memcpy(b + NET_COMPAT + 4 + NET_TOKEN + NET_META, net_session_info(h->s), LDN_NETWORK_INFO);
    lsend(h, L_REGISTER, b, LR_REGISTER, 0);
}

static void lscan(Node *x, uint32_t session) {
    uint8_t b[LR_SCAN];
    memcpy(b, x->cookie, NET_COOKIE);
    memcpy(b + NET_COOKIE, compat_a, NET_COMPAT);
    put32(b + NET_COOKIE + NET_COMPAT, session);
    lsend(x, L_SCAN, b, LR_SCAN, 1);
}

static void lconnect(Node *x, uint32_t session, int relay_only) {
    uint8_t b[LR_CONNECT];
    memcpy(b, x->cookie, NET_COOKIE);
    memcpy(b + NET_COOKIE, compat_a, NET_COMPAT);
    put32(b + NET_COOKIE + NET_COMPAT, session);
    b[NET_COOKIE + NET_COMPAT + 4] = (uint8_t)relay_only;
    x->peer = 0;
    lsend(x, L_CONNECT, b, LR_CONNECT, 1);
}

static int done_registered(Node **x) { return x[0]->registered; }
static int done_peer(Node **x) { return x[1]->peer; }
static int done_scanned(Node **x) { return x[1]->scanned > 0; }
static int done_joined(Node **x) { return net_session_state(x[1]->s) != NET_JOINING; }

static void t_lobby(uint16_t lport) {
    scenario = "lobby";
    lobby = (NetAddr){0x7f000001, lport};
    Node h, a;
    const char *pw = "KQXT4MNP7R";
    CHECK(node_new(&h, 0, compat_a, pw), "host");
    uint8_t ni[LDN_NETWORK_INFO];
    base_info(ni, 8);
    net_session_host(h.s, ni);
    net_session_on_lobby(h.s, lobby_msg, &h);
    CHECK(node_new(&a, 0, compat_a, pw), "station");
    net_session_on_lobby(a.s, lobby_msg, &a);
    Node *all[] = {&h, &a};
    lregister(&h, 0);
    CHECK(pump_until(all, 2, 2000, done_registered), "the lobby did not register the session");
    net_session_relay(h.s, h.session);
    char inv[NET_INVITE_LEN];
    net_invite_make(h.session, pw, inv);
    // the station reads the invite, finds the private session by number (a public scan does not list it)
    uint32_t sess;
    char got_pw[NET_PASSWORD_LETTERS + 1];
    CHECK(net_invite_read(inv, &sess, got_pw) && sess == h.session, "invite");
    lscan(&a, 0);
    pump(all, 2, 200);
    CHECK(a.scanned == 0, "a private session was listed publicly");
    lscan(&a, sess);
    CHECK(pump_until(all, 2, 2000, done_scanned), "scan by invite found nothing");
    // a stale invite: an unknown number gets nothing
    a.scanned = 0;
    lscan(&a, sess ^ 0x5555u);
    pump(all, 2, 200);
    CHECK(a.scanned == 0, "an unknown session answered");
    // direct, introduced by the lobby
    lconnect(&a, sess, 0);
    CHECK(pump_until(all, 2, 2000, done_peer) && a.peer_direct && a.peer_host.port == net_session_port(h.s),
          "no introduction (%d %d)", a.peer, a.peer_direct);
    net_session_relay(a.s, sess);
    net_session_join(a.s, a.peer_host, 0, ni + NI_SESSION_ID, user, 3, security);
    CHECK(pump_until(all, 2, 4000, done_joined) && net_session_state(a.s) == NET_JOINED, "direct join failed (%d)",
          net_session_refused(a.s));
    net_session_free(a.s);
    a.s = NULL;
    pump(all, 2, 50);
    // through the relay only
    node_new(&a, 0, compat_a, got_pw);
    net_session_on_lobby(a.s, lobby_msg, &a);
    all[1] = &a;
    lconnect(&a, sess, 1);
    CHECK(pump_until(all, 2, 2000, done_peer) && !a.peer_direct, "relay-only answer");
    net_session_relay(a.s, sess);
    net_session_join(a.s, (NetAddr){0, 0}, 1, ni + NI_SESSION_ID, user, 3, security);
    CHECK(pump_until(all, 2, 4000, done_joined) && net_session_state(a.s) == NET_JOINED, "relayed join failed (%d)",
          net_session_refused(a.s));
    net_session_send(a.s, 9, net_session_vip(h.s), 10, "via relay", 9);
    net_session_send(h.s, 10, net_session_vip(a.s), 9, "back", 4);
    uint8_t big[NET_PAYLOAD_MAX];
    memset(big, 3, sizeof big);
    pump(all, 2, 100);
    CHECK(h.got >= 1 && a.got >= 1 && !memcmp(h.data, "via relay", 9), "relayed data %d %d", h.got, a.got);
    net_session_send(a.s, 9, net_session_vip(h.s), 10, big, NET_PAYLOAD_MAX);
    pump(all, 2, 100);
    CHECK(h.n == NET_PAYLOAD_MAX, "the largest datagram did not pass the relay (%d)", h.n);
    // a stranger without the password, through the same relay, gets nothing
    Node z;
    node_new(&z, 0, compat_a, "AAAAAAAAAA");
    net_session_on_lobby(z.s, lobby_msg, &z);
    Node *all3[] = {&h, &z};
    lconnect(&z, sess, 1);
    pump_until(all3, 2, 2000, done_peer);
    net_session_relay(z.s, sess);
    net_session_join(z.s, (NetAddr){0, 0}, 1, ni + NI_SESSION_ID, user, 3, security);
    pump_until(all3, 2, 4000, done_joined);
    CHECK(net_session_state(z.s) == NET_GONE && net_session_refused(z.s) == NET_REFUSE_PASSWORD, "stranger: %d %d",
          net_session_state(z.s), net_session_refused(z.s));
    net_session_free(z.s), net_session_free(a.s), net_session_free(h.s);
    printf("ok lobby\n");
}

int main(int argc, char **argv) {
    verbose = getenv("NET_VERBOSE") != NULL;
    srand((unsigned)time(NULL));
    memcpy(security + SC_PASS, "mkw-passphrase", 14);
    st16(security + SC_PASS_SIZE, 14);
    const char *what = argc > 1 ? argv[1] : "";
    if (!strcmp(what, "lan")) t_lan();
    else if (!strcmp(what, "password")) t_password();
    else if (!strcmp(what, "version")) t_version();
    else if (!strcmp(what, "fuzz")) t_fuzz();
    else if (!strcmp(what, "invite")) t_invite();
    else if (!strcmp(what, "lobby") && argc > 2) t_lobby((uint16_t)atoi(argv[2]));
    else { fprintf(stderr, "usage: net_host lan|password|version|fuzz|invite|lobby PORT\n"); return 2; }
    return failures ? 1 : 0;
}
