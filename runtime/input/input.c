// Host input for nn::hid (runtime/ov_input.c): an SDL3 gamepad (no SDL window needed), the keyboard and the mouse,
// merged into Switch npad button bits and stick axes. Button order follows libnx HidNpadButton (ISC). The key and pad
// bindings are the tables in input_map.c (mkw.ini overrides them). The keyboard and the mouse come from the window
// system's back end (input_wsys.h: input_sdl.c), which also owns the cursor capture.
// Keys and their map: docs/controls.md.
// The gamepad also rumbles (input_rumble) and, when it has a gyro and an accelerometer, feeds the six-axis sensor from a
// thread of its own (input_motion_start). Every SDL call on the gamepad is made under `lock`.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifndef MKW_PS5_NATIVE
#include <SDL3/SDL.h>
#else
#include <stdbool.h>
#include "ps5_input.h"
#ifndef SDL_STANDARD_GRAVITY
#define SDL_STANDARD_GRAVITY 9.80665f
#endif
#endif

#include "cfg.h"
#include "hacks.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "input_script.h"
#include "input_wsys.h"
#include "switch2.h"
#include "nvn_api.h"
#include "ui/menu/menu.h"
#include "vk.h"
#include "platform/platform.h"

enum { STICK_MAX = 32767, STICK_DIR = 16384, TRIGGER_ON = 16384, RESCAN_MS = 1000 };

static PlatMutex lock = PLAT_MUTEX_INIT;
static int state;                    // 0 not started, 1 running, -1 disabled or SDL failed
#ifndef MKW_PS5_NATIVE
static SDL_Gamepad *pad;
static uint64_t next_scan;
static SDL_JoystickID sensor_pad[8]; // by player: the gamepad whose sensors the motion thread set up, 0 none
static int sensor_ok[8];             // ... and it has both sensors, enabled
#endif
static int pad_rumble;               // the open gamepad has rumble motors
static volatile int32_t pad_labels[4];  // ... and the labels of its south, east, west, north buttons (PAD_LABEL_*), any thread
static int pad_east_a;               // ... and labels its east face button A (Nintendo layout under MKW_PAD_LAYOUT=auto)
static int pad_layout = -1;          // PAD_LAYOUT_* in effect for the open gamepad, -1 to pick again; live (cfg_version)
static unsigned pad_layout_seen;
static volatile int32_t pad_layout_shown = -1;   // pad_layout for the menu (any thread), -1 without a gamepad
static volatile int32_t last_device;           // INPUT_DEVICE_* the player used last, 0 none yet (input_last_device)
static int motion_thread;            // 0 not started, 1 running, -1 off (MKW_MOTION=0, no SDL)
static uint32_t menu_pad_generation;  // under lock
static volatile int32_t g_menu;        // open (1) or closed and waiting for a neutral pad (2)

#ifndef MKW_PS5_NATIVE
// SDL buttons of the gamepad inputs (input_map.h PAD_*), by position; PAD_ZL and PAD_ZR are the triggers (axes).
static const SDL_GamepadButton pad_sdl[PAD_COUNT] = {
    [PAD_SOUTH] = SDL_GAMEPAD_BUTTON_SOUTH, [PAD_EAST] = SDL_GAMEPAD_BUTTON_EAST, [PAD_WEST] = SDL_GAMEPAD_BUTTON_WEST,
    [PAD_NORTH] = SDL_GAMEPAD_BUTTON_NORTH, [PAD_LSTICK] = SDL_GAMEPAD_BUTTON_LEFT_STICK,
    [PAD_RSTICK] = SDL_GAMEPAD_BUTTON_RIGHT_STICK, [PAD_L] = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    [PAD_R] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, [PAD_START] = SDL_GAMEPAD_BUTTON_START, [PAD_BACK] = SDL_GAMEPAD_BUTTON_BACK,
    [PAD_DPAD_LEFT] = SDL_GAMEPAD_BUTTON_DPAD_LEFT, [PAD_DPAD_UP] = SDL_GAMEPAD_BUTTON_DPAD_UP,
    [PAD_DPAD_RIGHT] = SDL_GAMEPAD_BUTTON_DPAD_RIGHT, [PAD_DPAD_DOWN] = SDL_GAMEPAD_BUTTON_DPAD_DOWN,
};
#else
// Consumed from PS5_VulkanTemplate/ps5/src/platform.c without colliding with runtime/platform/platform.h.
enum {
    PS5_PAD_CREATE = 0x000001, PS5_PAD_L3 = 0x000002, PS5_PAD_R3 = 0x000004, PS5_PAD_OPTIONS = 0x000008,
    PS5_PAD_UP = 0x000010, PS5_PAD_RIGHT = 0x000020, PS5_PAD_DOWN = 0x000040, PS5_PAD_LEFT = 0x000080,
    PS5_PAD_L1 = 0x000400, PS5_PAD_R1 = 0x000800,
    PS5_PAD_TRIANGLE = 0x001000, PS5_PAD_CIRCLE = 0x002000, PS5_PAD_CROSS = 0x004000, PS5_PAD_SQUARE = 0x008000,
    PS5_PAD_TOUCH_PAD = 0x100000, PS5_PAD_INTERCEPTED = 0x80000000u, PS5_PAD_PLAYERS = 4,
};
struct pad {
    uint32_t held, pressed;
    float left_x, left_y, right_x, right_y, l2, r2;
};
static Ps5PadState ps5_pads[PS5_PAD_PLAYERS];
struct pad_reading {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y, l2, r2;
    bool connected;
    uint64_t timestamp_us;
};
extern bool pad_open(void);
extern void pad_poll(struct pad *pad);
extern uint32_t pad_players(void);
extern int pad_player_readings(int player, const struct pad_reading **readings);
extern void pad_player_vibrate(int player, float large, float small);
#endif

