// The wire format of local wireless play carried over UDP (docs/ps5/multiplayer.md): one datagram is one message, an 8-byte
// header then a body of a fixed type with fixed limits. Pure: no sockets, no runtime; the session engine
// (net_session.c), the lobby client and the lobby server (server/lobby) share it, and tests/runtime/test_net.py fuzzes it.
//
// After the join handshake every message is sealed (XChaCha20-Poly1305, Monocypher): keys from an X25519 exchange and
// the session's join password, the password never sent. The design follows Squadala's (daurosk/oot-remake-multiplayer
// @ 39b98de, GPL-3.0: ideas only, no code): a proof keyed by the password over a fresh challenge, per-direction keys
// from the shared secret, the password and the exact handshake bytes, a counter in every nonce, a replay window.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    NET_VERSION = 1,          // header byte 4; another version is refused before its body is read
    NET_HEADER = 8,           // "MKWN", u8 version, u8 type, u16 body length (little-endian)
    NET_DGRAM_MAX = 1472,     // 1500-byte Ethernet MTU less IPv4 and UDP headers: never fragmented on a LAN
    NET_WIRE_MAX = NET_DGRAM_MAX + 8 + 6,   // a session datagram wrapped for the relay (net_lobby.h M_RELAY)
    NET_SEAL = 24,            // u64 counter + 16-byte tag
    NET_DATA_HEAD = 12,       // DATA: u32 src ip, u16 src port, u32 dst ip, u16 dst port
    NET_PAYLOAD_MAX = NET_DGRAM_MAX - NET_HEADER - NET_SEAL - NET_DATA_HEAD,   // 1428: one game datagram
    NET_COMPAT = 16,          // compatibility tag: protocol, game build, runtime build (net_compat_tag)
    NET_KEY = 32,
    NET_PASSWORD_MAX = 64,
    NET_PORT_DEFAULT = 11460, // the host's UDP port (MKW_NET_PORT); not ldn_mitm's 11452, another protocol
};

// Message types. Clear ones are read before a key exists; every other one must open with the peer's key.
enum {
    M_DISCOVER = 1,    // clear, LAN broadcast: compat
    M_ADVERT = 2,      // clear: compat, u8 flags (NET_ADV_*), NetworkInfo (0x480)
    M_HELLO = 3,       // clear: compat, X25519 public key, session id (16)
    M_CHALLENGE = 4,   // clear: nonce (32), X25519 public key
    M_PROOF = 5,       // clear: proof (32)
    M_REFUSE = 6,      // clear: u8 reason (NET_REFUSE_*)
    M_JOIN = 7,        // sealed: UserConfig (0x30), u16 local communication version, SecurityConfig (0x44)
    M_WELCOME = 8,     // sealed: u8 node id, NetworkInfo
    M_SYNC = 9,        // sealed: NetworkInfo
    M_DATA = 10,       // sealed: NET_DATA_HEAD + one game datagram
    M_PING = 11,       // sealed: u64 sender time (ms)
    M_PONG = 12,       // sealed: the ping's u64
    M_LEAVE = 13,      // sealed: u8 ldn DisconnectReason
    M_SESSION_LAST = 13,
    // 0x40..: the lobby's (net_lobby.h)
};

enum { NET_ADV_PASSWORD = 1, NET_ADV_LOCKED = 2, NET_ADV_FULL = 4 };
enum {
    NET_REFUSE_VERSION = 1,    // another protocol, game or runtime build
    NET_REFUSE_PASSWORD = 2,   // wrong join password (proof)
    NET_REFUSE_FULL = 3,
    NET_REFUSE_LOCKED = 4,     // the host closed the session (the game's RejectAll policy, or the host's lock)
    NET_REFUSE_SESSION = 5,    // no such session here
    NET_REFUSE_BUSY = 6,       // too many handshakes or attempts from this address: try later
    NET_REFUSE_KICKED = 7,
};

