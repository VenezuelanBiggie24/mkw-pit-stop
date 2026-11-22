# tools/ext/hello

The smallest extension ([docs/project/extensions.md](../../../docs/project/extensions.md)), and the test of the hook.

| File | What it is |
|---|---|
| `hello.c` | Replaces `nn::nifm::IsNetworkAvailable` with the runtime's own answer (no network) and logs its first call; built in with `build.py --ext tools/ext/hello` or as a library loaded through `MKW_EXT` |
