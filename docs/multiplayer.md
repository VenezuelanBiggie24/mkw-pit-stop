# Multiplayer

Mario Kart World has its own multiplayer; this port carries it, it invents no game sync. Three ways to play:

| Way | Status | Setting |
|---|---|---|
| Split screen, 2-4 players on one machine | The game's own; controllers mapped as the console's applet does. Not yet measured on the PS5 | none |
| Local wireless over the LAN (PS5 with PS5, PS5 with the desktop build) | Implemented, tested on loopback without the game; not yet with two machines | `local_wireless = lan` |
| Local wireless over the internet: public rooms, invite codes, relay | Implemented with the lobby (`server/lobby`), tested on loopback | `local_wireless = online`, `lobby = address:port` |

Never Nintendo's online service: the game's online mode needs `nn::nifm`, which stays a console without a network, and
every server name lookup fails (`GetAddrInfo`). Nothing here says or suggests "online play" in Nintendo's sense: what
travels over the internet is the game's **local wireless** mode.

## Split screen

- **Players.** Up to 8 (`runtime/input/input.h`). Desktop: player 1 is the keyboard plus the first gamepad, each other
  gamepad another player; `keyboard_player = own` (Controls tab) makes the keyboard a player alone. PS5: a player is a
  signed-in user's DualSense (the PS5 pairs each controller with a user; a guest user counts), up to 4 (the template's
  `pad_scan`, every second).
- **The controller applet** (`nn::hid::ShowControllerSupport`, `runtime/sdk/ov/ov_input.c`) answers at once with the
  players that have a controller and gives them the game's controller ids 0.. in connection order, as the console's
  applet does: with players 1 and 3 connected (2 signed out), ids 0 and 1 are theirs. Before the first applet id N is
  player N + 1. Each pad then shows its number: the DualSense light bar on the PS5 (blue, red, green, pink: the PS5's
  own order), SDL's player index and LED on the desktop.
- **A controller lost mid-race**: its id reads disconnected, the game pauses and opens the applet; the controller back
  (or another) takes the free id.
- **Pit Stop** opens from player 1's controller (Start+Back, or L3+R3 for the overlay) or the keyboard; the other
  players' chords are game input. With `keyboard_player = own`, the keyboard opens it.
- **Cost.** Each viewport is another pass of the game's renderer: more draws for the replay thread and the GPU, more
  culling and animation work on the game's job threads. The race on the PS5 is CPU-bound at ~24-26 ms a frame at 30
  fps (docs/ps5.md); whether 2 and 4 players hold 30 fps at full speed is the console measurement below.

## Local wireless: what the game does

Found with the executable database (`tools/analisis/q.py`):

- **The entry**: `main+0x22477b4` brings up the network for a mode (w1). Modes 1 and 3 call `nn::nifm` (3 with
  `SetLocalNetworkMode(true)`: a LAN mode over the console's network); mode 1 then logs in to Nintendo's service
  (`main+0x14c5c2c`). **Mode 2, local wireless, calls only `nn::socket::SetAllocator`, `nn::socket::internal::Initialize`
  and `nn::ldn::Initialize`: no nifm.** So nifm stays offline whatever `local_wireless` says.
- **ldn**: the game imports 25 `nn::ldn` calls, the Switch 1 names (`Scan`, `CreateNetwork[Private]`,
  `Connect[Private]`, `SetAdvertiseData`, `SetStationAcceptPolicy`, `Reject`, `GetNetworkInfo`, `GetIpv4Address`,
  `AttachStateChangeEvent`, ...); the SDK reaches them through the same `ldn:u` service as on the Switch 1 (`sdk+0xb9e6c4`).
  The callers are PIA's local communication layer (`main+0x1540348..0x15445xx`: create at `main+0x1541434`,
  `main+0x1542ad0`; scan at `main+0x1542320`).
- **Game traffic**: Nintendo's PIA (`nn::pia`, "Pia ReceiveThreadStream", "pia transport heap") over UDP sockets on the
  addresses ldn hands out: `Socket`, `Bind` (`main+0x151814c`), `SetSockOpt` with `SO_REUSEADDR` (4), `SO_BROADCAST`
  (0x20), `SO_REUSEPORT` (0x200) and IP option 4 (`main+0x15182c0..0x1518550`), `SendTo`, `RecvFrom` with
  `MSG_DONTWAIT` (0x80, `main+0x15186a8`). No TCP, no `Connect`/`Listen`. 32 `nn::socket` imports in all; before this
  work every one of them reached the console's `bsd:u` service, which the runtime does not serve (a fatal IPC).
- **Ticks and frame rate**: PIA sends from its own threads and the game's logic runs per tick; the PS5's 30 fps with
  two ticks a frame (`fps = 30`) should only bunch a peer's datagrams two ticks at a time. Not verified: the LAN test
  below runs a 30 fps PS5 against a 60 fps PC on purpose.

## Local wireless: how it is carried

