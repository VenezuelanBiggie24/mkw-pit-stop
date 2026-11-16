# docs/images

Screenshots for the main README, taken from the start screen itself with its test hooks (`runtime/ui/launcher`):
`MKW_LAUNCHER_SHOT=<frame>:<page>` saves the window at that frame to `MKW_LAUNCHER_SHOT_OUT`, `MKW_LAUNCHER_SETUP=<dump>:link`
starts the setup as the button does, and `MKW_MENU_LANG=en` sets the language. Scaled to 640 px.

| File | What it shows |
|---|---|
| `launcher-setup.png` | The Game files page during the setup, compiling the game code |
| `launcher-main.png` | The main page once the game is set up |
| `icon.svg` | The "MKW: Pit Stop" icon, 512 units square: our own drawing (a go-kart seen from the front, a wrench and a chequered flag, no art of the game); the lettering is Lato Black Italic (OFL) as outlines, so it needs no font. Drawn by `tools/build/pit_stop_icon.py` |
| `icon0.png` | `icon.svg` at 512 px, the PS5 title's home screen icon: `tools/ps5/build_game.py` puts it in `sce_sys` on every build and `tools/ps5/deploy.py --game` refreshes ShadowMountPlus's registered copy. Committed so the PS5 build needs neither cairosvg nor the font |
| `icon-mark.svg` | The same emblem without the lettering, for sizes where text does not read (64 px and under) |
| `banner.svg` | The 1280x640 version with "MKW: Pit Stop" on one side, for GitHub's social preview |
