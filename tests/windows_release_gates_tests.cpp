#ifdef _WIN32

#include "action_registry.hpp"
#include "axiom.hpp"
#include "file_index.hpp"
#include "filesystem_watcher.hpp"
#include "journal_store.hpp"
#include "local_data_archive.hpp"
#include "plugin_host.hpp"
#include "plugin_loader_win32.hpp"
#include "plugin_package.hpp"
#include "plugin_trust_store.hpp"
#include "startup_restore_recovery.hpp"
#include "support_bundle.hpp"
#include "watcher_health.hpp"

#include <process.h>
#include <windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error{std::string{message}};
}

void cleanup(const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

void write_text(const std::filesystem::path &path, std::string_view text) {
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream << text;
    if (!stream)
        throw std::runtime_error{"Unable to write Windows gate fixture."};
}

std::string read_text(const std::filesystem::path &path) {
    std::ifstream stream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, {}};
}

std::filesystem::path executable_path() {
    std::wstring buffer(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
        throw std::runtime_error{"GetModuleFileNameW failed for Windows gate child process."};
    }
    buffer.resize(length);
    return buffer;
}

int spawn_self(std::span<const std::wstring> arguments) {
    const auto executable = executable_path().wstring();
    const auto quoted_executable = L"\"" + executable + L"\"";
    std::vector<const wchar_t *> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(quoted_executable.c_str());
    for (const auto &argument : arguments)
        argv.push_back(argument.c_str());
    argv.push_back(nullptr);
    return static_cast<int>(_wspawnv(_P_WAIT, executable.c_str(), argv.data()));
}

HWND wait_for_axiom_window(std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (HWND window = FindWindowW(L"AxiomDesktopPaletteWindow", nullptr); window != nullptr) {
            return window;
        }
        std::this_thread::sleep_for(50ms);
    }
    return nullptr;
}

bool wait_for_visibility(HWND window, bool visible, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if ((IsWindowVisible(window) != FALSE) == visible)
            return true;
        std::this_thread::sleep_for(50ms);
    }
    return (IsWindowVisible(window) != FALSE) == visible;
}

bool close_existing_axiom() {
    HWND window = FindWindowW(L"AxiomDesktopPaletteWindow", nullptr);
    if (window == nullptr)
        return false;
    DWORD process_id{};
    GetWindowThreadProcessId(window, &process_id);
    HANDLE process =
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    require(process != nullptr,
            "existing Axiom process could not be opened for shutdown verification");
    PostMessageW(window, axiom::axiom_activate_message, 0, 0);
    const auto visible_deadline = std::chrono::steady_clock::now() + 2s;
    while (!IsWindowVisible(window) && std::chrono::steady_clock::now() < visible_deadline) {
        std::this_thread::sleep_for(20ms);
    }
    require(IsWindowVisible(window),
            "existing Axiom instance could not be activated for clean exit");
    require(PostMessageW(window, axiom::axiom_verification_shutdown_message, 0, 0),
            "existing Axiom instance could not receive clean shutdown request");
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline &&
           FindWindowW(L"AxiomDesktopPaletteWindow", nullptr) != nullptr) {
        std::this_thread::sleep_for(50ms);
    }
    require(FindWindowW(L"AxiomDesktopPaletteWindow", nullptr) == nullptr,
            "existing Axiom instance did not exit cleanly");
    require(WaitForSingleObject(process, 30'000) == WAIT_OBJECT_0,
            "existing Axiom process did not finish clean shutdown");
    CloseHandle(process);
    return true;
}

PROCESS_INFORMATION launch_axiom(const std::filesystem::path &executable) {
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                           executable.parent_path().c_str(), &startup, &process),
            "CreateProcessW failed for Axiom.exe");
    CloseHandle(process.hThread);
    return process;
}

void require_process_exit(PROCESS_INFORMATION &process, std::chrono::milliseconds timeout) {
    require(WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout.count())) ==
                WAIT_OBJECT_0,
            "Axiom process did not exit within the lifecycle timeout");
    DWORD exit_code = 1;
    require(GetExitCodeProcess(process.hProcess, &exit_code) && exit_code == 0,
            "Axiom process returned a non-zero lifecycle exit code");
    CloseHandle(process.hProcess);
    process.hProcess = nullptr;
}

