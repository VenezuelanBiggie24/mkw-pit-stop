// nn::ldn (local wireless play) for MKW, carried over UDP instead of the console's radio (docs/multiplayer.md).
//
// MKW's network layer (main+0x22477b4, mode 2) needs only nn::ldn and nn::socket for local wireless: it never asks
// nn::nifm, so nifm stays a console without a network (sdk/ov/ov_net.c) and the game never tries Nintendo's online
// service. lan (the default since a PS5 and a PC raced a whole race together, 17/11/2026) finds rooms by a UDP broadcast
// and nothing listens until the game makes or joins a room. With MKW_LOCAL_WIRELESS=off nn::ldn::Initialize fails as
// on a console in airplane mode. With online, rooms are also found through the lobby (MKW_LOBBY, server/lobby):
// public sessions show as rooms in the game's own room search, private ones by invite code (MKW_NET_JOIN), and the
// lobby's relay carries a session that a router keeps from connecting directly.
//
// The calls run on the game's thread and give the virtual core up while they wait (Scan, Connect). A thread of ours
// ("mkw-net") reads the session's socket and runs its timers; every call takes g_lock. The ldn state machine follows
// Ryujinx's IUserLocalCommunicationService (src/Ryujinx.HLE/HOS/Services/Ldn @ 6af327bd, MIT): states, results and
// what each call accepts in which state.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gamelib.h"
#include "host.h"
#include "net_game.h"
#include "net_lobby.h"
#include "net_session.h"
#include "net_ui.h"
#include "platform/platform.h"

#define LDN_R(d) (203u | (uint32_t)(d) << 9)   // module 203; descriptions from Ryujinx's Ldn/ResultCode.cs
enum {
    R_DISABLED = LDN_R(22), R_INVALID_STATE = LDN_R(32), R_NODE_NOT_FOUND = LDN_R(48), R_CONNECT_FAILURE = LDN_R(64),
    R_CONNECT_NOT_FOUND = LDN_R(65), R_CONNECT_TIMEOUT = LDN_R(66), R_CONNECT_REJECTED = LDN_R(67),
    R_INVALID_ARGUMENT = LDN_R(96), R_TOO_MANY = LDN_R(144),
};

enum { MODE_OFF, MODE_LAN, MODE_ONLINE };
enum { SCAN_LAN_MS = 400, SCAN_ONLINE_MS = 900, PEER_WAIT_MS = 1500, JOIN_WAIT_MS = 9000, REGISTER_EVERY_MS = 10000 };

static PlatMutex g_lock = PLAT_MUTEX_INIT;
static PlatCond g_cond;
static NetSession *g_sess;
static int g_state, g_reason, g_mode = -1, g_thread;
static uint64_t g_event;                    // the game's state change SystemEventType, 0 none
static uint8_t g_compat[NET_COMPAT];
static NetUiStatus g_ui;                    // what the Pit Stop page shows (net_ui.h)
static uint8_t g_adv[LDN_ADVERTISE_MAX];    // advertise data set on an open access point, before CreateNetwork
static uint16_t g_adv_size;
// lobby client
static NetAddr g_lobby;
static uint8_t g_cookie[NET_COOKIE], g_token[NET_TOKEN];
static uint32_t g_lobby_session, g_invite_session;
static char g_invite_password[NET_PASSWORD_LETTERS + 1];
static uint64_t g_registered_at, g_info_dirty_at;
static uint8_t g_pending[NET_DGRAM_MAX];    // the last request that needs a cookie, sent again when one arrives
static int g_pending_len, g_pending_type;
static uint32_t g_found_session[NET_FOUND_MAX];   // per found host: its lobby session, 0 found on the LAN
static struct { int got, direct; uint32_t session; NetAddr host; } g_peer;
static NetLobbyEntry g_list[NET_LIST_PAGE];
static int g_list_n;

static uint64_t now_ms(void) { return plat_now_ms(); }
static int rnd(void *p, size_t n) { return plat_random(p, n); }
static void put32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static uint32_t get32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int mode(void) {
    if (g_mode < 0) {
        const char *m = cfg_value("MKW_LOCAL_WIRELESS");
        g_mode = !strcmp(m, "lan") ? MODE_LAN : !strcmp(m, "online") ? MODE_ONLINE : MODE_OFF;
    }
    return g_mode;
}
// The game's sockets belong to local wireless only while ldn is initialized: MKW's local wireless mode initializes ldn
// before it opens a socket (main+0x22477b4), its other modes never do, and their sockets stay the host's (sdk/ov/
// ov_socket.c, MKW_LAN) now that lan is the default.
int net_game_enabled(void) { return mode() != MODE_OFF && g_state != LDN_STATE_NONE; }
void net_game_mode_reread(void) { g_mode = -1; }   // probes: MKW_LOCAL_WIRELESS read again at the next call

// Only builds whose tag is equal play together: the protocol, the game's main executable, the runtime version.
static void compat_init(void) {
    const RtGame *g = rt_game();
    const char *sha = "";
    for (int i = 0; i < g->nexefs; i++)
        if (!strcmp(g->exefs_sha256[i].file, "main")) sha = g->exefs_sha256[i].sha256;
    net_compat_tag(sha, g->version, rt_port_version, g_compat);
}

