# runtime/shaders/texture

Texture decoding and recompression on the GPU (`gfx/vk/tex/vk_astc.c`).

| File | What it does |
|---|---|
| `astc_decode.comp` | Compute ASTC decoder into RGBA8 (with direct Switch 2 block-linear input) |
| `bc7_encode.comp` | Mode 6 BC7 encoder: recompresses the decoded ASTC to 1 byte per texel instead of 4 |
| `bc1_encode.comp` | BC1 encoder: half a byte per texel, for ASTC without alpha |
| `bl_deswizzle.comp` | Generic Switch 2 block-linear to linear deswizzle for 4-byte-aligned textures |
