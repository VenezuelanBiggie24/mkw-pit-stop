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
    char error[512];            // why, when it failed
} SetupState;

int setup_available(const char *exe_dir);       // Python and prepare.py are there
int setup_ps5_available(const char *exe_dir);   // Python, prepare_ps5.py and ps5-bundle are there
// source NULL: the last setup's copy and RomFS mode (after an update: only what changed runs again)
void setup_start(const char *exe_dir, const char *source, int link_romfs);
void setup_ps5_start(const char *exe_dir, const char *source, const char *ps5_ip);
void setup_cancel(void);
SetupState setup_state(void);                   // a copy, any time