// The lobby's address from MKW_LOBBY ("a.b.c.d:port" or "name:port"; a name is resolved once, here).
static void lobby_init(void) {
    g_lobby = (NetAddr){0, 0};
    const char *l = cfg_get("MKW_LOBBY");
    if (mode() != MODE_ONLINE || !l || !*l) return;
    char host[128];
    unsigned port = NET_PORT_DEFAULT + 1;
    snprintf(host, sizeof host, "%s", l);
    char *colon = strrchr(host, ':');
    if (colon) *colon = 0, port = (unsigned)atoi(colon + 1);
    uint32_t ip = net_ip_parse(host);
    if (!ip) ip = net_resolve(host);
    if (!ip || !port || port > 65535) { rt_log("net: MKW_LOBBY '%s' is not an address: online off, LAN only", l); return; }
    g_lobby = (NetAddr){ip, (uint16_t)port};
    char a[16];
    net_ip_format(ip, a);
    rt_log("net: lobby %s:%u", a, port);
}

static void signal_change(void) {
    plat_cond_signal(&g_cond);
    if (g_event) kern_signal_system_event(g_event);
}

// ---- lobby client (under g_lock)
static void lobby_send(int type, const uint8_t *body, int len) {
    if (!g_sess || !g_lobby.ip) return;
    uint8_t d[NET_DGRAM_MAX];
    int n = net_put(d, type, body, len);
    if (type == L_SCAN || type == L_CONNECT || type == L_LIST) memcpy(g_pending, body, (size_t)len), g_pending_len = len, g_pending_type = type;
    net_session_raw(g_sess, g_lobby, d, n);
}

static void lobby_meta(NetLobbyMeta *m) {
    const uint8_t *ni = net_session_info(g_sess);
    memset(m, 0, sizeof *m);
    int pub = !strcmp(cfg_value("MKW_NET_HOST"), "public");
    m->flags = (uint8_t)((pub ? NET_LOBBY_PUBLIC : 0) | (g_ui.password[0] ? NET_ADV_PASSWORD : 0) | (g_ui.locked ? NET_ADV_LOCKED : 0));
    m->players = ni[NI_NODE_COUNT], m->slots = ni[NI_NODE_MAX] ? ni[NI_NODE_MAX] : LDN_NODES;
    static const char *const regions[] = {"any", "eu", "na", "sa", "asia", "oc", "af"};
    const char *r = cfg_value("MKW_NET_REGION");
    for (int i = 0; i < 7; i++) if (!strcmp(r, regions[i])) m->region = (uint8_t)i;
    const char *name = cfg_get("MKW_NET_NAME");
    snprintf(m->name, sizeof m->name, "%s", name && *name ? name : "Mario Kart World room");
}

static void lobby_register(void) {
    if (!g_sess || !g_lobby.ip || net_session_state(g_sess) != NET_HOSTING) return;
    uint8_t b[LR_REGISTER];
    NetLobbyMeta m;
    lobby_meta(&m);
    memcpy(b, g_compat, NET_COMPAT);
    put32be(b + NET_COMPAT, g_lobby_session);
    memcpy(b + NET_COMPAT + 4, g_token, NET_TOKEN);
    net_meta_put(b + NET_COMPAT + 4 + NET_TOKEN, &m);
    memcpy(b + NET_COMPAT + 4 + NET_TOKEN + NET_META, net_session_info(g_sess), LDN_NETWORK_INFO);
    lobby_send(L_REGISTER, b, LR_REGISTER);
    g_registered_at = now_ms();
}

static void lobby_message(void *ctx, int type, const uint8_t *b, int len) {
    (void)ctx, (void)len;
    if (type == L_COOKIE) {
        memcpy(g_cookie, b, NET_COOKIE);
        if (g_pending_len) {   // the request again, with the cookie
            memcpy(g_pending, g_cookie, NET_COOKIE);
            int t = g_pending_type, n = g_pending_len;
            g_pending_len = 0;
            uint8_t copy[NET_DGRAM_MAX];
            memcpy(copy, g_pending, (size_t)n);
            lobby_send(t, copy, n);
        }
    } else if (type == L_REGISTERED) {
        if (b[4]) {
            if (!g_lobby_session) rt_log("net: the lobby lists this session (%s)", g_ui.public_session ? "public" : "private, by invite");
            g_lobby_session = get32be(b);
            net_session_relay(g_sess, g_lobby_session);
            g_ui.lobby_session = g_lobby_session;
            if (g_ui.password[0]) net_invite_make(g_lobby_session, g_ui.password, g_ui.invite);
        } else
            rt_log("net: the lobby refused this session (reason %d: a name it does not accept, or too many sessions)", b[5]);
    } else if (type == L_SCAN_REPLY) {
        NetFound f = {.via_relay = 1, .flags = b[4]};
        memcpy(f.info, b + 5, LDN_NETWORK_INFO);
        int i = net_session_found_put(g_sess, &f);
        if (i >= 0) g_found_session[i] = get32be(b);
    } else if (type == L_PEER) {
        g_peer.got = 1, g_peer.session = get32be(b), g_peer.direct = b[4];
        g_peer.host = (NetAddr){get32be(b + 5), (uint16_t)(b[9] << 8 | b[10])};
        plat_cond_signal(&g_cond);
    } else if (type == L_INTRODUCE) {
        if (get32be(b) != g_lobby_session) return;
        net_session_punch(g_sess, (NetAddr){get32be(b + 4), (uint16_t)(b[8] << 8 | b[9])});
    } else if (type == L_LIST_REPLY) {
        int n = b[0] < NET_LIST_PAGE ? b[0] : NET_LIST_PAGE;
        if (len < 2 + n * NET_ENTRY) return;
        for (int i = 0; i < n; i++) {
            g_list[i].session = get32be(b + 2 + i * NET_ENTRY);
            net_meta_get(b + 2 + i * NET_ENTRY + 4, &g_list[i].meta);
        }
        g_list_n = n;
    }
}

// ---- the session's thread
static void on_data(void *ctx, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t *p, int n) {
    (void)ctx;
    net_sock_deliver(src_ip, src_port, dst_ip, dst_port, p, n);
}