static float mouse_sens = -1;              // MKW_MOUSE_SENS, 0 = mouse camera off; live (cfg_version)
static unsigned mouse_sens_seen;

static float mouse_sens_get(void) {
    unsigned ver = cfg_version();
    if (mouse_sens >= 0 && ver == mouse_sens_seen) return mouse_sens;
    mouse_sens_seen = ver;
    double v = cfg_double("MKW_MOUSE_SENS");
    mouse_sens = (float)(v < 0 ? 0 : v > 10 ? 10 : v);
    return mouse_sens;
}


// The mouse camera as a right-stick sample, held for the whole presented frame. Under `lock`.
static void mouse_camera(int32_t out[2]) {
    static uint64_t frame = ~0ull, t0;
    static int32_t held[2];
    uint64_t now = plat_now_ns();
    int32_t dx, dy;
    if (!ikb_captured()) {
        ikb_mouse_take(&dx, &dy);
        held[0] = held[1] = 0;
        frame = ~0ull;
    } else {
        uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        double age = frame == ~0ull ? 0 : (double)(now - t0) / 1e9;
        if (fr != frame) {
            double dt = age < 0.004 ? 0.004 : age > 0.1 ? 0.1 : age;
            ikb_mouse_take(&dx, &dy);
            input_mouse_stick(dx, dy, dt, mouse_sens_get(), held);
            frame = fr;
            t0 = now;
        } else if (age > 0.1) {
            held[0] = held[1] = 0;   // frames stopped: do not hold a deflection
        }
    }
    out[0] = held[0];
    out[1] = held[1];
}

#ifndef MKW_PS5_NATIVE
static float dead_zone = -1;               // MKW_STICK_DEADZONE as a fraction of full tilt; live (cfg_version)
static unsigned dead_zone_seen;

static float dead_zone_get(void) {
    unsigned ver = cfg_version();
    if (dead_zone >= 0 && ver == dead_zone_seen) return dead_zone;
    dead_zone_seen = ver;
    double v = cfg_double("MKW_STICK_DEADZONE");
    dead_zone = (float)(v < 0 ? 0 : v > 40 ? 0.4 : v / 100);
    return dead_zone;
}

// A stick's two axes (SDL's, y down) as the game's (y up): a round dead zone around the centre, and the tilt past it
// rescaled to the whole range, so a worn stick that rests off centre stays still without losing the small moves just
// outside it (the old one was per axis, 2500 of 32767, and jumped from 0 to 2500).
static void stick(int16_t x, int16_t y, int32_t out[2]) {
    float fx = (x < -STICK_MAX ? -STICK_MAX : x) / (float)STICK_MAX;
    float fy = (y < -STICK_MAX ? -STICK_MAX : y) / (float)STICK_MAX;
    float m = sqrtf(fx * fx + fy * fy), d = dead_zone_get();
    if (m <= d) {
        out[0] = out[1] = 0;
        return;
    }
    float s = ((m > 1 ? 1 : m) - d) / (1 - d) / m;
    out[0] = (int32_t)lrintf(fx * s * STICK_MAX);
    out[1] = -(int32_t)lrintf(fy * s * STICK_MAX);
}

// Players 2-8 (input_poll_player): one gamepad each, given in connection order to the first player without one; player
// 1's is `pad`. A player keeps the slot while the gamepad stays connected. Under lock.
static SDL_Gamepad *extra[INPUT_MAX_PLAYERS];   // [0] unused
static int extra_rumble[INPUT_MAX_PLAYERS], extra_east_a[INPUT_MAX_PLAYERS];

static int pad_is_open(SDL_JoystickID id) {
    if (pad && SDL_GetGamepadID(pad) == id) return 1;
    for (int p = 1; p < INPUT_MAX_PLAYERS; p++)
        if (extra[p] && SDL_GetGamepadID(extra[p]) == id) return 1;
    return 0;
}

static void pad_adopt(void);

