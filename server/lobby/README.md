# server/lobby

`mkw-lobby`, the lobby and relay of local wireless play over the internet ([docs/multiplayer.md](../../docs/multiplayer.md)):
public sessions listed (the game's room search shows them as rooms), private ones found by the number in an invite
code, stations introduced to their host, and a relay for players whose routers block direct connections. It never
reads game traffic (sealed by keys it does not have) and keeps nothing on disk. Linux first; tested by
`tests/test_net.py` without the game.

| File | What it is |
|---|---|
| `mkw_lobby.c` | The whole server: one UDP port, sessions, cookies, rate limits, relay |

Build and run (one UDP port open to the internet; the default is 11461):

```
cc -O2 -I runtime/net -I runtime/third_party/monocypher server/lobby/mkw_lobby.c runtime/net/net_proto.c \
   runtime/net/net_lobby.c runtime/net/net_udp.c runtime/third_party/monocypher/monocypher.c -o build/mkw-lobby
build/mkw-lobby --port 11461 [--words FILE] [--max-sessions 1024] [--no-relay] [--quiet]
```

Players then set `lobby = <address>:11461` and `local_wireless = online` in `mkw.ini` (the PS5 takes an address, not
a name). What one session costs the server: docs/multiplayer.md, "Running a lobby".
