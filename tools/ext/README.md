# tools/ext

Example extensions: code outside the runtime that replaces or watches the game's functions by name through
[`runtime/core/ext/mkw_ext.h`](../../runtime/core/ext/mkw_ext.h). How extensions are built, loaded and what they may
rely on: [docs/project/extensions.md](../../docs/project/extensions.md).

| Folder | What it is |
|---|---|
| [hello/](hello/README.md) | The smallest extension and the hook's test: replaces `nn::nifm::IsNetworkAvailable` with the runtime's own answer (no network) and logs its first call |