// Close the gamepads that went away and open the new ones (player 1 first, then the first free player); logs each change.
static void scan_pads(void) {
    if (pad && !SDL_GamepadConnected(pad)) {
        SDL_CloseGamepad(pad); pad = NULL; sensor_pad[0] = 0; sensor_ok[0] = 0;
        plat_xchg32(&pad_layout_shown, -1);
        for (int i = 0; i < 4; i++) plat_xchg32(&pad_labels[i], PAD_LABEL_NONE);
        rt_log("input: gamepad disconnected");
    }
    for (int p = 1; p < INPUT_MAX_PLAYERS; p++)
        if (extra[p] && !SDL_GamepadConnected(extra[p])) {
            SDL_CloseGamepad(extra[p]); extra[p] = NULL; sensor_pad[p] = 0; sensor_ok[p] = 0;
            rt_log("input: gamepad of player %d disconnected", p + 1);
        }
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) {
        if (pad_is_open(ids[i])) continue;
        int p = 0;
        while (p < INPUT_MAX_PLAYERS && (p ? extra[p] != NULL : pad != NULL)) p++;
        if (p == INPUT_MAX_PLAYERS) break;
        SDL_Gamepad *g = SDL_OpenGamepad(ids[i]);
        if (!g) continue;
        if (!p) { pad = g; pad_adopt(); continue; }
        extra[p] = g;
        extra_rumble[p] = SDL_GetBooleanProperty(SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        extra_east_a[p] = SDL_GetGamepadButtonLabel(g, SDL_GAMEPAD_BUTTON_EAST) == SDL_GAMEPAD_BUTTON_LABEL_A;
        rt_log("input: gamepad \"%s\" for player %d (%s)", SDL_GetGamepadName(g), p + 1,
               extra_rumble[p] ? "rumble" : "no rumble");
    }
    SDL_free(ids);
}

// Player 1's new gamepad: its rumble, button labels and layout.
static void pad_adopt(void) {
    menu_pad_generation++;
    pad_rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    pad_east_a = SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_EAST) == SDL_GAMEPAD_BUTTON_LABEL_A;
    for (int i = 0; i < 4; i++) {
        SDL_GamepadButtonLabel lb = SDL_GetGamepadButtonLabel(pad, pad_sdl[PAD_SOUTH + i]);
        plat_xchg32(&pad_labels[i], lb == SDL_GAMEPAD_BUTTON_LABEL_A          ? PAD_LABEL_A
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_B        ? PAD_LABEL_B
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_X        ? PAD_LABEL_X
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_Y        ? PAD_LABEL_Y
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_CROSS    ? PAD_LABEL_CROSS
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_CIRCLE   ? PAD_LABEL_CIRCLE
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_SQUARE   ? PAD_LABEL_SQUARE
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE ? PAD_LABEL_TRIANGLE
                                                                                      : PAD_LABEL_NONE);
    }
    pad_layout = -1;
    int motion = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL);
    rt_log("input: gamepad \"%s\" (%s, %s, east button %s)", SDL_GetGamepadName(pad), pad_rumble ? "rumble" : "no rumble",
           motion ? "gyro and accelerometer" : "no motion sensors", pad_east_a ? "labelled A" : "not labelled A");
}
#endif

// Under lock: the layout of the open gamepad, picked again when a setting or the gamepad changed; logs each change.
static int pad_layout_get(void) {
    unsigned ver = cfg_version();
    if (pad_layout >= 0 && ver == pad_layout_seen) return pad_layout;
    pad_layout_seen = ver;
    int was = pad_layout;
    const char *set = cfg_value("MKW_PAD_LAYOUT");
    pad_layout = input_pad_layout_pick(set, pad_east_a);
    if (pad_layout != was)
        rt_log("input: gamepad layout %s (pad_layout = %s)", pad_layout == PAD_LAYOUT_XBOX ? "xbox" : "nintendo", set);
    plat_xchg32(&pad_layout_shown, pad_layout);
    return pad_layout;
}

// The MKW_BIND_* values last applied, one per line; the tables are built again when one changes (the menu's remapping rows write
// them while the game runs). Under lock.
static unsigned binds_seen = ~0u;
static char binds_applied[4096];

static void binds_refresh(void) {
    unsigned v = cfg_version();
    if (v == binds_seen) return;
    binds_seen = v;
    char now[sizeof binds_applied];
    size_t len = 0;
    for (int a = 0; a < ACTION_COUNT; a++) {
        char var[32];
        snprintf(var, sizeof var, "MKW_BIND_%s", input_action_name(a));
        const char *s = cfg_get(var);
        len += (size_t)snprintf(now + len, len < sizeof now ? sizeof now - len : 0, "%s\n", s ? s : "");
    }
    if (!strcmp(now, binds_applied)) return;
    memcpy(binds_applied, now, sizeof now);
    input_map_configure();
}

#ifndef MKW_PS5_NATIVE
// MKW_VIRTUAL_PADS=N (1-8): N virtual SDL gamepads, opened like real ones after them, to try local multiplayer without
// controllers. They have no input of their own: the script's "p<N>" lines press for their players.
static void attach_virtual_pads(void) {
    const char *e = cfg_get("MKW_VIRTUAL_PADS");
    int n = e ? atoi(e) : 0;
    if (n <= 0) return;
    if (n > INPUT_MAX_PLAYERS) n = INPUT_MAX_PLAYERS;
    for (int i = 0; i < n; i++) {
        SDL_VirtualJoystickDesc d;
        SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        d.naxes = SDL_GAMEPAD_AXIS_COUNT;
        d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        d.name = "MKW virtual gamepad";
        if (!SDL_AttachVirtualJoystick(&d)) rt_fatal("input: MKW_VIRTUAL_PADS: cannot attach gamepad %d (%s)", i + 1, SDL_GetError());
    }
    rt_log("input: %d virtual gamepads (MKW_VIRTUAL_PADS)", n);
}

