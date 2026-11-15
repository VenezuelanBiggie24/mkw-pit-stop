# runtime/cpu

| File | What it is |
|---|---|
| `cpu.h` | `Cpu` (registers, NZCV, TPIDR, exclusives, FPCR/FPSR, vectors), memory accesses and the `rt_*` services the generated C uses |
| `simd.h`, `simd_fp.h` | AArch64 NEON and floating point in portable C |
| `dispatch.c`, `dispatch.h` | `rt_dispatch`: indirect calls through an address table, exercised without the game by the [PS5 smoke title](../../tools/ps5/README.md) |
| `hostfn.c` | Host functions at guest addresses (`HOST_OVERRIDE`): this is how an `nn::*` function is replaced by name; `HOST_FUNCTION` gives an address to a function without a symbol (virtual tables of objects the host hands to the game). `MKW_HOST_TIME=1`: time each one keeps a game thread stopped (render and the rest), every 600 frames |

From totk-pc-recomp (see `../README.md`).

The synthetic PS5 mixed-page test supplies `RT_GUEST_READ_PTR` and `RT_GUEST_WRITE_PTR` before including `cpu.h`.
The test checks the access width and returns the original address. Normal builds use identity conversions for both
hooks. The helpers still perform the original native load, store or atomic operation at that address.
A checked build would need freshly generated C because old lifter output can contain direct guest pointers.
