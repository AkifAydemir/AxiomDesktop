<p align="center">
  <img src="docs/assets/axiom-mark.svg" width="112" alt="Axiom Desktop mark">
</p>

# Axiom Desktop

[Türkçe](README.tr.md)

Axiom Desktop is a native, local-first command center for Windows. A global
`Alt+Space` palette brings file search, clipboard history, notes, reminders,
automations, diagnostics, and trusted native plugins into one keyboard-driven
workflow.

The project focuses on the engineering behind a dependable resident desktop
tool: filesystem change recovery, atomic local-data operations, explicit plugin
trust, crash-safe restore transactions, time-change handling, privacy-aware
support bundles, and deterministic tests around those boundaries.

> **Project context:** Axiom Desktop is an independent native Windows project
> developed while I was a high-school student. Its reliability claims are backed
> by the source, automated tests, and a structured Windows verification record.

![Axiom Desktop Release palette showing the diag status command result](docs/assets/axiom-diag-status-release.png)

*Real Release-build capture after `Alt+Space` → `diag status`. The palette shows
the command's session-scoped execution diagnostics.*

## Highlights

- Native C++20 and Win32; no embedded browser runtime.
- Global command palette with tray and single-instance lifecycle behavior.
- Incremental file indexing with `ReadDirectoryChangesW` recovery and rescan
  generation guards.
- Clipboard history, journal entries, reminders, notifications, and scheduled
  command automations.
- Local archive/restore with CurrentUser DPAPI protection and crash-recovery
  transactions.
- Native plugin manifests, SHA-256 verification, capability declarations, and a
  local trust store.
- Redacted support bundles and execution diagnostics.
- 30 registered CTest targets plus a Windows release-evidence verifier.

## Architecture

```mermaid
flowchart TD
    UI[Win32 command palette] --> AR[Action registry]
    UI --> FI[File index]
    UI --> CL[Clipboard and journal]
    UI --> RM[Reminders and notifications]
    AR --> AU[Automation engine]
    AR --> PH[Plugin host]
    FI --> FW[Filesystem watcher]
    FW --> RC[Overflow recovery and rescan gate]
    CL --> LD[Protected local data]
    RM --> LD
    AU --> RT[Runtime and diagnostics]
    PH --> TS[Manifest, hash and trust checks]
    LD --> TX[Atomic archive and restore transactions]
```

See [Architecture](docs/ARCHITECTURE.md) for subsystem boundaries and failure
handling.

## A 60-second product tour

1. Start `Axiom.exe` and press `Alt+Space`. Type `?` to browse the command
   library.
2. Run `diag status` to see the session-scoped result pictured above.
3. Run `index status` to inspect the file index and watcher recovery state.
4. Run `plugin status` and `data status` to inspect plugin trust and local-data
   privacy state without changing either.
5. Search your own indexed files with `find <query>`; `help remind` and
   `help auto` show the persistent reminder and automation commands.

The first four commands are read-only and make the palette's subsystem
boundaries inspectable in a fresh installation.

## Build on Windows

### Prerequisites

- Windows 10 or later, x64
- Visual Studio 2022 with the Desktop development with C++ workload
- CMake 3.24 or later

From an x64 Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The desktop executable is produced at `build/Release/Axiom.exe`. Start it once,
then use `Alt+Space` or the tray icon to show the palette.

## Repository layout

| Path | Purpose |
| --- | --- |
| `src/` | Win32 application and independently testable core libraries |
| `tests/` | Unit, contract, recovery, privacy, and Windows release-gate tests |
| `samples/hello_plugin/` | Minimal native plugin and manifest |
| `benchmarks/` | File-index benchmark target |
| `tools/` | Verification-evidence parser and gate checker |
| `verification-evidence/` | Structured Windows v0.16 release-gate record |
| `docs/` | Architecture and validation boundary |

## Plugin model

The sample plugin demonstrates the package boundary without hiding it behind a
framework. A manifest declares the plugin identity, ABI, DLL, and requested
capabilities. Installation and loading remain separate operations: package
hashing and local trust decisions happen before native code is loaded.

Native plugins execute in-process and therefore must be treated as trusted code.
The trust model reduces accidental or unreviewed loading; it is not a sandbox.

## Data and privacy

Axiom is local-first. Journal and archive protection uses Windows CurrentUser
DPAPI where enabled. Support-bundle fields are redacted by default and additional
data requires explicit opt-in. No cloud account or remote service is required by
the current source tree.

## Validation status

The v0.16 source candidate builds with Visual Studio 2022/MSVC and passes all 30
CTest targets. The included structured evidence also covers lifecycle,
single-instance activation, tray/hotkey behavior, plugin install/load/unload,
DPAPI round-trips, watcher recovery, clock/DST changes, crash recovery, and
support-bundle publication.

See [Validation evidence](docs/VALIDATION.md) for what was reproduced locally
and the boundary between the real palette capture and structured lifecycle
evidence.

## Scope boundaries

Axiom is a portfolio-stage Windows desktop system, not a security boundary or an
enterprise endpoint-management product. Plugin trust, local encryption, and
recovery logic are implemented and tested, but the project does not claim formal
security certification.

Current portfolio release: **0.16.0**.

## License

Licensed under the [MIT License](LICENSE).

Small, reviewable changes are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md) for
the local quality gate.
