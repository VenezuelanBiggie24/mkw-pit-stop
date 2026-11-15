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