static void start(void) {
    binds_refresh();
    const char *e = cfg_get("MKW_INPUT");
    if (e && !strcmp(e, "none")) { state = -1; rt_log("input: disabled (MKW_INPUT=none)"); return; }
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { state = -1; rt_log("input: SDL gamepad init failed (%s), keyboard only", SDL_GetError()); }
    else state = 1;
    if (state == 1) {
        attach_virtual_pads();
        switch2_start();   // Switch 2 controllers over Bluetooth, as virtual gamepads (MKW_SWITCH2)
        scan_pads();
        if (!pad) rt_log("input: no gamepad (keyboard only until one is connected)");
    }
    if (state == -1) state = 2;   // keyboard only
}
#else
static void start(void) {
    binds_refresh();
    const char *e = cfg_get("MKW_INPUT");
    if (e && !strcmp(e, "none")) { state = -1; rt_log("input: disabled (MKW_INPUT=none)"); return; }
    if (!pad_open()) {
        state = 2;
        rt_log("input: DualSense pad_open failed");
        return;
    }
    pad_rumble = 1;
    pad_east_a = 1;    // PS5 gameplay Auto follows the Switch positions

    plat_xchg32(&pad_labels[0], PAD_LABEL_CROSS);
    plat_xchg32(&pad_labels[1], PAD_LABEL_CIRCLE);
    plat_xchg32(&pad_labels[2], PAD_LABEL_SQUARE);
    plat_xchg32(&pad_labels[3], PAD_LABEL_TRIANGLE);
    (void)pad_layout_get();
    state = 1;
}
#endif

static void stick_bits(uint64_t *b, int32_t x, int32_t y, int first) {
    if (x < -STICK_DIR) *b |= 1ull << (first + 0);
    if (y > STICK_DIR) *b |= 1ull << (first + 1);
    if (x > STICK_DIR) *b |= 1ull << (first + 2);
    if (y < -STICK_DIR) *b |= 1ull << (first + 3);
}

static void pick(int32_t *x, int32_t *y, int32_t kx, int32_t ky) {
    int64_t a = (int64_t)*x * *x + (int64_t)*y * *y, b = (int64_t)kx * kx + (int64_t)ky * ky;
    if (b > a) { *x = kx; *y = ky; }
}

#ifndef MKW_PS5_NATIVE
// Under lock, state 1: look for new or lost gamepads once a second, then read every open one.
static void pads_update(void) {
    uint64_t now_ms = plat_now_ns() / 1000000u;
    if (now_ms >= next_scan) { next_scan = now_ms + RESCAN_MS; scan_pads(); }
    SDL_UpdateGamepads();
}

// A gamepad's inputs by position (pd[PAD_*] != 0 held).
static void gamepad_physical(SDL_Gamepad *g, uint8_t pd[PAD_COUNT]) {
    for (int p = PAD_SOUTH; p < PAD_ZL; p++) pd[p] = SDL_GetGamepadButton(g, pad_sdl[p]);
    pd[PAD_ZL] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_ON;
    pd[PAD_ZR] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_ON;
}

static void gamepad_read(SDL_Gamepad *g, uint8_t pd[PAD_COUNT], int32_t sl[2], int32_t sr[2]) {
    gamepad_physical(g, pd);
    stick(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY), sl);
    stick(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX), SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY), sr);
}

static int pad_read(uint8_t pd[PAD_COUNT], int32_t sl[2], int32_t sr[2], int *back, int *touchpad) {
    pads_update();
    if (!pad || !SDL_GamepadConnected(pad)) return 0;
    gamepad_read(pad, pd, sl, sr);
    *touchpad = input_pad_label(PAD_SOUTH) == PAD_LABEL_CROSS && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_TOUCHPAD);
    *back = pd[PAD_BACK] || *touchpad;
    return 1;
}
#else
static Ps5PadSample ps5_sample_of(const struct pad_reading *raw) {
    const uint32_t held = raw->buttons;
    return (Ps5PadSample){
        .down = {
            [PAD_SOUTH] = !!(held & PS5_PAD_CROSS), [PAD_EAST] = !!(held & PS5_PAD_CIRCLE),
            [PAD_WEST] = !!(held & PS5_PAD_SQUARE), [PAD_NORTH] = !!(held & PS5_PAD_TRIANGLE),
            [PAD_LSTICK] = !!(held & PS5_PAD_L3), [PAD_RSTICK] = !!(held & PS5_PAD_R3),
            [PAD_L] = !!(held & PS5_PAD_L1), [PAD_R] = !!(held & PS5_PAD_R1),
            [PAD_START] = !!(held & PS5_PAD_OPTIONS),
            [PAD_BACK] = !!(held & (PS5_PAD_CREATE | PS5_PAD_TOUCH_PAD)),
            [PAD_DPAD_LEFT] = !!(held & PS5_PAD_LEFT), [PAD_DPAD_UP] = !!(held & PS5_PAD_UP),
            [PAD_DPAD_RIGHT] = !!(held & PS5_PAD_RIGHT), [PAD_DPAD_DOWN] = !!(held & PS5_PAD_DOWN),
        },
        .lx = raw->left_x, .ly = raw->left_y, .rx = raw->right_x, .ry = raw->right_y,
        .l2 = raw->l2, .r2 = raw->r2, .intercepted = !!(held & PS5_PAD_INTERCEPTED),
    };
}