static void on_change(void *ctx) {
    (void)ctx;
    int st = net_session_state(g_sess);
    if (st == NET_GONE && g_state == LDN_STATE_STATION_CONNECTED) {
        g_state = LDN_STATE_STATION, g_reason = net_session_reason(g_sess);
        net_sock_session_ended();
    }
    signal_change();
}

static void net_main(void *arg) {
    (void)arg;
    for (;;) {
        plat_mutex_lock(&g_lock);
        NetSession *s = g_sess;
        int fd = s ? net_session_fd(s) : -1;
        plat_mutex_unlock(&g_lock);
        if (fd < 0) { plat_sleep_ms(20); continue; }
        net_udp_wait(fd, 10);
        plat_mutex_lock(&g_lock);
        if (g_sess == s) {
            net_session_poll(s);
            uint64_t t = now_ms();
            int hosting = net_session_state(s) == NET_HOSTING;
            if (hosting && g_lobby.ip && (t - g_registered_at >= REGISTER_EVERY_MS ||
                                          (g_info_dirty_at && t - g_registered_at >= 1000))) {
                g_info_dirty_at = 0;
                lobby_register();
            }
            NetStats st;
            net_session_stats(s, &st);
            memcpy(g_ui.rtt_ms, st.rtt_ms, sizeof g_ui.rtt_ms);
            g_ui.dropped = st.dropped;
            // every 10 s in a session: what one session costs (docs/multiplayer.md, "Running a lobby")
            static uint64_t stats_at, tx0, rx0, txd0, rxd0;
            int live = hosting || net_session_state(s) == NET_JOINED;
            if (!live) stats_at = 0;
            else if (!stats_at) stats_at = t, tx0 = st.tx_bytes, rx0 = st.rx_bytes, txd0 = st.tx_dgrams, rxd0 = st.rx_dgrams;
            else if (t - stats_at >= 10000) {
                double sec = (double)(t - stats_at) / 1000.0;
                rt_log("net: %s, %d nodes: out %.1f KiB/s %.0f dgram/s, in %.1f KiB/s %.0f dgram/s, rtt %u ms, dropped %llu",
                       hosting ? "hosting" : "joined", net_session_info(s)[NI_NODE_COUNT], (double)(st.tx_bytes - tx0) / 1024 / sec,
                       (double)(st.tx_dgrams - txd0) / sec, (double)(st.rx_bytes - rx0) / 1024 / sec, (double)(st.rx_dgrams - rxd0) / sec,
                       hosting ? st.rtt_ms[1] : st.rtt_ms[0], (unsigned long long)st.dropped);
                stats_at = t, tx0 = st.tx_bytes, rx0 = st.rx_bytes, txd0 = st.tx_dgrams, rxd0 = st.rx_dgrams;
            }
        }
        plat_mutex_unlock(&g_lock);
    }
}

// A session of ours, port 0 for any (a station). Under g_lock.
static int session_open(uint16_t port) {
    if (g_sess) return 1;
    if (!g_thread) {
        plat_cond_init(&g_cond);
        g_thread = plat_thread_spawn(net_main, NULL, "mkw-net", 256 * 1024);
        if (!g_thread) { rt_log("net: cannot start the network thread"); return 0; }
    }
    NetConfig cfg = {.port = port, .lobby = g_lobby, .random = rnd, .now_ms = now_ms, .log = rt_log,
                     .on_data = on_data, .on_change = on_change};
    memcpy(cfg.compat, g_compat, NET_COMPAT);
    g_sess = net_session_new(&cfg);
    if (!g_sess) { rt_log("net: no UDP socket on port %u (%s)", port, net_udp_error()); return 0; }
    net_session_on_lobby(g_sess, lobby_message, NULL);
    memset(g_found_session, 0, sizeof g_found_session);
    return 1;
}

static void session_close(void) {
    if (!g_sess) return;
    if (g_lobby.ip && g_lobby_session && net_session_state(g_sess) == NET_HOSTING) {
        uint8_t b[LR_UNREGISTER];
        put32be(b, g_lobby_session);
        memcpy(b + 4, g_token, NET_TOKEN);
        lobby_send(L_UNREGISTER, b, LR_UNREGISTER);
    }
    net_session_free(g_sess);
    g_sess = NULL, g_lobby_session = 0;
    memset(&g_ui.invite, 0, sizeof g_ui.invite);
    g_ui.lobby_session = 0;
    net_sock_session_ended();
}

// Waits on the game's thread with its virtual core given up, until done() or ms pass. Under g_lock.
// Polls in 10 ms sleeps with g_lock released, so the session thread runs meanwhile and the wait ends on time whether or
// not anything signals it; the virtual core is taken back with g_lock released too, as a game thread blocked on g_lock
// keeps its core while it waits. (17/11/2026 on the PS5 the first Scan never returned: the session thread sat in a
// blocking recvfrom with g_lock held, net_udp.c now never blocks.)
static int wait_for(int (*done)(void), uint32_t ms) {
    uint64_t end = now_ms() + ms;
    int away = kern_core_away();
    while (!done() && now_ms() < end) {
        plat_mutex_unlock(&g_lock);
        plat_sleep_ms(10);
        plat_mutex_lock(&g_lock);
    }
    plat_mutex_unlock(&g_lock);
    kern_core_back(&away);
    plat_mutex_lock(&g_lock);
    return done();
}

