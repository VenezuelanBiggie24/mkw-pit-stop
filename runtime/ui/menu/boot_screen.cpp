// The boot screen: a Pit Stop styled loading page (pit_loading_frame) from the start of rt_boot until the game's first
// present. A thread presents it at the display's rate through the same ImGui overlay the PS5 placeholder uses
// (menu_set_placeholder, gfx_window_boot_present); the game's first present (gfx_present.c) ends it under the present
// lock, so no boot frame lands after a game frame. Its presents do not count as game frames (gfx_counters.frames).
#include "ui/menu/boot_screen.h"

#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "ui/menu/pit_stop.h"

extern "C" {
#include "cfg.h"
#include "fs.h"
#include "gfx/nvn/host/gfx.h"
#include "platform/platform.h"
#include "ui/menu/menu.h"
#include "ui_text.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

static std::atomic<int> g_task[BOOT_TASK_COUNT];
static std::atomic<int> g_active, g_stop;
static uint64_t g_t0;

// 205660 -> "205,660": the count reads at a glance while it climbs
static void thousands(uint64_t v, char *out, size_t cap) {
    char digits[32];
    int n = snprintf(digits, sizeof digits, "%llu", (unsigned long long)v), k = 0;
    for (int i = 0; i < n && (size_t)k + 2 < cap; i++) {
        if (i && (n - i) % 3 == 0) out[k++] = ',';
        out[k++] = digits[i];
    }
    out[k] = 0;
}

static void draw_boot(float width, float height, void *) {
    static const UiTextId labels[BOOT_TASK_COUNT] = {UI_BOOT_CODE, UI_BOOT_SHADERS, UI_BOOT_FILES, UI_BOOT_GAME};
    PitLoadingRow rows[BOOT_TASK_COUNT];
    char files[64] = "", count[32];
    int done = 0;
    uint64_t found = romfs_index_progress(&done);
    for (int i = 0; i < BOOT_TASK_COUNT; i++) {
        int st = g_task[i].load(std::memory_order_relaxed);
        if (i == BOOT_TASK_FILES && done) st = BOOT_TASK_DONE;
        const char *status = menu_text(st == BOOT_TASK_DONE ? UI_BOOT_READY : st == BOOT_TASK_RUNNING ? UI_BOOT_WORKING
                                                                                                      : UI_BOOT_WAITING);
        if (i == BOOT_TASK_FILES && found) {
            thousands(found, count, sizeof count);
            snprintf(files, sizeof files, menu_text(UI_BOOT_FILES_COUNT), count);
            status = files;
        }
        rows[i] = {menu_text(labels[i]), status, st, -1.0f};
    }
    char foot[160];
    snprintf(foot, sizeof foot, menu_text(UI_BOOT_FOOT), (int)((plat_now_ns() - g_t0) / 1000000000ull));
    // players did not know the overlay was there: say how to open it while they wait (the PS5's chord, input.c
    // input_menu_chord: Options with the touchpad click); the desktop's F1 is on the launcher already
#ifdef MKW_PS5_NATIVE
    const char *hint = menu_text(UI_BOOT_MENU_HINT);
#else
    const char *hint = nullptr;
#endif
    pit_loading_frame(width, height, menu_text(UI_BOOT_TITLE), rows, BOOT_TASK_COUNT, foot, hint);
}

static void boot_main(void *) {
    uint64_t frames = 0;
    while (!g_stop.load(std::memory_order_acquire) && gfx_window_boot_present()) {
        frames++;
        plat_sleep_ns(16000000ull);   // a FIFO present already waits for the display; this caps it where it does not
    }
    rt_log("boot screen: %llu frames over %.1f s", (unsigned long long)frames, (plat_now_ns() - g_t0) / 1e9);
}

extern "C" void boot_screen_start(void) {
    // MKW_BOOT_SCREEN (diagnostic knob, cfg_get): 0 off, 1 on; unset, on for the PS5 build only
    const char *v = cfg_get("MKW_BOOT_SCREEN");
#ifdef MKW_PS5_NATIVE
    int on = !v || strcmp(v, "0");
#else
    int on = v && strcmp(v, "0");
#endif
    const char *gpu = cfg_get("MKW_GPU");
    if (!on || (gpu && !strcmp(gpu, "null")) || g_active.load()) return;
    g_t0 = plat_now_ns();
    menu_set_placeholder(draw_boot, nullptr);
    g_active.store(1, std::memory_order_release);
    if (!plat_thread_new(boot_main, nullptr, "boot screen")) {
        g_active.store(0);
        menu_set_placeholder(nullptr, nullptr);
    }
}

extern "C" void boot_screen_task(int task, int state) {
    if (task >= 0 && task < BOOT_TASK_COUNT) g_task[task].store(state, std::memory_order_relaxed);
}

extern "C" int boot_screen_active(void) { return g_active.load(std::memory_order_acquire); }

extern "C" void boot_screen_game_presenting(void) {
    if (!g_active.exchange(0)) return;
    g_stop.store(1, std::memory_order_release);
    menu_set_placeholder(nullptr, nullptr);   // before this present lays out its overlay
}
