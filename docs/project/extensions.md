# Extensions

An extension is code outside this repository that replaces or watches the game's functions by symbol name, the
way the runtime's own `HOST_OVERRIDE` and `HOST_OBSERVER` do. It exists so that projects this repository does not
accept can be built on top of it without a fork: online play (see [CONTRIBUTING.md](../../CONTRIBUTING.md)), tools,
experiments. The runtime knows nothing about what an extension does.

The interface is one header, [`runtime/core/ext/mkw_ext.h`](../../runtime/core/ext/mkw_ext.h). The smallest working
extension is [`tools/ext/hello`](../../tools/ext/hello/hello.c).

## Writing one

An extension includes `mkw_ext.h` and nothing else from the runtime, and defines a single entry point:

```c
#include "mkw_ext.h"

static const MkwExtApi *api;

static void my_replacement(MkwCpu *c) {
    uint64_t first = api->arg(c, 0);   // x0
    api->ret(c, first + 1);            // the result in x0
}

MKW_EXT_EXPORT int mkw_ext_init(const MkwExtApi *a) {
    if (a->version < MKW_EXT_API_VERSION) return 1;
    api = a;
    return api->override("_ZN2nn...", my_replacement);
}
```

`mkw_ext_init` runs once at boot, on the main thread, after the game code is chosen and before the game's modules are
loaded and their imports bound. It registers its functions and returns 0. Any other value stops the boot with
`ext: NAME did not start`. Registration fails (returns -1) once `mkw_ext_init` has returned.

- **`override(symbol, fn)`**: `fn` replaces the symbol everywhere the game's modules refer to it. If the runtime
  replaces that symbol too, the extension's version wins and the log says so (`ext: NAME replaces the runtime's own
  SYMBOL`). An override also hides any observer of the same symbol.
- **`observe(symbol, fn)`**: `fn` runs in place of an imported function, a call one module makes into another, and
  may call it with `call_original`. Observing a symbol the runtime overrides wraps the game's own code, not the
  runtime's replacement.

Registered functions run on the game's threads with that thread's registers. Arguments follow the AArch64 calling
convention: `arg(c, i)` for x0..x7 and the stack beyond, `arg_v(c, i)` for v0..v7, and `ret` or `ret_v` for the result.
A guest pointer is a host pointer. A function that can block (sockets, locks, sleeps) wraps the wait in
`wait_begin()` / `wait_end(token)`, so its virtual core goes to other game threads meanwhile, as the runtime's own
sockets do. Lengthy work belongs on a thread of the extension's own, never on the game thread
([AGENTS.md](../../AGENTS.md): nothing serial that can run elsewhere).

`MkwExtApi.size` is the size of the runtime's struct. Later versions only append fields, so an extension built against
a newer header checks `size` before using a field this version does not have. `MKW_EXT_API_VERSION` changes only when
an existing field changes.

## Building and loading

The same source builds two ways.

**Linked in.** This is the only way on PS5, which loads no libraries:

```bash
python3 tools/build/build.py --ext path/to/extension
```

```bash
python3 tools/ps5/build_game.py --stack STACK --generated GEN --ext path/to/extension
```

Every `.c` and `.cpp` under the folder is compiled with the runtime's flags into the executable. Its `mkw_ext_init` is
renamed `mkw_ext_init__FOLDER` by a define (`tools/build/ext_glue.py`), so several folders can be linked at once. They
start in command-line order, before any library.

**As a library** (Linux and Windows). Build a shared library that exports `mkw_ext_init`:

```bash
cc -shared -fPIC -O2 -Iruntime/core/ext tools/ext/hello/hello.c -o build/hello.so
```

Name it in `MKW_EXT` (environment, or `ext =` in `mkw.ini`). Several libraries are separated by `;`, and also by `:`
outside Windows:

```bash
MKW_EXT=build/hello.so build/mkw
```

A library that cannot be loaded, or that has no `mkw_ext_init`, stops the boot with the reason. It is never skipped
silently, because the player asked for it. The library stays loaded until the game exits.

## What the log shows

```
ext: hello: game 1.0.0, runtime 0.1.13
ext: hello replaces the runtime's own _ZN2nn4nifm18IsNetworkAvailableEv
ext: hello started
ext: hello: the game asked whether the network is up; answering no
```

## Limits

- Overrides bind when the modules load. A symbol cannot be replaced later, and a function the game reaches without
  a symbol (a virtual call inside one module) cannot be replaced by name. Patching by address is not part of this
  interface.
- Each of `override` and `observe` holds 64 entries, shared with the runtime's own probes.
- The game's `nn::socket` imports are datagram calls only (no `Connect`, `Listen` or `Accept`). An extension that
  replaces the socket layer serves those, plus whatever else the game reaches through other modules.
