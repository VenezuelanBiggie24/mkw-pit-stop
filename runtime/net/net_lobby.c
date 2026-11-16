// The lobby's message limits, session metadata and name filter (net_lobby.h), shared by the runtime and the lobby
// server.
#include "net_lobby.h"

#include <ctype.h>
#include <string.h>

int net_lobby_limits(int type, int *min, int *max) {
    int lo = 0, hi = 0;
    switch (type) {
    case L_REGISTER: lo = hi = LR_REGISTER; break;
    case L_REGISTERED: lo = hi = LR_REGISTERED; break;
    case L_UNREGISTER: lo = hi = LR_UNREGISTER; break;
    case L_LIST: lo = hi = LR_LIST; break;
    case L_LIST_REPLY: lo = 2, hi = 2 + NET_LIST_PAGE * NET_ENTRY; break;
    case L_SCAN: lo = hi = LR_SCAN; break;
    case L_SCAN_REPLY: lo = hi = LR_SCAN_REPLY; break;
    case L_CONNECT: lo = hi = LR_CONNECT; break;
    case L_INTRODUCE: lo = hi = LR_INTRODUCE; break;
    case L_PEER: lo = hi = LR_PEER; break;
    case L_COOKIE: lo = hi = LR_COOKIE; break;
    case M_RELAY: lo = NET_RELAY_HEAD + NET_HEADER, hi = NET_RELAY_HEAD + NET_DGRAM_MAX; break;
    default: return 0;
    }
    *min = lo, *max = hi;
    return 1;
}

int net_lobby_parse(const uint8_t *p, int n, int *type, const uint8_t **body, int *len) {
    int min, max;
    if (!net_parse(p, n, type, body, len) || !net_lobby_limits(*type, &min, &max)) return 0;
    return *len >= min && *len <= max;
}

void net_meta_put(uint8_t out[NET_META], const NetLobbyMeta *m) {
    memset(out, 0, NET_META);
    out[0] = m->flags, out[1] = m->players, out[2] = m->slots, out[3] = m->region, out[4] = m->mode;
    out[6] = (uint8_t)m->course, out[7] = (uint8_t)(m->course >> 8);
    memcpy(out + 8, m->name, strnlen(m->name, NET_NAME_MAX));
}

void net_meta_get(const uint8_t in[NET_META], NetLobbyMeta *m) {
    memset(m, 0, sizeof *m);
    m->flags = in[0], m->players = in[1], m->slots = in[2], m->region = in[3], m->mode = in[4];
    m->course = (uint16_t)(in[6] | in[7] << 8);
    memcpy(m->name, in + 8, NET_NAME_MAX);
}

// Words a public session name may not contain (ASCII, compared without case or the characters between letters).
// Short on purpose: the lobby's operator adds more (server/lobby --words FILE, checked after this).
// Only words with no innocent superstring (no "rape": "grapes").
static const char *const blocked[] = {"nigger", "faggot", "retard", "nazi", "hitler"};

static int has_word(const char *name, const char *word) {
    char flat[NET_NAME_MAX + 1];
    int n = 0;
    for (const char *p = name; *p && n < NET_NAME_MAX; p++)
        if (isalnum((unsigned char)*p)) {
            char c = (char)tolower((unsigned char)*p);
            flat[n++] = c == '0' ? 'o' : c == '1' ? 'i' : c == '3' ? 'e' : c == '4' ? 'a' : c == '5' ? 's' : c;
        }
    flat[n] = 0;
    return strstr(flat, word) != NULL;
}

int net_name_ok(char name[NET_NAME_MAX + 1]) {
    name[NET_NAME_MAX] = 0;
    size_t n = strlen(name);
    while (n && name[n - 1] == ' ') name[--n] = 0;
    if (!n) return 0;
    // UTF-8, no control characters (C0, DEL, C1), no overlong or truncated sequences
    const unsigned char *p = (const unsigned char *)name;
    for (size_t i = 0; i < n;) {
        unsigned c = p[i];
        int k = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (k < 0 || i + (size_t)k >= n) return 0;
        if (!k && (c < 0x20 || c == 0x7f)) return 0;
        if (k == 1 && c < 0xc2) return 0;
        if (k == 1 && c == 0xc2 && p[i + 1] < 0xa0) return 0;   // C1 controls
        for (int j = 1; j <= k; j++)
            if ((p[i + (size_t)j] & 0xc0) != 0x80) return 0;
        i += (size_t)k + 1;
    }
    if (strspn(name, " ") == n) return 0;
    for (size_t i = 0; i < sizeof blocked / sizeof *blocked; i++)
        if (has_word(name, blocked[i])) return 0;
    return 1;
}