// Fixed body lengths (exact, except DATA: NET_DATA_HEAD..NET_DATA_HEAD + NET_PAYLOAD_MAX).
enum {
    L_DISCOVER = NET_COMPAT,
    L_ADVERT = NET_COMPAT + 1 + 0x480,
    L_HELLO = NET_COMPAT + 32 + 16,
    L_CHALLENGE = 32 + 32,
    L_PROOF = 32,
    L_REFUSE = 1,
    L_JOIN = 0x30 + 2 + 0x44,
    L_WELCOME = 1 + 0x480,
    L_SYNC = 0x480,
    L_PING = 8,
    L_LEAVE = 1,
};

// The body length a session message type must have (min..max), 0 for an unknown type. Clear: whether it is read
// without a key.
int net_msg_limits(int type, int *min, int *max, int *clear);
// A datagram's header: the type and the body, or 0 for anything malformed (magic, version, length, limits).
int net_parse(const uint8_t *p, int n, int *type, const uint8_t **body, int *len);
// Writes a header and a clear body; the datagram's length.
int net_put(uint8_t *out, int type, const void *body, int len);

// Per-direction traffic state of one peer.
typedef struct {
    uint8_t tx[NET_KEY], rx[NET_KEY];
    uint64_t tx_counter;          // last sent
    uint64_t rx_top, rx_window;   // newest counter opened; bit k: rx_top - k seen
    uint8_t tx_dir, rx_dir;       // nonce byte 0: 0 host to station, 1 station to host
} NetKeys;

// Seals body into a datagram (header authenticated, never encrypted); its length, 0 if too long.
int net_seal(NetKeys *k, uint8_t *out, int type, const void *body, int len);
// Opens a sealed datagram whose header net_parse accepted: plain gets the body, the length or -1 (forged, replayed,
// or more than 64 behind the newest).
int net_open(NetKeys *k, const uint8_t *dgram, int type, const uint8_t *body, int len, uint8_t *plain);

// Handshake. A fresh X25519 key pair from rnd (32 random bytes).
void net_keypair(const uint8_t rnd[32], uint8_t sk[32], uint8_t pk[32]);
// The station's proof of the password: keyed by the password, over the challenge nonce, both public keys and the
// shared secret (so a proof from one connection proves nothing on another, and a recorded exchange cannot be guessed
// offline without a private key).
void net_proof(const char *password, const uint8_t nonce[32], const uint8_t station_pk[32], const uint8_t host_pk[32],
               const uint8_t shared[32], uint8_t out[32]);
int net_proof_equal(const uint8_t a[32], const uint8_t b[32]);   // constant time
// Shared secret, then both traffic keys: BLAKE2b keyed by the secret over a label, the password and the exact HELLO
// and CHALLENGE datagrams. host 1 for the host's side.
int net_shared(const uint8_t sk[32], const uint8_t peer_pk[32], uint8_t shared[32]);   // 0 for a degenerate key
void net_keys(NetKeys *k, int host, const uint8_t shared[32], const char *password, const uint8_t *hello, int hello_len,
              const uint8_t *challenge, int challenge_len);
void net_wipe(void *p, size_t n);

// Compatibility tag: two ends play together only when theirs are equal (the protocol, the game's main executable,
// the runtime's version).
void net_compat_tag(const char *game_main_sha256, int game_version, const char *runtime_version, uint8_t out[NET_COMPAT]);

// Invite codes: letters that cannot be mixed up ("ABCDEFGHJKLMNPQRSTUVWXYZ23456789": no I, O, 0, 1), five per group.
// An invite names a lobby session by number and carries its password; never an address. 0x31, u32 session, the
// password's 10 letters packed in 50 bits, a 3-byte checksum: 15 bytes, 24 letters.
enum { NET_INVITE_LEN = 30, NET_PASSWORD_LETTERS = 10 };
void net_password_new(const uint8_t rnd[NET_PASSWORD_LETTERS], char out[NET_PASSWORD_LETTERS + 1]);   // 50 bits
int net_invite_make(uint32_t session, const char *password, char out[NET_INVITE_LEN]);   // 0 if not a made password
// Reads an invite (spaces, dashes and lower case allowed; nothing else); 0 if not one or a bad checksum.
int net_invite_read(const char *text, uint32_t *session, char password[NET_PASSWORD_LETTERS + 1]);
