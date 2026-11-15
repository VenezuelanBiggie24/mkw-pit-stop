# runtime/input

| File | What it is |
|---|---|
| `input.c`, `input.h`, `input_map.*` | Controller state, remapping and physical input for Pit Stop; one gamepad per player for multiplayer (`MKW_VIRTUAL_PADS=N` adds N virtual SDL gamepads); tracks the last device for button icons |
| `input_script.*` | Button script (`MKW_INPUT_SCRIPT`; a `p<N> ` prefix presses for player N, also through `ctl press`; paused while an `MKW_SEQ_HOLD` phase lasts; a `<phase>+<ticks> BUTTON` line counts from the start of that phase, with `MKW_SEQ_WATCH=1`); the A button of `MKW_SKIP_MOVIES` (`core/hacks/skip.c`) and `MKW_CTL` presses |
| `input_sdl.*`, `input_wsys.h` | SDL3 back end |
| `ps5_input.c`, `ps5_input.h` | SDL-free adapter from raw DualSense axes and physical buttons to the runtime's binding tables and stick conventions. `MKW_PAD_LAYOUT=auto` is the Switch layout by position (circle A, cross B), as in OcarinaPS5; `xbox` gives cross A |
| `ps5_wsys.c` | PS5 keyboard and mouse stubs for `input_wsys.h` |
| `motion_fusion.c` | Gyro and accelerometer |

Default keyboard (`input_map.c`, changeable in the `.ini`):

| Key | Button | In MKW |
|---|---|---|
| WASD | left stick | steering, menus |
| Mouse, arrows | right stick | camera; world map zoom |
| Space, Enter, E | A | accelerate, accept |
| Left Shift, Backspace | B | brake, back |
| F, X | ZR | drift |
| R | R | drift |
| Q | L | item |
| Z, right click | ZL | item |
| Left click | Y | |
| V | X | |
| 1-4 | D-pad | |
| Tab | + | pause |
| M | − | |
