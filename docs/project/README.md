# docs/project

The project's own documents: where it comes from, how fast it runs, what is next and what every release says.

| File | What it is |
|---|---|
| [research.md](research.md) | Initial research: the executable, Switch 2 hardware, every source used with its license, NVN2 against NVN1, shaders |
| [extensions.md](extensions.md) | Extensions: code outside the repository that replaces or watches the game's functions by name (`runtime/core/ext`), built in with `--ext` or loaded from `MKW_EXT` |
| [performance.md](performance.md) | Frame times measured on real machines, with their date; users add theirs through the performance report form |
| [roadmap.md](roadmap.md) | The open work in order of priority, with the measurements behind it: the serial race frame first |
| `release-template.md` | What every release's notes repeat (install, updating, known issues, reporting); `tools/build/notes.py` fills it with the version's section of `CHANGELOG.md` |
