# runtime/core/ext

Extensions: projects outside this repository that replace or watch the game's functions by symbol name, as the
runtime's own `HOST_OVERRIDE` and `HOST_OBSERVER` do. The runtime serves no online service
([CONTRIBUTING.md](../../../CONTRIBUTING.md)); this is the generic hook such projects build on. Guide:
[docs/project/extensions.md](../../../docs/project/extensions.md).

| File | What it is |
|---|---|
| `mkw_ext.h` | The interface an extension includes, alone: `mkw_ext_init`, `MkwExtApi` (registration, registers, guest calls, waits, log), `MKW_EXT_API_VERSION` |
| `ext.c` | Starts the extensions at boot, before `ldr_load`: the ones linked in (`build.py --ext`, the table in `defaults.c`), then the libraries `MKW_EXT` names (not on PS5) |