void axiom_lifecycle(const std::filesystem::path &executable) {
    (void)close_existing_axiom();
    auto first = launch_axiom(executable);
    HWND window = wait_for_axiom_window();
    require(window != nullptr, "Axiom.exe did not create its window");
    require(WaitForSingleObject(first.hProcess, 0) == WAIT_TIMEOUT,
            "Axiom.exe exited during startup");

    constexpr UINT activate_message = axiom::axiom_activate_message;
    constexpr UINT tray_callback_message = WM_APP + 43;
    PostMessageW(window, activate_message, 0, 0);
    require(wait_for_visibility(window, true), "Axiom activation did not show the palette");
    PostMessageW(window, WM_HOTKEY, 1, 0);
    require(wait_for_visibility(window, false), "Axiom Alt+Space handler did not hide the palette");
    PostMessageW(window, WM_HOTKEY, 1, 0);
    require(wait_for_visibility(window, true),
            "Axiom Alt+Space handler did not restore the palette");
    PostMessageW(window, WM_ACTIVATE, WA_INACTIVE, 0);
    require(wait_for_visibility(window, false),
            "Axiom inactive lifecycle did not hide the palette");
    PostMessageW(window, tray_callback_message, 0, WM_LBUTTONUP);
    require(wait_for_visibility(window, true), "Axiom tray activation did not show the palette");

    auto duplicate = launch_axiom(executable);
    require_process_exit(duplicate, 5s);
    require(FindWindowW(L"AxiomDesktopPaletteWindow", nullptr) == window &&
                WaitForSingleObject(first.hProcess, 0) == WAIT_TIMEOUT,
            "Axiom single-instance activation replaced or duplicated the live instance");

    PostMessageW(window, WM_TIMECHANGE, 0, 0);
    std::this_thread::sleep_for(100ms);
    require(WaitForSingleObject(first.hProcess, 0) == WAIT_TIMEOUT,
            "Axiom exited while handling WM_TIMECHANGE");
    require(PostMessageW(window, axiom::axiom_verification_shutdown_message, 0, 0),
            "Axiom lifecycle instance could not receive clean shutdown request");
    require_process_exit(first, 10s);
    require(FindWindowW(L"AxiomDesktopPaletteWindow", nullptr) == nullptr,
            "Axiom window survived clean exit");

    auto restarted = launch_axiom(executable);
    window = wait_for_axiom_window();
    require(window != nullptr && WaitForSingleObject(restarted.hProcess, 0) == WAIT_TIMEOUT,
            "Axiom clean restart failed");
    require(PostMessageW(window, axiom::axiom_verification_shutdown_message, 0, 0),
            "Axiom restarted instance could not receive clean shutdown request");
    require_process_exit(restarted, 10s);
}

char hex_digit(unsigned value) {
    return static_cast<char>(value < 10 ? '0' + value : 'a' + value - 10);
}

std::string hex_encode(std::string_view input) {
    std::string output;
    output.reserve(input.size() * 2);
    for (const unsigned char ch : input) {
        output.push_back(hex_digit((ch >> 4u) & 0x0fu));
        output.push_back(hex_digit(ch & 0x0fu));
    }
    return output;
}

