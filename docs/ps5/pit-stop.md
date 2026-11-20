# Pit Stop

Pit Stop replaces the in-game F1 settings overlay. It opens with **Resume** selected. Under that are **Master volume**,
**Vibration strength**, **Performance HUD**, and **All settings**, in that order. The first three settings are live
and also appear in their respective categories. Each reads the same setting wherever it appears.

## Screen

Dim the presented game image and keep it visible on the right. Use an opaque navy surface for the left-hand list,
a yellow filled row with dark type for focus, white text for the other rows, and pale cyan for arrows and headings.
An angled blue edge, a small checker strip, and a dashed lane marking behind the lower-right help panel give the
overlay a Mario Kart World look. Draw these with ImGui rectangles and triangles and use the embedded Roboto. The
live game image provides the scenery.

At 1920 × 1080, start with 96 px side margins, a 72 px top margin, a 62 px title, 30 px row labels, 68 px rows,
and 22 px help and footer text. Show up to six rows and scroll by whole rows as focus moves. Keep values aligned on
the right and indicate when more rows are available. The help panel gives the focused option's description and
its restart or lock status. Scale from the available width and height. At narrow aspect ratios, place help below
the list and reduce the number of visible rows before reducing type size. Check the longest Spanish and Russian
labels at 720p.

## Controller

| DualSense | Behaviour |
|---|---|
| Options + touchpad click | Open or close from any screen. Keep F1 as the desktop equivalent. |
| D-pad or left stick up/down | Move one row. Hold to repeat. Stop at the first and last rows. |
| D-pad or left stick left/right | Adjust the focused value. Stop at the ends. Toggle values use left for Off and right for On. |
| Circle (confirm) | Open a category, activate an action, or toggle a switch. Numeric and enumerated settings use left/right. |
| Cross (back) | Return one screen and restore its row and scroll position. Close from Pit Stop. |
| L1/R1 | Switch to the previous or next category from a settings page. Stop at the ends. |

All settings opens a vertical list of Display, Graphics, Controls, Audio, Game, Menu, Advanced, Hacks, and Quit game.
Each category remembers its last focus during the overlay session. Back from a category returns to its entry in
All settings. Quit and Reset all settings open confirmations with the safe action focused. Category shortcuts and
underlying row controls are inactive while a confirmation or binding capture is open.

Confirm and back follow the gameplay layout, as the game's own menus do: in the Nintendo layout (the DualSense's default,
the Switch's positions) the east button confirms and the south one goes back, Circle and Cross; in the Xbox layout south
confirms. Show footer prompts for the focused row and active screen. Use Circle/Cross on DualSense, A/B on Xbox and Nintendo
controllers, and Enter/Esc on keyboard. Use the matching physical button for each prompt. Desktop keeps arrows,
Q/E for categories, and mouse support for rows and visible value arrows.

Controls opens a controller binding list with one focusable row per game action. Confirm (Circle) starts capture, and
capture waits for it to be released before taking the next input. Tap back (Cross) to assign it, or hold it for 600 ms to
cancel. Esc also cancels. Use the current five-second timeout and restore the binding row on exit. Report swaps and
keep the overlay buttons usable after any remap. Desktop users can access the existing keyboard and mouse binding
cells from Controls.

## Runtime integration

- Replace the layout in [menu.cpp](../../runtime/ui/menu/menu.cpp) at the existing ImGui overlay entry point. Use the
  current Vulkan present pass and font atlas. Put the theme in the overlay's style scope, since the launcher also
  uses [menu_look.cpp](../../runtime/ui/menu/menu_look.cpp).
- Track screen, modal, focused row ID and scroll position in the menu. Keep one navigation path per frame. Route
  controller and keyboard commands through that state and use ImGui hit targets for pointer input. Apply the existing
  stick hysteresis (0.50 to engage, 0.35 to release). Arm menu input on neutral after opening or reconnecting, and
  consume the transition press before changing screens. Keep the guest input block and release gate in
  [input.c](../../runtime/input/input.c).
- The menu-pad snapshot takes physical positions and the left stick from the same SDL or PS5 sample. Navigation
  and labels follow physical buttons so a game binding cannot strand the user in Pit Stop. Physical Start/Back
  uses a release latch for controller opening. The input path keeps the
  keyboard shortcut and gameplay mapping separate from the overlay's physical controls. If the two chord buttons
  arrive on separate polls, keep the current single-button behaviour and suppress the chord once both are held.
  Reuse the [pad glyph drawing](../../runtime/ui/glyphs/pad_icons.h) and use text for buttons without a glyph.
- Reuse the setting definitions, labels, actions and quality preset logic in
  [settings_tabs.cpp](../../runtime/ui/menu/settings_tabs.cpp). Expose category bodies to Pit Stop independently of
  the existing tab bar, which the launcher keeps using. The quick rows use `MKW_VOLUME` (0–100% in steps of 5),
  `MKW_RUMBLE_STRENGTH` (0–300% in steps of 10) and `MKW_PERF_OVERLAY` (Off, FPS, Full).
  Keep the [cfg metadata](../../runtime/core/base/cfg.h) as the source for defaults, environment locks and restart
  flags. The runtime exposes the existing catalogue, with platform-specific rows where supported.
- Reuse setting writes, debounced persistence and reset from
  [option_rows.cpp](../../runtime/ui/menu/option_rows.cpp) and the existing binding edit logic in
  [binds_page.cpp](../../runtime/ui/menu/binds_page.cpp). A failed save should say the change is active for the
  session. The PS5 build keeps using `/app0/mkw.ini`.
  The settings worker coalesces writes. Window shutdown waits for it to finish before exiting.
- On PS5, route desktop-only features through platform availability. The current PS5 window fixes the client
  dimensions to its output mode, the native input path has no motion sensor polling, and the platform stubs for
  opening a folder and copying to the clipboard return failure. Show controls and actions where their backends work.
- Rows the PS5 build leaves out (20/11/2026, `settings_tabs.cpp` under `MKW_PS5_NATIVE`), each because it does nothing
  or the wrong thing on the console: fullscreen and window size (no window), graphics card (one device), presentation
  mode (VideoOut's own vsynced present), aspect (the TV's 16:9: the ultrawide modes and "fit the window" have nothing to
  fit), HDR (the swapchain is SDR, `ps5_window.c` `wsys_hdr`), Auto-set (it reads the card's dedicated memory and took
  the PS5's shared memory for a light card: Medium at 720p), VRR (VideoOut runs at the TV's fixed rate), speakers 5.1 and
  background mute (AudioOut opens stereo, the title has no window to leave), keyboard button icons, motion, trigger
  rumble, Switch 2 and DSU controllers, mouse and keyboard rows, Discord, the update check and start screen, free cores
  (the game crashes) and the 1 ms timer (Windows'), memory mode (mirroring is a discrete card's, untested on shared
  memory), Vulkan validation (no layer in the title), and the freeze keys (keyboard).

## Acceptance checks

- On a game-free input test, cover opening with each chord button first, holding a button through a screen change,
  disconnecting during a hold, and closing with Cross or Circle held. The release must precede any guest action.
- Exercise the whole flow with a DualSense at 720p and 1080p. Change all three quick rows, enter every category, use
  shoulder shortcuts and back out from the last item in a scrolling list. Remap a face button, cancel a second
  capture, cancel Quit, and resume with a single confirm from a fresh open.
- Check that a live change takes effect, a restart-only change is saved and marked, and an environment override
  cannot be changed. Relaunch to verify persistence on PS5. Use the existing `MKW_MENU_TEST` screenshot hook to
  check row clamping and text at representative resolutions.
