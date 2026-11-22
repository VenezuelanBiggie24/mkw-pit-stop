# runtime/ui/menu

Pit Stop is the in-game menu (F1 or Start+Back). It shares setting definitions, Roboto and remapping with the start screen.

| File | What it is |
|---|---|
| `menu.cpp`, `menu.h` | Menu core: ImGui state, event queue, controller navigation, Vulkan back end and pages; no window system |
| `menu_notice.cpp`, `menu_notice.h` | `menu_notice`: a timed line at the top of the picture, menu open or not (missing controllers) |
| `text_entry.cpp`, `text_entry.h` | The number pad of a software keyboard request for a number or a code (`sdk/swkbd/swkbd_sdl.c`): drawn in the overlay, worked with a controller, the mouse or the keys; `MKW_KEYPAD_TEST=<frame>` opens one to try it |
| `menu_sdl.cpp` | Event adapter for the SDL3 window |
| `settings_tabs.cpp`, `settings_tabs.h` | The settings tabs (bar and rows of each tab), shared by the menu and the start screen; the Controls tab starts with each player's controller and its battery while the game runs |
| `menu_look.cpp`, `menu_look.h` | Colours and sizes of the menu, and the fonts it shares with the start screen (embedded Roboto) |
| `option_rows.cpp`, `option_rows.h` | Two-pane settings pages: rows of the `cfg.h` table on the left, description on the right (from SavageWindRecomp, MIT); their colours are the menu's unless a caller passes its own (`opt_look`); Pit Stop's focus and scrolling rows |
| `net_page.cpp`, `net_page.h` | The Multiplayer tab: local wireless settings, status, invite code (copy, paste on desktop), players (ping, kick), close the room, the lobby's public rooms (`runtime/net/net_ui.h`) |
| `binds_page.cpp`, `binds_page.h` | Remapping rows of the controls tab (`input_map.h`, `MKW_BIND_*` lines) |
| `id_check.cpp`, `id_check.h` | ImGui ID conflicts written to `mkw.log` (window and page) instead of ImGui's red popup, for the menu and the start screen |
| `pit_stop.cpp`, `pit_stop.h` | Pit Stop layout, physical-button navigation, settings categories and modal screens |
| `boot_screen.cpp`, `boot_screen.h` | Boot screen: a Pit Stop styled loading page (game code, shaders, RomFS index count, starting the game) from `rt_boot` until the game's first present; on by default on PS5 (`MKW_BOOT_SCREEN`) |
| `ps5_placeholder.cpp`, `ps5_placeholder.h` | PS5 game-free placeholder / missing-game-files screen (console IP, expected runtime version, `PIT1` receive progress bar) |
