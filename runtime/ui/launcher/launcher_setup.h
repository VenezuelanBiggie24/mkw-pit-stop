// The game files setup of the start screen (launcher_setup.cpp): runs prepare.py (desktop) or prepare_ps5.py (PS5)
// in a child process and reads its progress into SetupState.
#pragma once

enum { SETUP_MAX_STEPS = 12, SETUP_STEPS = 8, SETUP_PS5_STEPS = 9 };
enum { SETUP_MODE_DESKTOP = 0, SETUP_MODE_PS5 = 1 };

typedef struct {
    int running;
    int mode;                   // SETUP_MODE_DESKTOP or SETUP_MODE_PS5
    int step;                   // the step being run, 0..step_count-1; -1 none yet
    int step_count;             // SETUP_STEPS or SETUP_PS5_STEPS
    int done[SETUP_MAX_STEPS];  // 1 run, 2 skipped (already done by an earlier setup)
    long progress, total;       // of the step being run; total 0: no count
    int finished;               // 1 done, -1 failed or cancelled
    char code[16];              // why, when it failed: prepare.py's error code (its header), or "stopped", "cancelled",
                                // "no_setup" (the tools missing) and "" for a message of SDL's
    char error[512];            // the detail: a folder, a build id, a tool's English message...
    char note[200];             // the running step's own line (prepare_ps5.py's 'note': which phase, speed, time left)
} SetupState;

int setup_available(const char *exe_dir);       // the Python and the script are there
int setup_ps5_available(const char *exe_dir);   // Python, prepare_ps5.py and ps5-bundle are there
// The package's Python and tools/<rel> (import.py), for pages that run scripts like the setup does.
int setup_tool(const char *exe_dir, const char *rel, char *python, char *script, size_t cap);
// source NULL: the last setup's copy (after an update: only what changed runs again). link_romfs: the player's pick
// from the files page's cards (0 copy, 1 link), or < 0 for neither and prepare.py's own record of both.
void setup_start(const char *exe_dir, const char *game_dir, const char *source, int link_romfs);
// usb set: the title goes to usb/homebrew/PPSA99193 (prepare_ps5.py --usb), else to ps5_ip's receiver (--send).
// console_game set: the copy is already on the PS5 in that folder (--console-game): no game files are sent.
void setup_ps5_start(const char *exe_dir, const char *source, const char *ps5_ip, const char *usb,
                     const char *console_game);
void setup_cancel(void);
SetupState setup_state(void);                   // a copy, any time
