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
    // The PC builder's own step while it builds, before it sends (PIT1T, tools/ps5/send_ps5.py StatusNotifier):
    // pc_index of pc_count (0: no word from it yet), its name or, pc_failed, why it failed; pc_progress of pc_total.
    uint8_t pc_index, pc_count, pc_failed;
    uint32_t pc_progress, pc_total;
    char pc_label[201];
    // Where a send's time goes, to find why it runs at ~1 MB/s where FTP to the same console is far faster (players,
    // r/PS5_Jailbreak, 23/11/2026): send_ns since PIT1B; of it, the receiving thread waiting on the network (net_ns) and
    // for a free buffer while the writer is behind (slot_ns); the writer thread in write() (write_ns) and in SHA-256
    // (hash_ns). rcvbuf: the receive buffer the console gave the connection (SO_RCVBUF asked 4 MiB).
    uint64_t send_ns, net_ns, slot_ns, write_ns, hash_ns;
    int rcvbuf;
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
// first, then the same in root_dir's subfolders up to two levels down (the PC builder keeps the dump's own folders
// below the one the player picked), then the folders listed in root_dir/pitstop-game-dirs.txt (the copy already on
// the console elsewhere), each with romfs/ or romfs.nup; matched by exefs/main's SHA-256 when more than one is there. NULL when none is.
const char *ps5_game_dir(const char *root_dir, char *why, size_t why_cap);

// The RomFS of a game folder (ps5_game_dir's): dir/romfs.nup when there, else dir/romfs; 0 when neither exists.
int ps5_game_romfs(const char *dir, char *out, size_t cap);

// After a clean ProsperoStore update (the app's folder replaced by the release zip, the old one moved to
// <drive>/prosperostore/previous/<title_id>), move back every entry of the old folder the new one lacks: the game files
// the PC builder sent, the saves and mkw.ini. Only when store_root/receipts/<title_id>.json names the folder running as
// app_dir and that folder is fresh from the zip. Returns the number of entries moved (0: nothing to do).
int ps5_store_restore(const char *store_root, const char *title_id, const char *app_dir);

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
