// SDL-free conversion from the PS5 pad's raw axes and physical button positions.
#pragma once

#include "input_map.h"
#include <stdbool.h>

typedef struct {
    uint8_t down[PAD_COUNT];
    uint8_t lx, ly, rx, ry, l2, r2;
    bool intercepted;
} Ps5PadSample;

typedef struct {
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    bool valid;
} Ps5PadState;

// Call after the native poll. NULL sample means the connected controller
// produced no new reading. Disconnect or interception clears held state.
void input_ps5_update(Ps5PadState *state, bool connected, const Ps5PadSample *sample);
