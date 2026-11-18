# docs/images

Images for the main README: the logo, and screenshots taken from the start screen itself with its test hooks (`runtime/ui/launcher`):
`MKW_LAUNCHER_SHOT=<frame>:<page>` saves the window at that frame to `MKW_LAUNCHER_SHOT_OUT`, and `MKW_MENU_LANG=en`
sets the language. At 1280 x 720, compressed with `pngquant` and `oxipng`; the game folder shown is a neutral path
(`/mnt/MarioKartWorld`, a bind mount), not a personal one.

| File | What it shows |
|---|---|
| `launcher-main.png` | The main page once the game is set up, with the link to the project's GitHub page at the lower right (first by @ViniciusStanula, from #70) |
| `launcher-files.png` | The Game files page before the setup, cropped to its content |
| `launcher-ps5.png` | The PS5 builder package's Game files page (Windows and Linux): the copy chosen (a neutral path) and the example address `192.168.1.50` in `ps5-ip.txt`, cropped to its content. Its window, not the game: nothing of the game shows |
| `placeholder-ps5.png` | The PS5 placeholder title (`runtime/ui/menu/ps5_placeholder.cpp`) on a base PS5 over Remote Play while a Windows PC builds (step 4 of 9, the RomFS): the card cropped, the console's address blurred. Our screen, not the game: nothing of the game shows |
| `icon.svg` | The "MKW: Pit Stop" icon, 512 units square: our own drawing (a go-kart seen from the front, a wrench and a chequered flag, no art of the game); the lettering is Lato Black Italic (OFL) as outlines, so it needs no font. Drawn by `tools/build/pit_stop_icon.py` |
| `icon0.png` | `icon.svg` at 512 px, the PS5 title's home screen icon: `tools/ps5/build_game.py` puts it in `sce_sys` on every build and `tools/ps5/deploy.py --game` refreshes ShadowMountPlus's registered copy. Committed so the PS5 build needs neither cairosvg nor the font |
| `icon-mark.svg` | The same emblem without the lettering, for sizes where text does not read (64 px and under) |
| `icon-mark.png` | `icon-mark.svg` at 256 px: `tools/build/icon.py` makes the PC program's 48, 32 and 16 px icons from it (and its 256 and 128 px ones from `icon0.png`). Committed for the same reason as `icon0.png`; `pit_stop_icon.py` rewrites both |
| `banner.svg` | The 1280x640 version with "MKW: Pit Stop" on one side, for GitHub's social preview |
| `logo.svg` | The repo's logo, the same chequered flag as `mkw.exe`'s icon, drawn by `tools/build/icon.py docs/images/logo.svg` |
