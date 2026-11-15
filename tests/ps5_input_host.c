// Host vectors for the SDL-free PS5 adapter and the real MKW input tables.
#include "ps5_input.h"

#include <assert.h>
#include <string.h>

// ps5_input.c reads MKW_PAD_LAYOUT; the vectors switch it between unset ("auto": by position, cross B and circle A,
// OcarinaPS5's default) and "xbox" (by label, cross A).
static const char *pad_layout;
const char *cfg_value(const char *name) { return strcmp(name, "MKW_PAD_LAYOUT") ? NULL : pad_layout; }

#define BUTTON(bit) (1ull << (bit))

static void neutral(const Ps5PadState *state) {
    assert(!state->valid && !state->buttons && !state->lx && !state->ly && !state->rx && !state->ry);
}

int main(void) {
    Ps5PadState state = {0};
    Ps5PadSample sample = {.lx = 128, .ly = 128, .rx = 128, .ry = 128};
    input_ps5_update(&state, true, NULL);
    neutral(&state);
    input_ps5_update(&state, true, &sample);
    assert(state.valid && !state.buttons && !state.lx && !state.ly && !state.rx && !state.ry);

    sample.down[PAD_SOUTH] = 1;
    sample.down[PAD_EAST] = 1;
    sample.down[PAD_WEST] = 1;
    sample.down[PAD_NORTH] = 1;
    sample.down[PAD_START] = 1;
    sample.down[PAD_BACK] = 1;
    sample.down[PAD_DPAD_UP] = 1;
    sample.lx = 0;
    sample.ly = 0;
    sample.rx = 255;
    sample.ry = 255;
    sample.l2 = 127;
    sample.r2 = 128;
    input_ps5_update(&state, true, &sample);
    const uint64_t expected = BUTTON(B_A) | BUTTON(B_B) | BUTTON(B_X) | BUTTON(B_Y) |
                              BUTTON(B_PLUS) | BUTTON(B_MINUS) | BUTTON(B_UP);
    assert(state.buttons == (expected | BUTTON(B_ZR)));
    assert(state.lx == -32767 && state.ly == 32767 && state.rx == 32767 && state.ry == -32767);

    sample.l2 = 128;
    sample.r2 = 127;
    sample.lx = 119;
    sample.ly = 138;
    sample.rx = 118;
    sample.ry = 137;
    input_ps5_update(&state, true, &sample);
    assert(state.buttons == (expected | BUTTON(B_ZL)));
    assert(!state.lx && state.ly < -2500 && state.rx < -2500 && !state.ry);

    // Default: by position. Cross (south) is B, circle (east) A, square (west) Y, triangle (north) X.
    sample = (Ps5PadSample){.down = {[PAD_SOUTH] = 1}, .lx = 128, .ly = 128, .rx = 128, .ry = 128};
    input_ps5_update(&state, true, &sample);
    assert(state.buttons == BUTTON(B_B));
    sample.down[PAD_SOUTH] = 0, sample.down[PAD_WEST] = 1;
    input_ps5_update(&state, true, &sample);
    assert(state.buttons == BUTTON(B_Y));
    // MKW_PAD_LAYOUT=xbox: by label, cross A, square X.
    pad_layout = "xbox";
    input_ps5_update(&state, true, &sample);
    assert(state.buttons == BUTTON(B_X));
    sample.down[PAD_WEST] = 0, sample.down[PAD_SOUTH] = 1;
    input_ps5_update(&state, true, &sample);
    assert(state.buttons == BUTTON(B_A));
    pad_layout = NULL;
    input_ps5_update(&state, true, NULL);
    assert(state.valid && state.buttons == BUTTON(B_A));

    sample.intercepted = true;
    input_ps5_update(&state, true, &sample);
    neutral(&state);
    input_ps5_update(&state, true, NULL);
    neutral(&state);
    sample.intercepted = false;
    sample.down[PAD_SOUTH] = 0;
    sample.down[PAD_EAST] = 1;
    input_ps5_update(&state, true, &sample);
    assert(state.valid && state.buttons == BUTTON(B_A));
    input_ps5_update(&state, false, &sample);
    neutral(&state);
    input_ps5_update(&state, true, NULL);
    neutral(&state);
}
