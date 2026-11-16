// mkw-lobby: the lobby and relay of Mario Kart World local wireless play over the internet (docs/multiplayer.md).
//
// Why: two players behind home routers cannot find each other, and some cannot reach each other at all (symmetric NAT,
// CGNAT). The lobby lists public sessions (the game's own room search shows them as rooms), finds private ones by the
// number in an invite code, introduces a station to its host (each learns the other's public address from the lobby,
// never from the screen), and relays the sessions that still cannot connect directly. It never reads game traffic: what
// it relays is sealed by the session's keys, which it does not have. Nothing of the game is in here; anyone may run
// one (one UDP port, no state on disk).
//
// Limits: per address 20 messages a second (bursts twice), 2 hosted sessions, 10 wrong session numbers a minute;
// sessions expire 30 s after their host's last renewal; a session relays for at most 16 stations and 4 MiB/s; every
// answer longer than its request needs the cookie the lobby gave that address (no reflection).
//
// Build: cc -O2 -I runtime/net -I runtime/third_party/monocypher server/lobby/mkw_lobby.c runtime/net/net_proto.c
//        runtime/net/net_lobby.c runtime/net/net_udp.c runtime/third_party/monocypher/monocypher.c -o build/mkw-lobby
// Run:   build/mkw-lobby [--port 11461] [--words FILE] [--max-sessions 1024] [--no-relay] [--quiet]
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ldn_types.h"
#include "monocypher.h"
#include "net_lobby.h"
#include "net_udp.h"

enum {
    RATE = 20, ADDRS = 4096, SESSIONS_PER_IP = 2, EXPIRE_MS = 30000, FLOWS = 16, FLOW_IDLE_MS = 60000,
    ADMIT_MS = 30 * 60000, RELAY_BYTES = 4 << 20, BAD_SESSIONS = 10, SCAN_MAX = 16,
};

typedef struct { NetAddr addr; uint64_t last; uint64_t admitted; } Flow;
typedef struct {
    uint32_t id;
    uint8_t token[NET_TOKEN], compat[NET_COMPAT];
    NetAddr host;
    NetLobbyMeta meta;
    uint8_t info[LDN_NETWORK_INFO];
    uint64_t renewed;
    Flow flows[FLOWS];   // [k] is flow k + 1
    double relay_tokens;
    uint64_t relay_at, relayed;
} Session;
typedef struct { NetAddr addr; double tokens; uint64_t at; int bad; uint64_t bad_at; } Rate;

static Session *sessions;
static int max_sessions = 1024, relay_on = 1, quiet;
static Rate rates[ADDRS];
static uint8_t secret[32];
static int sock;
static char **words;
static int nwords;
static uint64_t stat_relay_bytes, stat_dropped;
static volatile sig_atomic_t stop;

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void say(const char *fmt, ...) {
    if (quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int random_bytes(void *p, size_t n) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return 0;
    size_t k = fread(p, 1, n, f);
    fclose(f);
    return k == n;
}

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static void put_addr(uint8_t *p, NetAddr a) { put32(p, a.ip), p[4] = (uint8_t)(a.port >> 8), p[5] = (uint8_t)a.port; }
static int same(NetAddr a, NetAddr b) { return a.ip == b.ip && a.port == b.port; }

static void send_msg(NetAddr to, int type, const void *body, int len) {
    uint8_t d[NET_WIRE_MAX];
    int n = net_put(d, type, body, len);
    if (n) net_udp_send(sock, d, n, to);
}

// ---- per-address limits
static Rate *rate_of(NetAddr a) {
    uint32_t h = (a.ip * 2654435761u) ^ a.port;
    Rate *r = &rates[h % ADDRS];
    if (!same(r->addr, a)) *r = (Rate){.addr = a, .tokens = 2 * RATE, .at = now_ms()};
    return r;
}

static int allowed(NetAddr a) {
    Rate *r = rate_of(a);
    uint64_t t = now_ms();
    r->tokens += (double)(t - r->at) * RATE / 1000.0;
    if (r->tokens > 2 * RATE) r->tokens = 2 * RATE;
    r->at = t;
    if (r->tokens < 1) { stat_dropped++; return 0; }
    r->tokens -= 1;
    return 1;
}

// Wrong session numbers (someone guessing invites): 10 a minute, then that address gets no answer for the minute.
static int guessing(NetAddr a, int wrong) {
    Rate *r = rate_of(a);
    uint64_t t = now_ms();
    if (t - r->bad_at > 60000) r->bad = 0, r->bad_at = t;
    if (wrong) r->bad++;
    return r->bad >= BAD_SESSIONS;
}

// ---- cookies: a keyed hash of the address and the minute; the current or the previous minute is accepted
static void cookie_of(NetAddr a, uint64_t minute, uint8_t out[NET_COOKIE]) {
    uint8_t m[14];
    put_addr(m, a);
    for (int i = 0; i < 8; i++) m[6 + i] = (uint8_t)(minute >> 8 * i);
    crypto_blake2b_keyed(out, NET_COOKIE, secret, sizeof secret, m, sizeof m);
}

static int cookie_ok(NetAddr a, const uint8_t *c) {
    uint64_t minute = (uint64_t)time(NULL) / 60;
    uint8_t want[NET_COOKIE];
    for (int k = 0; k < 2; k++) {
        cookie_of(a, minute - (uint64_t)k, want);
        if (!crypto_verify16(want, c)) return 1;
    }
    cookie_of(a, minute, want);
    send_msg(a, L_COOKIE, want, NET_COOKIE);   // never longer than the request it answers
    return 0;
}

// ---- sessions
static Session *find(uint32_t id) {
    if (!id) return NULL;
    for (int i = 0; i < max_sessions; i++)
        if (sessions[i].id == id) return &sessions[i];
    return NULL;
}

static int name_ok(char name[NET_NAME_MAX + 1]) {
    if (!net_name_ok(name)) return 0;
    char low[NET_NAME_MAX + 1];
    int n = 0;
    for (const char *p = name; *p; p++)
        if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) low[n++] = *p;
        else if (*p >= 'A' && *p <= 'Z') low[n++] = (char)(*p - 'A' + 'a');
    low[n] = 0;
    for (int i = 0; i < nwords; i++)
        if (strstr(low, words[i])) return 0;
    return 1;
}

