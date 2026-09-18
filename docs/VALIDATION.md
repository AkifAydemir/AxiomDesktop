# Validation evidence

## Reproduced Windows build

The v0.16 source candidate has been built on Windows with Visual Studio 2022,
MSVC x64, the Windows SDK, and the Visual Studio CMake distribution.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Expected test summary:

```text
100% tests passed, 0 tests failed out of 30
```

The release build produces the native desktop executable, sample plugin, plugin
manifest, benchmark, and verification tool.

## Structured v0.16 evidence

`verification-evidence/windows-v16.txt` is a machine-readable eight-gate record.
It documents the original Windows verification environment, procedure, pass
criteria, and evidence for:

1. MSVC Release build and 30 CTest targets.
2. Axiom lifecycle, tray, hotkey, single-instance handoff, exit, and restart.
3. Sample plugin install, trust, load, invocation, unload, and removal.
4. CurrentUser DPAPI persistence and corrupt-input rejection.
5. Real watcher loss/error recovery and generation-safe rescan.
6. `WM_TIMECHANGE` and DST gap/fallback scheduling behavior.
7. Interrupted restore recovery across child-process restart.
8. Redacted and atomically published Win32 support bundles.

The `AxiomWindowsVerification` target validates the structure and status of this
evidence as part of the release test suite.

## Validation boundary

The local build and automated suite have been independently reproduced. The
structured lifecycle evidence is retained and checked by the source tree. A
manually captured Release-build palette screenshot in the README shows the
`diag status` query and its executed result. This visual evidence does not, by
itself, demonstrate tray activation or independently replay every lifecycle gate.

The project does not claim penetration testing, formal security certification,
or exhaustive compatibility across third-party shell/hotkey utilities.