// ---- nn::ldn
static void ov_ldn_initialize(Cpu *c) {
    plat_mutex_lock(&g_lock);
    uint32_t r = 0;
    if (mode() == MODE_OFF) r = R_DISABLED;
    else if (g_state == LDN_STATE_NONE) {
        compat_init();
        lobby_init();
        g_state = LDN_STATE_INITIALIZED;
        rt_log("net: local wireless over %s (port %d)", mode() == MODE_LAN ? "the local network" : "the local network and the lobby",
               cfg_int("MKW_NET_PORT"));
    }
    g_ui.mode = mode();
    plat_mutex_unlock(&g_lock);
    ret_x(c, r);
}

static void ov_ldn_finalize(Cpu *c) {
    (void)c;
    plat_mutex_lock(&g_lock);
    session_close();
    g_state = LDN_STATE_NONE;
    g_event = 0;   // the game destroys its state change event after Finalize and attaches it again later
    plat_mutex_unlock(&g_lock);
}

static void ov_ldn_state(Cpu *c) { ret_x(c, (uint32_t)g_state); }
static void ov_ldn_reason(Cpu *c) { ret_x(c, (uint32_t)g_reason); }

static void set_state(int st) { g_state = st, g_ui.ldn_state = st; signal_change(); }

static void ov_open_ap(Cpu *c) {
    plat_mutex_lock(&g_lock);
    if (g_state == LDN_STATE_INITIALIZED) g_adv_size = 0, memset(g_adv, 0, sizeof g_adv);   // a new access point
    uint32_t r = g_state == LDN_STATE_INITIALIZED || g_state == LDN_STATE_AP ? (set_state(LDN_STATE_AP), 0u) : R_INVALID_STATE;
    plat_mutex_unlock(&g_lock);
    ret_x(c, r);
}

static void ov_close_ap(Cpu *c) {
    plat_mutex_lock(&g_lock);
    uint32_t r = R_INVALID_STATE;
    if (g_state == LDN_STATE_AP || g_state == LDN_STATE_AP_CREATED) session_close(), set_state(LDN_STATE_INITIALIZED), r = 0;
    plat_mutex_unlock(&g_lock);
    ret_x(c, r);
}

// NetworkInfo of a new network from the game's NetworkConfig, SecurityConfig and UserConfig; sp: SecurityParameter of
// a private network (its session id and data), NULL for a new one.
static void build_info(uint8_t *ni, const uint8_t *cfg, const uint8_t *sec, const uint8_t *sp, const uint8_t *user) {
    memset(ni, 0, LDN_NETWORK_INFO);
    memcpy(ni + NI_INTENT, cfg + NC_INTENT, 16);
    if (sp) memcpy(ni + NI_SESSION_ID, sp + SP_SESSION_ID, 16), memcpy(ni + NI_SEC_PARAM, sp + SP_DATA, 16);
    else rnd(ni + NI_SESSION_ID, 16), rnd(ni + NI_SEC_PARAM, 16);
    static const char hex[] = "0123456789abcdef";
    ni[NI_SSID_LEN] = 32;
    for (int i = 0; i < 16; i++) ni[NI_SSID + 2 * i] = (uint8_t)hex[ni[NI_SESSION_ID + i] >> 4], ni[NI_SSID + 2 * i + 1] = (uint8_t)hex[ni[NI_SESSION_ID + i] & 15];
    uint16_t ch = ld16(cfg + NC_CHANNEL);
    st16(ni + NI_CHANNEL, ch ? ch : 6);
    ni[NI_LINK_LEVEL] = 3, ni[NI_NETWORK_TYPE] = 2;
    st16(ni + NI_SEC_MODE, ld16(sec + SC_MODE));
    int max = cfg[NC_NODE_MAX];
    ni[NI_NODE_MAX] = (uint8_t)(max > 0 && max <= LDN_NODES ? max : LDN_NODES);
    uint8_t *n0 = ni_node(ni, 0);
    memcpy(n0 + NODE_NAME, user, LDN_USERNAME);
    st16(n0 + NODE_LCV, ld16(cfg + NC_LCV));
}

static void create(Cpu *c, const uint8_t *cfg, const uint8_t *sec, const uint8_t *sp, const uint8_t *user) {
    if (!cfg || !sec || !user) { ret_x(c, R_INVALID_ARGUMENT); return; }
    plat_mutex_lock(&g_lock);
    uint32_t r = 0;
    if (g_state != LDN_STATE_AP) r = R_INVALID_STATE;
    else if (!session_open((uint16_t)cfg_int("MKW_NET_PORT"))) r = R_DISABLED;
    else {
        uint8_t ni[LDN_NETWORK_INFO];
        build_info(ni, cfg, sec, sp, user);
        memcpy(ni + NI_ADV, g_adv, LDN_ADVERTISE_MAX);   // what SetAdvertiseData set before CreateNetwork
        st16(ni + NI_ADV_SIZE, g_adv_size);
        net_session_host(g_sess, ni);
        net_session_security(g_sess, sec);
        g_ui.public_session = !strcmp(cfg_value("MKW_NET_HOST"), "public");
        memset(g_ui.password, 0, sizeof g_ui.password);
        if (mode() == MODE_ONLINE && !g_ui.public_session) {   // private: a made password, for the invite code
            uint8_t b[NET_PASSWORD_LETTERS];
            rnd(b, sizeof b);
            net_password_new(b, g_ui.password);
        }
        net_session_password(g_sess, g_ui.password);
        g_ui.locked = 0;
        rnd(g_token, sizeof g_token);
        g_lobby_session = 0;
        lobby_register();
        set_state(LDN_STATE_AP_CREATED);
    }
    plat_mutex_unlock(&g_lock);
    ret_x(c, r);
}