static void on_register(NetAddr from, const uint8_t *b) {
    uint32_t id = get32(b + NET_COMPAT);
    const uint8_t *token = b + NET_COMPAT + 4, *meta = token + NET_TOKEN, *info = meta + NET_META;
    uint8_t reply[LR_REGISTERED] = {0};
    Session *s = find(id);
    if (id && (!s || crypto_verify16(s->token, token))) {   // not this host's: refused as unknown
        put32(reply, id), reply[5] = 1;
        send_msg(from, L_REGISTERED, reply, LR_REGISTERED);
        return;
    }
    NetLobbyMeta m;
    net_meta_get(meta, &m);
    char name[NET_NAME_MAX + 1];
    memcpy(name, m.name, NET_NAME_MAX);
    name[NET_NAME_MAX] = 0;
    int ok = (!(m.flags & NET_LOBBY_PUBLIC) || name_ok(name)) && m.slots >= 1 && m.slots <= LDN_NODES &&
             m.players <= m.slots && m.region <= 6;
    if (!ok) {
        put32(reply, id), reply[5] = 2;
        send_msg(from, L_REGISTERED, reply, LR_REGISTERED);
        return;
    }
    if (!s) {
        int mine = 0;
        Session *free_slot = NULL;
        for (int i = 0; i < max_sessions; i++) {
            if (sessions[i].id && sessions[i].host.ip == from.ip) mine++;
            if (!sessions[i].id && !free_slot) free_slot = &sessions[i];
        }
        if (mine >= SESSIONS_PER_IP || !free_slot) {
            reply[5] = 3;
            send_msg(from, L_REGISTERED, reply, LR_REGISTERED);
            return;
        }
        s = free_slot;
        memset(s, 0, sizeof *s);
        do random_bytes(&s->id, sizeof s->id); while (!s->id || find(s->id) != s);
        memcpy(s->token, token, NET_TOKEN);
        s->relay_tokens = RELAY_BYTES, s->relay_at = now_ms();
        say("session %08x registered (%s, %d/%d)", s->id, m.flags & NET_LOBBY_PUBLIC ? "public" : "private", m.players, m.slots);
    }
    memcpy(s->compat, b, NET_COMPAT);
    s->host = from;   // a router may have moved the host's port: the token proves it is the same host
    memset(name + strlen(name), 0, NET_NAME_MAX + 1 - strlen(name));
    memcpy(m.name, name, NET_NAME_MAX);
    s->meta = m;
    memcpy(s->info, info, LDN_NETWORK_INFO);
    s->renewed = now_ms();
    put32(reply, s->id), reply[4] = 1;
    send_msg(from, L_REGISTERED, reply, LR_REGISTERED);
}

static void on_unregister(NetAddr from, const uint8_t *b) {
    Session *s = find(get32(b));
    if (s && same(s->host, from) && !crypto_verify16(s->token, b + 4)) {
        say("session %08x closed by its host", s->id);
        memset(s, 0, sizeof *s);
    }
}

static int listed(const Session *s, const uint8_t *compat) {
    return s->id && (s->meta.flags & NET_LOBBY_PUBLIC) && !memcmp(s->compat, compat, NET_COMPAT);
}

