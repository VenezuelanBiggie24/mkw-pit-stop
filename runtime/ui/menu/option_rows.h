// Settings pages in two panes (ui/menu/option_rows.cpp): option rows on the left, the selected (controller) or hovered (mouse)
// row's description on the right. Each row shows a setting of the cfg table (cfg.h) and changes it through cfg_set: A or
// a click steps it (toggle, next value), left and right step it. A row marks the default value, says "after restart" when
// the setting is read at start, and is greyed out with a note when an environment variable fixes it.
//
//   opt_begin("##screen", scale);
//   opt_toggle("MKW_FULLSCREEN", UI_MENU_FULLSCREEN, UI_MENU_FULLSCREEN_DESC);
//   opt_end();
//
// key: the setting's MKW_ name, which must have a cfg_meta row. Present path only (ImGui).
#pragma once
#include "imgui.h"

extern "C" {
#include "ui_text.h"
}

void opt_begin(const char *id, float scale);
void opt_end(void);
// The height the next pages leave free under their panes (the menu's bottom row).
void opt_reserve_bottom(float h);
// The next page's first row takes the controller's cursor (opening the menu, switching tabs).
void opt_focus_first(void);
// The row of key (or button id) takes the cursor, scrolled into view, when it is next drawn.
void opt_focus_key(const char *key);
// The next row is greyed out like one the environment fixes, with note under its label and in its description.
void opt_disable_next(UiTextId note);

// Page text that is not a row: a group heading in the accent colour, a wrapped paragraph (warn: the warning colour).
void opt_group(UiTextId title);
void opt_text(UiTextId text, bool warn = false);

// The return value of every row: true when it changed the setting this frame.
// inverted: the row shows "on" while the key is 0 (a setting whose key names the opposite).
bool opt_toggle(const char *key, UiTextId label, UiTextId desc, bool inverted = false);
// One name per value of the key's cfg_meta values list, in its order. When the key has no fixed default (an empty
// def), names[0] is the automatic choice (the key unset) and names[1..] follow the values. note: a short line under
// the value in place of the "default" mark (NULL: none).
bool opt_choice(const char *key, UiTextId label, const UiTextId *names, int n, UiTextId desc,
                const char *note = nullptr);
// A choice over a list the caller builds (devices, window sizes): values[i] is written, names[i] shown; "" means the key
// unset. cur: the index in effect, -1 to find cfg_value(key) in values (no match: the value shown as it is).
bool opt_choice_list(const char *key, UiTextId label, const char *const *values, const char *const *names, int n,
                     UiTextId desc, int cur = -1, const char *note = nullptr);
// A row that runs an action: true on A or a click. id: unique on the page and not a setting ("##copy_info"); value:
// shown on the right ("" none).
bool opt_button(const char *id, UiTextId label, UiTextId desc, const char *value = "");
// lo..hi in steps of step (left and right held for a second go ten times as fast); fmt formats the value ("%.0f %%").
bool opt_slider(const char *key, UiTextId label, float lo, float hi, UiTextId desc, float step = 1,
                const char *fmt = "%g");

// Pit Stop uses the same option edits with its own focus and viewport. Rows are submitted in order, including
// those outside the viewport. The preceding frame's row count bounds navigation before drawing.
struct OptPitPage { int row, top, count; };
struct OptPitLayout { ImVec2 pos; float width, row_height, gap, scale; int visible; };
struct OptPitInput { int move, adjust; bool select, clear; };
struct OptPitDetail { char label[160], text[768]; bool adjust, select, clear; };
inline constexpr ImU32 kPitInk = IM_COL32(16, 24, 43, 255);
inline constexpr ImU32 kPitWhite = IM_COL32(244, 247, 255, 255);
inline constexpr ImU32 kPitIce = IM_COL32(169, 231, 255, 255);
inline constexpr ImU32 kPitFocus = IM_COL32(255, 224, 84, 255);
inline constexpr ImU32 kPitNavy = IM_COL32(16, 31, 59, 255);
inline constexpr ImU32 kPitLine = IM_COL32(80, 108, 149, 255);
void opt_pit_begin(OptPitPage &page, const OptPitLayout &layout, const OptPitInput &input);
OptPitDetail opt_pit_end(void);
bool opt_pit_action(const char *label, const char *description, bool enter = false);
// Returns 1 to capture, 2 to clear, or 0. The binding cells can supply their physical icon.
int opt_pit_bind(const char *label, const char *description, const char *value, const char *lock, const char *note,
                 int pad, int kind);

// Binding rows (remapping on the Controls tab): a label and three cells, each selectable on its own (left / right move between them).
// opt_bind_header: the column titles above them. opt_bind_row: key is the setting's name without MKW_ (BIND_A; the
// description names it when the environment sets it); a disabled cell shows its text greyed and takes no cursor; note
// (NULL none) follows the description. Returns the cell A / Enter / a click activated (0..2), 10 + the cell Delete,
// Backspace or X cleared, or -1. opt_focus_key("<key>#<cell>") puts the cursor on a cell.
void opt_bind_header(UiTextId c0, UiTextId c1, UiTextId c2);
void opt_test_scroll(float fraction);   // test hooks: every page scrolled to that fraction of its height
void opt_bind_icon(int cell, int pad, int kind);   // before opt_bind_row: cell shows that pad button's icon
int opt_bind_row(const char *key, const char *label, UiTextId desc, const char *const cells[3], const bool enabled[3],
                 const char *note = nullptr);

// mkw.ini is written a moment after the last change (a held slider would rewrite it every frame) and when the menu
// closes. opt_save_tick: on every frame the menu lays out; force: write now if anything is pending. 1 while a write is
// pending.
int opt_save_tick(int force);
// A change made outside a row (Reset, F10): mkw.ini is written as after a row's change.
void opt_save_soon(void);