std::string path_utf8(const std::filesystem::path &path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

std::filesystem::path artifact_path(const std::filesystem::path &destination,
                                    std::wstring_view token, std::wstring_view suffix) {
    return std::filesystem::path{destination.wstring() + L".axiom-restore." + std::wstring{token} +
                                 L"." + std::wstring{suffix}};
}

void write_axrt1(const std::filesystem::path &journal, std::string_view phase,
                 std::string_view token, const std::filesystem::path &destination,
                 const std::filesystem::path &temporary, const std::filesystem::path &backup) {
    std::filesystem::create_directories(journal.parent_path());
    std::ofstream stream{journal, std::ios::binary | std::ios::trunc};
    stream << "AXRT1\n";
    stream << "phase=" << phase << '\n';
    stream << "token=" << token << '\n';
    stream << "count=1\n";
    stream << "item=" << hex_encode("settings.conf") << '|' << hex_encode(path_utf8(destination))
           << '|' << hex_encode(path_utf8(temporary)) << '|' << hex_encode(path_utf8(backup))
           << "|1|1|1|1\n";
    if (!stream)
        throw std::runtime_error{"Unable to write AXRT1 gate fixture."};
}

void plugin_lifecycle(const std::filesystem::path &source_manifest,
                      const std::filesystem::path &source_library,
                      const std::filesystem::path &root) {
    const auto source = root / "plugin-source";
    std::filesystem::create_directories(source);
    std::filesystem::copy_file(source_manifest, source / "hello.axp");
    std::filesystem::copy_file(source_library, source / "AxiomHelloPlugin.dll");

    axiom::PluginTrustStore trust{root / "plugin-trust.bin"};
    (void)trust.load();
    axiom::PluginPackageManager packages{root / "plugins", trust};
    const auto installed = packages.install(source / "hello.axp");
    require(installed.success && installed.plugin_id == "axiom.sample.hello",
            "sample plugin installation failed");
    auto record = trust.find(installed.plugin_id);
    require(record.has_value(), "sample plugin trust record missing");
    require(packages.verify(*record).success, "sample plugin hash/manifest verification failed");
    require(trust.set_enabled(record->id, true), "sample plugin enable failed");

    axiom::ExecutionDiagnostics diagnostics;
    axiom::ActionRegistry actions{&diagnostics};
    axiom::PluginHost host{actions, &diagnostics};
    axiom::PluginLoaderWin32 loader{host, &diagnostics};
    record = trust.find(installed.plugin_id);
    const auto verified = packages.verify(*record);
    const auto loaded = loader.load_manifest(verified.manifest_path);
    require(loaded.success && loader.is_loaded(record->id), "sample plugin DLL load failed");
    const auto command = host.invoke_command(L"hello", L"Windows gate");
    require(command.success && command.message.find(L"Windows gate") != std::wstring::npos,
            "sample plugin command callback failed");
    const auto action = actions.invoke(L"hello-action", L"Windows action gate");
    require(action.success && action.message.find(L"Windows action gate") != std::wstring::npos,
            "sample plugin action callback failed");
    require(loader.unload_plugin(record->id), "sample plugin unload failed");
    require(!host.contains_command(L"hello") && !actions.contains(L"hello-action"),
            "sample plugin callbacks remained visible after unload");
    require(loader.load_manifest(verified.manifest_path).success, "sample plugin reload failed");
    require(loader.unload_plugin(record->id), "sample plugin second unload failed");
    require(trust.set_enabled(record->id, false), "sample plugin disable failed");
    require(packages.remove(record->id).success, "sample plugin removal failed");
    require(!trust.find(record->id).has_value(), "sample plugin trust record survived removal");
}

int dpapi_child(const std::filesystem::path &root) {
    axiom::DpapiDataProtector protector{L"WindowsReleaseGate"};
    axiom::JournalStore live{root / "live" / "journal.bin", 100, &protector, true};
    const auto live_snapshot = live.load();
    require(live_snapshot.entries.size() == 1 &&
                live_snapshot.entries[0].text == L"DPAPI restart payload \u0130stanbul",
            "same-user protected journal did not decrypt after process restart");

    const auto archive = root / "protected.axbak";
    const auto inspected = axiom::LocalDataArchive::inspect(archive, &protector);
    require(inspected.protected_by_os && inspected.entries.size() == 1,
            "same-user protected backup inspection failed after restart");
    const std::vector<axiom::LocalDataFile> destinations{
        {"journal.bin", root / "restored" / "journal.bin"}};
    (void)axiom::LocalDataArchive::restore(archive, destinations, root / "dpapi-restore.journal",
                                           &protector);
    axiom::JournalStore restored{destinations[0].path, 100, &protector, true};
    const auto restored_snapshot = restored.load();
    require(restored_snapshot.entries.size() == 1 &&
                restored_snapshot.entries[0].text == L"DPAPI restart payload \u0130stanbul",
            "protected backup payload did not round-trip losslessly");

    const auto corrupt = root / "corrupt.axbak";
    std::filesystem::copy_file(archive, corrupt, std::filesystem::copy_options::overwrite_existing);
    {
        std::fstream stream{corrupt, std::ios::binary | std::ios::in | std::ios::out};
        stream.seekp(-1, std::ios::end);
        char byte = 0;
        stream.read(&byte, 1);
        stream.seekp(-1, std::ios::end);
        byte ^= 0x5a;
        stream.write(&byte, 1);
    }
    bool rejected = false;
    try {
        (void)axiom::LocalDataArchive::inspect(corrupt, &protector);
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "corrupt DPAPI backup candidate did not fail closed");
    return 0;
}

void dpapi_restart_roundtrip(const std::filesystem::path &root) {
    axiom::DpapiDataProtector protector{L"WindowsReleaseGate"};
    require(protector.available(), "CurrentUser DPAPI is unavailable on Windows");
    const auto live_path = root / "live" / "journal.bin";
    axiom::JournalStore journal{live_path, 100, &protector, true};
    (void)journal.load();
    (void)journal.add_note(L"DPAPI restart payload \u0130stanbul", {L"windows-gate"});
    require(journal.status().protected_at_rest, "live journal was not DPAPI protected");
    const auto raw = read_text(live_path);
    require(raw.find("DPAPI restart payload") == std::string::npos,
            "protected live journal exposed plaintext payload");
    const std::vector<axiom::LocalDataFile> files{{"journal.bin", live_path}};
    const auto backup =
        axiom::LocalDataArchive::create(root / "protected.axbak", files, &protector);
    require(backup.protected_by_os, "backup was not CurrentUser DPAPI protected");
    const std::array arguments{std::wstring{L"--dpapi-child"}, root.wstring()};
    require(spawn_self(arguments) == 0, "DPAPI child-process restart verification failed");
}

int axrt_child(const std::filesystem::path &root, std::wstring_view scenario) {
    const auto journal = root / "restore.journal";
    const auto destination =
        std::filesystem::absolute(root / "live" / "settings.conf").lexically_normal();
    const std::array allowed{destination};
    if (scenario == L"forged") {
        bool rejected = false;
        try {
            (void)axiom::StartupRestoreRecovery::recover_before_store_load(journal, allowed);
        } catch (const std::exception &) {
            rejected = true;
        }
        require(rejected, "forged AXRT1 artifacts did not fail closed before store reads");
        return 0;
    }

    const auto report = axiom::StartupRestoreRecovery::recover_before_store_load(journal, allowed);
    require(report.journal_found && report.recovery.journal_removed,
            "startup AXRT1 recovery did not consume its journal");
    const auto value = read_text(destination);
    if (scenario == L"uncommitted") {
        require(report.recovery.files_rolled_back == 1 && value == "old-settings",
                "uncommitted AXRT1 work was not rolled back after restart");
    } else {
        require(report.recovery.files_rolled_back == 0 && value == "new-settings",
                "committed AXRT1 work was not preserved after restart");
    }
    return 0;
}

void prepare_axrt_scenario(const std::filesystem::path &root, std::string_view phase,
                           std::wstring_view token, bool forged) {
    cleanup(root);
    const auto destination =
        std::filesystem::absolute(root / "live" / "settings.conf").lexically_normal();
    const auto temporary = artifact_path(destination, token, L"new");
    auto backup = artifact_path(destination, token, L"bak");
    write_text(destination, "new-settings");
    write_text(temporary, "stale-temporary");
    write_text(backup, "old-settings");
    if (forged)
        backup = std::filesystem::absolute(root / "forged-backup").lexically_normal();
    write_axrt1(root / "restore.journal", phase, path_utf8(std::filesystem::path{token}),
                destination, temporary, backup);
}

void axrt_restart_recovery(const std::filesystem::path &root) {
    const auto uncommitted = root / "axrt-uncommitted";
    prepare_axrt_scenario(uncommitted, "committing", L"a1-1", false);
    const std::array uncommitted_args{std::wstring{L"--axrt-child"}, uncommitted.wstring(),
                                      std::wstring{L"uncommitted"}};
    require(spawn_self(uncommitted_args) == 0, "uncommitted AXRT1 restart child failed");

    const auto committed = root / "axrt-committed";
    prepare_axrt_scenario(committed, "committed", L"a2-2", false);
    const std::array committed_args{std::wstring{L"--axrt-child"}, committed.wstring(),
                                    std::wstring{L"committed"}};
    require(spawn_self(committed_args) == 0, "committed AXRT1 restart child failed");

    const auto forged = root / "axrt-forged";
    prepare_axrt_scenario(forged, "committing", L"a3-3", true);
    const std::array forged_args{std::wstring{L"--axrt-child"}, forged.wstring(),
                                 std::wstring{L"forged"}};
    require(spawn_self(forged_args) == 0, "forged AXRT1 restart child failed");
}

void complete_exact_resync(axiom::FileIndex &index, axiom::WatcherHealth &health,
                           std::uint64_t issue_generation, bool restart_required) {
    if (restart_required) {
        require(health.mark_restarted(issue_generation), "watcher restart generation was rejected");
    }
    const auto scan_generation = index.rebuild_tracked();
    require(scan_generation != 0, "watcher recovery did not start a full scan generation");
    require(index.wait_for_scan(scan_generation) == axiom::IndexScanWaitResult::succeeded,
            "watcher recovery exact-generation scan failed");
    require(health.mark_resynced(issue_generation), "watcher recovery generation was rejected");
}

void watcher_loss_and_error_recovery(const std::filesystem::path &root) {
    const auto watched = root / "watched";
    std::filesystem::create_directories(watched);
    axiom::FileIndex index;
    index.start({watched});
    const auto initial_generation = index.current_scan_generation();
    require(initial_generation != 0 &&
                index.wait_for_scan(initial_generation) == axiom::IndexScanWaitResult::succeeded,
            "initial watcher gate index scan failed");

    HANDLE directory =
        CreateFileW(watched.c_str(), FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    require(directory != INVALID_HANDLE_VALUE, "ReadDirectoryChangesW gate directory open failed");
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(event != nullptr, "ReadDirectoryChangesW gate event creation failed");
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    std::array<std::byte, sizeof(FILE_NOTIFY_INFORMATION)> tiny_buffer{};
    const BOOL started =
        ReadDirectoryChangesW(directory, tiny_buffer.data(), static_cast<DWORD>(tiny_buffer.size()),
                              TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
                              nullptr, &overlapped, nullptr);
    require(started, "ReadDirectoryChangesW loss probe did not start");
    write_text(watched / std::string(80, 'x'), "overflow-event");
    require(WaitForSingleObject(event, 5'000) == WAIT_OBJECT_0,
            "ReadDirectoryChangesW loss probe did not complete");
    DWORD bytes = 1;
    const BOOL completed = GetOverlappedResult(directory, &overlapped, &bytes, FALSE);
    const DWORD overflow_error = completed ? ERROR_NOTIFY_ENUM_DIR : GetLastError();
    CloseHandle(event);
    CloseHandle(directory);
    require(completed && bytes == 0,
            "real ReadDirectoryChangesW undersized-buffer loss did not return zero bytes");

    axiom::WatcherHealth health;
    health.started(1);
    const auto overflow_generation = health.note_overflow(overflow_error);
    complete_exact_resync(index, health, overflow_generation, false);

    std::array<std::byte, 256> buffer{};
    DWORD ignored = 0;
    SetLastError(ERROR_SUCCESS);
    const BOOL invalid_result = ReadDirectoryChangesW(
        INVALID_HANDLE_VALUE, buffer.data(), static_cast<DWORD>(buffer.size()), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME, &ignored, nullptr, nullptr);
    const DWORD generic_error = GetLastError();
    require(!invalid_result && generic_error != ERROR_SUCCESS &&
                generic_error != ERROR_NOTIFY_ENUM_DIR,
            "real ReadDirectoryChangesW generic-error probe did not fail generically");
    const auto error_generation = health.note_error(generic_error);

    axiom::FileSystemWatcher watcher;
    watcher.start({watched}, [](const axiom::FileChange &) {}, [](const axiom::WatcherIssue &) {});
    require(watcher.watch_count() == 1, "watcher restart did not restore the valid root");
    complete_exact_resync(index, health, error_generation, true);
    watcher.stop();
    index.stop();

    const auto stale = health.note_overflow(ERROR_NOTIFY_ENUM_DIR);
    const auto newer = health.note_error(ERROR_INVALID_HANDLE);
    require(newer > stale && !health.mark_resynced(stale),
            "stale watcher recovery generation cleared a newer issue");
}

void support_bundle_win32_publication(const std::filesystem::path &root) {
    const auto path = root / "support" / "bundle.txt";
    const std::vector<axiom::SupportField> fields{
        {"product-version", "0.16.0", false},
        {"local-data-root", "C:/Users/example/Axiom", true},
    };
    const std::vector<axiom::SupportDiagnostic> diagnostics{
        {1, 100, "warning", "runtime", "gate", "private diagnostic"},
    };
    const auto first = axiom::SupportBundle::create(path, fields, diagnostics);
    require(first.sensitive_fields_omitted == 1 && first.diagnostic_messages_omitted == 1,
            "Win32 support bundle default redaction failed");
    auto text = read_text(path);
    require(text.find("private diagnostic") == std::string::npos &&
                text.find("local-data-root") == std::string::npos,
            "Win32 support bundle leaked default-redacted fields");

    axiom::SupportBundleOptions options;
    options.include_sensitive_fields = true;
    options.include_diagnostic_messages = true;
    (void)axiom::SupportBundle::create(path, fields, diagnostics, options);
    text = read_text(path);
    require(text.find("private diagnostic") != std::string::npos &&
                text.find("local-data-root") != std::string::npos,
            "Win32 support bundle explicit opt-ins failed");

    const auto blocked = root / "support" / "blocked.txt";
    std::filesystem::create_directories(blocked);
    bool rejected = false;
    try {
        (void)axiom::SupportBundle::create(blocked, fields, diagnostics);
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "Win32 support bundle publication failure was not surfaced");
    for (const auto &entry : std::filesystem::directory_iterator{blocked.parent_path()}) {
        require(!entry.path().filename().wstring().starts_with(L"blocked.txt.tmp."),
                "Win32 support bundle failure left a temporary artifact");
    }
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 3 && std::wstring_view{argv[1]} == L"--dpapi-child") {
            return dpapi_child(argv[2]);
        }
        if (argc == 4 && std::wstring_view{argv[1]} == L"--axrt-child") {
            return axrt_child(argv[2], argv[3]);
        }
        if (argc == 2 && std::wstring_view{argv[1]} == L"--close-existing") {
            (void)close_existing_axiom();
            return 0;
        }
        if (argc != 4) {
            std::wcerr << L"AxiomWindowsReleaseGatesTests arguments (" << argc << L"):";
            for (int index = 0; index < argc; ++index)
                std::wcerr << L" [" << argv[index] << L"]";
            std::wcerr << L'\n';
            throw std::invalid_argument{
                "Usage: AxiomWindowsReleaseGatesTests <sample-manifest> <sample-dll> <Axiom.exe>"};
        }

        const auto root =
            std::filesystem::temp_directory_path() /
            (L"axiom_windows_release_gates_" + std::to_wstring(GetCurrentProcessId()));
        cleanup(root);
        std::filesystem::create_directories(root);
        try {
            axiom_lifecycle(argv[3]);
            plugin_lifecycle(argv[1], argv[2], root / "plugin-gate");
            dpapi_restart_roundtrip(root / "dpapi-gate");
            watcher_loss_and_error_recovery(root / "watcher-gate");
            axrt_restart_recovery(root);
            support_bundle_win32_publication(root);
        } catch (...) {
            cleanup(root);
            throw;
        }
        cleanup(root);
        std::cout << "AxiomWindowsReleaseGatesTests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "AxiomWindowsReleaseGatesTests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}

#endif