How others did it: **Ryujinx** (MIT; read at 6af327bd, `src/Ryujinx.HLE/HOS/Services/Ldn`) has two back ends:
`LdnMitm`, compatible with ldn_mitm on hacked Switches (UDP broadcast on port 11452, the game binds the PC's real LAN
address), and `LdnRyu`, a TCP connection to their master server with virtual addresses (10.114.x.x) and every game
socket proxied through it (`ProxyData` with source and destination address and port). It handles Switch 1 titles;
nothing Switch 2 specific was found. **ldn_mitm** (GPL) does the LAN way on a Switch. Eden and EglantinePS5 (GPL): not
needed. We take Ryujinx's structure layouts and state machine (MIT, cited in `ldn_types.h`, `net_ldn.c`) and the idea of
virtual addresses with proxied sockets, with our own protocol:

- **One UDP port, a star.** The host listens on UDP 11460 (`net_port`); every station talks to the host only and the
  host forwards between stations. Only the host must be reachable; the relay carries a session unchanged; the host
  stamps every datagram's source with the station's own address (no spoofing).
- **Virtual addresses.** The session is 169.254.X.0/24 (X random), the host .1, node n .n+1, as the console's ldn. The
  game's sockets are virtual (`net_socket.c`): bound to a port of that address, their datagrams sent through the
  session and delivered into their queue. Nothing of the host's network is reachable from the game.
- **Discovery.** On the LAN a station broadcasts `DISCOVER` (255.255.255.255 and the interface's /24; `net_iface`
  picks the interface); hosts of the same build answer `ADVERT` with their `NetworkInfo` (the game's advertise data
  included: the room as the game describes it). Online, the lobby's public rooms and the invited one join the list.
  The game's own room search shows all of them as rooms.
- **No acknowledgements.** The host sends the whole `NetworkInfo` on every change and once a second; peers ping every
  500 ms and drop each other after 5 s of silence (ldn's SignalLost). The game's datagrams ride unreliable, as over the
  radio: PIA resends what it needs.
- **Limits.** One game datagram per session datagram, up to 1428 bytes (1472 less our 44); a longer one is dropped and
  logged once. Per address before a key: 20 messages a second; 5 wrong passwords in a minute block the address for a
  minute; at most 8 handshakes at once. Per station: 1 MiB/s and 1000 datagrams a second. Every message's length is
  fixed (or bounded) and checked before it is read; a datagram with trailing bytes is refused whole.

### Protocol (version 1)

Header: `MKWN`, u8 version, u8 type, u16 body length. Types (`net_proto.h`): clear `DISCOVER`, `ADVERT`, `HELLO`,
`CHALLENGE`, `PROOF`, `REFUSE`; sealed `JOIN`, `WELCOME`, `SYNC`, `DATA`, `PING`, `PONG`, `LEAVE`; lobby 0x40.. and
`RELAY` 0x50 (`net_lobby.h`).

Join: `HELLO` (compatibility tag, X25519 key, session id) -> `CHALLENGE` (32-byte nonce, the host's key for this
station) -> `PROOF` + `JOIN` (UserConfig, local communication version, the game's SecurityConfig) -> `WELCOME` (node
id, NetworkInfo) and a `SYNC` to everyone.

### Security

The design follows Squadala (daurosk/oot-remake-multiplayer @ 39b98de, GPL-3.0: ideas only, no code); primitives from
Monocypher 4.0.2.

- **Every session's traffic is encrypted** (XChaCha20-Poly1305) once the handshake ends. Keys: BLAKE2b keyed by the
  X25519 shared secret over a label, the password and the exact `HELLO` and `CHALLENGE` datagrams, one per direction.
  Nonce: direction, message type, a 64-bit counter; a counter seen, or more than 64 behind the newest, is dropped.
- **The password is never sent.** `PROOF` is BLAKE2b keyed by the password over the nonce, both public keys and the
  shared secret: valid on one connection only, and a recorded exchange cannot be guessed offline without a private
  key (stronger than a proof over public values). A host that does not know the password cannot make the keys either.
  Private online sessions get a made password (10 letters, 50 bits); open ones (LAN, public) have none, and are still
  encrypted (against listeners, not against someone who joins).
- **Builds**: the compatibility tag is BLAKE2b of the protocol version, the SHA-256 of the game's `main`, the game
  version and the runtime version; a host of another build does not answer a scan and refuses a `HELLO`
  (`REFUSE version`). The host also checks the game's own passphrase and local communication version, as the console's
  ldn does (`LEAVE rejected`).
- **Nothing listens** unless the game is hosting a room (the host's port) or searching/joined (an ephemeral port).
- **Invite codes** name a lobby session by number and carry its password, never an address: `0x31`, u32 session, the
  password's 50 bits, a 3-byte BLAKE2b checksum = 24 letters from `ABCDEFGHJKLMNPQRSTUVWXYZ23456789`, in groups of five.
  Every single changed letter is caught (test).

## Online: lobby, rooms, invite codes, relay

`server/lobby/mkw_lobby.c` (one UDP port, 11461 by default; no disk state). The runtime's lobby client is in
`net_ldn.c` and uses the session's own socket, so the address the lobby sees is the one stations reach.

- **Hosting** online registers the room (`REGISTER`: compatibility tag, a random token, name, players and slots,
  region, flags, the NetworkInfo) and renews it every 10 s and on every change; it expires 30 s after the last renewal.
  `net_host = public` lists it; `private` (default) gives an invite code (Multiplayer page; select it to copy).
- **Room search**: the lobby's public rooms of the same build (not closed, not full; up to 16) and the invited one
  (`net_join`, or "Join with an invite code" on the desktop, which reads the clipboard) appear in the game's own search.
  What the game's scan can carry: a `NetworkInfo` per room, i.e. the host's user name (33 bytes), players and slots, and
  the game's 384 bytes of advertise data (its own room description); the lobby's name, region and mode are for the
  Multiplayer page's list, the game never sees them.
- **Joining**: `CONNECT` -> the lobby tells the host the station's address (`INTRODUCE`; the host sends three
  DISCOVER-sized datagrams to open its router) and the station the host's (`PEER`). The station tries direct for 2 s,
  then the relay. `net_relay_only = 1` skips direct: no peer ever learns this player's address. Addresses are never
  shown on screen.
- **Relay**: `RELAY` wraps a whole session datagram (the lobby cannot read it). A station is admitted by its `CONNECT`
  (for 30 minutes); a session relays for up to 16 stations and 4 MiB/s.
- **Abuse**: names 1-32 bytes of printable UTF-8, filtered (a short built-in list plus `--words FILE`), public rooms
  only; per address 20 messages a second and 2 hosted rooms; 10 wrong session numbers a minute silence an address;
  every answer longer than its request needs the address's cookie (no reflection); the host kicks (Multiplayer page,
  and the game's own `Reject`) and closes its room (the page, and the game's `RejectAll` at race start).