static void pads_update(void) {
    struct pad ignored = {0};
    pad_poll(&ignored);
    uint32_t connected = pad_players();
    for (int p = 0; p < PS5_PAD_PLAYERS; ++p) {
        const struct pad_reading *readings;
        int count = pad_player_readings(p, &readings);
        Ps5PadState *sample = &ps5_pads[p];
        bool was_valid = sample->valid;
        bool interrupted = false;
        for (int i = 0; p == 0 && i < count - 1; ++i) {
            if (!readings[i].connected || (readings[i].buttons & PS5_PAD_INTERCEPTED)) {
                interrupted = true;
                break;
            }
        }
        if (count > 0) {
            const struct pad_reading *latest = &readings[count - 1];
            Ps5PadSample raw = ps5_sample_of(latest);
            input_ps5_update(sample, (connected & (1u << p)) && latest->connected, &raw);
        } else {
            input_ps5_update(sample, (connected & (1u << p)) != 0, NULL);
        }
        if (p == 0 && (interrupted || was_valid != sample->valid)) menu_pad_generation++;
    }
}

static int ps5_player_read(int player, uint8_t pd[PAD_COUNT], int32_t sl[2], int32_t sr[2]) {
    if (player < 0 || player >= PS5_PAD_PLAYERS || !ps5_pads[player].valid) return 0;
    const Ps5PadState *ps = &ps5_pads[player];
    memcpy(pd, ps->down, PAD_COUNT);
    sl[0] = ps->lx; sl[1] = ps->ly;
    sr[0] = ps->rx; sr[1] = ps->ry;
    return 1;
}

static int pad_read(uint8_t pd[PAD_COUNT], int32_t sl[2], int32_t sr[2], int *back, int *touchpad) {
    pads_update();
    int ok = ps5_player_read(0, pd, sl, sr);
    *back = *touchpad = pd[PAD_BACK];
    return ok;
}
#endif

static int poll_merged(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    uint64_t b = 0;
    int32_t sl[2] = {0, 0}, sr[2] = {0, 0};
    int toggle = 0, physical_held = 0;
    plat_mutex_lock(&lock);
    if (!state) start();
    if (state > 0) {
        binds_refresh();
        uint64_t pb = 0;
        int32_t pl[2] = {0, 0}, pr[2] = {0, 0};
        uint8_t pd[PAD_COUNT] = {0};
        int back = 0, touchpad = 0;
        if (state == 1) {
            pad_read(pd, pl, pr, &back, &touchpad);
            static int chord_held;
            toggle = input_menu_chord(&chord_held, pd[PAD_START] | (back << 1));
            static const uint8_t neutral[PAD_COUNT];
            physical_held = memcmp(pd, neutral, PAD_COUNT) != 0 || touchpad ||
                            abs(pl[0]) > 11468 || abs(pl[1]) > 11468 ||
                            abs(pr[0]) > 11468 || abs(pr[1]) > 11468;
            if (chord_held) pd[PAD_START] = pd[PAD_BACK] = 0;
            b = pb = input_map_pad_buttons(pd, pad_layout_get());
            sl[0] = pl[0], sl[1] = pl[1], sr[0] = pr[0], sr[1] = pr[1];
        }
        uint64_t kb = 0;
        int keyboard_stick = 0;
        if (ikb_foreground()) {
            uint8_t down[KEY_COUNT] = {0};
            // Mouse buttons count only while the cursor is captured, and a button already held when the capture began
            // (the click that refocused the window) is ignored until it is released.
            static int was_captured;
            static uint8_t mouse_ignore[KEY_COUNT];
            int cap = ikb_captured();
            for (int k = 1; k < KEY_COUNT; k++) {
                down[k] = ikb_down(k);
                if (k < KEY_MOUSE_L || k > KEY_MOUSE_X2) continue;
                if (cap && !was_captured) mouse_ignore[k] = down[k];
                if (!down[k]) mouse_ignore[k] = 0;
                if (!cap || mouse_ignore[k]) down[k] = 0;
            }
            was_captured = cap;
            kb = input_map_buttons(down);
            int32_t k[2];
            input_map_stick(down, 0, k);
            keyboard_stick = k[0] || k[1];
            pick(&sl[0], &sl[1], k[0], k[1]);
            input_map_stick(down, 1, k);
            pick(&sr[0], &sr[1], k[0], k[1]);
            mouse_camera(k);
            pick(&sr[0], &sr[1], k[0], k[1]);
        }
        static int key_chord;
        toggle |= input_menu_chord(&key_chord, (unsigned)(kb >> B_PLUS) & 3u);
        int keyboard_moved = kb != 0 || keyboard_stick;
        if (key_chord) kb &= ~(3ull << B_PLUS);
        b |= kb;
        // the device last used, for the button icons (ui/glyphs/glyphs.c): a press, or a stick past a third of its travel
        int pad_moved = physical_held || pb || abs(pl[0]) > 11000 || abs(pl[1]) > 11000 ||
                        abs(pr[0]) > 11000 || abs(pr[1]) > 11000;
        if (keyboard_moved && !pad_moved) plat_xchg32(&last_device, INPUT_DEVICE_KEYBOARD);
        else if (pad_moved && !keyboard_moved) plat_xchg32(&last_device, INPUT_DEVICE_PAD);
    }
    script_apply(&b, sl, sr, 0);
    skip_input(&b);
    rec_apply(b, sl, sr);
    if (toggle || menu_is_open()) {
        b = 0;
        sl[0] = sl[1] = sr[0] = sr[1] = 0;
    }
    static int overlay_held;   // L3+R3 step the performance overlay (F10)
    int overlay = input_overlay_gate(&overlay_held, &b);
    plat_mutex_unlock(&lock);
    if (toggle) menu_toggle();
    if (overlay) menu_perf_cycle();
    stick_bits(&b, sl[0], sl[1], B_SL_LEFT);
    stick_bits(&b, sr[0], sr[1], B_SR_LEFT);
    *buttons = b; *lx = sl[0]; *ly = sl[1]; *rx = sr[0]; *ry = sr[1];
    return physical_held;
}

