# docs

| File | What it is |
|---|---|
| [research.md](research.md) | Initial research: the executable, Switch 2 hardware, every source used with its license, NVN2 against NVN1, shaders |
| [performance.md](performance.md) | Frame times measured on real machines, with their date; users add theirs through the performance report form |
| `logo.svg` | The repo's logo, the same chequered flag as `mkw.exe`'s icon, drawn by `tools/build/icon.py docs/logo.svg` |
| [i18n/](i18n/README.md) | The README in other languages (Simplified Chinese, Spanish, Arabic, Hindi, Brazilian and European Portuguese), built by `tools/docs/i18n_readme.py` |
| [images/](images/README.md) | Screenshots of the start screen for the README |
| [roadmap.md](roadmap.md) | The open work in order of priority, with the measurements behind it: the serial race frame first |
| [ps5.md](ps5.md) | Mario Kart World: Pit Stop (PS5 port): architecture, memory, frame rate, profiling and release flow |
| [multiplayer.md](multiplayer.md) | Split screen, local wireless over the LAN and over the internet (lobby, invite codes, relay): what the game does, the protocol, security, limits, settings, tests, what a lobby costs |
| [pit-stop.md](pit-stop.md) | Controller-first design for the in-game Pit Stop settings overlay |
| `release-template.md` | What every PC release's notes repeat (install, updating, known issues, reporting); `tools/build/notes.py` fills it with the version's section of `CHANGELOG.md` |
| `release-template-ps5.md` | What every PS5 release's notes repeat (requirements, PC preparation, console transfer, updating, known issues, reporting); `tools/build/notes.py --template docs/release-template-ps5.md` fills it |