static void on_list(NetAddr from, const uint8_t *b) {
    if (!cookie_ok(from, b)) return;
    const uint8_t *compat = b + NET_COOKIE, *f = compat + NET_COMPAT;
    uint8_t out[2 + NET_LIST_PAGE * NET_ENTRY];
    int n = 0, total = 0, skip = f[3] * NET_LIST_PAGE;
    for (int i = 0; i < max_sessions; i++) {
        Session *s = &sessions[i];
        if (!listed(s, compat) || (f[0] && s->meta.region && s->meta.region != f[0]) || (f[1] && s->meta.mode != f[1]) ||
            (f[2] && (s->meta.flags & (NET_ADV_PASSWORD | NET_ADV_LOCKED | NET_ADV_FULL))))
            continue;
        if (total++ < skip || n == NET_LIST_PAGE) continue;
        put32(out + 2 + n * NET_ENTRY, s->id);
        net_meta_put(out + 2 + n * NET_ENTRY + 4, &s->meta);
        n++;
    }
    out[0] = (uint8_t)n, out[1] = (uint8_t)(total > 255 ? 255 : total);
    send_msg(from, L_LIST_REPLY, out, 2 + n * NET_ENTRY);
}

static void scan_reply(NetAddr to, const Session *s) {
    uint8_t out[LR_SCAN_REPLY];
    put32(out, s->id);
    out[4] = s->meta.flags & (NET_ADV_PASSWORD | NET_ADV_LOCKED | NET_ADV_FULL);
    memcpy(out + 5, s->info, LDN_NETWORK_INFO);
    send_msg(to, L_SCAN_REPLY, out, LR_SCAN_REPLY);
}

static void on_scan(NetAddr from, const uint8_t *b) {
    if (!cookie_ok(from, b)) return;
    const uint8_t *compat = b + NET_COOKIE;
    uint32_t id = get32(compat + NET_COMPAT);
    if (id) {   // by invite: public or private, the same build only
        if (guessing(from, 0)) return;
        Session *s = find(id);
        if (!s || memcmp(s->compat, compat, NET_COMPAT)) { guessing(from, 1); return; }
        scan_reply(from, s);
        return;
    }
    int n = 0;
    for (int i = 0; i < max_sessions && n < SCAN_MAX; i++)
        if (listed(&sessions[i], compat) && !(sessions[i].meta.flags & (NET_ADV_LOCKED | NET_ADV_FULL)))
            scan_reply(from, &sessions[i]), n++;
}

static void on_connect(NetAddr from, const uint8_t *b) {
    if (!cookie_ok(from, b)) return;
    if (guessing(from, 0)) return;
    const uint8_t *compat = b + NET_COOKIE;
    Session *s = find(get32(compat + NET_COMPAT));
    int relay_only = compat[NET_COMPAT + 4];
    if (!s || memcmp(s->compat, compat, NET_COMPAT)) { guessing(from, 1); return; }
    // admit the station to the relay for this session: its address may send relayed datagrams for 30 minutes
    Flow *f = NULL, *oldest = NULL;
    for (int k = 0; k < FLOWS; k++) {
        Flow *x = &s->flows[k];
        if (same(x->addr, from)) f = x;
        if (!oldest || x->last < oldest->last) oldest = x;
    }
    if (!f) {
        f = oldest;
        if (f->addr.ip && now_ms() - f->last < FLOW_IDLE_MS) f = NULL;   // every flow busy
        else memset(f, 0, sizeof *f), f->addr = from;
    }
    if (f) f->admitted = now_ms(), f->last = now_ms();
    uint8_t peer[LR_PEER] = {0};
    put32(peer, s->id);
    if (!relay_only) {
        uint8_t intro[LR_INTRODUCE];
        put32(intro, s->id);
        put_addr(intro + 4, from);
        send_msg(s->host, L_INTRODUCE, intro, LR_INTRODUCE);
        peer[4] = 1;
        put_addr(peer + 5, s->host);
    }
    if (!relay_on && relay_only) return;   // no way through here
    send_msg(from, L_PEER, peer, LR_PEER);
}

static int relay_budget(Session *s, int n) {
    uint64_t t = now_ms();
    s->relay_tokens += (double)(t - s->relay_at) * RELAY_BYTES / 1000.0;
    if (s->relay_tokens > 2.0 * RELAY_BYTES) s->relay_tokens = 2.0 * RELAY_BYTES;
    s->relay_at = t;
    if (s->relay_tokens < n) { stat_dropped++; return 0; }
    s->relay_tokens -= n;
    return 1;
}

