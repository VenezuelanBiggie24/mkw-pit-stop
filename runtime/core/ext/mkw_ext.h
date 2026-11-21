// mkw_ext.h: the interface between the runtime and an extension, a project outside this repository that replaces or
// watches functions of the game by symbol name, as the runtime's own HOST_OVERRIDE / HOST_OBSERVER do
// (docs/project/extensions.md). Self-contained on purpose: an extension includes this file alone, never the runtime's
// headers, so it builds against any runtime with the same MKW_EXT_API_VERSION.
//
// An extension defines one function:
//
//     MKW_EXT_EXPORT int mkw_ext_init(const MkwExtApi *api);
//
// called once at boot, before the game's modules are loaded, on the main thread. It registers its functions with
// api->override / api->observe and returns 0; anything else stops the boot (the extension could not start).
//
// The same source builds two ways:
// - linked into the runtime: `build.py --ext DIR` (and tools/ps5/build_game.py --ext DIR) compiles every .c in DIR into
//   the executable; this is the only way on PS5, which loads no libraries;
// - as a library (Linux .so, Windows .dll) named in MKW_EXT, loaded at boot.
//
// Functions the extension registers run on the game's threads with the guest's registers (MkwCpu): arguments in x0..x7
// and v0..v7 as the AArch64 calling convention places them, the result in x0 or v0. Guest pointers are host pointers.
// A function that can wait (a socket, a lock, a sleep) brackets the wait with wait_begin / wait_end, so the waiting
// thread gives its virtual core to the others, as the runtime's own sockets do.
#pragma once
#include <stdint.h>

#define MKW_EXT_API_VERSION 1

#ifdef _WIN32
#define MKW_EXT_EXPORT __declspec(dllexport)
#else
#define MKW_EXT_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MkwCpu MkwCpu;                // a game thread's registers; opaque, read through the api
typedef void (*MkwExtFn)(MkwCpu *c);         // a replacement or observer of one guest function

typedef struct MkwExtApi {
    uint32_t version;                        // MKW_EXT_API_VERSION of the runtime
    uint32_t size;                           // sizeof(MkwExtApi) in the runtime: later versions only append fields
    const char *game_version;                // the game code in use, as the game shows it: "1.0.0", "1.8.0"
    const char *runtime_version;             // the runtime's own version string

    // Registration, inside mkw_ext_init only (after it returns, these fail). symbol is a mangled name as the game
    // imports it ("_ZN2nn4nifm18IsNetworkAvailableEv"); the string must stay valid. 0 on success, -1 when the table is
    // full or registration has ended.
    // override: fn replaces the symbol in every module. It wins over the runtime's own replacement of the same symbol
    //           (the log says so), and over any observer of it.
    // observe:  fn runs instead of an imported function and may call it with call_original. Imports through the PLT
    //           only (a function a module calls from another module).
    int (*override)(const char *symbol, MkwExtFn fn);
    int (*observe)(const char *symbol, MkwExtFn fn);

    // Registers of the calling game thread.
    uint64_t (*arg)(const MkwCpu *c, int i);         // integer or pointer argument i (8 and beyond from the stack)
    void *(*arg_ptr)(const MkwCpu *c, int i);        // the same, as a pointer
    uint64_t (*arg_v)(const MkwCpu *c, int i);       // low 64 bits of v[i], i < 8: a float or double argument's bits
    void (*ret)(MkwCpu *c, uint64_t x0);             // integer or pointer result
    void (*ret_v)(MkwCpu *c, uint64_t v0);           // float or double result, as bits
    uint64_t (*call_original)(MkwCpu *c);            // in an observer: the guest's own function, current x0..x7; x0
    uint64_t (*call_guest)(MkwCpu *c, uint64_t fn, int n, const uint64_t *args);   // a guest function, n <= 8 args

    // Waits: wait_begin before a call that can block, wait_end with its value after it.
    int (*wait_begin)(void);
    void (*wait_end)(int token);

    void (*log)(const char *fmt, ...);               // one line to the runtime's log, "ext: " in front
} MkwExtApi;

typedef int (*MkwExtInit)(const MkwExtApi *api);
MKW_EXT_EXPORT int mkw_ext_init(const MkwExtApi *api);

#ifdef __cplusplus
}
#endif
