// Pit Stop owns the in-game layout, navigation and capture state. Called on the present path.
#pragma once
extern "C" {
#include "input.h"
}

enum class PitResult { None, Resume, Quit };
void pit_load_fonts(float scale);
void pit_open(void);
PitResult pit_frame(float width, float height, const InputMenuPad &pad, int test_page);

// The boot screen's page (ui/menu/boot_screen.cpp) in Pit Stop's look: its backdrop and heading, one row per boot task
// (state 0 waiting, 1 running, 2 done), the running row in the focus colour with a bar (progress 0..1, or below 0 when
// the total is unknown: a moving segment), and a line under the footer rule.
struct PitLoadingRow {
    const char *label, *status;
    int state;
    float progress;
};
void pit_loading_frame(float width, float height, const char *title, const PitLoadingRow *rows, int n,
                       const char *foot);