// Text entry (swkbd_*.c): while it is open the game reads no input, as behind the keyboard applet; after it closes
// the game reads none until every button and stick direction is released, so the press that closed it stays there.
// g_block: 0 off, 1 text entry open, 2 closed, waiting for the release. The cursor is released while it is open.
static volatile int32_t g_block;

int input_cursor_allowed(void) {
    const char *in = cfg_get("MKW_INPUT");
    return g_block != 1 && g_menu != 1 && mouse_sens_get() > 0 && !(in && !strcmp(in, "none"));
}

void input_block(int on) {
    plat_xchg32(&g_block, on ? 1 : 2);
    ikb_cursor_update();
}

// The in-game menu (ui/menu/menu.cpp) blocks the game's input the same way: g_menu 0 closed, 1 open, 2 closed, waiting for
// the release. The cursor is released while it is open, for the menu's mouse.
void input_menu(int open) {
    plat_xchg32(&g_menu, open ? 1 : 2);
    if (!open) ikb_menu_closed();   // the capture before Esc comes back
    ikb_cursor_update();
}

// 1 while flag (g_block, g_menu) blocks the game's input; state 2 ends at the first poll with nothing held.
static int blocked(volatile int32_t *flag, uint64_t buttons) {
    int32_t k = *flag;
    return k && !(k == 2 && !buttons && plat_cas32(flag, 0, 2) == 2);
}

void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    lat_input_read();   // MKW_LATENCY: the first read of each frame
    int physical_held = poll_merged(buttons, lx, ly, rx, ry);
    int text = blocked(&g_block, *buttons), menu = blocked(&g_menu, *buttons || physical_held);
    if (!text && !menu) return;
    *buttons = 0; *lx = *ly = *rx = *ry = 0;
}

void input_poll_ui(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    poll_merged(buttons, lx, ly, rx, ry);
}

uint32_t input_player_mask(void) {
    uint32_t m = 1;
    plat_mutex_lock(&lock);
    if (!state) start();
#ifndef MKW_PS5_NATIVE
    for (int p = 1; state == 1 && p < INPUT_MAX_PLAYERS; p++)
        if (extra[p] && SDL_GamepadConnected(extra[p])) m |= 1u << p;
#else
    if (state == 1) {
        pads_update();
        m |= pad_players();
    }
#endif
    plat_mutex_unlock(&lock);
    return m;
}

// Players 2-8: their gamepad and the script's lines for them. The text entry and the menu block them as they block
// player 1, and only player 1's release ends that block.
void input_poll_player(int player, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    if (player <= 0) { input_poll(buttons, lx, ly, rx, ry); return; }
    uint64_t b = 0;
    int32_t sl[2] = {0, 0}, sr[2] = {0, 0};
    plat_mutex_lock(&lock);
    if (!state) start();
    if (state == 1 && player < INPUT_MAX_PLAYERS) {
        uint8_t pd[PAD_COUNT] = {0};
        binds_refresh();
        pads_update();
#ifndef MKW_PS5_NATIVE
        SDL_Gamepad *g = extra[player];
        if (g && SDL_GamepadConnected(g)) {
            gamepad_read(g, pd, sl, sr);
            b = input_map_pad_buttons(pd, input_pad_layout_pick(cfg_value("MKW_PAD_LAYOUT"), extra_east_a[player]));
        }
#else
        if (ps5_player_read(player, pd, sl, sr)) b = input_map_pad_buttons(pd, pad_layout_get());
#endif
    }
    plat_mutex_unlock(&lock);
    script_apply(&b, sl, sr, player);
    stick_bits(&b, sl[0], sl[1], B_SL_LEFT);
    stick_bits(&b, sr[0], sr[1], B_SR_LEFT);
    if (g_block || g_menu) b = 0, sl[0] = sl[1] = sr[0] = sr[1] = 0;
    *buttons = b; *lx = sl[0]; *ly = sl[1]; *rx = sr[0]; *ry = sr[1];
}

