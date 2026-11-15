// The remapping rows of the menu's Controls tab (ui/menu/binds_page.cpp). Present path only (ImGui).
#pragma once
#include <stddef.h>

// Inside the page's opt_begin / opt_end: every action's two keys and its controller button, remapped by selecting a
// cell and pressing the new one, and the reset of them all. binds_after_page, after opt_end: the window that waits for
// the press.
void binds_rows(float scale);
void binds_after_page(void);
// A cell waits for a press (or its press is still reaching the menu): B, Esc, L / R and the tab keys stay on the page.
bool binds_modal(void);

// Pit Stop lists one controller row per action, or the two keyboard/mouse slots on desktop.
bool binds_pit_rows(bool keyboard, int icon_kind);
bool binds_pit_active(void);
bool binds_pit_keyboard(void);
void binds_pit_cancel(void);
void binds_pit_apply(int input);
void binds_pit_title(char *text, size_t size);
void binds_reset(void);
