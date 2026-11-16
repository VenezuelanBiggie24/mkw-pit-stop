// The lobby and relay's messages (server/lobby, docs/multiplayer.md): public sessions listed and found, a station
// introduced to a host, and the relay that carries a session for routers that block direct connections. The lobby
// never reads game traffic: what it relays is the session's sealed datagrams.
//
// Every request that may get a longer answer carries a cookie the lobby gave the same address (L_COOKIE, a keyed hash
// of the address and the minute): an answer is never sent to an address that did not ask from there, and an answer
// to a request without a valid cookie is never longer than the request (no reflection).
#pragma once
#include <stdint.h>

#include "net_proto.h"

enum {
    L_REGISTER = 0x40,      // host: compat, u32 session (0 new), token (16), NetLobbyMeta, NetworkInfo (0x480)
    L_REGISTERED = 0x41,    // u32 session, u8 ok (0 refused: name, limits), u8 reason
    L_UNREGISTER = 0x42,    // host: u32 session, token
    L_LIST = 0x43,          // client: cookie, compat, NetLobbyFilter
    L_LIST_REPLY = 0x44,    // u8 count, u8 total, count x NetLobbyEntry
    L_SCAN = 0x45,          // client: cookie, compat, u32 session (0: the public ones; else that one, by invite)
    L_SCAN_REPLY = 0x46,    // u32 session, u8 flags, NetworkInfo; one datagram per session
    L_CONNECT = 0x47,       // station: cookie, compat, u32 session, u8 relay only
    L_INTRODUCE = 0x48,     // to the host: u32 session, station address (u32 ip, u16 port)
    L_PEER = 0x49,          // to the station: u32 session, u8 direct, host address (u32 ip, u16 port; 0 relay only)
    L_COOKIE = 0x4f,        // cookie (16)
    M_RELAY = 0x50,         // u32 session, u16 flow (0 from and to a station), then one whole session datagram
};

enum { NET_COOKIE = 16, NET_TOKEN = 16, NET_NAME_MAX = 32, NET_LIST_PAGE = 24, NET_RELAY_HEAD = 6 };

// What the session browser shows; the host's choice, the lobby checks it (lengths, name filter, players <= slots).
typedef struct {
    uint8_t flags;          // NET_ADV_PASSWORD, NET_ADV_LOCKED, NET_ADV_FULL; bit 7 public
    uint8_t players, slots; // nodes in the session and its NodeCountMax
    uint8_t region;         // 0 any, 1 Europe, 2 North America, 3 South America, 4 Asia, 5 Oceania, 6 Africa
    uint8_t mode;           // 0 unknown, 1 race, 2 battle, 3 knockout tour (what the host's Pit Stop page says)
    uint8_t reserved;
    uint16_t course;        // 0 unknown
    char name[NET_NAME_MAX];   // UTF-8, NUL padded
} NetLobbyMeta;
enum { NET_META = 40, NET_ENTRY = 4 + NET_META };   // NetLobbyEntry: u32 session, then the meta
enum { NET_LOBBY_PUBLIC = 0x80 };

typedef struct { uint8_t region, mode, open_only, page; } NetLobbyFilter;

enum {
    LR_REGISTER = NET_COMPAT + 4 + NET_TOKEN + NET_META + 0x480,
    LR_REGISTERED = 6,
    LR_UNREGISTER = 4 + NET_TOKEN,
    LR_LIST = NET_COOKIE + NET_COMPAT + 4,
    LR_SCAN = NET_COOKIE + NET_COMPAT + 4,
    LR_SCAN_REPLY = 4 + 1 + 0x480,
    LR_CONNECT = NET_COOKIE + NET_COMPAT + 4 + 1,
    LR_INTRODUCE = 4 + 6,
    LR_PEER = 4 + 1 + 6,
    LR_COOKIE = NET_COOKIE,
};

// The lobby message's body length limits (as net_msg_limits); 0 for another type.
int net_lobby_limits(int type, int *min, int *max);
// Lobby datagrams use the session header with these types: net_lobby_parse is net_parse for them.
int net_lobby_parse(const uint8_t *p, int n, int *type, const uint8_t **body, int *len);
void net_meta_put(uint8_t out[NET_META], const NetLobbyMeta *m);
void net_meta_get(const uint8_t in[NET_META], NetLobbyMeta *m);
// A session name the lobby accepts: 1..32 bytes of printable UTF-8, no control characters, not only spaces, none of
// the filtered words (case-insensitive). The text is cleaned in place first (trailing spaces).
int net_name_ok(char name[NET_NAME_MAX + 1]);
