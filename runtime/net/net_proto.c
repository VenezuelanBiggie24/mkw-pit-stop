// Wire format, sealing, handshake and invite codes of local wireless play over UDP (net_proto.h).
#include "net_proto.h"

#include <ctype.h>
#include <string.h>

#include "monocypher.h"

static const uint8_t magic[4] = {'M', 'K', 'W', 'N'};

int net_msg_limits(int type, int *min, int *max, int *clear) {
    static const struct { uint16_t min, max; uint8_t clear; } t[M_SESSION_LAST + 1] = {
        [M_DISCOVER] = {L_DISCOVER, L_DISCOVER, 1}, [M_ADVERT] = {L_ADVERT, L_ADVERT, 1},
        [M_HELLO] = {L_HELLO, L_HELLO, 1},          [M_CHALLENGE] = {L_CHALLENGE, L_CHALLENGE, 1},
        [M_PROOF] = {L_PROOF, L_PROOF, 1},          [M_REFUSE] = {L_REFUSE, L_REFUSE, 1},
        [M_JOIN] = {L_JOIN, L_JOIN, 0},             [M_WELCOME] = {L_WELCOME, L_WELCOME, 0},
        [M_SYNC] = {L_SYNC, L_SYNC, 0},             [M_DATA] = {NET_DATA_HEAD, NET_DATA_HEAD + NET_PAYLOAD_MAX, 0},
        [M_PING] = {L_PING, L_PING, 0},             [M_PONG] = {L_PING, L_PING, 0},
        [M_LEAVE] = {L_LEAVE, L_LEAVE, 0},
    };
    if (type <= 0 || type > M_SESSION_LAST || !t[type].max) return 0;
    *min = t[type].min, *max = t[type].max, *clear = t[type].clear;
    return 1;
}

int net_parse(const uint8_t *p, int n, int *type, const uint8_t **body, int *len) {
    if (n < NET_HEADER || n > NET_WIRE_MAX || memcmp(p, magic, 4) || p[4] != NET_VERSION) return 0;
    int l = p[6] | p[7] << 8;
    if (l != n - NET_HEADER || !p[5]) return 0;   // trailing bytes or a short datagram: refused whole
    if (n > NET_DGRAM_MAX && p[5] != 0x50) return 0;   // only M_RELAY may exceed a session datagram
    *type = p[5], *body = p + NET_HEADER, *len = l;
    return 1;
}

static void header(uint8_t *out, int type, int len) {
    memcpy(out, magic, 4);
    out[4] = NET_VERSION, out[5] = (uint8_t)type, out[6] = (uint8_t)len, out[7] = (uint8_t)(len >> 8);
}

int net_put(uint8_t *out, int type, const void *body, int len) {
    if (len < 0 || NET_HEADER + len > (type == 0x50 ? NET_WIRE_MAX : NET_DGRAM_MAX)) return 0;   // M_RELAY: wrapped
    header(out, type, len);
    if (len) memcpy(out + NET_HEADER, body, (size_t)len);
    return NET_HEADER + len;
}

// Nonce: direction, the type (a message moved to another type does not open), zeros, the counter.
static void nonce_of(uint8_t n[24], uint8_t dir, int type, uint64_t counter) {
    memset(n, 0, 24);
    n[0] = dir, n[1] = (uint8_t)type;
    for (int i = 0; i < 8; i++) n[16 + i] = (uint8_t)(counter >> 8 * i);
}

int net_seal(NetKeys *k, uint8_t *out, int type, const void *body, int len) {
    int total = len + NET_SEAL;
    if (len < 0 || NET_HEADER + total > NET_DGRAM_MAX) return 0;
    header(out, type, total);
    uint64_t c = ++k->tx_counter;
    uint8_t *p = out + NET_HEADER, n[24];
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(c >> 8 * i);
    nonce_of(n, k->tx_dir, type, c);
    crypto_aead_lock(p + 8, p + 8 + len, k->tx, n, out, NET_HEADER + 8, (const uint8_t *)body, (size_t)len);
    return NET_HEADER + total;
}

