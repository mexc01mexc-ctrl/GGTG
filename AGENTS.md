# Instructions for AI agents

Read `PROJECT_MAP.md` first: product overview, folder layout, builds, data locations,
key mechanisms and code conventions of GGTG.
Read `docs/UI_STYLE_GUIDE.md` for UI and design system conventions (STRICT).
`docs/FILE_INDEX.md` lists every GGTG source file with a one-line description
(regenerate with `python3 tools/update_file_index.py`).

## Core Rules That Matter Most:
1. **Strict UI Style & No Emoji:**
   - Follow `docs/UI_STYLE_GUIDE.md` for all screens and dialogs.
   - UI uses **official Telegram icons only**, NEVER emoji as icons or status bullets (`🟢`, `🟡`, `🔴`, `💻`, `🍏`, `📱`, `⚙️`, `🚀`, `🎲`, `🔥`, etc.).
   - NEVER output raw HTML tags like `<b>...</b>` in `Ui::FlatLabel` or `TextView`.
   - Preset/mode selection must use real radiobuttons (`Ui::RadiobuttonGroup` + `Ui::Radiobutton` with `st::ggtgChoiceRowPadding` on Desktop).
   - Account pickers MUST use the standard folder accordion (`AddAccountPicker` on Desktop; `GgtgAccountPicker` on Android), never flat checkbox lists.
2. **Platform Parity:**
   - Keep macOS and Android in parity; shared formats live in `docs/ggtg-formats.md`.
   - Every feature on Desktop must have an equivalent on Android and vice versa.
3. **Code Style & Modularity:**
   - Mark GGTG edits inside Telegram code with `// ng:` or `// ggtg:`; prefer separate `ggtg_*` files.
   - Desktop code must also build on Windows with Qt 5 (no Qt 6-only APIs).
4. **Environment & Builds:**
   - Do not delete the hidden links `~/Desktop/ggtg-macos` and `~/Desktop/ggtg-Android`: the build caches depend on them.
   - The macOS DMG can also be built locally and incrementally with `tools/mac_local_build.sh` (needs full Xcode; the first run downloads the prebuilt libraries from the `macos-deps-storage` branch). The APK can be built locally with `tools/android_local_build.sh` (arm64 only, stops the Gradle daemons when done; `setup` installs the missing SDK/NDK). EXE is built ONLY on GitHub Actions. On GitHub Actions: push to `main`, or run `tools/ci.sh run [android|macos|windows|all]`; fetch results with `tools/ci.sh get`.
   - Fetched artifacts go to `APP/` (`APP/GGTG.dmg`, `APP/GGTG-Android.apk`, `APP/GGTG-Windows-x64.zip`); macOS crash symbols to `APP/symbols/<UUID>/GGTG`.
   - Run `python3 tools/update_file_index.py` after creating or removing any files.