void input_menu_pad(InputMenuPad *sample) {
    uint8_t pd[PAD_COUNT] = {0};
    int32_t sl[2] = {0, 0}, sr[2] = {0, 0};
    int back = 0, touchpad = 0;
    plat_mutex_lock(&lock);
    if (!state) start();
    int connected = state == 1 && pad_read(pd, sl, sr, &back, &touchpad);
    if (connected) (void)pad_layout_get();
    uint32_t bits = 0;
    for (int p = PAD_SOUTH; p < PAD_COUNT; ++p)
        bits |= (uint32_t)(pd[p] != 0) << p;
    // Confirm on the button the game reads as A: the east one in the Nintendo layout (MKW_PAD_LAYOUT auto on the PS5,
    // where a DualSense follows the Switch's positions: Circle confirms, Cross goes back), the south one in Xbox's.
    int east_confirm = connected && pad_layout_get() == PAD_LAYOUT_NINTENDO;
    *sample = (InputMenuPad){bits, menu_pad_generation, sl[0], sl[1], connected, east_confirm};
    plat_mutex_unlock(&lock);
}

uint16_t rumble_motor(float a, float curve, float strength) {
    if (!(a > 0) || !(strength > 0)) return 0;
    float v = powf(a > 1 ? 1 : a, curve) * strength * 65535.0f + 0.5f;
    return v >= 65535.0f ? 65535 : (uint16_t)v;
}

int input_last_device(void) {
    int d = (int)plat_cas32(&last_device, 0, 0);
    if (d) return d;
    return (int32_t)plat_cas32(&pad_layout_shown, -1, -1) >= 0 ? INPUT_DEVICE_PAD : INPUT_DEVICE_KEYBOARD;
}

int input_pad_layout(void) {
    return (int)plat_cas32(&pad_layout_shown, 0, 0);
}

int input_pad_label(int pad_id) {
    if (pad_id < PAD_SOUTH || pad_id > PAD_NORTH) return PAD_LABEL_NONE;
    return (int)plat_cas32(&pad_labels[pad_id - PAD_SOUTH], 0, 0);
}

void input_map_snapshot(InputMap *m) {
    plat_mutex_lock(&lock);
    if (!state) start();
    if (state > 0) binds_refresh();
    *m = input_map;
    plat_mutex_unlock(&lock);
}

int input_capture_key(int reset, int *id) {
    static uint8_t prev_k[KEY_COUNT];
    static int prev_esc;
    uint8_t k[KEY_COUNT] = {0};
    int fg = ikb_foreground();
    for (int i = 1; i < KEY_COUNT; i++) k[i] = fg && ikb_down(i);
    int esc = fg && ikb_escape();
    int got = INPUT_CAPTURE_NONE;
    if (!reset) {
        if (esc && !prev_esc) got = INPUT_CAPTURE_CANCEL;
        for (int i = 1; i < KEY_COUNT && !got; i++)
            if (k[i] && !prev_k[i]) got = INPUT_CAPTURE_KEY, *id = i;
    }
    memcpy(prev_k, k, sizeof k);
    prev_esc = esc;
    return got;
}

int input_capture(int reset, int *id) {
    static uint32_t previous, generation;
    InputMenuPad pad;
    input_menu_pad(&pad);
    uint32_t pressed = !reset && generation == pad.generation ? pad.buttons & ~previous : 0;
    previous = pad.buttons;
    generation = pad.generation;
    int got = input_capture_key(reset, id);
    if (!got && pressed) {
        *id = __builtin_ctz(pressed);
        return INPUT_CAPTURE_PAD;
    }
    return got;
}

int input_rumble_triggers_enabled(void) {
    return cfg_bool("MKW_RUMBLE_TRIGGERS");
}

int input_rumble(int player, uint16_t low, uint16_t high, uint32_t ms) {
    int ok = 0;
    static int triggers = -1;
    static unsigned seen;
    if (triggers < 0 || seen != cfg_version()) seen = cfg_version(), triggers = input_rumble_triggers_enabled();
    plat_mutex_lock(&lock);
#ifndef MKW_PS5_NATIVE
    SDL_Gamepad *g = player <= 0 ? pad : player < INPUT_MAX_PLAYERS ? extra[player] : NULL;
    int rumble = player <= 0 ? pad_rumble : player < INPUT_MAX_PLAYERS && extra_rumble[player];
    if (state == 1 && g && rumble) {
        ok = SDL_RumbleGamepad(g, low, high, ms);
        if (triggers) SDL_RumbleGamepadTriggers(g, high, high, ms);   // unsupported pads return false: ignored
    }
#else
    (void)ms;
    (void)triggers;
    if (state == 1 && player >= 0 && player < PS5_PAD_PLAYERS) {
        pad_player_vibrate(player, low / 65535.0f, high / 65535.0f);
        ok = 1;
    }
#endif
    plat_mutex_unlock(&lock);
    return ok;
}

