# runtime/input

| File | What it is |
|---|---|
| `input.c`, `input.h`, `input_map.*` | Controller state, remapping and physical input for Pit Stop; one gamepad per player for multiplayer (`MKW_VIRTUAL_PADS=N` adds N virtual SDL gamepads; `MKW_KEYBOARD_PLAYER=own`: the keyboard alone is player 1); each pad shows its controller number (SDL player index and LED, the DualSense light bar on the PS5: blue, red, green, pink); tracks the last device for button icons |
| `input_script.*` | Button script (`MKW_INPUT_SCRIPT`; a `p<N> ` prefix presses for player N, also through `ctl press`; paused while an `MKW_SEQ_HOLD` phase lasts; a `<phase>+<ticks> BUTTON` line counts from the start of that phase, with `MKW_SEQ_WATCH=1`); the A button of `MKW_SKIP_MOVIES` (`core/hacks/skip.c`) and `MKW_CTL` presses |
| `input_sdl.*`, `input_wsys.h` | SDL3 back end |
| `ps5_input.c`, `ps5_input.h` | SDL-free adapter from raw DualSense axes and physical buttons to the runtime's binding tables and stick conventions. `MKW_PAD_LAYOUT=auto` is the Switch layout by position (circle A, cross B), as in OcarinaPS5; `xbox` gives cross A |
| `ps5_wsys.c` | PS5 keyboard and mouse stubs for `input_wsys.h` |
| `motion_fusion.c` | Gyro and accelerometer |
| `switch2_proto.h` | The Switch 2 controllers' Bluetooth LE protocol (Pro Controller 2, Joy-Con 2: GATT ids, handshake, reports, stick calibration, rumble, pairing), from seedds/mkw-neo (MIT) with our button bits; checked by `tests/test_switch2.py` |
| `switch2.h`, `switch2.c` | Switch 2 controllers over Bluetooth LE (`MKW_SWITCH2`, off by default; SDL has them over USB only): the session on top of a transport (identity, pairing, calibration, features, player lights, rumble), each pad a virtual SDL gamepad with its gyro, so players, layout and rumble work as for any gamepad |
| `linux_switch2.c` | Its Linux transport: BlueZ over the system D-Bus, libdbus-1 loaded at run time; `MKW_SWITCH2=2` logs each advertising device once |
| `switch2_win.c` | Its Windows transport: WinRT through its C ABI (advertisement watcher, `BluetoothLEDevice`, GATT), its events queued for the transport's thread; `MKW_SWITCH2=2` as on Linux |

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