int net_open(NetKeys *k, const uint8_t *dgram, int type, const uint8_t *body, int len, uint8_t *plain) {
    if (len < NET_SEAL) return -1;
    uint64_t c = 0;
    for (int i = 0; i < 8; i++) c |= (uint64_t)body[i] << 8 * i;
    if (!c || (c <= k->rx_top && (k->rx_top - c >= 64 || (k->rx_window >> (k->rx_top - c) & 1)))) return -1;
    uint8_t n[24];
    nonce_of(n, k->rx_dir, type, c);
    int plen = len - NET_SEAL;
    if (crypto_aead_unlock(plain, body + 8 + plen, k->rx, n, dgram, NET_HEADER + 8, body + 8, (size_t)plen)) return -1;
    if (c > k->rx_top) {
        uint64_t shift = c - k->rx_top;
        k->rx_window = shift >= 64 ? 1 : k->rx_window << shift | 1;
        k->rx_top = c;
    } else
        k->rx_window |= 1ull << (k->rx_top - c);
    return plen;
}

void net_keypair(const uint8_t rnd[32], uint8_t sk[32], uint8_t pk[32]) {
    memcpy(sk, rnd, 32);
    crypto_x25519_public_key(pk, sk);
}

int net_shared(const uint8_t sk[32], const uint8_t peer_pk[32], uint8_t shared[32]) {
    crypto_x25519(shared, sk, peer_pk);
    static const uint8_t zero[32];
    return crypto_verify32(shared, zero) != 0;   // a low-order public key gives all zeros: refused
}

static void absorb_str(crypto_blake2b_ctx *c, const char *s, size_t max) {
    size_t n = s ? strnlen(s, max) : 0;
    uint8_t len[2] = {(uint8_t)n, (uint8_t)(n >> 8)};
    crypto_blake2b_update(c, len, 2);
    if (n) crypto_blake2b_update(c, (const uint8_t *)s, n);
}

void net_proof(const char *password, const uint8_t nonce[32], const uint8_t station_pk[32], const uint8_t host_pk[32],
               const uint8_t shared[32], uint8_t out[32]) {
    uint8_t key[32];
    crypto_blake2b_ctx c;
    crypto_blake2b_init(&c, 32);
    crypto_blake2b_update(&c, (const uint8_t *)"mkw-ldn proof key v1", 20);
    absorb_str(&c, password, NET_PASSWORD_MAX);
    crypto_blake2b_final(&c, key);
    crypto_blake2b_keyed_init(&c, 32, key, 32);
    crypto_blake2b_update(&c, nonce, 32);
    crypto_blake2b_update(&c, station_pk, 32);
    crypto_blake2b_update(&c, host_pk, 32);
    crypto_blake2b_update(&c, shared, 32);
    crypto_blake2b_final(&c, out);
    crypto_wipe(key, sizeof key);
}

int net_proof_equal(const uint8_t a[32], const uint8_t b[32]) { return crypto_verify32(a, b) == 0; }

void net_keys(NetKeys *k, int host, const uint8_t shared[32], const char *password, const uint8_t *hello, int hello_len,
              const uint8_t *challenge, int challenge_len) {
    uint8_t km[64];
    crypto_blake2b_ctx c;
    crypto_blake2b_keyed_init(&c, 64, shared, 32);
    crypto_blake2b_update(&c, (const uint8_t *)"mkw-ldn traffic v1", 18);
    absorb_str(&c, password, NET_PASSWORD_MAX);
    uint8_t l[2] = {(uint8_t)hello_len, (uint8_t)(hello_len >> 8)};
    crypto_blake2b_update(&c, l, 2);
    crypto_blake2b_update(&c, hello, (size_t)hello_len);
    l[0] = (uint8_t)challenge_len, l[1] = (uint8_t)(challenge_len >> 8);
    crypto_blake2b_update(&c, l, 2);
    crypto_blake2b_update(&c, challenge, (size_t)challenge_len);
    crypto_blake2b_final(&c, km);
    memset(k, 0, sizeof *k);
    memcpy(host ? k->tx : k->rx, km, 32);        // host to station
    memcpy(host ? k->rx : k->tx, km + 32, 32);   // station to host
    k->tx_dir = host ? 0 : 1, k->rx_dir = host ? 1 : 0;
    crypto_wipe(km, sizeof km);
}

void net_wipe(void *p, size_t n) { crypto_wipe(p, n); }

