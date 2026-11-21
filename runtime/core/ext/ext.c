// Extensions: projects outside this repository that replace or watch functions of the game by symbol name
// (mkw_ext.h, docs/project/extensions.md). This file starts them at boot, before ldr_load binds the modules' imports:
// first the ones linked in (build.py --ext, listed in defaults.c by the build), then the libraries MKW_EXT names.
//
// The runtime itself serves no network service (CONTRIBUTING.md): this is the generic hook such projects build on.
// Nothing here knows what an extension does; their overrides go through hostfn_add_override / hostfn_add_observer,
// which the loader consults before the linked tables.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "mkw_ext.h"
#include "platform/platform.h"
#include "version.h"

// defaults.c (tools/build/build.py, tools/ps5/build_game.py): the extensions compiled into this executable
extern const MkwExtInit rt_ext_linked[];
extern const char *const rt_ext_linked_names[];
extern const int rt_ext_linked_count;

static int registering;   // inside an mkw_ext_init: registration is open
static const char *current;   // its name, for the log

static int api_override(const char *symbol, MkwExtFn fn) {
    if (!registering || !symbol || !fn) return -1;
    if (hostfn_override(symbol)) rt_log("ext: %s replaces the runtime's own %s", current, symbol);
    hostfn_add_override(symbol, (GuestFn)(void *)fn);
    return 0;
}

static int api_observe(const char *symbol, MkwExtFn fn) {
    if (!registering || !symbol || !fn) return -1;
    hostfn_add_observer(symbol, (GuestFn)(void *)fn);
    return 0;
}

static uint64_t api_arg(const MkwCpu *c, int i) { return arg_x((const Cpu *)c, i); }
static void *api_arg_ptr(const MkwCpu *c, int i) { return arg_p((const Cpu *)c, i); }
static uint64_t api_arg_v(const MkwCpu *c, int i) { return i >= 0 && i < 8 ? (uint64_t)((const Cpu *)c)->v[i] : 0; }
static void api_ret(MkwCpu *c, uint64_t v) { ret_x((Cpu *)c, v); }
static void api_ret_v(MkwCpu *c, uint64_t v) { ((Cpu *)c)->v[0] = v; }
static uint64_t api_call_original(MkwCpu *c) { return rt_original((Cpu *)c); }

static uint64_t api_call_guest(MkwCpu *mc, uint64_t fn, int n, const uint64_t *a) {
    Cpu *c = (Cpu *)mc;
    uint64_t z[8] = {0};
    if (n < 0 || n > 8) rt_fatal("ext: call_guest with %d arguments (8 at most)", n);
    if (n && a) memcpy(z, a, (size_t)n * 8);
    return rt_call(c, fn, n, z[0], z[1], z[2], z[3], z[4], z[5], z[6], z[7]);
}

static int api_wait_begin(void) { return kern_core_away(); }
static void api_wait_end(int token) { kern_core_back(&token); }

static void api_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    rt_log("ext: %s", line);
}

static void start(const char *name, MkwExtInit init, const MkwExtApi *api) {
    current = name;
    registering = 1;
    int r = init(api);
    registering = 0;
    if (r) rt_fatal("ext: %s did not start (mkw_ext_init returned %d)", name, r);
    rt_log("ext: %s started", name);
}

// MKW_EXT: libraries to load, separated by ';' (and ':' outside Windows, as PATH)
static void load_libraries(const MkwExtApi *api) {
    const char *list = cfg_get("MKW_EXT");   // extension libraries to load at boot (docs/project/extensions.md)
    if (!list || !*list) return;
#ifdef MKW_PS5_NATIVE
    (void)api;
    rt_fatal("ext: MKW_EXT=%s: the console loads no libraries; build the extension in (build_game.py --ext)", list);
#else
#ifdef _WIN32
    const char *seps = ";";
#else
    const char *seps = ";:";
#endif
    for (const char *p = list; *p;) {
        size_t n = strcspn(p, seps);
        char path[1024];
        if (n >= sizeof path) rt_fatal("ext: MKW_EXT entry longer than %zu bytes", sizeof path - 1);
        memcpy(path, p, n);
        path[n] = 0;
        p += n + (p[n] != 0);
        if (!n) continue;
        // the library stays loaded for the whole run: the game calls its functions until the process ends
        void *lib = plat_lib_open(path);
        if (!lib) rt_fatal("ext: cannot load %s", path);
        MkwExtInit init = (MkwExtInit)plat_lib_sym(lib, "mkw_ext_init");
        if (!init) rt_fatal("ext: %s has no mkw_ext_init", path);
        start(path, init, api);   // the name is used only while init runs
    }
#endif
}

void rt_ext_start(void) {
    static MkwExtApi api;
    if (api.version) return;   // once
    api = (MkwExtApi){
        .version = MKW_EXT_API_VERSION,
        .size = sizeof api,
        .game_version = rt_version()->display,
        .runtime_version = rt_port_version,
        .override = api_override,
        .observe = api_observe,
        .arg = api_arg,
        .arg_ptr = api_arg_ptr,
        .arg_v = api_arg_v,
        .ret = api_ret,
        .ret_v = api_ret_v,
        .call_original = api_call_original,
        .call_guest = api_call_guest,
        .wait_begin = api_wait_begin,
        .wait_end = api_wait_end,
        .log = api_log,
    };
    for (int i = 0; i < rt_ext_linked_count; i++) start(rt_ext_linked_names[i], rt_ext_linked[i], &api);
    load_libraries(&api);
}
