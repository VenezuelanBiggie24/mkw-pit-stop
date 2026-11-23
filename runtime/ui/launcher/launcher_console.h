// The PS5 for the launcher's PS5 setup wizard (ui/launcher/launcher_console.cpp): tools/ps5/console_probe.py finds
// consoles on the local network (Pit Stop's receiver or an FTP server) and says what one holds, so the wizard's first
// step can offer the console instead of asking for its address, and say before the build whether it is a first
// install, an upgrade or unreachable.
#pragma once

#define CONSOLE_FOUND_MAX 8
#define CONSOLE_LINES_MAX 4
#define CONSOLE_GAMES_MAX 4

typedef struct {
    char ip[64];
    char kind[12];      // "receiver" or "ftp"
    char what[160];     // for the player: "Pit Stop placeholder, waiting for the PC"
} ConsoleFound;

typedef struct {
    int finding, found_count;   // a search under way; what it found so far
    ConsoleFound found[CONSOLE_FOUND_MAX];
    int checking;               // a check under way
    char checked_ip[64];        // the address the last check answered for ("" none yet)
    char kind[12];              // its answer: "receiver", "ftp" or "none"
    int line_count, game_count;
    char lines[CONSOLE_LINES_MAX][320];
    char game_version[CONSOLE_GAMES_MAX][24], game_dir[CONSOLE_GAMES_MAX][96];
} ConsoleState;

void console_find_start(const char *exe_dir);                  // the local networks, a few seconds; again to search again
void console_check_start(const char *exe_dir, const char *ip); // what that console holds
ConsoleState console_state(void);                              // a copy, any time
