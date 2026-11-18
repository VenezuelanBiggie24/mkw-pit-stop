# runtime/net

Local wireless play (the game's own Wireless Play, `nn::ldn`) carried over UDP on the local network or through a
lobby on the internet, and the game's sockets carried inside it. Design, protocol, security and limits:
[docs/ps5/multiplayer.md](../../docs/ps5/multiplayer.md). Off by default (`local_wireless = off` in `mkw.ini`).

| File | What it is |
|---|---|
| `net_ldn.c` | `nn::ldn` replaced by name: state machine (Ryujinx's, MIT), host and station, room search on the LAN and through the lobby, the lobby client, the session's thread (`mkw-net`) |
| `net_socket.c` | `nn::socket` replaced by name: virtual UDP sockets inside the session (PIA's), no name resolution, no address outside the session |
| `net_session.c`, `net_session.h` | One session over one UDP socket, host or station: handshake, node table, routing of game datagrams through the host, keepalives, per-address and per-station limits, the relay path. No runtime dependency (the tests build it alone) |
| `net_proto.c`, `net_proto.h` | Wire format: header, fixed limits per message, sealing (XChaCha20-Poly1305), password proof and keys (X25519, BLAKE2b), compatibility tag, invite codes |
| `net_lobby.c`, `net_lobby.h` | The lobby's messages and limits, session metadata, the session name filter (shared with `server/lobby`) |
| `net_udp.c`, `net_udp.h` | The host system's UDP sockets: Linux and PS5 (BSD), Windows (Winsock) |
| `ldn_types.h` | `nn::ldn` structure offsets (Ryujinx @ 6af327bd, MIT) |
| `net_game.h` | Between `net_ldn.c` and `net_socket.c` |
| `net_ui.h` | What the Multiplayer page shows (`ui/menu/net_page.cpp`) |