static void on_relay(NetAddr from, const uint8_t *b, int len) {
    if (!relay_on) return;
    Session *s = find(get32(b));
    if (!s) return;
    uint16_t flow = (uint16_t)(b[4] << 8 | b[5]);
    uint8_t out[NET_RELAY_HEAD + NET_DGRAM_MAX];
    memcpy(out, b, (size_t)len);
    uint64_t t = now_ms();
    if (same(from, s->host)) {   // host to a station
        if (!flow || flow > FLOWS) return;
        Flow *f = &s->flows[flow - 1];
        if (!f->addr.ip || t - f->admitted > ADMIT_MS || !relay_budget(s, len)) return;
        out[4] = out[5] = 0;
        send_msg(f->addr, M_RELAY, out, len);
    } else {                     // a station to its host
        if (flow) return;
        int k = 0;
        while (k < FLOWS && !same(s->flows[k].addr, from)) k++;
        if (k == FLOWS || t - s->flows[k].admitted > ADMIT_MS || !relay_budget(s, len)) return;
        s->flows[k].last = t;
        out[4] = (uint8_t)((k + 1) >> 8), out[5] = (uint8_t)(k + 1);
        send_msg(s->host, M_RELAY, out, len);
    }
    s->relayed += (uint64_t)len;
    stat_relay_bytes += (uint64_t)len;
}

static void handle(NetAddr from, const uint8_t *d, int n) {
    int type, len;
    const uint8_t *b;
    if (!net_lobby_parse(d, n, &type, &b, &len)) { stat_dropped++; return; }
    // relayed traffic has its own budget per session; everything else the per-address one
    if (type == M_RELAY) { on_relay(from, b, len); return; }
    if (!allowed(from)) return;
    switch (type) {
    case L_REGISTER: on_register(from, b); break;
    case L_UNREGISTER: on_unregister(from, b); break;
    case L_LIST: on_list(from, b); break;
    case L_SCAN: on_scan(from, b); break;
    case L_CONNECT: on_connect(from, b); break;
    default: stat_dropped++; break;   // answers are for clients, not for the lobby
    }
}

static void expire(void) {
    uint64_t t = now_ms();
    for (int i = 0; i < max_sessions; i++)
        if (sessions[i].id && t - sessions[i].renewed > EXPIRE_MS) {
            say("session %08x expired (its host stopped renewing it)", sessions[i].id);
            memset(&sessions[i], 0, sizeof sessions[i]);
        }
}

static void load_words(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "mkw-lobby: cannot read %s\n", path); exit(2); }
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char w[128];
        int n = 0;
        for (char *p = line; *p; p++)
            if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) w[n++] = *p;
            else if (*p >= 'A' && *p <= 'Z') w[n++] = (char)(*p - 'A' + 'a');
        if (n < 3) continue;
        w[n] = 0;
        words = realloc(words, sizeof *words * (size_t)(nwords + 1));
        words[nwords++] = strdup(w);
    }
    fclose(f);
}

static void on_signal(int sig) { (void)sig; stop = 1; }

int main(int argc, char **argv) {
    unsigned port = NET_PORT_DEFAULT + 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--words") && i + 1 < argc) load_words(argv[++i]);
        else if (!strcmp(argv[i], "--max-sessions") && i + 1 < argc) max_sessions = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-relay")) relay_on = 0;
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else {
            fprintf(stderr, "usage: mkw-lobby [--port N] [--words FILE] [--max-sessions N] [--no-relay] [--quiet]\n");
            return 2;
        }
    }
    if (max_sessions < 1 || max_sessions > 65536 || !port || port > 65535) { fprintf(stderr, "mkw-lobby: bad limits\n"); return 2; }
    sessions = calloc((size_t)max_sessions, sizeof *sessions);
    if (!sessions || !random_bytes(secret, sizeof secret)) { fprintf(stderr, "mkw-lobby: no memory or randomness\n"); return 1; }
    uint16_t bound;
    sock = net_udp_open(0, (uint16_t)port, 0, &bound);
    if (sock < 0) { fprintf(stderr, "mkw-lobby: %s\n", net_udp_error()); return 1; }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "mkw-lobby: UDP port %u, up to %d sessions, relay %s\n", bound, max_sessions, relay_on ? "on" : "off");
    uint64_t last_expire = now_ms(), last_stats = now_ms();
    while (!stop) {
        net_udp_wait(sock, 200);
        uint8_t d[NET_WIRE_MAX + 16];
        NetAddr from;
        for (int k = 0; k < 1024; k++) {
            int n = net_udp_recv(sock, d, sizeof d, &from);
            if (n <= 0) break;
            handle(from, d, n);
        }
        uint64_t t = now_ms();
        if (t - last_expire >= 1000) expire(), last_expire = t;
        if (t - last_stats >= 60000) {
            int live = 0;
            for (int i = 0; i < max_sessions; i++) live += sessions[i].id != 0;
            say("stats: %d sessions, %.1f KiB/s relayed, %llu dropped", live, (double)stat_relay_bytes / 1024 / 60,
                (unsigned long long)stat_dropped);
            stat_relay_bytes = 0, last_stats = t;
        }
    }
    net_udp_close(sock);
    return 0;
}
