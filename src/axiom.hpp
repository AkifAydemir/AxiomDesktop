#pragma once
#include <windows.h>
#include "action_registry.hpp"
#include "app_settings.hpp"
#include "automation_engine.hpp"
#include "automation_store.hpp"
#include "clipboard_store.hpp"
#include "execution_diagnostics.hpp"
#include "file_index.hpp"
#include "filesystem_watcher.hpp"
#include "journal_store.hpp"
#include "local_data_archive.hpp"
#include "notification_center.hpp"
#include "plugin_host.hpp"
#include "plugin_loader_win32.hpp"
#include "plugin_package.hpp"
#include "plugin_trust_store.hpp"
#include "runtime.hpp"
#include "startup_restore_recovery.hpp"
#include "system_metrics.hpp"
#include "watcher_health.hpp"
#include "watcher_resync.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace axiom {
inline constexpr wchar_t axiom_window_class_name[] = L"AxiomDesktopPaletteWindow";
inline constexpr UINT axiom_activate_message = WM_APP + 42;
inline constexpr UINT axiom_verification_shutdown_message = WM_APP + 44;
enum class ResultKind {
    info,
    heading,
    success,
    warning,
    error,
};
struct ResultLine {
    std::wstring text;
    ResultKind kind{ResultKind::info};
};
struct CommandResult {
    std::vector<ResultLine> lines;
    bool request_exit{false};
};
struct CommandSuggestion {
    std::wstring completion;
    std::wstring display;
    ResultKind kind{ResultKind::info};
};
class CommandEngine {
  public:
    using IndexConfigurationChanged = std::function<void()>;
    using IndexRecoveryHandler = std::function<CommandResult(std::wstring_view)>;
    using AppSettingsHandler = std::function<CommandResult(std::wstring_view)>;
    using DataHandler = std::function<CommandResult(std::wstring_view)>;
    using PluginHandler = std::function<CommandResult(std::wstring_view)>;
    using JournalContextPolicy = std::function<bool()>;
    CommandEngine(FileIndex &file_index, ClipboardStore &clipboard_store,
                  ReminderCenter &reminder_center, RuntimeScheduler &runtime_scheduler,
                  BoundedExecutor &action_executor, ExecutionDiagnostics &execution_diagnostics,
                  SystemMetricsSampler &system_metrics, NotificationCenter &notification_center,
                  JournalStore &journal_store, ActionRegistry &action_registry,
                  PluginHost &plugin_host, AutomationEngine &automation_engine,
                  IndexConfigurationChanged index_configuration_changed = {},
                  IndexRecoveryHandler index_recovery_handler = {},
                  AppSettingsHandler app_settings_handler = {}, DataHandler data_handler = {},
                  PluginHandler plugin_handler = {},
                  JournalContextPolicy journal_context_policy = {});
    [[nodiscard]] CommandResult execute(std::wstring_view input) const;
    [[nodiscard]] bool is_command_name(std::wstring_view name) const noexcept;
    [[nodiscard]] std::vector<CommandSuggestion> suggestions(std::wstring_view input,
                                                             std::size_t limit = 16) const;
    [[nodiscard]] std::optional<std::wstring> complete(std::wstring_view input) const;

  private:
    using Handler = std::function<CommandResult(std::wstring_view)>;
    struct CommandTopic {
        std::wstring name;
        std::wstring usage;
        std::wstring summary;
        std::wstring example;
    };
    struct Command {
        std::wstring name;
        std::wstring category;
        std::wstring usage;
        std::wstring summary;
        std::wstring example;
        std::vector<CommandTopic> topics;
        Handler handler;
    };
    FileIndex &file_index_;
    ClipboardStore &clipboard_store_;
    ReminderCenter &reminder_center_;
    RuntimeScheduler &runtime_scheduler_;
    BoundedExecutor &action_executor_;
    ExecutionDiagnostics &execution_diagnostics_;
    SystemMetricsSampler &system_metrics_;
    NotificationCenter &notification_center_;
    JournalStore &journal_store_;
    ActionRegistry &action_registry_;
    PluginHost &plugin_host_;
    AutomationEngine &automation_engine_;
    IndexConfigurationChanged index_configuration_changed_;
    IndexRecoveryHandler index_recovery_handler_;
    AppSettingsHandler app_settings_handler_;
    DataHandler data_handler_;
    PluginHandler plugin_handler_;
    JournalContextPolicy journal_context_policy_;
    std::vector<Command> commands_;
    [[nodiscard]] const Command *find(std::wstring_view name) const noexcept;
    [[nodiscard]] CommandResult help(std::wstring_view arguments) const;
    [[nodiscard]] CommandResult search_files(std::wstring_view query) const;
    [[nodiscard]] CommandResult index_command(std::wstring_view arguments) const;
    [[nodiscard]] CommandResult clipboard_command(std::wstring_view arguments) const;
    [[nodiscard]] CommandResult reminder_command(std::wstring_view arguments) const;
    [[nodiscard]] CommandResult system_command(std::wstring_view arguments) const;
    [[nodiscard]] CommandResult diagnostics_command(std::wstring_view arguments) const;
    [[nodiscard]] CommandResult notification_command(std::wstring_view arguments) const;
    [[nodiscard]] CommandResult journal_command(std::wstring_view arguments) const;
    [[nodiscard]] CommandResult automation_command(std::wstring_view arguments) const;
    [[nodiscard]] CommandResult plugin_command(std::wstring_view arguments) const;
};
class App final {
  public:
    explicit App(HINSTANCE instance);
    ~App();
    App(const App &) = delete;
    App &operator=(const App &) = delete;
    int run();

