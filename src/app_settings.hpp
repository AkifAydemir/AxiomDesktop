#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace axiom {
enum class AppTheme : std::uint32_t {
    system,
    light,
    dark,
    oled,
    graphite,
    midnight,
    nord,
    high_contrast,
};
[[nodiscard]] std::wstring_view app_theme_name(AppTheme theme) noexcept;
[[nodiscard]] std::optional<AppTheme> parse_app_theme(std::wstring_view value);
struct AppSettings {
    std::vector<std::filesystem::path> index_roots;
    std::vector<std::wstring> excluded_directory_names;
    bool launch_at_startup{false};
    bool close_to_tray{true};
    bool notifications_enabled{true};
    std::uint32_t journal_max_entries{50000};
    std::uint32_t journal_activity_retention_days{90};
    std::uint32_t journal_note_retention_days{0}; // 0 = keep indefinitely
    bool journal_context_enabled{true};
    bool plugins_enabled{false};
    bool journal_live_protection_enabled{true};
    AppTheme theme{AppTheme::system};
};
class AppSettingsStore final {
  public:
    explicit AppSettingsStore(std::filesystem::path file_path);
    [[nodiscard]] AppSettings load(AppSettings fallback = {},
                                   const std::filesystem::path &legacy_index_file = {}) const;
    void save(const AppSettings &settings) const;
    [[nodiscard]] const std::filesystem::path &file_path() const noexcept;

  private:
    std::filesystem::path file_path_;
};
} // namespace axiom
