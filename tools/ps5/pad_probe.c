// Consume the pinned MIT PS5_VulkanTemplate pad API (91179df); see docs/project/research.md.
#include "ps5_input.h"
#include "platform.h"

typedef enum { WAIT_NEUTRAL, WAIT_PRESS, WAIT_RELEASE, COMPLETE } ProbePhase;

static Ps5PadState state;
static ProbePhase phase;

static Ps5PadSample physical(const struct pad_reading *raw) {
    const uint32_t held = raw->buttons;
    return (Ps5PadSample){
        .down = {
            [PAD_SOUTH] = !!(held & PAD_CROSS), [PAD_EAST] = !!(held & PAD_CIRCLE),
            [PAD_WEST] = !!(held & PAD_SQUARE), [PAD_NORTH] = !!(held & PAD_TRIANGLE),
            [PAD_LSTICK] = !!(held & PAD_L3), [PAD_RSTICK] = !!(held & PAD_R3),
            [PAD_L] = !!(held & PAD_L1), [PAD_R] = !!(held & PAD_R1),
            [PAD_START] = !!(held & PAD_OPTIONS), [PAD_BACK] = !!(held & PAD_TOUCH_PAD),
            [PAD_DPAD_LEFT] = !!(held & PAD_LEFT), [PAD_DPAD_UP] = !!(held & PAD_UP),
            [PAD_DPAD_RIGHT] = !!(held & PAD_RIGHT), [PAD_DPAD_DOWN] = !!(held & PAD_DOWN),
        },
        .lx = raw->left_x, .ly = raw->left_y, .rx = raw->right_x, .ry = raw->right_y,
        .l2 = raw->l2, .r2 = raw->r2, .intercepted = !!(held & PAD_INTERCEPTED),
    };
}

void mkw_pad_probe_start(void) {
    say("PS5 DualSense input: release Cross, then press and release it before the test ends");
}

static void interrupt_sequence(void) {
    if (phase == COMPLETE) return;
    if (phase == WAIT_RELEASE) say("PS5 DualSense input: press discarded after loss of input");
    phase = WAIT_NEUTRAL;
}

bool mkw_pad_probe_frame(void) {
    const struct pad_reading *readings;
    const int count = pad_player_readings(0, &readings);
    bool connected = (pad_players() & 1) != 0;
    if (count > 0) {
        // The newest sample drives the frame. Earlier samples can still
        // invalidate an in-progress press/release sequence.
        for (int i = 0; i < count - 1; ++i) {
            if (!readings[i].connected || (readings[i].buttons & PAD_INTERCEPTED)) {
                interrupt_sequence();
                break;
            }
        }
        const struct pad_reading *latest = &readings[count - 1];
        Ps5PadSample sample = physical(latest);
        input_ps5_update(&state, connected && latest->connected, &sample);
    } else {
        input_ps5_update(&state, connected, NULL);
    }

    const bool held = (state.buttons & (1ull << B_A)) != 0;
    if (!state.valid) {
        interrupt_sequence();
    } else if (count > 0 && state.valid) {
        if (phase == WAIT_NEUTRAL && !held) {
            phase = WAIT_PRESS;
        } else if ((phase == WAIT_PRESS && held) || (phase == WAIT_RELEASE && !held)) {
            phase = held ? WAIT_RELEASE : COMPLETE;
            say("PS5 DualSense input: mapped A %s buttons=%#llx sticks=%d,%d/%d,%d",
                held ? "pressed" : "released", (unsigned long long)state.buttons,
                state.lx, state.ly, state.rx, state.ry);
        }
    }
    return held;
}

void mkw_pad_probe_finish(void) {
    say("PS5 DualSense input: %s", phase == COMPLETE ? "passed" : "failed");
}