// Result CreateNetwork(const NetworkConfig &, const SecurityConfig &, const UserConfig &)
static void ov_create(Cpu *c) { create(c, arg_p(c, 0), arg_p(c, 1), NULL, arg_p(c, 2)); }
// Result CreateNetworkPrivate(const NetworkConfig &, const SecurityConfig &, const SecurityParameter &,
//                             const UserConfig &, int count, const AddressEntry *)
static void ov_create_private(Cpu *c) { create(c, arg_p(c, 0), arg_p(c, 1), arg_p(c, 2), arg_p(c, 3)); }

static void ov_destroy(Cpu *c) {
    plat_mutex_lock(&g_lock);
    uint32_t r = R_INVALID_STATE;
    if (g_state == LDN_STATE_AP_CREATED) session_close(), set_state(LDN_STATE_AP), r = 0;
    plat_mutex_unlock(&g_lock);
    ret_x(c, r);
}

// Result SetAdvertiseData(const void *, size_t): the game's room description, which every scan shows
static void ov_advertise(Cpu *c) {
    const uint8_t *p = arg_p(c, 0);
    uint64_t n = arg_x(c, 1);
    if (n > LDN_ADVERTISE_MAX || (n && !p)) { ret_x(c, R_INVALID_ARGUMENT); return; }
    plat_mutex_lock(&g_lock);
    uint32_t r = 0;
    if (g_sess && net_session_state(g_sess) == NET_HOSTING) {
        uint8_t *ni = net_session_info(g_sess);
        memset(ni + NI_ADV, 0, LDN_ADVERTISE_MAX);
        if (n) memcpy(ni + NI_ADV, p, n);
        st16(ni + NI_ADV_SIZE, (uint16_t)n);
        net_session_info_changed(g_sess);
        g_info_dirty_at = now_ms();
    } else if (g_state == LDN_STATE_AP) {
        // Kept for the network CreateNetwork makes: MKW's PIA sets its room description between OpenAccessPoint and
        // CreateNetwork, then reads it back with GetNetworkInfo and destroys the room when it is missing (17/11/2026: the
        // character select showed for half a second, then error 2318-0008).
        memset(g_adv, 0, sizeof g_adv);
        if (n) memcpy(g_adv, p, n);
        g_adv_size = (uint16_t)n;
    } else
        r = R_INVALID_STATE;
    plat_mutex_unlock(&g_lock);
    ret_x(c, r);
}

// Result SetStationAcceptPolicy(AcceptPolicy): RejectAll closes the room (MKW: when the race starts)
static void ov_accept_policy(Cpu *c) {
    uint8_t policy = (uint8_t)arg_x(c, 0);
    if (g_state != LDN_STATE_AP && g_state != LDN_STATE_AP_CREATED) { ret_x(c, R_INVALID_STATE); return; }
    plat_mutex_lock(&g_lock);
    if (g_sess && net_session_state(g_sess) == NET_HOSTING) {
        net_session_info(g_sess)[NI_ACCEPT_POLICY] = policy;
        net_session_info_changed(g_sess);
        g_info_dirty_at = now_ms();
    }
    plat_mutex_unlock(&g_lock);
    static volatile int32_t said;
    if ((policy == LDN_BLACKLIST || policy == LDN_WHITELIST) && !plat_xchg32(&said, 1))
        rt_log("net: the game set accept policy %d (lists): every station is accepted", policy);
    ret_x(c, 0);
}

// AddAcceptFilterEntry(MacAddress): accepted while hosting, see ov_accept_policy
static void ov_accept_filter(Cpu *c) {
    ret_x(c, g_state == LDN_STATE_AP || g_state == LDN_STATE_AP_CREATED ? 0 : R_INVALID_STATE);
}

// Result Reject(Ipv4Address)
static void ov_reject(Cpu *c) {
    uint32_t ip = (uint32_t)arg_x(c, 0);
    plat_mutex_lock(&g_lock);
    uint32_t r = g_sess && net_session_state(g_sess) == NET_HOSTING ? (net_session_kick(g_sess, ip) ? 0u : R_NODE_NOT_FOUND)
                                                                     : R_INVALID_STATE;
    plat_mutex_unlock(&g_lock);
    ret_x(c, r);
}

static void ov_open_station(Cpu *c) {
    plat_mutex_lock(&g_lock);
    uint32_t r = R_INVALID_STATE;
    if (g_state == LDN_STATE_INITIALIZED || g_state == LDN_STATE_STATION) r = session_open(0) ? (set_state(LDN_STATE_STATION), 0u) : R_DISABLED;
    plat_mutex_unlock(&g_lock);
    ret_x(c, r);
}

static void ov_close_station(Cpu *c) {
    plat_mutex_lock(&g_lock);
    uint32_t r = R_INVALID_STATE;
    if (g_state == LDN_STATE_STATION || g_state == LDN_STATE_STATION_CONNECTED) session_close(), set_state(LDN_STATE_INITIALIZED), r = 0;
    plat_mutex_unlock(&g_lock);
    ret_x(c, r);
}

static int filter_ok(const uint8_t *ni, const uint8_t *f) {
    uint32_t flags = f ? ld32(f + SF_FLAGS) : 0;
    if ((flags & SF_LCID) && ld64(ni + NI_LCID) != ld64(f + SF_NETWORK_ID + NI_LCID)) return 0;
    if ((flags & SF_SCENE) && ld16(ni + NI_SCENE) != ld16(f + SF_NETWORK_ID + NI_SCENE)) return 0;
    if ((flags & SF_SESSION) && memcmp(ni + NI_SESSION_ID, f + SF_NETWORK_ID + NI_SESSION_ID, 16)) return 0;
    if ((flags & SF_BY_SSID) && memcmp(ni + NI_SSID_LEN, f + SF_SSID, 0x22)) return 0;
    if ((flags & SF_BY_MAC) && memcmp(ni + NI_MAC, f + SF_MAC, 6)) return 0;
    return 1;
}

