// Pit Stop owns the in-game layout, navigation and capture state. Called on the present path.
#pragma once
extern "C" {
#include "input.h"
}

enum class PitResult { None, Resume, Quit };
void pit_load_fonts(float scale);
void pit_open(void);
PitResult pit_frame(float width, float height, const InputMenuPad &pad, int test_page);
