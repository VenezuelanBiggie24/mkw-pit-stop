# runtime/ui/menu

Pit Stop is the in-game menu (F1 or Start+Back). It shares setting definitions, Roboto and remapping with the start screen.

| File | What it is |
|---|---|
| `menu.cpp`, `menu.h` | ImGui state, input event queue, Vulkan present integration, screenshot hooks and performance HUD |
| `pit_stop.cpp`, `pit_stop.h` | Pit Stop layout, physical-button navigation, settings categories and modal screens |
| `menu_sdl.cpp` | Event adapter for the SDL3 window |
| `settings_tabs.cpp`, `settings_tabs.h` | The settings tabs (bar and rows of each tab), shared by the menu and the start screen |
| `menu_look.cpp`, `menu_look.h` | Colours, sizes and fonts of the menu and the start screen (embedded Roboto) |
| `option_rows.cpp`, `option_rows.h` | Shared cfg edits, launcher settings rows (SavageWindRecomp, MIT), and Pit Stop's focus and scrolling rows |
| `binds_page.cpp`, `binds_page.h` | Remapping rows of the controls tab (`input_map.h`, `MKW_BIND_*` lines) |
| `boot_screen.cpp`, `boot_screen.h` | Boot screen: a Pit Stop styled loading page (game code, shaders, RomFS index count, starting the game) from `rt_boot` until the game's first present; on by default on PS5 (`MKW_BOOT_SCREEN`) |
| `ps5_placeholder.cpp`, `ps5_placeholder.h` | PS5 game-free placeholder / missing-game-files screen (console IP, expected runtime version, `PIT1` receive progress bar) |