// SDL gamepad sensor axes (SDL_sensor.h, a gamepad held in front of you): x to the right, y up out of the face (lying
// flat face up, +y points at the ceiling), z toward the player (out of the bottom edge). The accelerometer reads the
// push against gravity in m/s^2 (lying flat: y = +9.81); the gyro reads rad/s, counter-clockwise positive (right hand).
// Switch six-axis axes (nn::hid, as MacNativeGames' GCMotion feed and its Pro Controller HID path use them): x to the
// right, y toward the top edge (away from the player), z out of the face; acceleration as gravity reads (lying flat:
// z = -1 G). So Switch x = SDL x, Switch y = -SDL z, Switch z = SDL y: a rotation (right-handed to right-handed), so the
// gyro keeps its sense, while the acceleration is also negated (push -> gravity) and scaled to G.
void input_motion_axes(const float a[3], const float g[3], double acc[3], double gyro[3]) {
    acc[0] = -a[0] / SDL_STANDARD_GRAVITY;
    acc[1] = a[2] / SDL_STANDARD_GRAVITY;
    acc[2] = -a[1] / SDL_STANDARD_GRAVITY;
    gyro[0] = g[0];
    gyro[1] = -g[2];
    gyro[2] = g[1];
}

#ifndef MKW_PS5_NATIVE
// Under lock: (re)enable the sensors of player p's gamepad g when it changed; 1 if it has both and they are on.
static int sensors_ready(int p, SDL_Gamepad *g) {
    SDL_JoystickID id = SDL_GetGamepadID(g);
    if (id == sensor_pad[p]) return sensor_ok[p];
    sensor_pad[p] = id;
    // Nothing drains the SDL event queue: no event per sample (the data still updates, SDL_SendJoystickSensor)
    SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_SENSOR_UPDATE, false);
    sensor_ok[p] = SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL) &&
                   SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true) &&
                   SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, true);
    if (sensor_ok[p]) rt_log("input: player %d motion sensors on (gyro %.0f Hz, accelerometer %.0f Hz)", p + 1,
                             (double)SDL_GetGamepadSensorDataRate(g, SDL_SENSOR_GYRO),
                             (double)SDL_GetGamepadSensorDataRate(g, SDL_SENSOR_ACCEL));
    else rt_log("input: \"%s\" (player %d) has no gyro and accelerometer: its six-axis sensor reads a controller at rest",
                SDL_GetGamepadName(g), p + 1);
    return sensor_ok[p];
}

int input_motion_enabled(void) {
    static int on = -1;
    static unsigned seen;
    unsigned v = cfg_version();
    if (on < 0 || v != seen) {
        seen = v;
        on = cfg_bool("MKW_MOTION");
    }
    return on;
}

// 250 Hz: the gamepads update (reports read, rumble expiry) and each player's newest gyro and accelerometer sample goes
// into that player's fusion. Players 2-8 have their own (#302: player 2's gyro did nothing). Without a gamepad with
// sensors, or with motion off (MKW_MOTION=0, live), nothing is fed and the six-axis sensor reads at rest.
static void motion_main(void *arg) {
    (void)arg;
    _Static_assert(sizeof sensor_pad / sizeof sensor_pad[0] == INPUT_MAX_PLAYERS, "one sensor slot per player");
    for (;;) {
        plat_wait_ns(4000000);
        float a[INPUT_MAX_PLAYERS][3], g[INPUT_MAX_PLAYERS][3];
        int have[INPUT_MAX_PLAYERS] = {0}, updated = 0;
        plat_mutex_lock(&lock);
        for (int p = 0; p < INPUT_MAX_PLAYERS; p++) {
            SDL_Gamepad *gp = p ? extra[p] : pad;
            if (!gp || !SDL_GamepadConnected(gp)) continue;
            if (!updated) SDL_UpdateGamepads(), updated = 1;
            have[p] = sensors_ready(p, gp) && SDL_GetGamepadSensorData(gp, SDL_SENSOR_ACCEL, a[p], 3) &&
                      SDL_GetGamepadSensorData(gp, SDL_SENSOR_GYRO, g[p], 3);
        }
        plat_mutex_unlock(&lock);
        if (!input_motion_enabled()) continue;
        uint64_t now = kern_now_ns();
        for (int p = 0; p < INPUT_MAX_PLAYERS; p++) {
            if (!have[p]) continue;
            double acc[3], gyro[3];
            input_motion_axes(a[p], g[p], acc, gyro);
            motion_feed(motion_player(p), acc, gyro, now);
        }
    }
}

void input_motion_start(void) {
    plat_mutex_lock(&lock);
    if (!state) start();
    if (!motion_thread) {
        if (!input_motion_enabled()) rt_log("input: motion off (MKW_MOTION=0); it can be turned on while the game runs");
        if (state != 1) motion_thread = -1;
        else {
            if (!plat_thread_spawn(motion_main, NULL, "motion", 0)) rt_fatal("input: cannot start the motion thread");
            motion_thread = 1;
        }
    }
    plat_mutex_unlock(&lock);
}
#else
int input_motion_enabled(void) {
    static int on = -1;
    static unsigned seen;
    unsigned v = cfg_version();
    if (on < 0 || v != seen) {
        seen = v;
        on = cfg_bool("MKW_MOTION");
    }
    return on;
}

void input_motion_start(void) {
    plat_mutex_lock(&lock);
    if (!state) start();
    motion_thread = -1;
    plat_mutex_unlock(&lock);
}
#endif