void net_compat_tag(const char *game_main_sha256, int game_version, const char *runtime_version, uint8_t out[NET_COMPAT]) {
    crypto_blake2b_ctx c;
    crypto_blake2b_init(&c, NET_COMPAT);
    crypto_blake2b_update(&c, (const uint8_t *)"mkw-ldn compat v1", 17);
    uint8_t v[5] = {NET_VERSION, (uint8_t)game_version, (uint8_t)(game_version >> 8), (uint8_t)(game_version >> 16),
                    (uint8_t)(game_version >> 24)};
    crypto_blake2b_update(&c, v, sizeof v);
    absorb_str(&c, game_main_sha256, 128);
    absorb_str(&c, runtime_version, 128);
    crypto_blake2b_final(&c, out);
}

// ---- invite codes
static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

static int letter_value(int ch) {
    const char *p = ch ? strchr(alphabet, toupper(ch)) : NULL;
    return p ? (int)(p - alphabet) : -1;
}

void net_password_new(const uint8_t rnd[NET_PASSWORD_LETTERS], char out[NET_PASSWORD_LETTERS + 1]) {
    for (int i = 0; i < NET_PASSWORD_LETTERS; i++) out[i] = alphabet[rnd[i] & 31];
    out[NET_PASSWORD_LETTERS] = 0;
}

enum { INVITE_KIND = 0x31, INVITE_BYTES = 15, INVITE_LETTERS = 24 };

static void invite_sum(const uint8_t *b, int n, uint8_t out[3]) {
    uint8_t h[32];
    crypto_blake2b_ctx c;
    crypto_blake2b_init(&c, 32);
    crypto_blake2b_update(&c, (const uint8_t *)"mkw-ldn invite v1", 17);
    crypto_blake2b_update(&c, b, (size_t)n);
    crypto_blake2b_final(&c, h);
    memcpy(out, h, 3);
}

// bits [at, at + n) of b, most significant first
static uint32_t get_bits(const uint8_t *b, int at, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = v << 1 | (b[(at + i) / 8] >> (7 - (at + i) % 8) & 1);
    return v;
}
static void put_bits(uint8_t *b, int at, int n, uint32_t v) {
    for (int i = 0; i < n; i++) {
        int bit = at + i, on = v >> (n - 1 - i) & 1;
        b[bit / 8] = (uint8_t)(on ? b[bit / 8] | 0x80 >> bit % 8 : b[bit / 8] & ~(0x80 >> bit % 8));
    }
}

int net_invite_make(uint32_t session, const char *password, char out[NET_INVITE_LEN]) {
    if (!password || strlen(password) != NET_PASSWORD_LETTERS) return 0;
    uint8_t b[INVITE_BYTES] = {INVITE_KIND, (uint8_t)(session >> 24), (uint8_t)(session >> 16), (uint8_t)(session >> 8),
                               (uint8_t)session};
    for (int i = 0; i < NET_PASSWORD_LETTERS; i++) {
        int v = letter_value((unsigned char)password[i]);
        if (v < 0) return 0;
        put_bits(b + 5, i * 5, 5, (uint32_t)v);   // 50 bits in 7 bytes, 6 zero bits after
    }
    invite_sum(b, 12, b + 12);
    int o = 0;
    for (int i = 0; i < INVITE_LETTERS; i++) {
        if (i && i % 5 == 0) out[o++] = '-';
        out[o++] = alphabet[get_bits(b, i * 5, 5)];
    }
    out[o] = 0;
    return 1;
}

int net_invite_read(const char *text, uint32_t *session, char password[NET_PASSWORD_LETTERS + 1]) {
    uint8_t b[INVITE_BYTES] = {0};
    int n = 0;
    for (const char *p = text; p && *p; p++) {
        if (*p == ' ' || *p == '-' || *p == '\t') continue;
        int v = letter_value((unsigned char)*p);
        if (v < 0 || n == INVITE_LETTERS) return 0;
        put_bits(b, n++ * 5, 5, (uint32_t)v);
    }
    uint8_t sum[3];
    if (n != INVITE_LETTERS || b[0] != INVITE_KIND || get_bits(b + 5, 50, 6)) return 0;
    invite_sum(b, 12, sum);
    if (memcmp(sum, b + 12, 3)) return 0;
    *session = (uint32_t)b[1] << 24 | b[2] << 16 | b[3] << 8 | b[4];
    for (int i = 0; i < NET_PASSWORD_LETTERS; i++) password[i] = alphabet[get_bits(b + 5, i * 5, 5)];
    password[NET_PASSWORD_LETTERS] = 0;
    return 1;
}