static int scan_done(void) { return 0; }

// Discovery: a broadcast on the LAN (255.255.255.255 and the interface's /24), and the lobby's public sessions and the
// invited one. Under g_lock.
static void scan_now(void) {
    // the invite code is read at every search: the Multiplayer page may have set it since (a live setting)
    const char *inv = cfg_get("MKW_NET_JOIN");
    uint32_t was = g_invite_session;
    g_invite_session = 0;
    if (inv && *inv && !net_invite_read(inv, &g_invite_session, g_invite_password)) g_invite_session = 0;
    if (g_invite_session && g_invite_session != was) rt_log("net: looking for the invited room too");
    uint32_t iface = net_ip_parse(cfg_get("MKW_NET_IFACE") ? cfg_get("MKW_NET_IFACE") : "");
    if (!iface) iface = net_udp_local_ip(0);
    uint16_t port = (uint16_t)cfg_int("MKW_NET_PORT");
    NetAddr to[2] = {{0xffffffffu, port}, {iface | 0xff, port}};
    net_session_scan(g_sess, to, iface ? 2 : 1);
    memset(g_found_session, 0, sizeof g_found_session);
    if (!g_lobby.ip) return;
    uint8_t b[LR_SCAN];
    memcpy(b, g_cookie, NET_COOKIE);
    memcpy(b + NET_COOKIE, g_compat, NET_COMPAT);
    put32be(b + NET_COOKIE + NET_COMPAT, 0);
    lobby_send(L_SCAN, b, LR_SCAN);
    if (g_invite_session) {
        put32be(b + NET_COOKIE + NET_COMPAT, g_invite_session);
        lobby_send(L_SCAN, b, LR_SCAN);
    }
}

// Result Scan(NetworkInfo *out, int *count, int max, const ScanFilter &, int channel)
static void ov_scan(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    int32_t *count = arg_p(c, 1);
    int max = (int)arg_x(c, 2);
    const uint8_t *filter = arg_p(c, 3);
    if (!out || !count || max < 0) { ret_x(c, R_INVALID_ARGUMENT); return; }
    plat_mutex_lock(&g_lock);
    if (!g_sess) {   // an access point before CreateNetwork: nothing to scan with, as a console that finds no room
        plat_mutex_unlock(&g_lock);
        *count = 0;
        ret_x(c, g_state == LDN_STATE_AP ? 0 : R_INVALID_STATE);
        return;
    }
    if (g_state != LDN_STATE_STATION && g_state != LDN_STATE_AP && g_state != LDN_STATE_AP_CREATED) {
        plat_mutex_unlock(&g_lock);
        ret_x(c, R_INVALID_STATE);
        return;
    }
    scan_now();
    wait_for(scan_done, g_lobby.ip ? SCAN_ONLINE_MS : SCAN_LAN_MS);
    if (!g_sess) { plat_mutex_unlock(&g_lock); *count = 0; ret_x(c, R_INVALID_STATE); return; }
    const NetFound *f;
    int n = g_sess ? net_session_found(g_sess, &f) : 0, k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (filter_ok(f[i].info, filter)) memcpy(out + (size_t)k++ * LDN_NETWORK_INFO, f[i].info, LDN_NETWORK_INFO);
    plat_mutex_unlock(&g_lock);
    *count = k;
    static volatile int32_t said;
    if (!plat_xchg32(&said, 1) || k) rt_log("net: room search found %d of %d sessions", k, n);
    ret_x(c, 0);
}

static int peer_got(void) { return g_peer.got; }
static int joined_or_gone(void) {
    int st = g_sess ? net_session_state(g_sess) : NET_GONE;
    return st == NET_JOINED || st == NET_GONE;
}

// Joins the host found with this session id. Under g_lock.
static uint32_t connect_to(const uint8_t *session_id, const uint8_t *sec, const uint8_t *user, uint16_t lcv) {
    if (g_state != LDN_STATE_STATION) return R_INVALID_STATE;
    const NetFound *f;
    int n = net_session_found(g_sess, &f), at = -1;
    for (int i = 0; i < n && at < 0; i++) if (!memcmp(f[i].info + NI_SESSION_ID, session_id, 16)) at = i;
    if (at < 0) return R_CONNECT_NOT_FOUND;
    NetFound host = f[at];
    uint32_t lobby_session = g_found_session[at];
    int relay_only = cfg_bool("MKW_NET_RELAY_ONLY");
    net_session_password(g_sess, lobby_session && lobby_session == g_invite_session ? g_invite_password : "");
    if (lobby_session) {   // the lobby introduces us (or gives the relay)
        memset(&g_peer, 0, sizeof g_peer);
        uint8_t b[LR_CONNECT];
        memcpy(b, g_cookie, NET_COOKIE);
        memcpy(b + NET_COOKIE, g_compat, NET_COMPAT);
        put32be(b + NET_COOKIE + NET_COMPAT, lobby_session);
        b[NET_COOKIE + NET_COMPAT + 4] = (uint8_t)relay_only;
        lobby_send(L_CONNECT, b, LR_CONNECT);
        if (!wait_for(peer_got, PEER_WAIT_MS) || g_peer.session != lobby_session || !g_sess) return R_CONNECT_TIMEOUT;
        net_session_relay(g_sess, lobby_session);
        int direct = g_peer.direct && !relay_only;
        net_session_join(g_sess, direct ? g_peer.host : (NetAddr){0, 0}, !direct, session_id, user, lcv, sec);
    } else
        net_session_join(g_sess, host.addr, 0, session_id, user, lcv, sec);
    wait_for(joined_or_gone, JOIN_WAIT_MS);
    if (!g_sess) return R_CONNECT_FAILURE;   // finalized while waiting
    if (net_session_state(g_sess) == NET_JOINED) {
        set_state(LDN_STATE_STATION_CONNECTED);
        g_reason = LDN_DR_NONE;
        return 0;
    }
    int why = net_session_refused(g_sess);
    rt_log("net: could not join (%s)", why == NET_REFUSE_PASSWORD ? "wrong password" : why == NET_REFUSE_VERSION ? "another game or runtime build"
                                       : why == NET_REFUSE_FULL ? "the room is full" : why == NET_REFUSE_LOCKED ? "the room is closed"
                                       : why ? "refused" : "no answer");
    return why == NET_REFUSE_FULL ? R_TOO_MANY : why ? R_CONNECT_REJECTED : R_CONNECT_TIMEOUT;
}

