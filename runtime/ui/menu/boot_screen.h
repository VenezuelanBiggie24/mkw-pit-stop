// The boot screen (ui/menu/boot_screen.cpp): a Pit Stop styled loading page shown from the start of rt_boot until the
// game presents its first image, with what the boot is doing (game code, shaders, the RomFS index) and how far it is.
// On by default on the PS5, where the boot took ~35 s of black screen (RomFS index 29-31 s, 15-16/11/2026);
// MKW_BOOT_SCREEN=0 turns it off, 1 turns it on elsewhere.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum { BOOT_TASK_CODE, BOOT_TASK_SHADERS, BOOT_TASK_FILES, BOOT_TASK_GAME, BOOT_TASK_COUNT };
enum { BOOT_TASK_WAITING, BOOT_TASK_RUNNING, BOOT_TASK_DONE };

void boot_screen_start(void);                 // rt_boot, before the work it shows; starts the present thread
void boot_screen_task(int task, int state);   // any thread
int boot_screen_active(void);                 // 1 from boot_screen_start until the game's first present
// The present path (gfx_present.c), present lock held, at the game's first present: no boot screen frame after it.
void boot_screen_game_presenting(void);

#ifdef __cplusplus
}
#endif