- **Not done** (roadmap): UPnP and PCP/NAT-PMP port mappings and STUN. The lobby already gives the host's public
  address (what STUN would) and the introduction opens cone NATs; symmetric NAT and CGNAT take the relay. Whether the
  PS5 may open UPnP mappings is untested.

### Running a lobby

Who runs the public lobby, and pays for it, is the project owner's decision; anyone can run one. Per session, as
designed (to confirm with the console run's `net:` lines every 10 s):

- **Direct sessions** cost the lobby almost nothing: a 1.2 KB `REGISTER` every 10 s per room, a few small messages per
  join.
- **Relayed** players cost twice their traffic: in and out. PIA's local wireless traffic is not measured yet; at
  30-60 datagrams a second of 100-300 bytes per peer plus our 72 bytes (44 ours, 28 UDP/IPv4), a relayed player is
  roughly 10-25 KiB/s each way, so ~20-50 KiB/s of server bandwidth, a full room of 7 relayed stations ~0.3 MiB/s.
- **CPU**: the relay copies datagrams and checks limits, no cryptography: a single core forwards tens of thousands of
  datagrams a second. A small VPS (1 vCPU, 1 GB, 1 TB/month) holds hundreds of direct rooms and a few tens of fully
  relayed ones; traffic, not CPU, is the cost.

## Settings

| Key | Default | What it does |
|---|---|---|
| `local_wireless` | `off` | `off` (the game sees airplane mode), `lan`, `online`; read at start |
| `lobby` | (none) | The lobby's `address:port` (a name on the desktop; the PS5 takes an address) |
| `net_name` | "Mario Kart World room" | Your public rooms' name in the lobby |
| `net_host` | `private` | Rooms you host online: `private` (invite code) or `public` |
| `net_region` | `any` | Shown with public rooms |
| `net_relay_only` | `0` | Always through the relay online |
| `net_port` | `11460` | The host's UDP port |
| `net_iface` | (auto) | The IPv4 address of the interface to search the LAN on |
| `net_join` | (none) | An invite code: that room shows in the search |
| `keyboard_player` | `shared` | Desktop: `own` makes the keyboard player 1 alone |

## Testing

- Without the game: `uv run pytest tests/test_net.py` (loopback sessions, the lobby and relay, fuzzing, invites,
  passwords, versions; under AddressSanitizer), `build/mkw --probe-services` (ldn off answers airplane mode, the
  sockets outside a session).
- On the consoles: `build/run/ps5/mkw.ini.task44-split` (split screen, 1/2/4 players: frame, busy, GPU, replay, game
  threads' CPU, memory) and `mkw.ini.task44-lan` (two machines on a LAN: search, join, race, leave, a dropped peer, a
  closed room; the `net:` bandwidth lines). Online after the LAN works: the same with `local_wireless = online` and a
  lobby, one machine behind a phone's hotspot (CGNAT: the relay).