// Result Connect(const NetworkInfo &, const SecurityConfig &, const UserConfig &, int version, ConnectOption)
static void ov_connect(Cpu *c) {
    const uint8_t *ni = arg_p(c, 0), *sec = arg_p(c, 1), *user = arg_p(c, 2);
    if (!ni || !sec || !user) { ret_x(c, R_INVALID_ARGUMENT); return; }
    plat_mutex_lock(&g_lock);
    uint32_t r = connect_to(ni + NI_SESSION_ID, sec, user, (uint16_t)arg_x(c, 3));
    if (r == R_CONNECT_NOT_FOUND && g_state == LDN_STATE_STATION && g_sess) {
        // MKW finalizes ldn between its room search and the join, so the station that joins is a new one that has
        // seen no host yet (17/11/2026: "Unable to find the room you tried to join"): search again, then join
        scan_now();
        wait_for(scan_done, g_lobby.ip ? SCAN_ONLINE_MS : SCAN_LAN_MS);
        r = connect_to(ni + NI_SESSION_ID, sec, user, (uint16_t)arg_x(c, 3));
    }
    if (r) rt_log("net: the join failed (result %#x)", r);
    plat_mutex_unlock(&g_lock);
    ret_x(c, r);
}

// Result ConnectPrivate(const NetworkConfig &, const SecurityConfig &, const SecurityParameter &, const UserConfig &,
//                       int version, ConnectOption): a host known by its session id; scanned again if not seen yet
static void ov_connect_private(Cpu *c) {
    const uint8_t *sec = arg_p(c, 1), *sp = arg_p(c, 2), *user = arg_p(c, 3);
    if (!sec || !sp || !user) { ret_x(c, R_INVALID_ARGUMENT); return; }
    plat_mutex_lock(&g_lock);
    uint32_t r = connect_to(sp + SP_SESSION_ID, sec, user, (uint16_t)arg_x(c, 4));
    if (r == R_CONNECT_NOT_FOUND && g_state == LDN_STATE_STATION && g_sess) {
        scan_now();
        wait_for(scan_done, g_lobby.ip ? SCAN_ONLINE_MS : SCAN_LAN_MS);
        r = connect_to(sp + SP_SESSION_ID, sec, user, (uint16_t)arg_x(c, 4));
    }
    plat_mutex_unlock(&g_lock);
    ret_x(c, r);
}

static void ov_disconnect(Cpu *c) {
    plat_mutex_lock(&g_lock);
    uint32_t r = R_INVALID_STATE;
    if (g_state == LDN_STATE_STATION_CONNECTED) {
        session_close();
        session_open(0);
        g_reason = LDN_DR_USER;
        set_state(LDN_STATE_STATION);
        r = 0;
    }
    plat_mutex_unlock(&g_lock);
    ret_x(c, r);
}

static int in_session(void) {
    return g_sess && (net_session_state(g_sess) == NET_HOSTING || net_session_state(g_sess) == NET_JOINED);
}

// Result GetNetworkInfo(NetworkInfo *)
static void ov_network_info(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (!out) { ret_x(c, R_INVALID_ARGUMENT); return; }
    plat_mutex_lock(&g_lock);
    int ok = in_session();
    if (ok) memcpy(out, net_session_info(g_sess), LDN_NETWORK_INFO);
    plat_mutex_unlock(&g_lock);
    ret_x(c, ok ? 0 : R_INVALID_STATE);
}

// Result GetIpv4Address(Ipv4Address *, SubnetMask *)
static void ov_ipv4(Cpu *c) {
    uint32_t *ip = arg_p(c, 0), *mask = arg_p(c, 1);
    plat_mutex_lock(&g_lock);
    int ok = in_session();
    if (ok && ip) *ip = net_session_vip(g_sess);
    if (ok && mask) *mask = net_session_mask();
    plat_mutex_unlock(&g_lock);
    ret_x(c, ok ? 0 : R_INVALID_STATE);
}

// Ipv4Address MakeBroadcastAddress(Ipv4Address, SubnetMask)
static void ov_broadcast(Cpu *c) { ret_x(c, ((uint32_t)arg_x(c, 0) | ~(uint32_t)arg_x(c, 1)) & 0xffffffffu); }

// Result GetSecurityParameter(SecurityParameter *)
static void ov_security_parameter(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    plat_mutex_lock(&g_lock);
    int ok = in_session() && out;
    if (ok) {
        const uint8_t *ni = net_session_info(g_sess);
        memcpy(out + SP_DATA, ni + NI_SEC_PARAM, 16);
        memcpy(out + SP_SESSION_ID, ni + NI_SESSION_ID, 16);
    }
    plat_mutex_unlock(&g_lock);
    ret_x(c, ok ? 0 : R_INVALID_STATE);
}