  private:
    static constexpr int hotkey_id = 1;
    static constexpr UINT_PTR edit_id = 1001;
    static constexpr UINT_PTR result_list_id = 1002;
    static constexpr UINT_PTR live_search_timer_id = 2001;
    static constexpr UINT live_search_debounce_ms = 35;
    static constexpr UINT notification_pump_timer_id = 2002;
    static constexpr UINT notification_pump_interval_ms = 1200;
    static constexpr UINT notification_ready_message = WM_APP + 41;
    static constexpr UINT tray_callback_message = WM_APP + 43;
    static constexpr UINT tray_icon_id = 1;
    static constexpr UINT tray_open_command_id = 3001;
    static constexpr UINT tray_startup_command_id = 3002;
    static constexpr UINT tray_exit_command_id = 3003;
    HINSTANCE instance_{};
    HWND window_{};
    HWND edit_{};
    HWND results_{};
    HFONT font_{};
    HFONT heading_font_{};
    HBRUSH background_brush_{};
    HBRUSH surface_brush_{};
    COLORREF palette_background_{};
    COLORREF palette_surface_{};
    COLORREF palette_text_{};
    COLORREF palette_muted_{};
    COLORREF palette_accent_{};
    COLORREF palette_selection_{};
    COLORREF palette_selection_text_{};
    COLORREF palette_success_{};
    COLORREF palette_warning_{};
    COLORREF palette_error_{};
    FileIndex file_index_{};
    ClipboardStore clipboard_store_{};
    FileSystemWatcher file_watcher_{};
    WatcherHealth watcher_health_{};
    WatcherResyncCoordinator watcher_resync_;
    std::atomic<bool> watcher_restart_in_progress_{false};
    StartupRestoreRecoveryReport startup_restore_recovery_{};
    AppSettingsStore app_settings_store_;
    AppSettings settings_;
    RuntimeScheduler runtime_scheduler_{};
    ReminderStore reminder_store_;
    ReminderCenter reminder_center_;
    SystemMetricsSampler system_metrics_;
    NotificationCenter notification_center_;
    DpapiDataProtector journal_protector_;
    JournalStore journal_store_;
    ExecutionDiagnostics execution_diagnostics_{512};
    BoundedExecutor action_executor_{2, 64};
    ActionRegistry action_registry_;
    PluginTrustStore plugin_trust_store_;
    PluginPackageManager plugin_packages_;
    PluginHost plugin_host_;
    PluginLoaderWin32 plugin_loader_;
    AutomationStore automation_store_;
    AutomationEngine automation_engine_;
    CommandEngine command_engine_;
    std::vector<std::filesystem::path> live_result_paths_;
    std::vector<std::wstring> live_result_completions_;
    bool tray_icon_added_{false};
    bool exit_requested_{false};
    void register_window_class();
    void create_window();
    void create_controls();
    void apply_theme();
    void refresh_theme_resources();
    [[nodiscard]] AppTheme effective_theme() const;
    void register_hotkey();
    void add_tray_icon();
    void remove_tray_icon() noexcept;
    void show_tray_menu();
    void show_palette();
    void hide_palette();
    void execute_input();
    void update_live_results();
    void render_live_search(std::wstring_view query);
    void render_result(const CommandResult &result);
    void center_on_active_monitor();
    void capture_clipboard_text();
    void on_index_configuration_changed();
    [[nodiscard]] CommandResult settings_command(std::wstring_view arguments);
    [[nodiscard]] CommandResult data_command(std::wstring_view arguments);
    [[nodiscard]] CommandResult plugin_management_command(std::wstring_view arguments);
    void load_enabled_plugins();
    void reconcile_plugin_automations();
    void apply_journal_retention();
    void save_settings();
    [[nodiscard]] bool set_launch_at_startup(bool enabled, std::wstring &error_message);
    [[nodiscard]] bool startup_registration_matches() const;
    void restart_file_watcher(bool recovery_restart = false);
    void handle_file_change(const FileChange &change);
    void handle_watcher_issue(const WatcherIssue &issue);
    [[nodiscard]] bool perform_watcher_resync(std::stop_token stop_token,
                                              std::uint64_t issue_generation);
    [[nodiscard]] CommandResult index_recovery_command(std::wstring_view arguments);
    void on_reminder_fired(const Reminder &reminder);
    void register_builtin_actions();
    void on_automation_executed(const Automation &automation, const ActionResult &result);
    void pump_notifications();
    void show_notification(const Notification &notification);
    [[nodiscard]] bool activate_selected_result();
    void set_input_text(std::wstring_view text);
    [[nodiscard]] std::wstring current_input() const;
    [[nodiscard]] std::optional<std::wstring> current_live_file_query() const;
    LRESULT window_proc(UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK window_proc_thunk(HWND hwnd, UINT message, WPARAM wparam,
                                              LPARAM lparam);
};
} // namespace axiom
