// hello: the smallest extension (docs/project/extensions.md), and the test of the extension hook.
//
// What it does: it replaces nn::nifm::IsNetworkAvailable with a function that answers what the runtime answers by
// default (no network) and logs its first call. Nothing changes in the game; the log shows that the extension started,
// that its override won over the runtime's own ("ext: hello replaces the runtime's own ..."), and that the game called
// it. Why this symbol: the game calls it at boot, and an outside network project would start from the same place.
//
// Build and run, linked in:   python3 tools/build/build.py --ext tools/ext/hello
// or as a library (Linux):    cc -shared -fPIC -O2 -Iruntime/core/ext tools/ext/hello/hello.c -o build/hello.so
//                             MKW_EXT=build/hello.so build/mkw
// Windows: the same with -o hello.dll and MKW_EXT=hello.dll.
#include <stdatomic.h>

#include "mkw_ext.h"

static const MkwExtApi *api;

// bool nn::nifm::IsNetworkAvailable()
static void is_network_available(MkwCpu *c) {
    static atomic_int calls;
    if (atomic_fetch_add(&calls, 1) == 0) api->log("hello: the game asked whether the network is up; answering no");
    api->ret(c, 0);
}

MKW_EXT_EXPORT int mkw_ext_init(const MkwExtApi *a) {
    if (a->version < MKW_EXT_API_VERSION) return 1;   // a runtime older than this header
    api = a;
    api->log("hello: game %s, runtime %s", api->game_version, api->runtime_version);
    return api->override("_ZN2nn4nifm18IsNetworkAvailableEv", is_network_available);
}
