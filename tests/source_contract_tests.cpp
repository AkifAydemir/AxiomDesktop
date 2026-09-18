#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#ifndef AXIOM_SOURCE_DIR
#error AXIOM_SOURCE_DIR must be defined by CMake.
#endif
namespace {
std::string read_all(const std::filesystem::path &path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error{"Unable to open source contract file: " + path.string()};
    }
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}
std::string without_source_layout(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto character = static_cast<unsigned char>(text[index]);
        if (!std::isspace(character)) {
            if (character == 'L' && index + 1 < text.size() && text[index + 1] == '"') {
                continue;
            }
            if (character == '"') {
                continue;
            }
            result.push_back(static_cast<char>(character));
        }
    }
    return result;
}
void expect_contains(std::string_view text, std::string_view needle, std::string_view message) {
    if (text.find(needle) == std::string_view::npos &&
        without_source_layout(text).find(without_source_layout(needle)) == std::string::npos) {
        throw std::runtime_error{std::string{message}};
    }
}
void run_tests() {
    const std::filesystem::path root{AXIOM_SOURCE_DIR};
    const auto header = read_all(root / "src" / "axiom.hpp");
    const auto source = read_all(root / "src" / "axiom.cpp");
    const auto plugin_api = read_all(root / "src" / "plugin_api.hpp");
    const auto plugin_loader = read_all(root / "src" / "plugin_loader_win32.cpp");
    const auto cmake = read_all(root / "CMakeLists.txt");
    const auto automation_store = read_all(root / "src" / "automation_store.cpp");
    const auto automation_schedule = read_all(root / "src" / "automation_schedule.cpp");
    const auto file_index_header = read_all(root / "src" / "file_index.hpp");
    const auto file_index_source = read_all(root / "src" / "file_index.cpp");
    const auto index_scan_gate_header = read_all(root / "src" / "index_scan_gate.hpp");

    const auto watcher_header = read_all(root / "src" / "filesystem_watcher.hpp");
    const auto watcher_source = read_all(root / "src" / "filesystem_watcher.cpp");
    const auto watcher_health_header = read_all(root / "src" / "watcher_health.hpp");
    const auto watcher_resync_source = read_all(root / "src" / "watcher_resync.cpp");
    expect_contains(
        header, "BoundedExecutor& action_executor,\n ExecutionDiagnostics& execution_diagnostics,",
        "v14 CommandEngine executor/diagnostics dependencies missing");
    expect_contains(header, "PluginHost& plugin_host,\n AutomationEngine& automation_engine,",
                    "CommandEngine header contract is missing plugin/automation dependencies");
    expect_contains(header,
                    "using PluginHandler = std::function<CommandResult(std::wstring_view)>;",
                    "v12 plugin management handler contract missing");
    expect_contains(source, "PluginHandler plugin_handler,",
                    "v12 CommandEngine plugin handler source wiring missing");
    expect_contains(source, "plugin_host_{plugin_host},\n automation_engine_{automation_engine}",
                    "CommandEngine constructor initializer contract is inconsistent");
    expect_contains(source, "action_executor_,\n execution_diagnostics_,",
                    "v14 App must pass executor/diagnostics dependencies to CommandEngine");
    expect_contains(source, "plugin_host_,\n automation_engine_,",
                    "App must pass plugin/automation dependencies to CommandEngine");
    expect_contains(source, "(void)journal_store_.load();",
                    "App startup must restore persistent journal before product callbacks");
    expect_contains(source, "register_builtin_actions();",
                    "App startup must register built-in actions before automation restore");
    expect_contains(source, "const auto automation_restore = automation_engine_.restore();",
                    "App startup must restore persistent automations");
    expect_contains(source, "journal_protector_{L\"JournalLive\"}",
                    "v13 journal-specific protector scope must be wired");
    expect_contains(source, "settings_.journal_live_protection_enabled",
                    "v13 journal protection preference must come from unified settings");
    expect_contains(source, "&journal_protector_",
                    "v13 JournalStore must receive the live-store protector");
    expect_contains(source, "apply_journal_retention();",
                    "App startup must apply journal retention policy");
    expect_contains(source, "LocalDataArchive::create",
                    "v10 local-data backup command must be wired");
    expect_contains(source, "LocalDataArchive::restore",
                    "v10 local-data restore command must be wired");
    expect_contains(source, "export_journal", "v10 journal export must be wired");
    expect_contains(source, "import_journal", "v10 journal import must be wired");
    expect_contains(header, "using DataHandler = std::function<CommandResult(std::wstring_view)>;",
                    "v10 data handler contract missing from header");
    expect_contains(source, "DataHandler data_handler,",
                    "v10 data handler constructor wiring missing from source");
    expect_contains(source, "DpapiDataProtector protector;",
                    "v10 Windows DPAPI backup protector must remain wired");
    expect_contains(header, "using JournalContextPolicy = std::function<bool()>;",
                    "v10 independent context privacy policy missing");
    expect_contains(source, "settings_.journal_context_enabled",
                    "v10 context privacy switch must be wired");
    expect_contains(header, "PluginHost& plugin_host,",
                    "v11 PluginHost dependency missing from CommandEngine");
    expect_contains(header, "PluginLoaderWin32 plugin_loader_;",
                    "v11 Windows plugin loader member missing");
    expect_contains(source, "std::filesystem::path default_plugin_directory()",
                    "v11 plugin directory helper missing");
    expect_contains(source, "if (settings_.plugins_enabled)",
                    "v11 plugin loading must be disabled-by-default and settings-gated");
    expect_contains(header, "PluginTrustStore plugin_trust_store_;",
                    "v12 per-plugin trust store member missing");
    expect_contains(header, "PluginPackageManager plugin_packages_;",
                    "v12 managed plugin package member missing");
    expect_contains(source, "plugin_trust_store_{default_plugin_trust_path()}",
                    "v12 trust metadata path wiring missing");
    expect_contains(source, "plugin_packages_{default_plugin_directory(), plugin_trust_store_}",
                    "v12 managed plugin root wiring missing");
    expect_contains(source, "plugin_packages_.verify(record)",
                    "v12 pinned package verification missing from load path");
    expect_contains(source, "plugin_loader_.load_manifest(verification.manifest_path)",
                    "v12 loader must consume verified managed manifest");
    expect_contains(source, "plugin_trust_store_.quarantine",
                    "v12 hash/identity failures must quarantine plugins");
    expect_contains(source, "plugin install <path-to-.axp>",
                    "v12 explicit install command missing");
    expect_contains(source, "plugin enable <plugin-id>", "v12 explicit enable command missing");
    expect_contains(source, "plugin disable <plugin-id>", "v12 explicit disable command missing");
    expect_contains(source, "plugin verify <plugin-id|all>", "v12 verify command missing");
    expect_contains(source, "plugin reload <plugin-id|all>", "v12 reload command missing");
    expect_contains(source, "plugin remove <plugin-id>", "v12 remove command missing");
    expect_contains(source, "automation_engine_.reconcile_action_availability()",
                    "v12 plugin automation parking/re-arm reconciliation missing");
    expect_contains(source, "plugin_loader_.unload_all();",
                    "v11 plugin loader must support explicit runtime unload");
    expect_contains(source, "plugin_loader_.unload_all();",
                    "v11 plugin unload must run before process teardown");
    expect_contains(source, "plugin_host_.contains_command(name)",
                    "v11 plugin command dispatch missing");
    expect_contains(source, "CommandResult CommandEngine::plugin_command",
                    "plugin command dispatch missing");
    expect_contains(source, "plugin_handler_(arguments)",
                    "v12 plugin command must delegate to App management handler");
    expect_contains(source, "plugin_host_.reserve_command_name(command.name);",
                    "v11 built-in commands must be protected from plugin shadowing");
    expect_contains(plugin_api, "AXIOM_PLUGIN_ABI_V1", "v11 versioned plugin ABI constant missing");
    expect_contains(plugin_loader, "descriptor_matches",
                    "v11 plugin loader descriptor validation missing");
    expect_contains(plugin_loader, "descriptor.requested_capabilities & ~manifest.capabilities",
                    "v11 loader capability subset validation missing");
    expect_contains(plugin_loader, "LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR",
                    "plugin DLL search isolation flag missing");
    expect_contains(plugin_loader, "host_.unregister_plugin(id);",
                    "v14 loader must close/wait callback gate before FreeLibrary");
    const auto plugin_host = read_all(root / "src" / "plugin_host.cpp");
    expect_contains(plugin_host, "gate->accepting = false",
                    "v12 callback gate must reject new calls during unload");
    expect_contains(plugin_host, "gate->idle.wait", "v12 callback gate must wait for active calls");
    const auto trust_store = read_all(root / "src" / "plugin_trust_store.cpp");
    expect_contains(trust_store, "checksum=", "v12 trust metadata must be checksummed");
    expect_contains(trust_store, "recovered_from_alternate_",
                    "v12 trust metadata must support alternate recovery");
    const auto journal_store = read_all(root / "src" / "journal_store.cpp");
    const auto settings_header = read_all(root / "src" / "app_settings.hpp");
    const auto settings_source = read_all(root / "src" / "app_settings.cpp");
    const auto local_data_source = read_all(root / "src" / "local_data_archive.cpp");
    const auto restore_transaction_source = read_all(root / "src" / "restore_transaction.cpp");
    const auto startup_restore_source = read_all(root / "src" / "startup_restore_recovery.cpp");
    const auto support_bundle_header = read_all(root / "src" / "support_bundle.hpp");
    const auto support_bundle_source = read_all(root / "src" / "support_bundle.cpp");
    const auto release_verification_header = read_all(root / "src" / "release_verification.hpp");
    const auto release_verification_source = read_all(root / "src" / "release_verification.cpp");
    const auto windows_verification_tool = read_all(root / "tools" / "windows_verification.cpp");
    expect_contains(journal_store, "protected_magic",
                    "v13 AXJP protected journal envelope missing");
    expect_contains(journal_store,
                    "Journal store candidates exist but none are valid or decryptable.",
                    "v13 journal corruption must fail closed");
    expect_contains(journal_store, "remove_plaintext_alternates",
                    "v13 migration must clean plaintext journal alternates");
    expect_contains(
        source, "journal_store_.plaintext_snapshot_bytes",
        "v13 portable backup must export inner AXJR snapshot instead of DPAPI-bound live bytes");
    expect_contains(source, "journal_store_.set_protection_enabled",
                    "v13 explicit journal protection toggle missing");
    expect_contains(settings_source, "version=5", "v13 unified settings schema v5 missing");
    expect_contains(settings_source, "journal_live_protection_enabled=",
                    "v13 journal protection preference persistence missing");
    expect_contains(local_data_source, "AxiomDesktop.\" + purpose_ + L\".v1",
                    "v13 DPAPI purpose separation missing");
    expect_contains(
        local_data_source, "RestoreTransaction::commit",
        "v16 WIP local-data restore must commit through the transactional restore layer");
    expect_contains(local_data_source, "explicit recovery journal path",
                    "v16 WIP local-data restore must require explicit recovery metadata location");
    expect_contains(restore_transaction_source, "state.committed = true",
                    "v16 WIP restore transaction durable commit marker missing");
    expect_contains(restore_transaction_source, "forged artifact paths",
                    "v16 WIP restore recovery must reject forged artifact paths");
    expect_contains(startup_restore_source, "recover_before_store_load",
                    "v16 WIP startup restore recovery entry point missing");
    expect_contains(header, "StartupRestoreRecoveryReport startup_restore_recovery_{};",
                    "v16 WIP startup recovery state must be declared before AppSettingsStore");
    expect_contains(source,
                    "startup_restore_recovery_{recover_default_local_data_before_store_load()},"
                    "\napp_settings_store_{default_app_settings_path()}",
                    "v16 WIP restore recovery must run before settings are loaded");
    expect_contains(source, "default_restore_recovery_journal_path()",
                    "v16 WIP product restore recovery journal helper missing");
    expect_contains(source, "default_restore_destinations()",
                    "v16 WIP startup recovery destination allowlist missing");
    expect_contains(source, "restore-recovery-startup=",
                    "v16 WIP startup recovery observability missing from data status");
    expect_contains(cmake, "src/restore_transaction.cpp",
                    "v16 WIP restore transaction implementation missing from AxiomLocalData");
    expect_contains(cmake, "src/startup_restore_recovery.cpp",
                    "v16 WIP startup restore recovery implementation missing from AxiomLocalData");
    expect_contains(cmake, "AxiomRestoreTransactionTests",
                    "v16 WIP restore transaction regression target missing");
    expect_contains(cmake, "AxiomStartupRestoreRecoveryTests",
                    "v16 WIP startup recovery regression target missing");
    expect_contains(cmake, "src/support_bundle.cpp",
                    "v16 WIP support bundle implementation missing from build");
    expect_contains(cmake, "AxiomSupportBundleTests",
                    "v16 WIP support bundle regression target missing");
    expect_contains(support_bundle_header, "include_sensitive_fields",
                    "v16 WIP support bundle sensitive-field opt-in contract missing");
    expect_contains(support_bundle_header, "include_diagnostic_messages",
                    "v16 WIP support bundle diagnostic-message opt-in contract missing");
    expect_contains(support_bundle_source, "privacy=explicit-opt-in",
                    "v16 WIP support bundle explicit privacy marker missing");
    expect_contains(support_bundle_source, "O_EXCL",
                    "v16 WIP support bundle exclusive temporary publication missing");
    expect_contains(support_bundle_source, "0600",
                    "v16 WIP POSIX support bundle temporary must be owner-only");
    expect_contains(source, "action == L\"support-bundle\"",
                    "v16 WIP support bundle command surface missing");
    expect_contains(source, "parse_support_bundle_arguments",
                    "v16 WIP support bundle explicit option parser missing");
    expect_contains(source, "execution_diagnostics_.recent(256)",
                    "v16 WIP support bundle diagnostics snapshot wiring missing");
    expect_contains(source, "support_bundle_root_limit = 32",
                    "v16 WIP support bundle index-root export must remain bounded");
    expect_contains(source, "settings_.journal_live_protection_enabled",
                    "v16 WIP support bundle must integrate current settings metadata");
    expect_contains(source,
                    "Raw action payloads, clipboard contents, journal entries, backup payload",
                    "v16 WIP support bundle privacy surface prefix missing");
    expect_contains(source, "plugin binaries are never bundle inputs.",
                    "v16 WIP support bundle privacy surface suffix missing");
    expect_contains(cmake, "src/release_verification.cpp",
                    "v16 WIP release verification implementation missing from build");
    expect_contains(cmake, "AxiomReleaseVerificationTests",
                    "v16 WIP release verification regression target missing");
    expect_contains(cmake, "AxiomWindowsVerification --checklist",
                    "v16 WIP Windows verification checklist smoke test missing");
    expect_contains(release_verification_header, "std::array<ReleaseVerificationGateEvidence, 8>",
                    "v16 WIP Windows verification ledger must remain bounded to eight gates");
    expect_contains(release_verification_source, "msvc-release-build",
                    "v16 WIP MSVC build gate missing");
    expect_contains(release_verification_source, "axiom-lifecycle",
                    "v16 WIP Axiom lifecycle gate missing");
    expect_contains(release_verification_source, "sample-plugin-lifecycle",
                    "v16 WIP sample plugin lifecycle gate missing");
    expect_contains(release_verification_source, "current-user-dpapi-roundtrip",
                    "v16 WIP DPAPI gate missing");
    expect_contains(release_verification_source, "watcher-overflow-resync",
                    "v16 WIP watcher recovery gate missing");
    expect_contains(release_verification_source, "wm-timechange-dst",
                    "v16 WIP local time/DST gate missing");
    expect_contains(release_verification_source, "axrt1-restart-recovery",
                    "v16 WIP AXRT1 restart gate missing");
    expect_contains(release_verification_source, "support-bundle-win32-publish",
                    "v16 WIP SupportBundle Win32 gate missing");
    expect_contains(windows_verification_tool,
                    "PASS/FAIL evidence recording is disabled on non-Windows builds.",
                    "v16 WIP non-Windows verification evidence must fail closed");
    expect_contains(windows_verification_tool, "--attest-executed-on-windows",
                    "v16 WIP Windows evidence recording requires explicit execution attestation");
    expect_contains(windows_verification_tool, "No gate is implied PASS by this checklist.",
                    "v16 WIP verification checklist must not imply PASS");
    const auto runtime_header = read_all(root / "src" / "runtime.hpp");
    const auto runtime_source = read_all(root / "src" / "runtime.cpp");
    const auto automation_source = read_all(root / "src" / "automation_engine.cpp");
    const auto action_source = read_all(root / "src" / "action_registry.cpp");
    const auto diagnostics_header = read_all(root / "src" / "execution_diagnostics.hpp");
    expect_contains(runtime_header, "class BoundedExecutor final",
                    "v14 bounded execution boundary missing");
    expect_contains(runtime_source, "queue_.size() >= queue_capacity_",
                    "v14 executor queue must reject saturation instead of growing without bound");
    expect_contains(automation_source, "state->executor->submit",
                    "v14 automation actions must execute off the scheduler timing worker");
    expect_contains(automation_source, "Automation execution queue is full or stopping.",
                    "v14 automation queue rejection diagnostic missing");
    expect_contains(header, "ExecutionDiagnostics execution_diagnostics_{512};",
                    "v14 bounded diagnostics member missing");
    expect_contains(header, "BoundedExecutor action_executor_{2, 64};",
                    "v14 bounded action executor member missing");
    expect_contains(source, "action_executor_.stop(true);\n plugin_loader_.unload_all();",
                    "v14 executor must drain before plugin DLL unload");
    expect_contains(source, "CommandResult CommandEngine::diagnostics_command",
                    "v14 diagnostics command surface missing");
    expect_contains(source, "Diagnostics intentionally omit action payloads",
                    "v14 diagnostics privacy contract missing from user surface");
    expect_contains(action_source, "DiagnosticDomain::action",
                    "v14 action execution diagnostics missing");
    expect_contains(action_source, "DiagnosticDomain::plugin",
                    "v14 plugin-owned action aggregation missing");
    expect_contains(plugin_host, "host_api_.log", "v14 plugin host log callback must be wired");
    expect_contains(plugin_loader, "LoadDiagnosticRecorder",
                    "v14 plugin load failure history missing");
    expect_contains(diagnostics_header, "capacity = 512",
                    "v14 diagnostics history must remain bounded and session-scoped by default");
    expect_contains(cmake, "project(AxiomDesktop VERSION 0.16.0",
                    "v16 project version must be 0.16.0");
    expect_contains(cmake, "src/index_scan_gate.cpp",
                    "v16 WIP FileIndex scan gate implementation missing from AxiomIndex");
    expect_contains(cmake, "AxiomIndexScanGateTests",
                    "v16 WIP IndexScanGate regression target missing");
    expect_contains(file_index_header, "rebuild_tracked()",
                    "v16 WIP tracked FileIndex rebuild API missing");
    expect_contains(file_index_header, "wait_for_scan(",
                    "v16 WIP event-driven FileIndex wait API missing");
    expect_contains(file_index_source, "scan_gate_.complete(generation, true)",
                    "v16 WIP FileIndex successful scan completion signal missing");
    expect_contains(file_index_source, "scan_gate_.cancel(generation)",
                    "v16 WIP FileIndex cancellation/supersession signal missing");
    expect_contains(index_scan_gate_header, "std::condition_variable_any",
                    "v16 WIP event-driven IndexScanGate primitive missing");
    expect_contains(cmake, "src/watcher_health.cpp",
                    "v16 WIP watcher health implementation missing from build");
    expect_contains(cmake, "src/watcher_resync.cpp",
                    "v16 WIP watcher resync implementation missing from build");
    expect_contains(cmake, "AxiomWatcherHealthTests",
                    "v16 WIP watcher health regression target missing");
    expect_contains(cmake, "AxiomWatcherResyncTests",
                    "v16 WIP watcher resync regression target missing");
    expect_contains(cmake, "AxiomWatcherWin32ContractTests",
                    "v16 WIP Win32 watcher contract target missing");
    expect_contains(watcher_header, "using IssueCallback",
                    "v16 WIP watcher issue callback contract missing");
    expect_contains(watcher_source, "bytes == 0",
                    "v16 WIP zero-byte watcher overflow path missing");
    expect_contains(watcher_source, "ERROR_NOTIFY_ENUM_DIR",
                    "v16 WIP watcher enumeration-loss path missing");
    expect_contains(watcher_health_header, "request_resync()",
                    "v16 WIP explicit watcher resync generation API missing");
    expect_contains(watcher_resync_source, "Do not synthesize retries from health state here",
                    "v16 WIP no-hot-loop watcher recovery contract missing");
    expect_contains(header, "WatcherResyncCoordinator watcher_resync_;",
                    "v16 WIP App watcher resync coordinator member missing");
    expect_contains(source, "handle_watcher_issue(issue);",
                    "v16 WIP watcher issue App wiring missing");
    expect_contains(source, "file_index_.rebuild_tracked()",
                    "v16 WIP watcher resync must use tracked FileIndex rebuild");
    expect_contains(source, "file_index_.wait_for_scan(scan_generation, stop_token)",
                    "v16 WIP watcher resync must wait for the exact full scan generation");
    expect_contains(source, "watcher_health_.mark_restarted(issue_generation)",
                    "v16 WIP generic watcher error must confirm restart before health recovery");
    expect_contains(source, "action == L\"resync\"",
                    "v16 WIP explicit index resync command missing");
    expect_contains(cmake, "src/automation_schedule.cpp",
                    "v15 calendar/retry schedule implementation missing from build");
    expect_contains(cmake, "AxiomAutomationScheduleTests",
                    "v15 automation schedule regression target missing");
    expect_contains(automation_store, "constexpr std::uint32_t format_version = 2",
                    "v15 AutomationStore v2 format missing");
    expect_contains(automation_store, "constexpr std::uint32_t legacy_format_version = 1",
                    "v15 AutomationStore v1 compatibility missing");
    expect_contains(automation_store, "AutomationScheduleKind::local_calendar",
                    "v15 persisted local calendar schedule missing");
    expect_contains(automation_schedule, "schedule.time_zone != L\"local\"",
                    "v15 named zones must fail closed; only explicit local is supported");
    expect_contains(automation_schedule, "retry_delay_for",
                    "v15 bounded retry/backoff helper missing");
    expect_contains(automation_source, "schedule_calendar",
                    "v15 calendar schedule engine wiring missing");
    expect_contains(automation_source, "retry_resume_at",
                    "v15 durable recurring retry resume state missing");
    expect_contains(automation_source, "post-retry schedule could not be resolved",
                    "v15 post-retry stale-occurrence skip contract missing");
    expect_contains(automation_source, "rebase_local_calendar_state",
                    "v15 local-calendar restore/enable rebase contract missing");
    expect_contains(automation_source, "refresh_calendar_schedules",
                    "v15 local-calendar live refresh contract missing");
    expect_contains(source,
                    "case WM_TIMECHANGE:", "v15 Windows time-change calendar refresh hook missing");
    expect_contains(source, "automation_engine_.refresh_calendar_schedules()",
                    "v15 WM_TIMECHANGE must reconcile local calendar schedules");
    expect_contains(
        source,
        "auto "
        "[list|actions|status|show|in|at|every|daily|weekly|policy|enable|disable|cancel|clear]",
        "v15 automation command help missing calendar/policy surface");
    expect_contains(source, "action == L\"daily\" || action == L\"weekly\"",
                    "v15 daily/weekly command dispatch missing");
    expect_contains(source, "automation_engine_.schedule_calendar",
                    "v15 calendar command must delegate to AutomationEngine");
    expect_contains(source, "automation_engine_.set_failure_policy",
                    "v15 failure policy command must delegate to AutomationEngine");
    expect_contains(source, "schedule.time_zone = L\"local\"",
                    "v15 command calendar timezone must be explicit local");
    expect_contains(source,
                    "retry-policy=", "v15 sys/status surface must expose retry policy counts");
    expect_contains(header, "struct CommandSuggestion",
                    "productization command suggestion metadata surface missing");
    expect_contains(header, "std::vector<CommandTopic> topics;",
                    "productization subcommand metadata must live inside CommandEngine registry");
    expect_contains(source, "Axiom command library",
                    "productization command library surface missing");
    expect_contains(source, "Tab completes", "productization keyboard completion hint missing");
    expect_contains(source, "command_engine_.complete(current_input())",
                    "productization Tab completion wiring missing");
    expect_contains(source, "plugin_host_.list_commands()",
                    "productization help/suggestions must include loaded plugin command metadata");
    expect_contains(source,
                    "settings theme <system|light|dark|oled|graphite|midnight|nord|high-contrast>",
                    "productization theme command surface missing");
    expect_contains(source, "LBS_OWNERDRAWFIXED",
                    "productization native result hierarchy/selection drawing missing");
    expect_contains(
        source, "case WM_DRAWITEM:", "productization result-list visual hierarchy drawing missing");
    expect_contains(source, "case WM_THEMECHANGED:",
                    "productization System/High Contrast refresh hook missing");
    expect_contains(settings_header, "AppTheme theme{AppTheme::system};",
                    "productization persistent theme setting missing");
    expect_contains(settings_source, "version=6",
                    "productization settings schema v6 writer missing");
    expect_contains(settings_source, "theme=", "productization theme persistence missing");
    expect_contains(settings_source, "AppTheme::nord",
                    "productization built-in Nord palette setting missing");
    expect_contains(settings_source, "AppTheme::high_contrast",
                    "productization high-contrast palette setting missing");
}
} // namespace
int main() {
    try {
        run_tests();
        std::cout << "AxiomSourceContractTests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "AxiomSourceContractTests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
