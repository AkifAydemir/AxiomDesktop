# Contributing

Axiom Desktop is a native, local-first Windows application. Changes should keep
failure recovery, user privacy, and plugin trust boundaries explicit.

## Local quality gate

From an x64 Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Before opening a pull request:

- add regression coverage for changed persistence, watcher, scheduler, plugin,
  or lifecycle behavior;
- preserve atomic-write and crash-recovery behavior when touching local data;
- run `clang-format` using the repository `.clang-format` file for changed C++
  sources;
- treat native plugins as trusted code and do not describe the trust store as a
  sandbox;
- do not commit `build`, IDE state, generated packages, or personal local data.
