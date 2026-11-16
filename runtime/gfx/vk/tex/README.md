# runtime/gfx/vk/tex

Textures: a cache of NVN images as Vulkan images, uploads with deswizzle and ASTC, descriptor slots and samplers,
sparse and precomputed BC7.

| File | What it is |
|---|---|
| `vk_texture.c`, `vk_tex_internal.h`, `vk_formats.c` | Image cache: its state, keys and statistics; the header is what the `vk_tex_*.c` parts share; NVN to Vulkan format table |
| `vk_tex_image.c` | Image creation; which ASTC images go to BC7, BC1 or stay RGBA8 (arrays and cube maps with slices of 64x64 or more count their layers; `MKW_ASTC_BC_ARRAYS=0` keeps the per-slice rule) |
| `vk_tex_evict.c` | Image destruction, freed entries, eviction under memory pressure and the list of textures the GPU wrote |
| `vk_tex_upload.c` | Uploads with deswizzle and ASTC, the check of whether the storage changed, ASTC copies between images of different formats (decoded from the source's storage), copies out of CPU-written textures read region by region from their storage (`vk_texture_region_copy`, `MKW_TEX_REGION_COPY`), and `MKW_ASTC_BC_CHECK` (BC re-encoded uploads against their RGBA8 decode) |
| `vk_tex_alias.c` | Storage aliases (`MKW_TEX_ALIAS`) and GPU writes to mirrored textures |
| `vk_tex_slots.c` | Texture and sampler descriptor slots, and the VkSamplers |
| `vk_tex_texel.c` | ReadTexels / WriteTexels and CPU reads of what the GPU wrote |
| `vk_astc.c` | ASTC on the GPU: decoded by compute and recompressed to BC7 or BC1 (`MKW_ASTC_BC7`), in chunks of 32 MiB of texels (a larger region in bands of block rows) through one kept scratch buffer |
| `vk_sparse.c` | MKW's own: sparse textures as Vulkan sparse images, tiles bound at each `MapVirtual` |
| `vk_texpre.c` | MKW's own: BC7 of the ASTC textures encoded offline (`MKW_TEXPRE`, off) |
