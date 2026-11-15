// Translate PS5 raw readings to the input.h button and stick conventions.
#include "ps5_input.h"

#include "cfg.h"

#include <string.h>

static int32_t stick(uint8_t raw) {
    int32_t value = (int32_t)raw - 128;
    value = value * 32767 / (value < 0 ? 128 : 127);
    return value > -2500 && value < 2500 ? 0 : value;   // input.c's dead zone
}

void input_ps5_update(Ps5PadState *state, bool connected, const Ps5PadSample *sample) {
    if (!connected || (sample && sample->intercepted)) {
        *state = (Ps5PadState){0};
        return;
    }
    if (!sample) return;

    uint8_t down[PAD_COUNT];
    memcpy(down, sample->down, sizeof down);
    // Half the 8-bit trigger range corresponds to input.c's SDL threshold.
    down[PAD_ZL] = sample->l2 >= 128;
    down[PAD_ZR] = sample->r2 >= 128;
    // MKW_PAD_LAYOUT as on the desktop; "auto" is the Switch layout by position (circle A, cross B, triangle X,
    // square Y), as OcarinaPS5's [pad] label_abxy = 0 default does: a DualSense has no letters to go by.
    const int layout = input_pad_layout_pick(cfg_value("MKW_PAD_LAYOUT"), 1);
    *state = (Ps5PadState){
        .buttons = input_map_pad_buttons(down, layout),
        .lx = stick(sample->lx), .ly = -stick(sample->ly),
        .rx = stick(sample->rx), .ry = -stick(sample->ry),
        .valid = true,
    };
    memcpy(state->down, down, sizeof down);
}
