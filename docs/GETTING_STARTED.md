# First successful session

## Portable preview (no development tools on the recipient's PC)

The maintainer can run `powershell -NoProfile -File tools/package.ps1` to build,
test and create a new ZIP under `artifacts/` with a SHA-256 digest. Extract the
whole ZIP before launching the executable; do not run it inside the archive.
These are unsigned preview builds, not certified installers. Never disable OS
security checks to launch them. Native components may require Microsoft's
Visual C++ 2015–2022 x64 runtime. No clean-machine installation claim is made.

## Build from source

1. Install Visual Studio 2022 (Desktop development with C++) and CMake 3.24+ on Windows x64.
2. From the repository root run: `powershell -NoProfile -File tools/check.ps1`.
3. Launch `build/Release/Axiom.exe`. The application is resident: look in the notification area, not only the taskbar.
4. Press Alt+Space, type `?`, then try `diag status`, `index status`, `plugin status` and `data status`.
5. Use the built-in help before creating reminders, automations or loading plugins.

## If something goes wrong

- Palette hidden: use the tray icon; another application may own the same hotkey.
- Search empty: inspect `index status` before assuming a missing file is indexed.
- Protected archive cannot open on another account: CurrentUser DPAPI is intentionally bound to the original Windows user.
- Plugin rejected: inspect its manifest/hash/trust status. Do not bypass checks for unknown DLLs. Plugins run in-process, not in a sandbox.

Automated checks do not replace a real hotkey/tray/restart check. See [validation](VALIDATION.md) and [architecture](ARCHITECTURE.md).
