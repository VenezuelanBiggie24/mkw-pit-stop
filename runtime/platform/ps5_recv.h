// Placeholder TCP file receiver, path validator, console IP query, and game-files check for PS5 Pit Stop.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS5_RECV_PORT 19913

enum {
    PS5_RECV_IDLE = 0,
    PS5_RECV_WAITING = 1,
    PS5_RECV_RECEIVING = 2,
    PS5_RECV_DONE = 3,
    PS5_RECV_ERROR = 4,
};

typedef struct {
    int state;
    uint16_t port;
    char ip[64];
    uint64_t files_done;
    uint64_t files_total;
    uint64_t bytes_done;
    uint64_t bytes_total;
    char current_file[256];
    char error[256];
} Ps5RecvStatus;

// Validate a relative path from the builder: must stay strictly inside the title folder.
// Returns 1 if valid, 0 if rejected.
int ps5_recv_validate_path(const char *rel);

// Query the console's local IPv4 address (on PS5 via libSceNetCtl; fallback string if offline/unavailable).
// Returns 1 if an IP was obtained, 0 otherwise.
int ps5_console_ip(char *out, size_t cap);

// Check whether the game's exefs/main and romfs/ (ps5_game_dir), and spirv/ exist under root_dir and (if
// pitstop-game.id is present) match expected_game_id. Returns 1 if ready, 0 if missing/mismatched (writing a short
// reason to why).
int ps5_game_files_ready(const char *root_dir, const char *expected_game_id, char *why, size_t why_cap);

// The SHA-256 (hex) of the exefs/main the built-in game code was generated from (RtGame.exefs_sha256), set before
// ps5_game_files_ready / ps5_game_dir so they pick the dump's matching version.
void ps5_game_set_main_sha256(const char *hex);

// The folder under root_dir holding exefs/ + romfs/ of the version the game code was built for: the dump as the
// player copied it, so updates/<title version>/ (a console dump's 1.8.0, 917504) as well as root_dir itself, newest
// first, matched by exefs/main's SHA-256 when more than one is there. NULL when none is (reason in why).
const char *ps5_game_dir(const char *root_dir, char *why, size_t why_cap);

// Start the receiver listener thread on 0.0.0.0:port (0 selects PS5_RECV_PORT or MKW_PS5_RECV_PORT).
// Returns 1 on success, 0 on bind/listen/thread failure.
int ps5_recv_start(const char *root_dir, uint16_t port, const char *port_version, const char *rt_game_id,
                   int has_game_code);

// Stop the receiver and join its thread (safe to call even if never started).
void ps5_recv_stop(void);

// Thread-safe snapshot of receiver progress and status for the placeholder UI.
void ps5_recv_status(Ps5RecvStatus *out);

#ifdef __cplusplus
}
#endif
