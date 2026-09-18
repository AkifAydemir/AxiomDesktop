# Architecture

Axiom is one native Windows process with a thin Win32 presentation layer and a
set of independently testable C++20 libraries. The decomposition follows state
ownership and failure boundaries rather than UI screens.

## Application shell

`src/axiom.cpp` owns the palette window, tray integration, global hotkey,
single-instance activation, command dispatch, and projection of subsystem state
into Win32 controls. `src/main.cpp` is the process entry point.

The application normally resides in the notification area. `Alt+Space`, tray
activation, or a second-instance handoff can reveal the palette. Losing focus can
hide it without terminating the resident process.

## Index and watcher recovery

`AxiomIndex` owns searchable file metadata. The Win32 watcher reports incremental
changes, while `AxiomWatcherRecovery` tracks watcher health, loss/overflow, and
rescan generations. A stale scan generation cannot clear a newer recovery need.
This prevents an old scan from declaring the index healthy after a later watcher
failure.

## Actions and automations

`AxiomActions` provides a central action registry. Built-in commands and trusted
plugin commands share the registry boundary. `AxiomAutomations` persists local
schedules and invokes registered actions through the runtime layer.

Calendar scheduling explicitly accounts for Windows time-change notifications,
DST gaps, and ambiguous fallback times. Retry state is persisted separately from
the human-readable schedule.

## Local data and recovery

Clipboard, journal, reminder, automation, settings, and trust stores remain
local. `AxiomLocalData` coordinates archive inspection and transactional restore.
Restore intent is journaled so startup recovery can distinguish uncommitted work
that must roll back from committed work whose cleanup was interrupted.

Optional protection uses Windows CurrentUser DPAPI. This binds protected payloads
to the current Windows user; it is not a portable encryption format.

## Plugin boundary

Plugin packages include a manifest and native DLL. The host validates identity,
ABI, declared capabilities, and SHA-256 trust state before loading. Registry
entries are removed before a plugin can unload or be replaced, preventing stale
callbacks from remaining visible.

Native modules execute in the Axiom process. The trust store is an explicit
authorization mechanism, not process isolation.

## Diagnostics and support bundles

Execution diagnostics collect bounded failure context. Support bundles default to
redacted output and use unique temporary files, durable flush, and atomic
replacement on Windows. Failed publication removes incomplete temporary data.

## Verification model

The CTest suite contains unit tests, source-contract tests for Win32-only paths,
restart/recovery child-process tests, and release-gate checks. A separate verifier
parses `verification-evidence/windows-v16.txt` and rejects missing, duplicate,
failed, or malformed release gates.
