# runtime/third_party/monocypher

[Monocypher](https://monocypher.org) 4.0.2 (BSD-2-Clause or CC0-1.0, chosen by the user: both in `LICENCE.md`),
`monocypher.c` and `monocypher.h` unchanged. Local wireless play seals its datagrams and proves the room password with
it (`runtime/net/net_proto.c`: X25519, BLAKE2b, XChaCha20-Poly1305), and so does the lobby server (`server/lobby`).

| File | What it is |
|---|---|
| `monocypher.c`, `monocypher.h` | The library |
| `LICENCE.md` | Its two licences |
