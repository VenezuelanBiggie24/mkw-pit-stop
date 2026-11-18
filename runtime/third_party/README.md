# runtime/third_party

| Folder | What it is |
|---|---|
| `monocypher/` | [Monocypher](https://monocypher.org) 4.0.2 (BSD-2-Clause or CC0, `LICENCE.md`; tarball SHA-256 38d07179…0ae0879c), `monocypher.c` and `.h` unchanged: X25519, BLAKE2b, XChaCha20-Poly1305 for local wireless play (`runtime/net/net_proto.c`) and `server/lobby` |
| `imgui/` | Dear ImGui 1.91.9 (MIT) with its back ends: Vulkan (in-game menu), SDL3 and SDL_Renderer (start screen). One local change, marked `mkw-recomp:` in `imgui_draw.cpp`: word wrap does not break after a dot or comma followed by a digit |
| `nanosvg/` | `nanosvg.h` and `nanosvgrast.h` from [nanosvg](https://github.com/memononen/nanosvg) @ 239e102e (zlib, its text at the top of each file), unchanged: rasterises the GitHub and Discord marks of the start screen (`ui/launcher/launcher_brand.cpp`) |
| `nxformat/` | `nx_reader.hpp` from [NXFormat](https://github.com/fab918/nxformat) @ 208b815 (MIT, its `LICENSE` beside it), unchanged: the reader of `.nx` dumps, called from `sdk/fs/nx.cpp` |
| `stb/` | `stb_image.h` v2.30 from [stb](https://github.com/nothings/stb) @ 048d2aa5 (MIT or public domain, its license at the end of the file), unchanged: decodes the GameBanana preview pictures (JPEG, PNG) of the Mods page (`ui/launcher/mods_browse.cpp`) |
| `bc7enc/` | `bc7enc.cpp` from [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) @ b9438627 (MIT), unchanged; `bc7enc_c.cpp` calls it from C (`vk_texpre.c`) |