// void AttachStateChangeEvent(SystemEventType *): signalled on every state or node change (auto clear, as the SDK
// attaches the service's event)
static void ov_attach_event(Cpu *c) {
    uint64_t ev = arg_x(c, 0);
    if (!ev) return;
    kern_bind_system_event(c, ev, 1);
    plat_mutex_lock(&g_lock);
    g_event = ev;
    plat_mutex_unlock(&g_lock);
}

// ---- net_game.h
int net_game_send(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const void *p, int n) {
    plat_mutex_lock(&g_lock);
    int ok = in_session() && net_session_send(g_sess, src_port, dst_ip, dst_port, p, n);
    plat_mutex_unlock(&g_lock);
    return ok;
}
uint32_t net_game_vip(void) {
    plat_mutex_lock(&g_lock);
    uint32_t v = in_session() ? net_session_vip(g_sess) : 0;
    plat_mutex_unlock(&g_lock);
    return v;
}
uint32_t net_game_broadcast(void) {
    uint32_t v = net_game_vip();
    return v ? v | 0xff : 0;
}

// ---- net_ui.h (the Pit Stop page; any thread)
void net_ui_status(NetUiStatus *out) {
    plat_mutex_lock(&g_lock);
    g_ui.mode = mode();
    g_ui.ldn_state = g_state;
    g_ui.hosting = g_sess && net_session_state(g_sess) == NET_HOSTING;
    g_ui.lobby = g_lobby.ip != 0;
    g_ui.nplayers = 0;
    if (in_session()) {
        const uint8_t *ni = net_session_info(g_sess);
        for (int i = 0; i < LDN_NODES; i++) {
            const uint8_t *nd = ni_node_c(ni, i);
            if (!nd[NODE_CONNECTED]) continue;
            NetUiPlayer *p = &g_ui.players[g_ui.nplayers++];
            p->node = i, p->ip = ld32(nd + NODE_IP), p->me = p->ip == net_session_vip(g_sess);
            memcpy(p->name, nd + NODE_NAME, LDN_USERNAME);
            p->name[LDN_USERNAME - 1] = 0;
        }
    }
    g_ui.nlist = g_list_n;
    memcpy(g_ui.list, g_list, sizeof g_list);
    *out = g_ui;
    plat_mutex_unlock(&g_lock);
}

void net_ui_kick(uint32_t ip) {
    plat_mutex_lock(&g_lock);
    if (g_sess && net_session_state(g_sess) == NET_HOSTING) net_session_kick(g_sess, ip);
    plat_mutex_unlock(&g_lock);
}

void net_ui_lock(int on) {
    plat_mutex_lock(&g_lock);
    g_ui.locked = on;
    if (g_sess) net_session_lock(g_sess, on), g_info_dirty_at = now_ms();
    plat_mutex_unlock(&g_lock);
}

void net_ui_refresh(const NetLobbyFilter *f) {
    plat_mutex_lock(&g_lock);
    if (g_sess && g_lobby.ip) {
        uint8_t b[LR_LIST];
        memcpy(b, g_cookie, NET_COOKIE);
        memcpy(b + NET_COOKIE, g_compat, NET_COMPAT);
        b[NET_COOKIE + NET_COMPAT] = f->region, b[NET_COOKIE + NET_COMPAT + 1] = f->mode;
        b[NET_COOKIE + NET_COMPAT + 2] = f->open_only, b[NET_COOKIE + NET_COMPAT + 3] = f->page;
        lobby_send(L_LIST, b, LR_LIST);
    }
    plat_mutex_unlock(&g_lock);
}

HOST_OVERRIDE("_ZN2nn3ldn10InitializeEv", ov_ldn_initialize);
HOST_OVERRIDE("_ZN2nn3ldn8FinalizeEv", ov_ldn_finalize);
HOST_OVERRIDE("_ZN2nn3ldn8GetStateEv", ov_ldn_state);
HOST_OVERRIDE("_ZN2nn3ldn19GetDisconnectReasonEv", ov_ldn_reason);
HOST_OVERRIDE("_ZN2nn3ldn10DisconnectEv", ov_disconnect);
HOST_OVERRIDE("_ZN2nn3ldn11OpenStationEv", ov_open_station);
HOST_OVERRIDE("_ZN2nn3ldn12CloseStationEv", ov_close_station);
HOST_OVERRIDE("_ZN2nn3ldn15OpenAccessPointEv", ov_open_ap);
HOST_OVERRIDE("_ZN2nn3ldn16CloseAccessPointEv", ov_close_ap);
HOST_OVERRIDE("_ZN2nn3ldn14DestroyNetworkEv", ov_destroy);
HOST_OVERRIDE("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ov_scan);
HOST_OVERRIDE("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ov_scan);
HOST_OVERRIDE("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", ov_create);
HOST_OVERRIDE("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE",
              ov_create_private);
HOST_OVERRIDE("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE",
              ov_connect);
HOST_OVERRIDE("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE",
              ov_connect_private);
HOST_OVERRIDE("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", ov_ipv4);
HOST_OVERRIDE("_ZN2nn3ldn20MakeBroadcastAddressENS0_11Ipv4AddressENS0_10SubnetMaskE", ov_broadcast);
HOST_OVERRIDE("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", ov_network_info);
HOST_OVERRIDE("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", ov_security_parameter);
HOST_OVERRIDE("_ZN2nn3ldn16SetAdvertiseDataEPKvm", ov_advertise);
HOST_OVERRIDE("_ZN2nn3ldn20AddAcceptFilterEntryENS0_10MacAddressE", ov_accept_filter);
HOST_OVERRIDE("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", ov_accept_policy);
HOST_OVERRIDE("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", ov_reject);
HOST_OVERRIDE("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ov_attach_event);
