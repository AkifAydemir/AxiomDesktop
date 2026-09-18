#include "app_settings.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string_view>
namespace axiom {
namespace {
[[nodiscard]] std::string trim_ascii(std::string_view value) {
    const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    auto first = value.begin();
    while (first != value.end() && is_space(static_cast<unsigned char>(*first))) {
        ++first;
    }
    auto last = value.end();
    while (last != first && is_space(static_cast<unsigned char>(*(last - 1)))) {
        --last;
    }
    return {first, last};
}
[[nodiscard]] std::wstring lower_copy(std::wstring_view value) {
    std::wstring output{value};
    std::transform(output.begin(), output.end(), output.begin(), [](wchar_t ch) {
        if (ch >= L'A' && ch <= L'Z') {
            return static_cast<wchar_t>(ch - L'A' + L'a');
        }
        return ch;
    });
    return output;
}
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path &path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view value) {
    std::u8string bytes;
    bytes.reserve(value.size());
    for (const unsigned char ch : value) {
        bytes.push_back(static_cast<char8_t>(ch));
    }
    return std::filesystem::path{bytes};
}
void append_utf8(std::string &output, std::uint32_t codepoint) {
    if (codepoint <= 0x7Fu) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFu) {
        output.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint <= 0xFFFFu) {
        output.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint <= 0x10FFFFu) {
        output.push_back(static_cast<char>(0xF0u | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else {
        throw std::runtime_error{"Invalid Unicode codepoint."};
    }
}
[[nodiscard]] std::string wide_to_utf8(std::wstring_view value) {
    std::string output;
    output.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        std::uint32_t codepoint = static_cast<std::uint32_t>(value[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
                if (i + 1 >= value.size()) {
                    throw std::runtime_error{"Unpaired UTF-16 high surrogate."};
                }
                const auto low = static_cast<std::uint32_t>(value[++i]);
                if (low < 0xDC00u || low > 0xDFFFu) {
                    throw std::runtime_error{"Invalid UTF-16 surrogate pair."};
                }
                codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) + (low - 0xDC00u);
            } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
                throw std::runtime_error{"Unpaired UTF-16 low surrogate."};
            }
        }
        append_utf8(output, codepoint);
    }
    return output;
}
[[nodiscard]] std::wstring utf8_to_wide(std::string_view value) {
    std::wstring output;
    output.reserve(value.size());
    std::size_t i = 0;
    while (i < value.size()) {
        const auto first = static_cast<unsigned char>(value[i++]);
        std::uint32_t codepoint = 0;
        int continuation = 0;
        if ((first & 0x80u) == 0) {
            codepoint = first;
        } else if ((first & 0xE0u) == 0xC0u) {
            codepoint = first & 0x1Fu;
            continuation = 1;
        } else if ((first & 0xF0u) == 0xE0u) {
            codepoint = first & 0x0Fu;
            continuation = 2;
        } else if ((first & 0xF8u) == 0xF0u) {
            codepoint = first & 0x07u;
            continuation = 3;
        } else {
            return {};
        }
        for (int part = 0; part < continuation; ++part) {
            if (i >= value.size()) {
                return {};
            }
            const auto byte = static_cast<unsigned char>(value[i++]);
            if ((byte & 0xC0u) != 0x80u) {
                return {};
            }
            codepoint = (codepoint << 6) | (byte & 0x3Fu);
        }
        if ((continuation == 1 && codepoint < 0x80u) || (continuation == 2 && codepoint < 0x800u) ||
            (continuation == 3 && codepoint < 0x10000u) || codepoint > 0x10FFFFu ||
            (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
            return {};
        }
        if constexpr (sizeof(wchar_t) == 2) {
            if (codepoint <= 0xFFFFu) {
                output.push_back(static_cast<wchar_t>(codepoint));
            } else {
                codepoint -= 0x10000u;
                output.push_back(static_cast<wchar_t>(0xD800u + (codepoint >> 10)));
                output.push_back(static_cast<wchar_t>(0xDC00u + (codepoint & 0x3FFu)));
            }
        } else {
            output.push_back(static_cast<wchar_t>(codepoint));
        }
    }
    return output;
}
[[nodiscard]] std::uint32_t parse_u32(std::string_view value, std::uint32_t fallback) {
    try {
        std::size_t consumed{};
        const auto parsed = std::stoull(std::string{value}, &consumed, 10);
        if (consumed != value.size() || parsed > 0xffffffffull)
            return fallback;
        return static_cast<std::uint32_t>(parsed);
    } catch (...) {
        return fallback;
    }
}
[[nodiscard]] bool parse_bool(std::string_view value, bool fallback) {
    if (value == "1" || value == "true" || value == "on" || value == "yes") {
        return true;
    }
    if (value == "0" || value == "false" || value == "off" || value == "no") {
        return false;
    }
    return fallback;
}
void normalize(AppSettings &settings) {
    std::vector<std::filesystem::path> roots;
    roots.reserve(settings.index_roots.size());
    for (auto &root : settings.index_roots) {
        if (root.empty()) {
            continue;
        }
        const auto normalized = root.lexically_normal();
        if (std::find(roots.begin(), roots.end(), normalized) == roots.end()) {
            roots.push_back(normalized);
        }
    }
    settings.index_roots = std::move(roots);
    std::vector<std::wstring> exclusions;
    exclusions.reserve(settings.excluded_directory_names.size());
    for (auto &exclusion : settings.excluded_directory_names) {
        auto normalized = lower_copy(exclusion);
        if (normalized.empty() || normalized.find_first_of(L"/\\") != std::wstring::npos) {
            continue;
        }
        if (std::find(exclusions.begin(), exclusions.end(), normalized) == exclusions.end()) {
            exclusions.push_back(std::move(normalized));
        }
    }
    std::sort(exclusions.begin(), exclusions.end());
    settings.excluded_directory_names = std::move(exclusions);
    settings.journal_max_entries =
        std::clamp<std::uint32_t>(settings.journal_max_entries, 100u, 200000u);
    settings.journal_activity_retention_days =
        std::min<std::uint32_t>(settings.journal_activity_retention_days, 3650u);
    settings.journal_note_retention_days =
        std::min<std::uint32_t>(settings.journal_note_retention_days, 3650u);
    switch (settings.theme) {
    case AppTheme::system:
    case AppTheme::light:
    case AppTheme::dark:
    case AppTheme::oled:
    case AppTheme::graphite:
    case AppTheme::midnight:
    case AppTheme::nord:
    case AppTheme::high_contrast:
        break;
    default:
        settings.theme = AppTheme::system;
        break;
    }
}
[[nodiscard]] AppSettings load_legacy_index_file(const std::filesystem::path &file,
                                                 AppSettings fallback) {
    std::ifstream stream{file, std::ios::binary};
    if (!stream) {
        return fallback;
    }
    AppSettings loaded = fallback;
    loaded.index_roots.clear();
    loaded.excluded_directory_names.clear();
    std::string line;
    bool saw_version = false;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto normalized = trim_ascii(line);
        if (normalized.empty() || normalized.starts_with('#')) {
            continue;
        }
        if (normalized == "version=1") {
            saw_version = true;
        } else if (normalized.starts_with("root=")) {
            const auto value = trim_ascii(std::string_view{normalized}.substr(5));
            if (!value.empty()) {
                loaded.index_roots.push_back(path_from_utf8(value));
            }
        } else if (normalized.starts_with("exclude=")) {
            const auto value = trim_ascii(std::string_view{normalized}.substr(8));
            const auto wide = utf8_to_wide(value);
            if (!wide.empty()) {
                loaded.excluded_directory_names.push_back(std::move(wide));
            }
        }
    }
    if (!saw_version) {
        return fallback;
    }
    normalize(loaded);
    if (loaded.index_roots.empty()) {
        loaded.index_roots = std::move(fallback.index_roots);
    }
    return loaded;
}
} // namespace
std::wstring_view app_theme_name(AppTheme theme) noexcept {
    switch (theme) {
    case AppTheme::light:
        return L"light";
    case AppTheme::dark:
        return L"dark";
    case AppTheme::oled:
        return L"oled";
    case AppTheme::graphite:
        return L"graphite";
    case AppTheme::midnight:
        return L"midnight";
    case AppTheme::nord:
        return L"nord";
    case AppTheme::high_contrast:
        return L"high-contrast";
    case AppTheme::system:
    default:
        return L"system";
    }
}
std::optional<AppTheme> parse_app_theme(std::wstring_view value) {
    const auto normalized = lower_copy(value);
    if (normalized == L"system")
        return AppTheme::system;
    if (normalized == L"light")
        return AppTheme::light;
    if (normalized == L"dark")
        return AppTheme::dark;
    if (normalized == L"oled" || normalized == L"black")
        return AppTheme::oled;
    if (normalized == L"graphite")
        return AppTheme::graphite;
    if (normalized == L"midnight")
        return AppTheme::midnight;
    if (normalized == L"nord")
        return AppTheme::nord;
    if (normalized == L"high-contrast" || normalized == L"highcontrast")
        return AppTheme::high_contrast;
    return std::nullopt;
}
AppSettingsStore::AppSettingsStore(std::filesystem::path file_path)
    : file_path_{std::move(file_path)} {
    if (file_path_.empty()) {
        throw std::invalid_argument{"AppSettingsStore requires a file path."};
    }
}
AppSettings AppSettingsStore::load(AppSettings fallback,
                                   const std::filesystem::path &legacy_index_file) const {
    normalize(fallback);
    std::ifstream stream{file_path_, std::ios::binary};
    if (!stream) {
        auto migrated = legacy_index_file.empty()
                            ? fallback
                            : load_legacy_index_file(legacy_index_file, fallback);
        std::error_code legacy_error;
        if (!legacy_index_file.empty() &&
            std::filesystem::exists(legacy_index_file, legacy_error) && !legacy_error) {
            save(migrated);
        }
        return migrated;
    }
    AppSettings loaded = fallback;
    loaded.index_roots.clear();
    loaded.excluded_directory_names.clear();
    std::string line;
    bool saw_version = false;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto normalized = trim_ascii(line);
        if (normalized.empty() || normalized.starts_with('#')) {
            continue;
        }
        if (normalized == "version=2" || normalized == "version=3" || normalized == "version=4" ||
            normalized == "version=5" || normalized == "version=6") {
            saw_version = true;
        } else if (normalized.starts_with("root=")) {
            const auto value = trim_ascii(std::string_view{normalized}.substr(5));
            if (!value.empty()) {
                loaded.index_roots.push_back(path_from_utf8(value));
            }
        } else if (normalized.starts_with("exclude=")) {
            const auto value = trim_ascii(std::string_view{normalized}.substr(8));
            const auto wide = utf8_to_wide(value);
            if (!wide.empty()) {
                loaded.excluded_directory_names.push_back(std::move(wide));
            }
        } else if (normalized.starts_with("startup=")) {
            loaded.launch_at_startup = parse_bool(
                trim_ascii(std::string_view{normalized}.substr(8)), fallback.launch_at_startup);
        } else if (normalized.starts_with("close_to_tray=")) {
            loaded.close_to_tray = parse_bool(trim_ascii(std::string_view{normalized}.substr(14)),
                                              fallback.close_to_tray);
        } else if (normalized.starts_with("notifications=")) {
            loaded.notifications_enabled =
                parse_bool(trim_ascii(std::string_view{normalized}.substr(14)),
                           fallback.notifications_enabled);
        } else if (normalized.starts_with("journal_max_entries=")) {
            loaded.journal_max_entries = parse_u32(
                trim_ascii(std::string_view{normalized}.substr(20)), fallback.journal_max_entries);
        } else if (normalized.starts_with("journal_activity_retention_days=")) {
            loaded.journal_activity_retention_days =
                parse_u32(trim_ascii(std::string_view{normalized}.substr(32)),
                          fallback.journal_activity_retention_days);
        } else if (normalized.starts_with("journal_note_retention_days=")) {
            loaded.journal_note_retention_days =
                parse_u32(trim_ascii(std::string_view{normalized}.substr(28)),
                          fallback.journal_note_retention_days);
        } else if (normalized.starts_with("journal_context_enabled=")) {
            loaded.journal_context_enabled =
                parse_bool(trim_ascii(std::string_view{normalized}.substr(24)),
                           fallback.journal_context_enabled);
        } else if (normalized.starts_with("plugins_enabled=")) {
            loaded.plugins_enabled = parse_bool(trim_ascii(std::string_view{normalized}.substr(16)),
                                                fallback.plugins_enabled);
        } else if (normalized.starts_with("journal_live_protection_enabled=")) {
            loaded.journal_live_protection_enabled =
                parse_bool(trim_ascii(std::string_view{normalized}.substr(32)),
                           fallback.journal_live_protection_enabled);
        } else if (normalized.starts_with("theme=")) {
            const auto wide = utf8_to_wide(trim_ascii(std::string_view{normalized}.substr(6)));
            if (const auto parsed = parse_app_theme(wide)) {
                loaded.theme = *parsed;
            }
        }
    }
    if (!saw_version) {
        return fallback;
    }
    normalize(loaded);
    if (loaded.index_roots.empty()) {
        loaded.index_roots = std::move(fallback.index_roots);
    }
    return loaded;
}
void AppSettingsStore::save(const AppSettings &settings) const {
    auto normalized = settings;
    normalize(normalized);
    if (const auto parent = file_path_.parent_path(); !parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            throw std::filesystem::filesystem_error{"Failed to create Axiom settings directory.",
                                                    parent, error};
        }
    }
    const auto temporary = file_path_.wstring() + L".tmp";
    {
        std::ofstream stream{std::filesystem::path{temporary}, std::ios::binary | std::ios::trunc};
        if (!stream) {
            throw std::runtime_error{"Failed to open temporary Axiom settings file."};
        }
        stream << "# Axiom Desktop unified settings\n";
        stream << "version=6\n";
        stream << "startup=" << (normalized.launch_at_startup ? "1" : "0") << '\n';
        stream << "close_to_tray=" << (normalized.close_to_tray ? "1" : "0") << '\n';
        stream << "notifications=" << (normalized.notifications_enabled ? "1" : "0") << '\n';
        stream << "journal_max_entries=" << normalized.journal_max_entries << '\n';
        stream << "journal_activity_retention_days=" << normalized.journal_activity_retention_days
               << '\n';
        stream << "journal_note_retention_days=" << normalized.journal_note_retention_days << '\n';
        stream << "journal_context_enabled=" << (normalized.journal_context_enabled ? "1" : "0")
               << '\n';
        stream << "plugins_enabled=" << (normalized.plugins_enabled ? "1" : "0") << '\n';
        stream << "journal_live_protection_enabled="
               << (normalized.journal_live_protection_enabled ? "1" : "0") << '\n';
        stream << "theme=" << wide_to_utf8(app_theme_name(normalized.theme)) << '\n';
        for (const auto &root : normalized.index_roots) {
            stream << "root=" << path_to_utf8(root) << '\n';
        }
        for (const auto &exclusion : normalized.excluded_directory_names) {
            stream << "exclude=" << wide_to_utf8(exclusion) << '\n';
        }
        stream.flush();
        if (!stream) {
            throw std::runtime_error{"Failed to write Axiom settings file."};
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, file_path_, error);
    if (error) {
        std::filesystem::remove(file_path_, error);
        error.clear();
        std::filesystem::rename(temporary, file_path_, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            throw std::filesystem::filesystem_error{"Failed to replace Axiom settings file.",
                                                    file_path_, error};
        }
    }
}
const std::filesystem::path &AppSettingsStore::file_path() const noexcept {
    return file_path_;
}
} // namespace axiom
