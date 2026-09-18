#include "app_settings.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
namespace {
void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}
void run_tests() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("axiom-app-settings-test-" + std::to_string(stamp));
    const auto file = root / "settings.conf";
    const auto legacy = root / "index.conf";
    struct Cleanup final {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    } cleanup{root};
    axiom::AppSettingsStore store{file};
    axiom::AppSettings fallback{{root / "Desktop"}, {L"build"}, false, true, true};
    const auto missing = store.load(fallback);
    expect(missing.index_roots == fallback.index_roots,
           "missing settings should use fallback roots");
    expect(missing.close_to_tray, "fallback close-to-tray should survive");
    axiom::AppSettings saved{
        {root / "Docs", root / "Downloads", root / "Docs"},
        {L"Cache", L"build", L"cache", L"önbellek"},
        true,
        false,
        false,
        12345,
        30,
        365,
        false,
        true,
        false,
        axiom::AppTheme::midnight,
    };
    store.save(saved);
    const auto loaded = store.load();
    expect(loaded.index_roots.size() == 2, "duplicate roots should be normalized away");
    expect(loaded.excluded_directory_names.size() == 3,
           "duplicate exclusions should be normalized away");
    expect(loaded.launch_at_startup, "startup flag should round-trip");
    expect(!loaded.close_to_tray, "close-to-tray flag should round-trip");
    expect(!loaded.notifications_enabled, "notification flag should round-trip");
    expect(loaded.journal_max_entries == 12345, "journal max entries should round-trip");
    expect(loaded.journal_activity_retention_days == 30, "activity retention should round-trip");
    expect(loaded.journal_note_retention_days == 365, "note retention should round-trip");
    expect(!loaded.journal_context_enabled, "journal context privacy switch should round-trip");
    expect(loaded.plugins_enabled, "plugin loading trust switch should round-trip");
    expect(!loaded.journal_live_protection_enabled,
           "journal live protection switch should round-trip");
    expect(loaded.theme == axiom::AppTheme::midnight, "theme should round-trip");
    expect(axiom::app_theme_name(loaded.theme) == L"midnight", "theme name should be stable");
    expect(axiom::parse_app_theme(L"BLACK") == axiom::AppTheme::oled,
           "black alias should resolve to OLED");
    expect(!axiom::parse_app_theme(L"unknown-theme").has_value(),
           "unknown theme should fail closed");
    {
        std::ifstream settings_text{file, std::ios::binary};
        const std::string bytes{std::istreambuf_iterator<char>{settings_text},
                                std::istreambuf_iterator<char>{}};
        expect(bytes.find("version=6\n") != std::string::npos,
               "settings writer should advance to version 6");
        expect(bytes.find("theme=midnight\n") != std::string::npos,
               "settings writer should persist theme");
    }
    expect(loaded.excluded_directory_names[2] == L"önbellek",
           "Unicode exclusion should round-trip");
    std::filesystem::remove(file);
    std::filesystem::create_directories(root);
    {
        std::ofstream out{legacy, std::ios::binary | std::ios::trunc};
        out << "version=1\n";
        out << "root=" << (root / "LegacyDocs").generic_string() << "\n";
        out << "exclude=legacy-cache\n";
    }
    const auto migrated = store.load(fallback, legacy);
    expect(migrated.index_roots.size() == 1, "legacy root should be imported");
    expect(migrated.index_roots.front().filename() == "LegacyDocs",
           "legacy root should be preserved");
    expect(migrated.excluded_directory_names == std::vector<std::wstring>{L"legacy-cache"},
           "legacy exclusion should be imported");
    expect(std::filesystem::exists(file), "legacy migration should materialize unified settings");
}
} // namespace
int main() {
    try {
        run_tests();
        std::cout << "AxiomAppSettingsTests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "AxiomAppSettingsTests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
