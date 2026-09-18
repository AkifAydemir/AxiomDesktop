#include "axiom.hpp"
#include "journal_transfer.hpp"
#include "support_bundle.hpp"
#include "win32_handle.hpp"
#include <winsock2.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <charconv>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>
namespace axiom {
namespace {
constexpr wchar_t window_title[] = L"Axiom";
constexpr int palette_width = 820;
constexpr int palette_height = 470;
constexpr int margin = 18;
constexpr int edit_height = 40;
constexpr int result_item_height = 30;
struct PaletteThemeColors {
    COLORREF background{};
    COLORREF surface{};
    COLORREF text{};
    COLORREF muted{};
    COLORREF accent{};
    COLORREF selection{};
    COLORREF selection_text{};
    COLORREF success{};
    COLORREF warning{};
    COLORREF error{};
};
[[nodiscard]] bool high_contrast_enabled() noexcept {
    HIGHCONTRASTW high_contrast{};
    high_contrast.cbSize = sizeof(high_contrast);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(high_contrast), &high_contrast, 0) !=
               FALSE &&
           (high_contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}
[[nodiscard]] bool windows_prefers_dark_apps() noexcept {
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS && value == 0;
}
[[nodiscard]] PaletteThemeColors palette_colors(AppTheme theme) noexcept {
    if (theme == AppTheme::high_contrast) {
        return {
            GetSysColor(COLOR_WINDOW),        GetSysColor(COLOR_WINDOW),
            GetSysColor(COLOR_WINDOWTEXT),    GetSysColor(COLOR_GRAYTEXT),
            GetSysColor(COLOR_HIGHLIGHT),     GetSysColor(COLOR_HIGHLIGHT),
            GetSysColor(COLOR_HIGHLIGHTTEXT), GetSysColor(COLOR_HIGHLIGHT),
            GetSysColor(COLOR_HIGHLIGHT),     GetSysColor(COLOR_HIGHLIGHT),
        };
    }
    switch (theme) {
    case AppTheme::light:
        return {RGB(246, 247, 249), RGB(255, 255, 255), RGB(30, 35, 43), RGB(102, 112, 133),
                RGB(47, 111, 237),  RGB(220, 232, 255), RGB(23, 52, 98), RGB(20, 132, 84),
                RGB(180, 108, 0),   RGB(196, 43, 28)};
    case AppTheme::dark:
        return {RGB(24, 26, 31),    RGB(31, 34, 40),   RGB(235, 238, 244), RGB(154, 162, 176),
                RGB(112, 163, 255), RGB(56, 74, 105),  RGB(245, 248, 255), RGB(83, 201, 139),
                RGB(245, 184, 85),  RGB(255, 112, 102)};
    case AppTheme::oled:
        return {RGB(0, 0, 0),       RGB(8, 8, 8),      RGB(242, 242, 242), RGB(145, 145, 145),
                RGB(112, 170, 255), RGB(28, 48, 76),   RGB(255, 255, 255), RGB(88, 210, 142),
                RGB(255, 190, 76),  RGB(255, 104, 104)};
    case AppTheme::graphite:
        return {RGB(35, 37, 41),    RGB(44, 47, 52),   RGB(235, 236, 238), RGB(159, 164, 171),
                RGB(140, 157, 190), RGB(68, 74, 85),   RGB(250, 250, 251), RGB(103, 190, 136),
                RGB(226, 174, 83),  RGB(225, 102, 102)};
    case AppTheme::midnight:
        return {RGB(9, 14, 29),    RGB(15, 23, 42),   RGB(226, 232, 240), RGB(148, 163, 184),
                RGB(96, 165, 250), RGB(30, 58, 95),   RGB(239, 246, 255), RGB(74, 222, 128),
                RGB(250, 204, 21), RGB(248, 113, 113)};
    case AppTheme::nord:
        return {RGB(46, 52, 64),    RGB(59, 66, 82),  RGB(236, 239, 244), RGB(180, 190, 205),
                RGB(136, 192, 208), RGB(67, 76, 94),  RGB(242, 244, 248), RGB(163, 190, 140),
                RGB(235, 203, 139), RGB(191, 97, 106)};
    case AppTheme::system:
    default:
        return windows_prefers_dark_apps() ? palette_colors(AppTheme::dark)
                                           : palette_colors(AppTheme::light);
    }
}
[[nodiscard]] std::wstring trim(std::wstring_view input) {
    const auto is_space = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    auto first = input.begin();
    while (first != input.end() && is_space(*first)) {
        ++first;
    }
    auto last = input.end();
    while (last != first && is_space(*(last - 1))) {
        --last;
    }
    return {first, last};
}
[[nodiscard]] std::pair<std::wstring, std::wstring> split_command(std::wstring_view input) {
    const auto normalized = trim(input);
    const auto space = normalized.find_first_of(L" \t");
    if (space == std::wstring::npos) {
        return {normalized, {}};
    }
    return {
        normalized.substr(0, space),
        trim(std::wstring_view{normalized}.substr(space + 1)),
    };
}
[[nodiscard]] std::wstring lower_copy(std::wstring_view value) {
    std::wstring output{value};
    std::transform(output.begin(), output.end(), output.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return output;
}
[[nodiscard]] bool equals_ignore_case(std::wstring_view lhs, std::wstring_view rhs) {
    return lower_copy(lhs) == lower_copy(rhs);
}
[[nodiscard]] bool starts_with_ignore_case(std::wstring_view value, std::wstring_view prefix) {
    if (prefix.size() > value.size()) {
        return false;
    }
    return equals_ignore_case(value.substr(0, prefix.size()), prefix);
}
[[nodiscard]] std::wstring win32_error_message(DWORD error) {
    LPWSTR raw_message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&raw_message), 0, nullptr);
    if (length == 0 || raw_message == nullptr) {
        return L"Windows error " + std::to_wstring(error);
    }
    std::wstring message{raw_message, length};
    LocalFree(raw_message);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}
[[nodiscard]] std::optional<DWORD> parse_dword(std::wstring_view text) {
    const auto normalized = trim(text);
    if (normalized.empty()) {
        return std::nullopt;
    }
    unsigned long long value = 0;
    for (const wchar_t ch : normalized) {
        if (ch < L'0' || ch > L'9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<unsigned long long>(ch - L'0');
        if (value > 0xFFFFFFFFull) {
            return std::nullopt;
        }
    }
    return static_cast<DWORD>(value);
}
[[nodiscard]] std::optional<std::uint64_t> parse_u64(std::wstring_view text) {
    const auto normalized = trim(text);
    if (normalized.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const wchar_t ch : normalized) {
        if (ch < L'0' || ch > L'9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(ch - L'0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}
[[nodiscard]] std::optional<std::chrono::milliseconds>
parse_duration_token(std::wstring_view text) {
    const auto normalized = lower_copy(trim(text));
    if (normalized.empty()) {
        return std::nullopt;
    }
    std::wstring_view number_part{normalized};
    std::uint64_t multiplier = 1000;
    if (normalized.size() >= 2 && normalized.ends_with(L"ms")) {
        number_part = std::wstring_view{normalized}.substr(0, normalized.size() - 2);
        multiplier = 1;
    } else if (normalized.ends_with(L"s")) {
        number_part = std::wstring_view{normalized}.substr(0, normalized.size() - 1);
        multiplier = 1000;
    } else if (normalized.ends_with(L"m")) {
        number_part = std::wstring_view{normalized}.substr(0, normalized.size() - 1);
        multiplier = 60ull * 1000ull;
    } else if (normalized.ends_with(L"h")) {
        number_part = std::wstring_view{normalized}.substr(0, normalized.size() - 1);
        multiplier = 60ull * 60ull * 1000ull;
    } else if (normalized.ends_with(L"d")) {
        number_part = std::wstring_view{normalized}.substr(0, normalized.size() - 1);
        multiplier = 24ull * 60ull * 60ull * 1000ull;
    }
    const auto value = parse_u64(number_part);
    if (!value || *value == 0) {
        return std::nullopt;
    }
    constexpr std::uint64_t max_delay_ms = 3650ull * 24ull * 60ull * 60ull * 1000ull;
    if (*value > max_delay_ms / multiplier) {
        return std::nullopt;
    }
    return std::chrono::milliseconds{static_cast<std::int64_t>(*value * multiplier)};
}
[[nodiscard]] std::optional<int> parse_fixed_decimal(std::wstring_view text, std::size_t digits) {
    if (text.size() != digits) {
        return std::nullopt;
    }
    int value = 0;
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<int>(ch - L'0');
    }
    return value;
}
struct WallClock final {
    std::uint8_t hour{};
    std::uint8_t minute{};
    std::uint8_t second{};
};
[[nodiscard]] std::optional<WallClock> parse_wall_clock(std::wstring_view time) {
    const auto normalized = trim(time);
    if ((normalized.size() != 5 && normalized.size() != 8) || normalized[2] != L':' ||
        (normalized.size() == 8 && normalized[5] != L':')) {
        return std::nullopt;
    }
    const auto hour = parse_fixed_decimal(normalized.substr(0, 2), 2);
    const auto minute = parse_fixed_decimal(normalized.substr(3, 2), 2);
    const auto second = normalized.size() == 8 ? parse_fixed_decimal(normalized.substr(6, 2), 2)
                                               : std::optional<int>{0};
    if (!hour || !minute || !second || *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }
    return WallClock{static_cast<std::uint8_t>(*hour), static_cast<std::uint8_t>(*minute),
                     static_cast<std::uint8_t>(*second)};
}
[[nodiscard]] std::optional<std::uint8_t> parse_weekday_mask(std::wstring_view text) {
    const auto normalized = lower_copy(trim(text));
    if (normalized == L"daily" || normalized == L"all") {
        return std::uint8_t{0x7Fu};
    }
    constexpr std::array<std::wstring_view, 7> names{L"sun", L"mon", L"tue", L"wed",
                                                     L"thu", L"fri", L"sat"};
    std::uint8_t mask = 0;
    std::size_t offset = 0;
    while (offset < normalized.size()) {
        const auto comma = normalized.find(L',', offset);
        const auto token = trim(std::wstring_view{normalized}.substr(
            offset, comma == std::wstring::npos ? std::wstring::npos : comma - offset));
        bool found = false;
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (token == names[index]) {
                mask = static_cast<std::uint8_t>(mask | (1u << index));
                found = true;
                break;
            }
        }
        if (!found) {
            return std::nullopt;
        }
        if (comma == std::wstring::npos) {
            break;
        }
        offset = comma + 1;
    }
    return mask == 0 ? std::nullopt : std::optional<std::uint8_t>{mask};
}
[[nodiscard]] std::wstring format_weekday_mask(std::uint8_t mask) {
    if ((mask & 0x7Fu) == 0x7Fu) {
        return L"daily";
    }
    constexpr std::array<std::wstring_view, 7> names{L"sun", L"mon", L"tue", L"wed",
                                                     L"thu", L"fri", L"sat"};
    std::wstring output;
    for (std::size_t index = 0; index < names.size(); ++index) {
        if ((mask & (1u << index)) == 0) {
            continue;
        }
        if (!output.empty()) {
            output += L",";
        }
        output += names[index];
    }
    return output;
}
[[nodiscard]] std::wstring format_calendar_schedule(const AutomationCalendarSchedule &schedule) {
    std::wostringstream stream;
    stream << format_weekday_mask(schedule.weekday_mask) << L" " << std::setfill(L'0')
           << std::setw(2) << static_cast<int>(schedule.hour) << L":" << std::setw(2)
           << static_cast<int>(schedule.minute) << L":" << std::setw(2)
           << static_cast<int>(schedule.second) << L" @" << schedule.time_zone;
    return stream.str();
}
[[nodiscard]] std::optional<std::chrono::system_clock::time_point>
parse_local_datetime(std::wstring_view date, std::wstring_view time) {
    if (date.size() != 10 || date[4] != L'-' || date[7] != L'-') {
        return std::nullopt;
    }
    if ((time.size() != 5 && time.size() != 8) || time[2] != L':' ||
        (time.size() == 8 && time[5] != L':')) {
        return std::nullopt;
    }
    const auto year = parse_fixed_decimal(date.substr(0, 4), 4);
    const auto month = parse_fixed_decimal(date.substr(5, 2), 2);
    const auto day = parse_fixed_decimal(date.substr(8, 2), 2);
    const auto hour = parse_fixed_decimal(time.substr(0, 2), 2);
    const auto minute = parse_fixed_decimal(time.substr(3, 2), 2);
    const auto second =
        time.size() == 8 ? parse_fixed_decimal(time.substr(6, 2), 2) : std::optional<int>{0};
    if (!year || !month || !day || !hour || !minute || !second || *year < 1970 || *year > 3000 ||
        *month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour < 0 || *hour > 23 ||
        *minute < 0 || *minute > 59 || *second < 0 || *second > 59) {
        return std::nullopt;
    }
    std::tm local{};
    local.tm_year = *year - 1900;
    local.tm_mon = *month - 1;
    local.tm_mday = *day;
    local.tm_hour = *hour;
    local.tm_min = *minute;
    local.tm_sec = *second;
    local.tm_isdst = -1;
    const std::time_t raw = std::mktime(&local);
    if (raw == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    std::tm round_trip{};
    if (localtime_s(&round_trip, &raw) != 0 || round_trip.tm_year != local.tm_year ||
        round_trip.tm_mon != local.tm_mon || round_trip.tm_mday != local.tm_mday ||
        round_trip.tm_hour != local.tm_hour || round_trip.tm_min != local.tm_min ||
        round_trip.tm_sec != local.tm_sec) {
        return std::nullopt;
    }
    return std::chrono::system_clock::from_time_t(raw);
}
[[nodiscard]] std::wstring format_interval(std::chrono::milliseconds interval) {
    const auto total_seconds = std::chrono::duration_cast<std::chrono::seconds>(interval).count();
    if (total_seconds >= 86400 && total_seconds % 86400 == 0) {
        return std::to_wstring(total_seconds / 86400) + L"d";
    }
    if (total_seconds >= 3600 && total_seconds % 3600 == 0) {
        return std::to_wstring(total_seconds / 3600) + L"h";
    }
    if (total_seconds >= 60 && total_seconds % 60 == 0) {
        return std::to_wstring(total_seconds / 60) + L"m";
    }
    if (interval.count() >= 1000 && interval.count() % 1000 == 0) {
        return std::to_wstring(total_seconds) + L"s";
    }
    return std::to_wstring(interval.count()) + L"ms";
}
[[nodiscard]] std::wstring format_failure_policy(const AutomationFailurePolicy &policy) {
    if (!policy.retries_enabled()) {
        return L"none";
    }
    std::wstring output = policy.kind == AutomationRetryKind::fixed ? L"fixed" : L"exponential";
    output += L" retries=" + std::to_wstring(policy.max_retries);
    output += L" initial=" + format_interval(policy.initial_delay);
    if (policy.kind == AutomationRetryKind::exponential) {
        output += L" max=" + format_interval(policy.max_delay);
    }
    output += policy.disable_on_exhaustion ? L" exhaust=disable" : L" exhaust=continue";
    return output;
}
[[nodiscard]] std::wstring format_due_time(std::chrono::system_clock::time_point due_at) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(due_at);
    std::tm local{};
    if (localtime_s(&local, &raw) != 0) {
        return L"<time unavailable>";
    }
    std::wostringstream stream;
    stream << std::put_time(&local, L"%Y-%m-%d %H:%M:%S");
    return stream.str();
}
[[nodiscard]] std::wstring format_remaining(std::chrono::system_clock::time_point due_at) {
    auto remaining =
        std::chrono::duration_cast<std::chrono::seconds>(due_at - std::chrono::system_clock::now());
    if (remaining < std::chrono::seconds::zero()) {
        remaining = std::chrono::seconds::zero();
    }
    const auto total = remaining.count();
    const auto days = total / 86400;
    const auto hours = (total % 86400) / 3600;
    const auto minutes = (total % 3600) / 60;
    const auto seconds = total % 60;
    if (days > 0) {
        return std::to_wstring(days) + L"d " + std::to_wstring(hours) + L"h";
    }
    if (hours > 0) {
        return std::to_wstring(hours) + L"h " + std::to_wstring(minutes) + L"m";
    }
    if (minutes > 0) {
        return std::to_wstring(minutes) + L"m " + std::to_wstring(seconds) + L"s";
    }
    return std::to_wstring(seconds) + L"s";
}
[[nodiscard]] bool write_clipboard_text(std::wstring_view text, std::wstring &error_message) {
    if (!OpenClipboard(nullptr)) {
        error_message = win32_error_message(GetLastError());
        return false;
    }
    struct ClipboardCloser {
        ~ClipboardCloser() {
            CloseClipboard();
        }
    } closer;
    if (!EmptyClipboard()) {
        error_message = win32_error_message(GetLastError());
        return false;
    }
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        error_message = L"GlobalAlloc failed.";
        return false;
    }
    void *destination = GlobalLock(memory);
    if (destination == nullptr) {
        GlobalFree(memory);
        error_message = L"GlobalLock failed.";
        return false;
    }
    std::memcpy(destination, text.data(), text.size() * sizeof(wchar_t));
    static_cast<wchar_t *>(destination)[text.size()] = L'\0';
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        error_message = win32_error_message(GetLastError());
        return false;
    }
    return true;
}
[[nodiscard]] std::wstring clipboard_preview(std::wstring_view text, std::size_t limit = 110) {
    std::wstring preview;
    preview.reserve(std::min(text.size(), limit) + 3);
    for (const wchar_t ch : text) {
        if (preview.size() >= limit) {
            break;
        }
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
            if (preview.empty() || preview.back() != L' ') {
                preview.push_back(L' ');
            }
        } else {
            preview.push_back(ch);
        }
    }
    if (text.size() > limit) {
        preview += L"...";
    }
    return preview;
}
[[nodiscard]] std::wstring format_clipboard_entry(const ClipboardEntry &entry) {
    std::wstring line = L"#" + std::to_wstring(entry.id);
    line += entry.pinned ? L" * [" : L" [";
    line += to_string(entry.kind);
    line += L"] ";
    line += clipboard_preview(entry.text);
    if (entry.capture_count > 1) {
        line += L" x" + std::to_wstring(entry.capture_count);
    }
    return line;
}
[[nodiscard]] std::wstring process_name_from_pid(DWORD pid) {
    UniqueWin32Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process) {
        return L"<unavailable>";
    }
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    const BOOL ok = QueryFullProcessImageNameW(process.get(), 0, path.data(), &size);
    if (!ok) {
        return L"<unavailable>";
    }
    path.resize(size);
    const std::filesystem::path fs_path{path};
    return fs_path.filename().wstring();
}
[[nodiscard]] CommandResult command_open(std::wstring_view arguments) {
    const auto target = trim(arguments);
    if (target.empty()) {
        return {{{L"Usage: open <path-or-url>", ResultKind::warning}}, false};
    }
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        return {
            {{L"Open failed. ShellExecute code: " + std::to_wstring(result), ResultKind::error}},
            false};
    }
    return {{{L"Opened: " + target, ResultKind::success}}, false};
}
[[nodiscard]] CommandResult command_run(std::wstring_view arguments) {
    auto command_line = trim(arguments);
    if (command_line.empty()) {
        return {{{L"Usage: run <command-line>", ResultKind::warning}}, false};
    }
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                                        CREATE_NEW_CONSOLE, nullptr, nullptr, &startup, &process);
    if (!created) {
        return {
            {{L"CreateProcess failed: " + win32_error_message(GetLastError()), ResultKind::error}},
            false};
    }
    const DWORD pid = process.dwProcessId;
    UniqueWin32Handle process_thread{process.hThread};
    UniqueWin32Handle process_handle{process.hProcess};
    return {{{L"Started process PID " + std::to_wstring(pid), ResultKind::success}}, false};
}
[[nodiscard]] CommandResult command_pid(std::wstring_view arguments) {
    auto query = trim(arguments);
    if (query.empty()) {
        return {{{L"Usage: pid <process-name>", ResultKind::warning}}, false};
    }
    if (query.find(L'.') == std::wstring::npos) {
        query += L".exe";
    }
    UniqueWin32Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (!snapshot) {
        return {{{L"Process snapshot failed: " + win32_error_message(GetLastError()),
                  ResultKind::error}},
                false};
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::vector<ResultLine> lines;
    if (Process32FirstW(snapshot.get(), &entry)) {
        do {
            if (equals_ignore_case(entry.szExeFile, query)) {
                lines.push_back({
                    std::wstring{entry.szExeFile} + L" PID=" + std::to_wstring(entry.th32ProcessID),
                    ResultKind::info,
                });
            }
        } while (Process32NextW(snapshot.get(), &entry));
    }
    if (lines.empty()) {
        lines.push_back({L"No process found: " + query, ResultKind::warning});
    }
    return {std::move(lines), false};
}
[[nodiscard]] CommandResult command_kill(std::wstring_view arguments) {
    const auto pid = parse_dword(arguments);
    if (!pid || *pid == 0) {
        return {{{L"Usage: kill <pid>", ResultKind::warning}}, false};
    }
    UniqueWin32Handle process{
        OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *pid)};
    if (!process) {
        return {
            {{L"OpenProcess failed: " + win32_error_message(GetLastError()), ResultKind::error}},
            false};
    }
    const std::wstring process_name = process_name_from_pid(*pid);
    const BOOL terminated = TerminateProcess(process.get(), 1);
    const DWORD error = terminated ? ERROR_SUCCESS : GetLastError();
    if (!terminated) {
        return {{{L"TerminateProcess failed: " + win32_error_message(error), ResultKind::error}},
                false};
    }
    return {{{L"Terminated " + process_name + L" (PID " + std::to_wstring(*pid) + L")",
              ResultKind::success}},
            false};
}
[[nodiscard]] CommandResult command_port(std::wstring_view arguments) {
    const auto parsed_port = parse_dword(arguments);
    if (!parsed_port || *parsed_port == 0 || *parsed_port > 65535) {
        return {{{L"Usage: port <1-65535>", ResultKind::warning}}, false};
    }
    ULONG size = 0;
    DWORD status = GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (status != ERROR_INSUFFICIENT_BUFFER) {
        return {
            {{L"GetExtendedTcpTable failed: " + win32_error_message(status), ResultKind::error}},
            false};
    }
    std::vector<std::byte> buffer(size);
    auto *table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
    status = GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (status != NO_ERROR) {
        return {
            {{L"GetExtendedTcpTable failed: " + win32_error_message(status), ResultKind::error}},
            false};
    }
    std::vector<ResultLine> lines;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto &row = table->table[i];
        const auto local_port = static_cast<DWORD>(ntohs(static_cast<u_short>(row.dwLocalPort)));
        if (local_port != *parsed_port) {
            continue;
        }
        const auto state = row.dwState == MIB_TCP_STATE_LISTEN ? L"LISTEN" : L"TCP";
        const auto process_name = process_name_from_pid(row.dwOwningPid);
        lines.push_back({
            std::wstring{state} + L" port=" + std::to_wstring(local_port) + L" PID=" +
                std::to_wstring(row.dwOwningPid) + L" " + process_name,
            ResultKind::info,
        });
    }
    if (lines.empty()) {
        lines.push_back({L"No IPv4 TCP endpoint found on port " + std::to_wstring(*parsed_port),
                         ResultKind::warning});
    }
    return {std::move(lines), false};
}
[[nodiscard]] std::optional<std::filesystem::path> known_folder(REFKNOWNFOLDERID folder_id) {
    PWSTR raw_path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(folder_id, KF_FLAG_DEFAULT, nullptr, &raw_path);
    if (FAILED(result) || raw_path == nullptr) {
        return std::nullopt;
    }
    std::filesystem::path path{raw_path};
    CoTaskMemFree(raw_path);
    return path;
}
[[nodiscard]] std::optional<std::filesystem::path> current_executable_path() {
    std::vector<wchar_t> buffer(512);
    while (buffer.size() <= 32768) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path{std::wstring_view{buffer.data(), length}};
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::nullopt;
}
[[nodiscard]] std::wstring startup_command_line() {
    const auto executable = current_executable_path();
    if (!executable) {
        return {};
    }
    return L"\"" + executable->wstring() + L"\" --background";
}
[[nodiscard]] std::vector<std::filesystem::path> default_index_roots() {
    std::vector<std::filesystem::path> roots;
    const auto append = [&](REFKNOWNFOLDERID folder_id) {
        if (const auto path = known_folder(folder_id)) {
            std::error_code error;
            if (std::filesystem::exists(*path, error) && !error) {
                roots.push_back(*path);
            }
        }
    };
    append(FOLDERID_Desktop);
    append(FOLDERID_Documents);
    append(FOLDERID_Downloads);
    return roots;
}
[[nodiscard]] std::filesystem::path default_app_settings_path() {
    if (const auto local_app_data = known_folder(FOLDERID_LocalAppData)) {
        return *local_app_data / L"Axiom" / L"settings.conf";
    }
    std::error_code error;
    auto fallback = std::filesystem::temp_directory_path(error);
    if (error) {
        fallback = L".";
    }
    return fallback / L"Axiom" / L"settings.conf";
}
[[nodiscard]] std::filesystem::path legacy_index_settings_path() {
    if (const auto local_app_data = known_folder(FOLDERID_LocalAppData)) {
        return *local_app_data / L"Axiom" / L"index.conf";
    }
    std::error_code error;
    auto fallback = std::filesystem::temp_directory_path(error);
    if (error) {
        fallback = L".";
    }
    return fallback / L"Axiom" / L"index.conf";
}
[[nodiscard]] std::filesystem::path default_reminder_store_path() {
    if (const auto local_app_data = known_folder(FOLDERID_LocalAppData)) {
        return *local_app_data / L"Axiom" / L"reminders.bin";
    }
    std::error_code error;
    auto fallback = std::filesystem::temp_directory_path(error);
    if (error) {
        fallback = L".";
    }
    return fallback / L"Axiom" / L"reminders.bin";
}
[[nodiscard]] std::filesystem::path default_journal_store_path() {
    if (const auto local_app_data = known_folder(FOLDERID_LocalAppData)) {
        return *local_app_data / L"Axiom" / L"journal.bin";
    }
    std::error_code error;
    auto fallback = std::filesystem::temp_directory_path(error);
    if (error) {
        fallback = L".";
    }
    return fallback / L"Axiom" / L"journal.bin";
}
[[nodiscard]] std::filesystem::path default_automation_store_path() {
    if (const auto local_app_data = known_folder(FOLDERID_LocalAppData)) {
        return *local_app_data / L"Axiom" / L"automations.bin";
    }
    std::error_code error;
    auto fallback = std::filesystem::temp_directory_path(error);
    if (error) {
        fallback = L".";
    }
    return fallback / L"Axiom" / L"automations.bin";
}
[[nodiscard]] std::filesystem::path default_plugin_directory() {
    if (const auto local_app_data = known_folder(FOLDERID_LocalAppData)) {
        return *local_app_data / L"Axiom" / L"plugins";
    }
    std::error_code error;
    auto fallback = std::filesystem::temp_directory_path(error);
    if (error) {
        fallback = L".";
    }
    return fallback / L"Axiom" / L"plugins";
}
[[nodiscard]] std::filesystem::path default_plugin_trust_path() {
    if (const auto local_app_data = known_folder(FOLDERID_LocalAppData)) {
        return *local_app_data / L"Axiom" / L"plugin-trust.conf";
    }
    std::error_code error;
    auto fallback = std::filesystem::temp_directory_path(error);
    if (error)
        fallback = L".";
    return fallback / L"Axiom" / L"plugin-trust.conf";
}
[[nodiscard]] std::filesystem::path default_restore_recovery_journal_path() {
    return default_app_settings_path().parent_path() / L"restore-recovery.journal";
}
[[nodiscard]] std::vector<std::filesystem::path> default_restore_destinations() {
    return {
        default_app_settings_path(),
        default_reminder_store_path(),
        default_automation_store_path(),
        default_journal_store_path(),
    };
}
[[nodiscard]] StartupRestoreRecoveryReport recover_default_local_data_before_store_load() {
    const auto destinations = default_restore_destinations();
    return StartupRestoreRecovery::recover_before_store_load(
        default_restore_recovery_journal_path(), destinations);
}
[[nodiscard]] std::wstring widen_ascii(std::string_view text) {
    return std::wstring{text.begin(), text.end()};
}
[[nodiscard]] std::optional<std::string> narrow_ascii(std::wstring_view text) {
    std::string output;
    output.reserve(text.size());
    for (const auto ch : text) {
        if (ch < 0 || ch > 0x7f)
            return std::nullopt;
        output.push_back(static_cast<char>(ch));
    }
    return output;
}
[[nodiscard]] std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty())
        return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error{"UTF-8 conversion input is too large."};
    }
    const int source_size = static_cast<int>(text.size());
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                             source_size, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::system_error{static_cast<int>(GetLastError()), std::system_category(),
                                "Unable to convert support bundle text to UTF-8"};
    }
    std::string output(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), source_size,
                                            output.data(), required, nullptr, nullptr);
    if (written != required) {
        throw std::system_error{static_cast<int>(GetLastError()), std::system_category(),
                                "Unable to convert support bundle text to UTF-8"};
    }
    return output;
}
struct SupportBundleCommandArguments {
    std::filesystem::path output_path;
    bool include_diagnostic_messages{};
    bool include_sensitive_fields{};
};
[[nodiscard]] std::optional<SupportBundleCommandArguments>
parse_support_bundle_arguments(std::wstring_view arguments) {
    auto remaining = trim(arguments);
    if (remaining.empty())
        return std::nullopt;
    std::wstring path_text;
    if (remaining.front() == L'"') {
        const auto closing = remaining.find(L'"', 1);
        if (closing == std::wstring::npos || closing == 1)
            return std::nullopt;
        path_text = remaining.substr(1, closing - 1);
        remaining = trim(std::wstring_view{remaining}.substr(closing + 1));
    } else {
        const auto [first, rest] = split_command(remaining);
        path_text = first;
        remaining = rest;
    }
    SupportBundleCommandArguments parsed{std::filesystem::path{path_text}};
    bool saw_messages = false;
    bool saw_sensitive = false;
    while (!remaining.empty()) {
        const auto [raw_option, rest] = split_command(remaining);
        const auto option = lower_copy(raw_option);
        if (option == L"messages") {
            if (saw_messages)
                return std::nullopt;
            saw_messages = true;
            parsed.include_diagnostic_messages = true;
        } else if (option == L"sensitive") {
            if (saw_sensitive)
                return std::nullopt;
            saw_sensitive = true;
            parsed.include_sensitive_fields = true;
        } else {
            return std::nullopt;
        }
        remaining = rest;
    }
    return parsed;
}
[[nodiscard]] AppSettings default_app_settings() {
    return AppSettings{default_index_roots(), {}, false, true, true};
}
[[nodiscard]] std::pair<std::wstring, std::vector<std::wstring>>
journal_text_and_tags(std::wstring_view raw) {
    std::wstring text;
    std::vector<std::wstring> tags;
    std::size_t offset = 0;
    while (offset < raw.size()) {
        while (offset < raw.size() && std::iswspace(raw[offset]))
            ++offset;
        const auto start = offset;
        while (offset < raw.size() && !std::iswspace(raw[offset]))
            ++offset;
        if (start == offset)
            break;
        const auto token = raw.substr(start, offset - start);
        if (token.size() > 1 && token.front() == L'#') {
            tags.emplace_back(token.substr(1));
        } else {
            if (!text.empty())
                text.push_back(L' ');
            text.append(token);
        }
    }
    return {trim(text), JournalStore::normalize_tags(tags)};
}
[[nodiscard]] std::pair<std::chrono::system_clock::time_point,
                        std::chrono::system_clock::time_point>
local_today_bounds() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    if (localtime_s(&local, &raw) != 0) {
        return {now - std::chrono::hours{24}, now + std::chrono::seconds{1}};
    }
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    auto begin_raw = std::mktime(&local);
    local.tm_mday += 1;
    auto end_raw = std::mktime(&local);
    if (begin_raw == static_cast<std::time_t>(-1) || end_raw == static_cast<std::time_t>(-1)) {
        return {now - std::chrono::hours{24}, now + std::chrono::seconds{1}};
    }
    return {std::chrono::system_clock::from_time_t(begin_raw),
            std::chrono::system_clock::from_time_t(end_raw)};
}
[[nodiscard]] std::wstring format_size(std::uintmax_t bytes) {
    constexpr std::uintmax_t kib = 1024;
    constexpr std::uintmax_t mib = 1024 * kib;
    constexpr std::uintmax_t gib = 1024 * mib;
    if (bytes >= gib) {
        return std::to_wstring(bytes / gib) + L" GB";
    }
    if (bytes >= mib) {
        return std::to_wstring(bytes / mib) + L" MB";
    }
    if (bytes >= kib) {
        return std::to_wstring(bytes / kib) + L" KB";
    }
    return std::to_wstring(bytes) + L" B";
}
} // namespace
CommandEngine::CommandEngine(
    FileIndex &file_index, ClipboardStore &clipboard_store, ReminderCenter &reminder_center,
    RuntimeScheduler &runtime_scheduler, BoundedExecutor &action_executor,
    ExecutionDiagnostics &execution_diagnostics, SystemMetricsSampler &system_metrics,
    NotificationCenter &notification_center, JournalStore &journal_store,
    ActionRegistry &action_registry, PluginHost &plugin_host, AutomationEngine &automation_engine,
    IndexConfigurationChanged index_configuration_changed,
    IndexRecoveryHandler index_recovery_handler, AppSettingsHandler app_settings_handler,
    DataHandler data_handler, PluginHandler plugin_handler,
    JournalContextPolicy journal_context_policy)
    : file_index_{file_index}, clipboard_store_{clipboard_store}, reminder_center_{reminder_center},
      runtime_scheduler_{runtime_scheduler}, action_executor_{action_executor},
      execution_diagnostics_{execution_diagnostics}, system_metrics_{system_metrics},
      notification_center_{notification_center}, journal_store_{journal_store},
      action_registry_{action_registry}, plugin_host_{plugin_host},
      automation_engine_{automation_engine},
      index_configuration_changed_{std::move(index_configuration_changed)},
      index_recovery_handler_{std::move(index_recovery_handler)},
      app_settings_handler_{std::move(app_settings_handler)},
      data_handler_{std::move(data_handler)}, plugin_handler_{std::move(plugin_handler)},
      journal_context_policy_{std::move(journal_context_policy)} {
    const auto add = [this](std::wstring name, std::wstring category, std::wstring usage,
                            std::wstring summary, std::wstring example,
                            std::vector<CommandTopic> topics, Handler handler) {
        commands_.push_back(Command{std::move(name), std::move(category), std::move(usage),
                                    std::move(summary), std::move(example), std::move(topics),
                                    std::move(handler)});
    };
    add(L"help", L"Core", L"help [command [subcommand]]",
        L"Browse the command library or inspect one command/subcommand.", L"help auto daily", {},
        [this](std::wstring_view arguments) { return help(arguments); });
    add(L"open", L"Core", L"open <path-or-url>",
        L"Open a file, folder, document, or URL with Windows Shell.",
        L"open C:\\Users\\Public\\Documents", {}, command_open);
    add(L"run", L"Core", L"run <command-line>", L"Start a process in a new console.",
        L"run cmd /c echo Axiom", {}, command_run);
    add(L"pid", L"System", L"pid <process-name>", L"Find process IDs by executable name.",
        L"pid explorer.exe", {}, command_pid);
    add(L"kill", L"System", L"kill <pid>", L"Terminate a process by PID.", L"kill 4242", {},
        command_kill);
    add(L"port", L"System", L"port <1-65535>",
        L"Show IPv4 TCP endpoints and owning processes for a local port.", L"port 8080", {},
        command_port);
    add(L"find", L"Search", L"find <query>",
        L"Search the local Axiom file index. Unknown text also falls back to file search.",
        L"find architecture.pdf", {},
        [this](std::wstring_view arguments) { return search_files(arguments); });
    add(L"index", L"Search", L"index [status|rebuild|resync|root|exclude]",
        L"Inspect, rebuild, recover, or configure indexed roots and directory exclusions.",
        L"index status",
        {
            {L"status", L"index status", L"Show index and watcher recovery state.",
             L"index status"},
            {L"rebuild", L"index rebuild", L"Start a plain background full index rebuild.",
             L"index rebuild"},
            {L"resync", L"index resync",
             L"Run explicit watcher recovery plus exact-generation full resync.", L"index resync"},
            {L"root list", L"index root list", L"List configured index roots.", L"index root list"},
            {L"root add", L"index root add <path>",
             L"Add an indexed root and restart watcher coverage.", L"index root add C:\\Projects"},
            {L"root remove", L"index root remove <path>", L"Remove an indexed root.",
             L"index root remove C:\\Archive"},
            {L"exclude list", L"index exclude list", L"List excluded directory names.",
             L"index exclude list"},
            {L"exclude add", L"index exclude add <directory-name>",
             L"Exclude a directory name from indexing.", L"index exclude add node_modules"},
            {L"exclude remove", L"index exclude remove <directory-name>",
             L"Remove a directory-name exclusion.", L"index exclude remove build"},
        },
        [this](std::wstring_view arguments) { return index_command(arguments); });
    add(L"clip", L"Clipboard", L"clip [list|search|show|copy|pin|unpin|delete|clear|status]",
        L"Browse, search, pin, restore, and manage text clipboard history.", L"clip list",
        {
            {L"list", L"clip list", L"List recent clipboard entries.", L"clip list"},
            {L"status", L"clip status", L"Show clipboard history statistics.", L"clip status"},
            {L"search", L"clip search <query>", L"Search clipboard text history.",
             L"clip search deployment"},
            {L"show", L"clip show <id>", L"Show one clipboard entry.", L"clip show 12"},
            {L"copy", L"clip copy <id>", L"Restore an entry to the Windows clipboard.",
             L"clip copy 12"},
            {L"pin", L"clip pin <id>", L"Pin an entry against normal eviction.", L"clip pin 12"},
            {L"unpin", L"clip unpin <id>", L"Unpin a clipboard entry.", L"clip unpin 12"},
            {L"delete", L"clip delete <id>", L"Delete one clipboard entry.", L"clip delete 12"},
            {L"clear", L"clip clear [all]",
             L"Clear unpinned history, or all history with explicit all.", L"clip clear"},
        },
        [this](std::wstring_view arguments) { return clipboard_command(arguments); });
    add(L"remind", L"Reminders",
        L"remind [in <duration>|at <date> <time>|every <duration>] <message>",
        L"Create persistent one-shot or recurring reminders restored across Axiom restarts.",
        L"remind in 20m Check the build",
        {
            {L"list", L"remind list", L"List active reminders.", L"remind list"},
            {L"status", L"remind status", L"Show reminder runtime/store state.", L"remind status"},
            {L"in", L"remind in <duration> <message>",
             L"Create a one-shot reminder relative to now.", L"remind in 20m Check the build"},
            {L"at", L"remind at <YYYY-MM-DD> <HH:MM[:SS]> <message>",
             L"Create a one-shot reminder at local date/time.",
             L"remind at 2026-09-14 09:30 Stand-up"},
            {L"every", L"remind every <duration> <message>",
             L"Create a fixed-interval recurring reminder.", L"remind every 2h Drink water"},
            {L"cancel", L"remind cancel <id>", L"Cancel one reminder.", L"remind cancel 4"},
            {L"clear", L"remind clear", L"Clear all active reminders.", L"remind clear"},
        },
        [this](std::wstring_view arguments) { return reminder_command(arguments); });
    add(L"note", L"Journal", L"note <text> [#tags]", L"Capture a persistent journal note quickly.",
        L"note Ship candidate #release", {}, [this](std::wstring_view arguments) {
            return journal_command(L"add " + std::wstring{arguments});
        });
    add(L"journal", L"Journal",
        L"journal [today|recent|add|search|tag|show|delete|clear-notes|context|status]",
        L"Capture, browse, search, tag, and retrieve the local journal/activity timeline.",
        L"journal today",
        {
            {L"today", L"journal today", L"Show today's journal/activity timeline.",
             L"journal today"},
            {L"recent", L"journal recent", L"Show recent journal entries.", L"journal recent"},
            {L"add", L"journal add <text> [#tags]", L"Add a persistent journal note.",
             L"journal add Release candidate ready #release"},
            {L"search", L"journal search <query>", L"Search journal text.",
             L"journal search restore"},
            {L"tag", L"journal tag <tag>", L"Filter journal entries by tag.",
             L"journal tag release"},
            {L"show", L"journal show <id>", L"Show one journal entry.", L"journal show 42"},
            {L"delete", L"journal delete <id>", L"Delete one journal entry.", L"journal delete 42"},
            {L"clear-notes", L"journal clear-notes",
             L"Delete user-created notes while preserving activity.", L"journal clear-notes"},
            {L"context", L"journal context",
             L"Show bounded context output when context access is enabled.", L"journal context"},
            {L"status", L"journal status", L"Show journal privacy/store state.", L"journal status"},
        },
        [this](std::wstring_view arguments) { return journal_command(arguments); });
    add(L"auto", L"Automation",
        L"auto "
        L"[list|actions|status|show|in|at|every|daily|weekly|policy|enable|disable|cancel|clear]",
        L"Create persistent interval/calendar automations and configure bounded retry/failure "
        L"policy.",
        L"auto daily 09:00 notify Build check",
        {
            {L"list", L"auto list", L"List persisted automations.", L"auto list"},
            {L"actions", L"auto actions", L"List currently registered automation actions.",
             L"auto actions"},
            {L"status", L"auto status", L"Show automation engine/store/retry state.",
             L"auto status"},
            {L"show", L"auto show <id>", L"Show one automation.", L"auto show 7"},
            {L"in", L"auto in <duration> <action> [payload]",
             L"Schedule a one-shot action relative to now.", L"auto in 10m notify Build finished"},
            {L"at", L"auto at <YYYY-MM-DD> <HH:MM[:SS]> <action> [payload]",
             L"Schedule a one-shot action at local date/time.",
             L"auto at 2026-09-14 09:00 notify Morning check"},
            {L"every", L"auto every <duration> <action> [payload]",
             L"Schedule a fixed-interval recurring action.", L"auto every 1h notify Hourly check"},
            {L"daily", L"auto daily <HH:MM[:SS]> <action> [payload]",
             L"Schedule a daily local wall-clock action.", L"auto daily 09:00 notify Daily check"},
            {L"weekly", L"auto weekly <sun,mon,...> <HH:MM[:SS]> <action> [payload]",
             L"Schedule a weekly local wall-clock action.",
             L"auto weekly mon,fri 18:00 notify Weekly wrap"},
            {L"policy", L"auto policy <id> <none|fixed|exponential> ...",
             L"Configure bounded retry/failure policy; help text covers all policy variants.",
             L"auto policy 7 fixed 3 30s continue"},
            {L"enable", L"auto enable <id>", L"Enable and re-arm an automation.", L"auto enable 7"},
            {L"disable", L"auto disable <id>", L"Disable an automation without deleting it.",
             L"auto disable 7"},
            {L"cancel", L"auto cancel <id>", L"Delete an automation.", L"auto cancel 7"},
            {L"clear", L"auto clear all", L"Delete all automations.", L"auto clear all"},
        },
        [this](std::wstring_view arguments) { return automation_command(arguments); });
    add(L"plugin", L"Plugins",
        L"plugin [status|list|commands|install|enable|disable|verify|reload|remove]",
        L"Manage explicitly installed and SHA-256 pinned Axiom SDK plugins.", L"plugin status",
        {
            {L"status", L"plugin status", L"Show managed plugin/trust state.", L"plugin status"},
            {L"list", L"plugin list", L"List installed managed plugins.", L"plugin list"},
            {L"commands", L"plugin commands", L"List commands registered by loaded plugins.",
             L"plugin commands"},
            {L"install", L"plugin install <manifest.axp>",
             L"Install and pin a plugin package without auto-enabling it.",
             L"plugin install C:\\Plugins\\hello.axp"},
            {L"enable", L"plugin enable <id>", L"Enable an installed verified plugin.",
             L"plugin enable axiom.sample.hello"},
            {L"disable", L"plugin disable <id>", L"Disable and unload a plugin.",
             L"plugin disable axiom.sample.hello"},
            {L"verify", L"plugin verify <id|all>",
             L"Verify pinned package hashes and quarantine failures.", L"plugin verify all"},
            {L"reload", L"plugin reload <id|all>", L"Unload, verify, and reload enabled plugins.",
             L"plugin reload axiom.sample.hello"},
            {L"remove", L"plugin remove <id>", L"Unload and remove an installed plugin package.",
             L"plugin remove axiom.sample.hello"},
        },
        [this](std::wstring_view arguments) { return plugin_command(arguments); });
    add(L"notify", L"System", L"notify [list|show <id>|clear|status]",
        L"Inspect Axiom's non-blocking local notification history and delivery queue.",
        L"notify list",
        {
            {L"list", L"notify list", L"List notification history.", L"notify list"},
            {L"status", L"notify status", L"Show pending/history notification counts.",
             L"notify status"},
            {L"show", L"notify show <id>", L"Show one notification.", L"notify show 3"},
            {L"clear", L"notify clear", L"Clear notification history.", L"notify clear"},
        },
        [this](std::wstring_view arguments) { return notification_command(arguments); });
    add(L"settings", L"Settings",
        L"settings [status|startup|tray|notifications|plugins|journal-protection|theme]",
        L"Inspect or change unified Axiom application settings.", L"settings theme dark",
        {
            {L"status", L"settings status", L"Show unified application settings.",
             L"settings status"},
            {L"startup", L"settings startup <on|off>", L"Enable or disable Start with Windows.",
             L"settings startup on"},
            {L"tray", L"settings tray <on|off>", L"Choose whether close hides Axiom to the tray.",
             L"settings tray on"},
            {L"notifications", L"settings notifications <on|off>",
             L"Enable or disable desktop notification delivery.", L"settings notifications on"},
            {L"plugins", L"settings plugins <on|off>", L"Enable or disable managed plugin loading.",
             L"settings plugins off"},
            {L"journal-protection", L"settings journal-protection <on|off>",
             L"Enable or disable live journal DPAPI protection.",
             L"settings journal-protection on"},
            {L"theme",
             L"settings theme <system|light|dark|oled|graphite|midnight|nord|high-contrast>",
             L"Persist and apply the native palette theme.", L"settings theme graphite"},
        },
        [this](std::wstring_view arguments) {
            if (!app_settings_handler_) {
                return CommandResult{{{L"Settings handler is unavailable.", ResultKind::error}},
                                     false};
            }
            return app_settings_handler_(arguments);
        });
    add(L"data", L"Data & Privacy",
        L"data "
        L"[status|support-bundle|backup|backup-portable|inspect|restore|journal-export|journal-"
        L"export-all|journal-import|journal-protection|context-access|retention]",
        L"Backup, restore, export/import, inspect privacy state, and configure local-data "
        L"retention.",
        L"data status",
        {
            {L"status", L"data status", L"Show local-data privacy/recovery state.", L"data status"},
            {L"support-bundle", L"data support-bundle <output-path> [messages] [sensitive]",
             L"Explicitly export bounded diagnostics; messages/paths are opt-in.",
             L"data support-bundle C:\\Temp\\axiom-support.txt"},
            {L"backup", L"data backup <archive-path>",
             L"Create the default protected local-data backup.",
             L"data backup C:\\Backups\\axiom.axb"},
            {L"backup-portable", L"data backup-portable <archive-path>",
             L"Create an explicitly portable backup.",
             L"data backup-portable D:\\Transfer\\axiom.axb"},
            {L"inspect", L"data inspect <archive-path>",
             L"Inspect an archive without restoring it.", L"data inspect C:\\Backups\\axiom.axb"},
            {L"restore", L"data restore <archive-path>",
             L"Transactionally restore validated local data.",
             L"data restore C:\\Backups\\axiom.axb"},
            {L"journal-export", L"data journal-export <file.axj>",
             L"Export journal notes to a portable AXJR file.",
             L"data journal-export C:\\Export\\notes.axj"},
            {L"journal-export-all", L"data journal-export-all <file.axj>",
             L"Export notes plus activity to a portable AXJR file.",
             L"data journal-export-all C:\\Export\\journal.axj"},
            {L"journal-import", L"data journal-import <file.axj>",
             L"Import a portable journal file.", L"data journal-import C:\\Export\\notes.axj"},
            {L"journal-protection", L"data journal-protection <on|off>",
             L"Alias the live journal protection setting.", L"data journal-protection on"},
            {L"context-access", L"data context-access <on|off>",
             L"Enable or disable bounded journal-context retrieval.", L"data context-access off"},
            {L"retention", L"data retention <max-entries|activity-days|note-days> <value>",
             L"Configure bounded journal retention controls.", L"data retention activity-days 90"},
        },
        [this](std::wstring_view arguments) {
            if (!data_handler_)
                return CommandResult{{{L"Local-data handler is unavailable.", ResultKind::error}},
                                     false};
            return data_handler_(arguments);
        });
    add(L"diag", L"Diagnostics",
        L"diag [status|recent [count]|runtime|plugin <id>|action <name>|automation <id>|clear]",
        L"Inspect bounded session diagnostics for runtime, plugin, action, and automation "
        L"execution.",
        L"diag status",
        {
            {L"status", L"diag status", L"Show diagnostics capacity and counts.", L"diag status"},
            {L"recent", L"diag recent [1-200]", L"Show recent bounded diagnostics.",
             L"diag recent 20"},
            {L"runtime", L"diag runtime", L"Show runtime-domain diagnostics.", L"diag runtime"},
            {L"plugin", L"diag plugin <id>", L"Show diagnostics for one plugin.",
             L"diag plugin axiom.sample.hello"},
            {L"action", L"diag action <name>", L"Show diagnostics for one action.",
             L"diag action notify"},
            {L"automation", L"diag automation <id>", L"Show diagnostics for one automation.",
             L"diag automation 7"},
            {L"clear", L"diag clear", L"Clear session diagnostics.", L"diag clear"},
        },
        [this](std::wstring_view arguments) { return diagnostics_command(arguments); });
    add(L"sys", L"System", L"sys",
        L"Show sampled system health and Axiom runtime scheduler/executor metrics.", L"sys", {},
        [this](std::wstring_view arguments) { return system_command(arguments); });
    add(L"exit", L"Core", L"exit", L"Exit Axiom instead of hiding it to the tray.", L"exit", {},
        [](std::wstring_view) {
            return CommandResult{{{L"Exiting Axiom.", ResultKind::info}}, true};
        });
    for (const auto &command : commands_) {
        plugin_host_.reserve_command_name(command.name);
    }
}
const CommandEngine::Command *CommandEngine::find(std::wstring_view name) const noexcept {
    const auto it = std::find_if(commands_.begin(), commands_.end(), [&](const Command &command) {
        return equals_ignore_case(command.name, name);
    });
    return it == commands_.end() ? nullptr : &*it;
}
bool CommandEngine::is_command_name(std::wstring_view name) const noexcept {
    const auto normalized = trim(name);
    return find(normalized) != nullptr || plugin_host_.contains_command(normalized);
}
std::vector<CommandSuggestion> CommandEngine::suggestions(std::wstring_view input,
                                                          std::size_t limit) const {
    std::vector<CommandSuggestion> output;
    if (limit == 0)
        return output;
    const auto normalized = trim(input);
    if (normalized.empty())
        return output;
    if (normalized == L"?") {
        output.push_back({L"? ", L"? — open the command library", ResultKind::heading});
        return output;
    }
    if (normalized.starts_with(L"?")) {
        const auto rest = trim(std::wstring_view{normalized}.substr(1));
        const auto mapped = rest.empty() ? std::wstring{L"help"} : L"help " + rest;
        return suggestions(mapped, limit);
    }
    const auto [name, arguments] = split_command(normalized);
    const auto *exact = find(name);
    if (exact == nullptr) {
        for (const auto &command : commands_) {
            if (!starts_with_ignore_case(command.name, name))
                continue;
            output.push_back(
                {command.name + L" ", command.usage + L" — " + command.summary, ResultKind::info});
            if (output.size() >= limit)
                return output;
        }
        for (const auto &plugin : plugin_host_.list_commands()) {
            if (!starts_with_ignore_case(plugin.name, name))
                continue;
            output.push_back({plugin.name + L" ",
                              plugin.usage + L" — " + plugin.summary + L" [plugin]",
                              ResultKind::info});
            if (output.size() >= limit)
                return output;
        }
        return output;
    }
    if (exact->topics.empty()) {
        if (arguments.empty()) {
            output.push_back(
                {exact->name + L" ", exact->usage + L" — " + exact->summary, ResultKind::info});
        }
        return output;
    }
    const auto topic_prefix = trim(arguments);
    for (const auto &topic : exact->topics) {
        const bool completing_topic =
            topic_prefix.empty() || starts_with_ignore_case(topic.name, topic_prefix);
        const bool inside_topic = !topic_prefix.empty() &&
                                  starts_with_ignore_case(topic_prefix, topic.name) &&
                                  (topic_prefix.size() == topic.name.size() ||
                                   std::iswspace(topic_prefix[topic.name.size()]) != 0);
        if (!completing_topic && !inside_topic)
            continue;
        output.push_back({inside_topic ? normalized : exact->name + L" " + topic.name + L" ",
                          topic.usage + L" — " + topic.summary, ResultKind::info});
        if (output.size() >= limit)
            break;
    }
    if (output.empty()) {
        output.push_back(
            {exact->name + L" ", exact->usage + L" — " + exact->summary, ResultKind::info});
    }
    return output;
}
std::optional<std::wstring> CommandEngine::complete(std::wstring_view input) const {
    const auto normalized = trim(input);
    if (normalized.empty())
        return std::nullopt;
    const auto candidates = suggestions(normalized, 64);
    if (candidates.empty())
        return std::nullopt;
    if (candidates.size() == 1) {
        return candidates.front().completion.size() > normalized.size()
                   ? std::optional<std::wstring>{candidates.front().completion}
                   : std::nullopt;
    }
    std::wstring common = candidates.front().completion;
    for (std::size_t i = 1; i < candidates.size() && !common.empty(); ++i) {
        const auto &candidate = candidates[i].completion;
        std::size_t count = 0;
        const auto limit = std::min(common.size(), candidate.size());
        while (count < limit && std::towlower(common[count]) == std::towlower(candidate[count])) {
            ++count;
        }
        common.resize(count);
    }
    if (common.size() <= normalized.size())
        return std::nullopt;
    return common;
}
CommandResult CommandEngine::help(std::wstring_view arguments) const {
    const auto requested = trim(arguments);
    if (!requested.empty()) {
        const auto [requested_name, requested_topic] = split_command(requested);
        const auto *command = find(requested_name);
        if (command == nullptr) {
            for (const auto &plugin : plugin_host_.list_commands()) {
                if (equals_ignore_case(plugin.name, requested_name) && requested_topic.empty()) {
                    return {
                        {
                            {L"Plugin command", ResultKind::heading},
                            {plugin.usage, ResultKind::info},
                            {plugin.summary, ResultKind::info},
                            {L"Owner: " + widen_ascii(plugin.owner_plugin_id), ResultKind::info},
                        },
                        false};
                }
            }
            return {{{L"Unknown command: " + requested_name, ResultKind::warning}}, false};
        }
        if (!requested_topic.empty()) {
            const auto it = std::find_if(command->topics.begin(), command->topics.end(),
                                         [&](const CommandTopic &topic) {
                                             return equals_ignore_case(topic.name, requested_topic);
                                         });
            if (it == command->topics.end()) {
                return {{
                            {L"Unknown " + command->name + L" subcommand: " + requested_topic,
                             ResultKind::warning},
                            {L"Try: help " + command->name, ResultKind::info},
                        },
                        false};
            }
            return {{
                        {command->name + L" / " + it->name, ResultKind::heading},
                        {it->usage, ResultKind::info},
                        {it->summary, ResultKind::info},
                        {L"Example: " + it->example, ResultKind::info},
                    },
                    false};
        }
        std::vector<ResultLine> lines{
            {command->name + L" — " + command->category, ResultKind::heading},
            {command->usage, ResultKind::info},
            {command->summary, ResultKind::info},
            {L"Example: " + command->example, ResultKind::info},
        };
        if (!command->topics.empty()) {
            lines.push_back({L"Subcommands", ResultKind::heading});
            for (const auto &topic : command->topics) {
                lines.push_back({topic.usage + L" — " + topic.summary, ResultKind::info});
            }
            lines.push_back({L"Use: help " + command->name + L" <subcommand> for an example.",
                             ResultKind::info});
        }
        return {std::move(lines), false};
    }
    std::vector<ResultLine> lines{
        {L"Axiom command library", ResultKind::heading},
        {L"Type a command/subcommand prefix to browse. Tab completes. '?' is an alias for help.",
         ResultKind::info},
    };
    const std::array<std::wstring_view, 9> categories{
        L"Core",    L"Search", L"Clipboard",   L"Reminders", L"Automation",
        L"Journal", L"System", L"Diagnostics", L"Settings", // Data & Privacy / Plugins follow
                                                            // below.
    };
    for (const auto category : categories) {
        bool wrote_heading = false;
        for (const auto &command : commands_) {
            if (command.category != category)
                continue;
            if (!wrote_heading) {
                lines.push_back({std::wstring{category}, ResultKind::heading});
                wrote_heading = true;
            }
            lines.push_back({command.usage + L" — " + command.summary, ResultKind::info});
        }
    }
    for (const auto category :
         {std::wstring_view{L"Data & Privacy"}, std::wstring_view{L"Plugins"}}) {
        bool wrote_heading = false;
        for (const auto &command : commands_) {
            if (command.category != category)
                continue;
            if (!wrote_heading) {
                lines.push_back({std::wstring{category}, ResultKind::heading});
                wrote_heading = true;
            }
            lines.push_back({command.usage + L" — " + command.summary, ResultKind::info});
        }
    }
    const auto plugins = plugin_host_.list_commands();
    if (!plugins.empty()) {
        lines.push_back({L"Loaded plugin commands", ResultKind::heading});
        for (const auto &plugin : plugins) {
            lines.push_back({plugin.usage + L" — " + plugin.summary, ResultKind::info});
        }
    }
    return {std::move(lines), false};
}
CommandResult CommandEngine::search_files(std::wstring_view query) const {
    const auto normalized = trim(query);
    if (normalized.empty()) {
        return {{{L"Usage: find <query>", ResultKind::warning}}, false};
    }
    const auto hits = file_index_.search(normalized, 24);
    const auto status = file_index_.snapshot();
    if (hits.empty()) {
        if (status.state == IndexState::indexing) {
            return {{{L"Index is still building. No match yet for: " + normalized,
                      ResultKind::warning}},
                    false};
        }
        if (status.state == IndexState::failed) {
            return {{{L"File index failed: " + status.last_error, ResultKind::error}}, false};
        }
        return {{{L"No indexed file or folder matched: " + normalized, ResultKind::warning}},
                false};
    }
    std::vector<ResultLine> lines;
    lines.reserve(hits.size() + 1);
    lines.push_back({
        L"Search results: " + std::to_wstring(hits.size()) +
            (status.state == IndexState::indexing ? L" (index still building)" : L""),
        ResultKind::success,
    });
    for (const auto &hit : hits) {
        std::wstring display = hit.is_directory ? L"[DIR] " : L"[FILE] ";
        display += hit.path.wstring();
        if (!hit.is_directory && hit.size > 0) {
            display += L" (" + format_size(hit.size) + L")";
        }
        lines.push_back({std::move(display), ResultKind::info});
    }
    return {std::move(lines), false};
}
CommandResult CommandEngine::index_command(std::wstring_view arguments) const {
    const auto normalized = trim(arguments);
    const auto [raw_action, raw_value] = split_command(normalized);
    const auto action = lower_copy(raw_action);
    if (action.empty() || action == L"status") {
        const auto snapshot = file_index_.snapshot();
        std::vector<ResultLine> lines;
        lines.push_back(
            {L"Index state: " + std::wstring{to_string(snapshot.state)}, ResultKind::success});
        lines.push_back({
            L"Entries=" + std::to_wstring(snapshot.entries) + L" files=" +
                std::to_wstring(snapshot.files) + L" directories=" +
                std::to_wstring(snapshot.directories) + L" incremental-updates=" +
                std::to_wstring(snapshot.incremental_updates),
            ResultKind::info,
        });
        if (snapshot.last_scan_time.count() > 0) {
            lines.push_back({
                L"Last full scan: " + std::to_wstring(snapshot.last_scan_time.count()) + L" ms",
                ResultKind::info,
            });
        }
        for (const auto &root : snapshot.roots) {
            lines.push_back({L"Root: " + root.wstring(), ResultKind::info});
        }
        for (const auto &exclusion : snapshot.excluded_directory_names) {
            lines.push_back({L"Exclude: " + exclusion, ResultKind::info});
        }
        if (!snapshot.last_error.empty()) {
            lines.push_back({L"Last error: " + snapshot.last_error, ResultKind::error});
        }
        if (index_recovery_handler_) {
            const auto recovery = index_recovery_handler_(L"status");
            lines.insert(lines.end(), recovery.lines.begin(), recovery.lines.end());
        }
        return {std::move(lines), false};
    }
    if (action == L"rebuild") {
        file_index_.rebuild();
        return {{{L"File index rebuild started in the background.", ResultKind::success}}, false};
    }
    if (action == L"resync") {
        if (!index_recovery_handler_) {
            return {{{L"Watcher/index recovery handler is unavailable.", ResultKind::error}},
                    false};
        }
        return index_recovery_handler_(L"resync");
    }
    if (action == L"root") {
        const auto [raw_operation, raw_path] = split_command(raw_value);
        const auto operation = lower_copy(raw_operation);
        if (operation.empty() || operation == L"list") {
            const auto snapshot = file_index_.snapshot();
            std::vector<ResultLine> lines;
            lines.push_back({L"Indexed roots", ResultKind::success});
            for (const auto &root : snapshot.roots) {
                lines.push_back({root.wstring(), ResultKind::info});
            }
            if (snapshot.roots.empty()) {
                lines.push_back({L"No configured roots.", ResultKind::warning});
            }
            return {std::move(lines), false};
        }
        const auto path_text = trim(raw_path);
        if (path_text.empty()) {
            return {{{L"Usage: index root <add|remove> <path>", ResultKind::warning}}, false};
        }
        bool changed = false;
        if (operation == L"add") {
            changed = file_index_.add_root(std::filesystem::path{path_text});
        } else if (operation == L"remove") {
            changed = file_index_.remove_root(std::filesystem::path{path_text});
        } else {
            return {{{L"Usage: index root [list|add <path>|remove <path>]", ResultKind::warning}},
                    false};
        }
        if (!changed) {
            return {{{L"Index root configuration was unchanged.", ResultKind::warning}}, false};
        }
        if (index_configuration_changed_) {
            index_configuration_changed_();
        }
        return {{{L"Index root configuration updated.", ResultKind::success}}, false};
    }
    if (action == L"exclude") {
        const auto [raw_operation, raw_name] = split_command(raw_value);
        const auto operation = lower_copy(raw_operation);
        if (operation.empty() || operation == L"list") {
            const auto snapshot = file_index_.snapshot();
            std::vector<ResultLine> lines;
            lines.push_back({L"Configured directory exclusions", ResultKind::success});
            for (const auto &exclusion : snapshot.excluded_directory_names) {
                lines.push_back({exclusion, ResultKind::info});
            }
            if (snapshot.excluded_directory_names.empty()) {
                lines.push_back({L"No custom exclusions.", ResultKind::info});
            }
            return {std::move(lines), false};
        }
        const auto name = trim(raw_name);
        if (name.empty()) {
            return {{{L"Usage: index exclude <add|remove> <directory-name>", ResultKind::warning}},
                    false};
        }
        bool changed = false;
        if (operation == L"add") {
            changed = file_index_.add_exclusion(name);
        } else if (operation == L"remove") {
            changed = file_index_.remove_exclusion(name);
        } else {
            return {
                {{L"Usage: index exclude [list|add <name>|remove <name>]", ResultKind::warning}},
                false};
        }
        if (!changed) {
            return {{{L"Index exclusion configuration was unchanged.", ResultKind::warning}},
                    false};
        }
        if (index_configuration_changed_) {
            index_configuration_changed_();
        }
        return {{{L"Index exclusion configuration updated.", ResultKind::success}}, false};
    }
    return {{{L"Usage: index [status|rebuild|resync|root|exclude]", ResultKind::warning}}, false};
}
CommandResult CommandEngine::clipboard_command(std::wstring_view arguments) const {
    const auto normalized = trim(arguments);
    const auto [raw_action, raw_value] = split_command(normalized);
    const auto action = lower_copy(raw_action);
    const auto list_entries = [](std::vector<ClipboardEntry> entries, std::wstring heading) {
        std::vector<ResultLine> lines;
        lines.reserve(entries.size() + 1);
        lines.push_back({std::move(heading), ResultKind::success});
        for (const auto &entry : entries) {
            lines.push_back({format_clipboard_entry(entry), ResultKind::info});
        }
        if (entries.empty()) {
            lines.push_back({L"Clipboard history is empty.", ResultKind::warning});
        }
        return CommandResult{std::move(lines), false};
    };
    if (action.empty() || action == L"list") {
        return list_entries(clipboard_store_.recent(20), L"Recent clipboard text");
    }
    if (action == L"status") {
        const auto snapshot = clipboard_store_.snapshot();
        return {{
                    {L"Clipboard history: " + std::to_wstring(snapshot.entries) + L" entries",
                     ResultKind::success},
                    {L"Pinned=" + std::to_wstring(snapshot.pinned) + L" text-bytes=" +
                         std::to_wstring(snapshot.text_bytes),
                     ResultKind::info},
                },
                false};
    }
    if (action == L"search") {
        const auto query = trim(raw_value);
        if (query.empty()) {
            return {{{L"Usage: clip search <query>", ResultKind::warning}}, false};
        }
        return list_entries(clipboard_store_.search(query, 20), L"Clipboard matches for: " + query);
    }
    if (action == L"clear") {
        if (equals_ignore_case(trim(raw_value), L"all")) {
            clipboard_store_.clear_all();
            return {
                {{L"Clipboard history cleared, including pinned entries.", ResultKind::success}},
                false};
        }
        const auto removed = clipboard_store_.clear_unpinned();
        return {{{L"Cleared " + std::to_wstring(removed) + L" unpinned clipboard entries.",
                  ResultKind::success}},
                false};
    }
    auto value = trim(raw_value);
    std::wstring effective_action = action;
    if (const auto shorthand_id = parse_u64(action)) {
        value = action;
        effective_action = L"copy";
    }
    const auto id = parse_u64(value);
    if (!id || *id == 0) {
        return {{{L"Usage: clip <copy|show|pin|unpin|delete> <id>", ResultKind::warning}}, false};
    }
    const auto entry = clipboard_store_.find(*id);
    if (!entry) {
        return {{{L"Clipboard entry not found: #" + std::to_wstring(*id), ResultKind::warning}},
                false};
    }
    if (effective_action == L"show") {
        return {{
                    {format_clipboard_entry(*entry), ResultKind::success},
                    {entry->text, ResultKind::info},
                },
                false};
    }
    if (effective_action == L"copy") {
        std::wstring error;
        if (!write_clipboard_text(entry->text, error)) {
            return {{{L"Clipboard write failed: " + error, ResultKind::error}}, false};
        }
        return {{{L"Restored clipboard entry #" + std::to_wstring(entry->id), ResultKind::success}},
                false};
    }
    if (effective_action == L"pin" || effective_action == L"unpin") {
        const bool pinned = effective_action == L"pin";
        (void)clipboard_store_.set_pinned(*id, pinned);
        return {
            {{(pinned ? L"Pinned #" : L"Unpinned #") + std::to_wstring(*id), ResultKind::success}},
            false};
    }
    if (effective_action == L"delete") {
        (void)clipboard_store_.erase(*id);
        return {{{L"Deleted clipboard entry #" + std::to_wstring(*id), ResultKind::success}},
                false};
    }
    return {{{L"Usage: clip [list|search <query>|show <id>|copy <id>|pin <id>|unpin <id>|delete "
              L"<id>|clear [all]|status]",
              ResultKind::warning}},
            false};
}
CommandResult CommandEngine::reminder_command(std::wstring_view arguments) const {
    const auto normalized = trim(arguments);
    const auto [raw_action, raw_value] = split_command(normalized);
    const auto action = lower_copy(raw_action);
    if (action.empty() || action == L"list") {
        const auto reminders = reminder_center_.snapshot();
        std::vector<ResultLine> lines;
        lines.push_back(
            {L"Active reminders: " + std::to_wstring(reminders.size()), ResultKind::success});
        for (const auto &reminder : reminders) {
            std::wstring schedule;
            if (reminder.recurring()) {
                schedule = L" [every " + format_interval(reminder.repeat_interval) + L", fired " +
                           std::to_wstring(reminder.fire_count) + L"]";
            }
            lines.push_back({
                L"#" + std::to_wstring(reminder.id) + L" " + format_due_time(reminder.due_at) +
                    L" (in " + format_remaining(reminder.due_at) + L")" + schedule + L" " +
                    reminder.message,
                ResultKind::info,
            });
        }
        if (reminders.empty()) {
            lines.push_back({L"No active reminders.", ResultKind::info});
        }
        return {std::move(lines), false};
    }
    if (action == L"status") {
        const auto runtime = runtime_scheduler_.stats();
        std::vector<ResultLine> lines{
            {L"Active reminders: " + std::to_wstring(reminder_center_.active_count()),
             ResultKind::success},
            {L"Runtime active=" + std::to_wstring(runtime.active_tasks) + L" scheduled=" +
                 std::to_wstring(runtime.total_scheduled) + L" executed=" +
                 std::to_wstring(runtime.total_executed) + L" cancelled=" +
                 std::to_wstring(runtime.total_cancelled) + L" failed=" +
                 std::to_wstring(runtime.total_failed),
             ResultKind::info},
        };
        if (const auto persistence = reminder_center_.persistence_status()) {
            lines.push_back({
                L"Persistence generation=" + std::to_wstring(persistence->generation) +
                    L" records=" + std::to_wstring(persistence->record_count) +
                    (persistence->recovered_from_alternate ? L" recovered=yes" : L" recovered=no"),
                persistence->recovered_from_alternate ? ResultKind::warning : ResultKind::info,
            });
            lines.push_back({L"Store: " + persistence->path.wstring(), ResultKind::info});
        }
        const auto persistence_error = reminder_center_.last_persistence_error();
        if (!persistence_error.empty()) {
            std::wstring wide_error{persistence_error.begin(), persistence_error.end()};
            lines.push_back({L"Last persistence error: " + wide_error, ResultKind::error});
        }
        return {std::move(lines), false};
    }
    if (action == L"in") {
        const auto [raw_duration, raw_message] = split_command(raw_value);
        const auto duration = parse_duration_token(raw_duration);
        const auto message = trim(raw_message);
        if (!duration || message.empty()) {
            return {{{L"Usage: remind in <duration> <message> (units: ms, s, m, h, d)",
                      ResultKind::warning}},
                    false};
        }
        const auto id = reminder_center_.schedule_after(*duration, message);
        const auto reminders = reminder_center_.snapshot();
        const auto it = std::find_if(reminders.begin(), reminders.end(),
                                     [id](const Reminder &reminder) { return reminder.id == id; });
        const std::wstring due = it == reminders.end() ? L"scheduled" : format_due_time(it->due_at);
        return {
            {{
                L"Reminder #" + std::to_wstring(id) + L" scheduled for " + due + L": " + message,
                ResultKind::success,
            }},
            false};
    }
    if (action == L"at") {
        const auto [first_token, after_first] = split_command(raw_value);
        std::wstring raw_date = first_token;
        std::wstring raw_time;
        std::wstring message;
        if (const auto separator = raw_date.find(L'T'); separator != std::wstring::npos) {
            raw_time = raw_date.substr(separator + 1);
            raw_date.resize(separator);
            message = trim(after_first);
        } else {
            const auto [time_token, raw_message] = split_command(after_first);
            raw_time = time_token;
            message = trim(raw_message);
        }
        const auto due_at = parse_local_datetime(raw_date, raw_time);
        if (!due_at || message.empty()) {
            return {{{L"Usage: remind at YYYY-MM-DD HH:MM[:SS] <message>", ResultKind::warning}},
                    false};
        }
        const auto id = reminder_center_.schedule_at(*due_at, message);
        return {{{
                    L"Reminder #" + std::to_wstring(id) + L" scheduled for " +
                        format_due_time(*due_at) + L": " + message,
                    ResultKind::success,
                }},
                false};
    }
    if (action == L"every") {
        const auto [raw_interval, raw_message] = split_command(raw_value);
        const auto interval = parse_duration_token(raw_interval);
        const auto message = trim(raw_message);
        if (!interval || message.empty()) {
            return {{{L"Usage: remind every <duration> <message> (units: ms, s, m, h, d)",
                      ResultKind::warning}},
                    false};
        }
        const auto id = reminder_center_.schedule_every(*interval, message);
        return {
            {{
                L"Recurring reminder #" + std::to_wstring(id) + L" every " +
                    format_interval(*interval) + L"; first due " +
                    format_due_time(std::chrono::system_clock::now() + *interval) + L": " + message,
                ResultKind::success,
            }},
            false};
    }
    if (action == L"cancel") {
        const auto id = parse_u64(raw_value);
        if (!id || *id == 0) {
            return {{{L"Usage: remind cancel <id>", ResultKind::warning}}, false};
        }
        if (!reminder_center_.cancel(*id)) {
            return {{{L"Reminder not found: #" + std::to_wstring(*id), ResultKind::warning}},
                    false};
        }
        return {{{L"Cancelled reminder #" + std::to_wstring(*id), ResultKind::success}}, false};
    }
    if (action == L"clear") {
        const auto count = reminder_center_.active_count();
        reminder_center_.clear();
        return {
            {{L"Cleared " + std::to_wstring(count) + L" active reminders.", ResultKind::success}},
            false};
    }
    return {{{L"Usage: remind [in <duration>|at <date> <time>|every <duration>] <message> | list | "
              L"cancel <id> | clear | status",
              ResultKind::warning}},
            false};
}
CommandResult CommandEngine::automation_command(std::wstring_view arguments) const {
    const auto normalized = trim(arguments);
    const auto [raw_action, raw_value] = split_command(normalized);
    const auto action = lower_copy(raw_action);
    const auto label_for = [](std::wstring_view action_name, std::wstring_view payload) {
        std::wstring label{action_name};
        const auto preview = clipboard_preview(payload, 72);
        if (!preview.empty()) {
            label += L": " + preview;
        }
        return label;
    };
    const auto parse_action_spec =
        [](std::wstring_view text) -> std::optional<std::pair<std::wstring, std::wstring>> {
        const auto [name, payload] = split_command(text);
        if (name.empty()) {
            return std::nullopt;
        }
        return std::pair<std::wstring, std::wstring>{lower_copy(name), trim(payload)};
    };
    const auto schedule_text = [](const Automation &item) {
        if (item.schedule_kind == AutomationScheduleKind::fixed_interval) {
            return std::wstring{L"every="} + format_interval(item.repeat_interval);
        }
        if (item.schedule_kind == AutomationScheduleKind::local_calendar) {
            return std::wstring{L"calendar="} + format_calendar_schedule(item.calendar_schedule);
        }
        return std::wstring{L"one-shot"};
    };
    if (action.empty() || action == L"list") {
        const auto automations = automation_engine_.snapshot();
        std::vector<ResultLine> lines;
        lines.push_back(
            {L"Automations: " + std::to_wstring(automations.size()), ResultKind::success});
        for (const auto &item : automations) {
            const bool resolved = action_registry_.contains(item.action_name);
            std::wstring retry;
            if (item.retry_pending()) {
                retry = L" retry=" + std::to_wstring(item.retry_attempt) + L"/" +
                        std::to_wstring(item.failure_policy.max_retries);
            }
            lines.push_back({
                L"#" + std::to_wstring(item.id) + (item.enabled ? L" [on] " : L" [off] ") +
                    format_due_time(item.due_at) + L" (in " + format_remaining(item.due_at) +
                    L") " + schedule_text(item) + retry + L" " + item.name + L" -> " +
                    item.action_name + (resolved ? L"" : L" [ACTION MISSING]"),
                resolved ? ResultKind::info : ResultKind::warning,
            });
        }
        if (automations.empty()) {
            lines.push_back({L"No persistent automations.", ResultKind::info});
        }
        return {std::move(lines), false};
    }
    if (action == L"actions") {
        const auto actions = action_registry_.list();
        std::vector<ResultLine> lines;
        lines.push_back({L"Registered automation actions", ResultKind::success});
        for (const auto &descriptor : actions) {
            lines.push_back({descriptor.name + L" - " + descriptor.summary, ResultKind::info});
        }
        if (actions.empty()) {
            lines.push_back({L"No actions are registered.", ResultKind::warning});
        }
        return {std::move(lines), false};
    }
    if (action == L"status") {
        const auto stats = automation_engine_.stats();
        const auto persistence = automation_engine_.persistence_status();
        std::vector<ResultLine> lines{
            {L"Automation engine", ResultKind::success},
            {L"total=" + std::to_wstring(stats.total) + L" enabled=" +
                 std::to_wstring(stats.enabled) + L" disabled=" + std::to_wstring(stats.disabled) +
                 L" recurring=" + std::to_wstring(stats.recurring) + L" calendar=" +
                 std::to_wstring(stats.calendar) + L" unresolved=" +
                 std::to_wstring(stats.unresolved_actions),
             ResultKind::info},
            {L"retry-policy=" + std::to_wstring(stats.retry_enabled) + L" retrying=" +
                 std::to_wstring(stats.retrying) + L" runs=" + std::to_wstring(stats.total_runs) +
                 L" failures=" + std::to_wstring(stats.total_failures),
             ResultKind::info},
            {L"Persistence generation=" + std::to_wstring(persistence.generation) + L" records=" +
                 std::to_wstring(persistence.record_count) +
                 (persistence.recovered_from_alternate ? L" recovered=yes" : L" recovered=no"),
             persistence.recovered_from_alternate ? ResultKind::warning : ResultKind::info},
            {L"Store: " + persistence.path.wstring(), ResultKind::info},
        };
        const auto error = automation_engine_.last_persistence_error();
        if (!error.empty()) {
            lines.push_back({L"Persistence error: " + std::wstring{error.begin(), error.end()},
                             ResultKind::error});
        }
        return {std::move(lines), false};
    }
    if (action == L"show") {
        const auto id = parse_u64(raw_value);
        if (!id || *id == 0) {
            return {{{L"Usage: auto show <id>", ResultKind::warning}}, false};
        }
        const auto item = automation_engine_.find(*id);
        if (!item) {
            return {{{L"Automation not found: #" + std::to_wstring(*id), ResultKind::warning}},
                    false};
        }
        std::vector<ResultLine> lines{
            {L"Automation #" + std::to_wstring(item->id) + L" — " + item->name,
             ResultKind::success},
            {L"state=" + std::wstring{item->enabled ? L"enabled" : L"disabled"} + L" action=" +
                 item->action_name +
                 (action_registry_.contains(item->action_name) ? L"" : L" [missing]"),
             ResultKind::info},
            {L"due=" + format_due_time(item->due_at) + L" (in " + format_remaining(item->due_at) +
                 L") " + schedule_text(*item),
             ResultKind::info},
            {L"failure-policy=" + format_failure_policy(item->failure_policy) + L" retry-attempt=" +
                 std::to_wstring(item->retry_attempt) + L" consecutive-failures=" +
                 std::to_wstring(item->consecutive_failures),
             ResultKind::info},
            {L"payload=" +
                 (item->action_payload.empty() ? std::wstring{L"<empty>"} : item->action_payload),
             ResultKind::info},
            {L"runs=" + std::to_wstring(item->run_count) + L" failures=" +
                 std::to_wstring(item->failure_count),
             ResultKind::info},
        };
        if (item->retry_resume_at) {
            lines.push_back(
                {L"retry-resume=" + format_due_time(*item->retry_resume_at), ResultKind::warning});
        }
        if (item->last_run_at) {
            lines.push_back({L"last-run=" + format_due_time(*item->last_run_at) + L" result=" +
                                 std::wstring{item->last_success ? L"success" : L"failure"},
                             item->last_success ? ResultKind::info : ResultKind::warning});
        }
        if (!item->last_error.empty()) {
            lines.push_back({L"last-error=" + item->last_error, ResultKind::error});
        }
        return {std::move(lines), false};
    }
    if (action == L"cancel") {
        const auto id = parse_u64(raw_value);
        if (!id || *id == 0) {
            return {{{L"Usage: auto cancel <id>", ResultKind::warning}}, false};
        }
        if (!automation_engine_.cancel(*id)) {
            return {{{L"Automation not found: #" + std::to_wstring(*id), ResultKind::warning}},
                    false};
        }
        return {{{L"Cancelled automation #" + std::to_wstring(*id), ResultKind::success}}, false};
    }
    if (action == L"enable" || action == L"disable") {
        const auto id = parse_u64(raw_value);
        if (!id || *id == 0) {
            return {{{L"Usage: auto <enable|disable> <id>", ResultKind::warning}}, false};
        }
        const bool enabled = action == L"enable";
        if (!automation_engine_.set_enabled(*id, enabled)) {
            return {{{L"Automation not found: #" + std::to_wstring(*id), ResultKind::warning}},
                    false};
        }
        return {{{std::wstring{enabled ? L"Enabled" : L"Disabled"} + L" automation #" +
                      std::to_wstring(*id),
                  ResultKind::success}},
                false};
    }
    if (action == L"policy") {
        const auto [raw_id, raw_policy] = split_command(raw_value);
        const auto id = parse_u64(raw_id);
        const auto [raw_kind, after_kind] = split_command(raw_policy);
        const auto kind = lower_copy(raw_kind);
        if (!id || *id == 0 || kind.empty()) {
            return {
                {{L"Usage: auto policy <id> <none|fixed|exponential> ...", ResultKind::warning}},
                false};
        }
        AutomationFailurePolicy policy;
        if (kind == L"none") {
            if (!trim(after_kind).empty()) {
                return {{{L"Usage: auto policy <id> none", ResultKind::warning}}, false};
            }
        } else if (kind == L"fixed") {
            const auto [raw_retries, after_retries] = split_command(after_kind);
            const auto [raw_delay, raw_exhaust] = split_command(after_retries);
            const auto retries = parse_u64(raw_retries);
            const auto delay = parse_duration_token(raw_delay);
            const auto exhaust = lower_copy(trim(raw_exhaust));
            if (!retries || *retries == 0 || *retries > 32 || !delay ||
                (!exhaust.empty() && exhaust != L"disable" && exhaust != L"continue")) {
                return {
                    {{L"Usage: auto policy <id> fixed <retries 1-32> <delay> [disable|continue]",
                      ResultKind::warning}},
                    false};
            }
            policy.kind = AutomationRetryKind::fixed;
            policy.max_retries = static_cast<std::uint32_t>(*retries);
            policy.initial_delay = *delay;
            policy.max_delay = *delay;
            policy.disable_on_exhaustion = exhaust == L"disable";
        } else if (kind == L"exponential") {
            const auto [raw_retries, after_retries] = split_command(after_kind);
            const auto [raw_initial, after_initial] = split_command(after_retries);
            const auto [raw_max, raw_exhaust] = split_command(after_initial);
            const auto retries = parse_u64(raw_retries);
            const auto initial = parse_duration_token(raw_initial);
            const auto max_delay = parse_duration_token(raw_max);
            const auto exhaust = lower_copy(trim(raw_exhaust));
            if (!retries || *retries == 0 || *retries > 32 || !initial || !max_delay ||
                (!exhaust.empty() && exhaust != L"disable" && exhaust != L"continue")) {
                return {{{L"Usage: auto policy <id> exponential <retries 1-32> <initial> <max> "
                          L"[disable|continue]",
                          ResultKind::warning}},
                        false};
            }
            policy.kind = AutomationRetryKind::exponential;
            policy.max_retries = static_cast<std::uint32_t>(*retries);
            policy.initial_delay = *initial;
            policy.max_delay = *max_delay;
            policy.disable_on_exhaustion = exhaust == L"disable";
        } else {
            return {
                {{L"Usage: auto policy <id> <none|fixed|exponential> ...", ResultKind::warning}},
                false};
        }
        try {
            if (!automation_engine_.set_failure_policy(*id, policy)) {
                return {{{L"Automation not found: #" + std::to_wstring(*id), ResultKind::warning}},
                        false};
            }
        } catch (const std::exception &exception) {
            return {
                {{L"Failure policy rejected: " + widen_ascii(exception.what()), ResultKind::error}},
                false};
        }
        return {{{L"Automation #" + std::to_wstring(*id) + L" failure policy: " +
                      format_failure_policy(policy),
                  ResultKind::success}},
                false};
    }
    if (action == L"clear") {
        if (lower_copy(trim(raw_value)) != L"all") {
            return {{{L"Usage: auto clear all", ResultKind::warning}}, false};
        }
        const auto count = automation_engine_.snapshot().size();
        automation_engine_.clear();
        return {{{L"Cleared " + std::to_wstring(count) + L" automations.", ResultKind::success}},
                false};
    }
    if (action == L"in" || action == L"every") {
        const auto [raw_duration, raw_spec] = split_command(raw_value);
        const auto duration = parse_duration_token(raw_duration);
        const auto spec = parse_action_spec(raw_spec);
        if (!duration || !spec) {
            return {{{L"Usage: auto " + action + L" <duration> <action> [payload]",
                      ResultKind::warning}},
                    false};
        }
        const auto &[action_name, payload] = *spec;
        const auto label = label_for(action_name, payload);
        const auto id =
            action == L"every"
                ? automation_engine_.schedule_every(*duration, label, action_name, payload)
                : automation_engine_.schedule_after(*duration, label, action_name, payload);
        const auto item = automation_engine_.find(id);
        return {{{
                    L"Automation #" + std::to_wstring(id) +
                        (action == L"every" ? L" scheduled every " + format_interval(*duration)
                                            : L" scheduled") +
                        (item ? L"; next " + format_due_time(item->due_at) : L"") + L" -> " +
                        action_name,
                    ResultKind::success,
                }},
                false};
    }
    if (action == L"at") {
        const auto [raw_date, remaining] = split_command(raw_value);
        const auto [raw_time, raw_spec] = split_command(remaining);
        const auto due_at = parse_local_datetime(raw_date, raw_time);
        const auto spec = parse_action_spec(raw_spec);
        if (!due_at || !spec) {
            return {{{L"Usage: auto at <YYYY-MM-DD> <HH:MM[:SS]> <action> [payload]",
                      ResultKind::warning}},
                    false};
        }
        const auto &[action_name, payload] = *spec;
        const auto id = automation_engine_.schedule_at(*due_at, label_for(action_name, payload),
                                                       action_name, payload);
        return {{{L"Automation #" + std::to_wstring(id) + L" scheduled for " +
                      format_due_time(*due_at) + L" -> " + action_name,
                  ResultKind::success}},
                false};
    }
    if (action == L"daily" || action == L"weekly") {
        std::uint8_t weekday_mask = 0x7Fu;
        std::wstring raw_time;
        std::wstring raw_spec;
        if (action == L"daily") {
            const auto [time_token, spec_text] = split_command(raw_value);
            raw_time = time_token;
            raw_spec = spec_text;
        } else {
            const auto [raw_days, after_days] = split_command(raw_value);
            const auto parsed_days = parse_weekday_mask(raw_days);
            const auto [time_token, spec_text] = split_command(after_days);
            if (!parsed_days) {
                return {{{L"Usage: auto weekly <mon,tue,...> <HH:MM[:SS]> <action> [payload]",
                          ResultKind::warning}},
                        false};
            }
            weekday_mask = *parsed_days;
            raw_time = time_token;
            raw_spec = spec_text;
        }
        const auto wall = parse_wall_clock(raw_time);
        const auto spec = parse_action_spec(raw_spec);
        if (!wall || !spec) {
            return {{{action == L"daily"
                          ? L"Usage: auto daily <HH:MM[:SS]> <action> [payload]"
                          : L"Usage: auto weekly <mon,tue,...> <HH:MM[:SS]> <action> [payload]",
                      ResultKind::warning}},
                    false};
        }
        AutomationCalendarSchedule schedule;
        schedule.weekday_mask = weekday_mask;
        schedule.hour = wall->hour;
        schedule.minute = wall->minute;
        schedule.second = wall->second;
        schedule.time_zone = L"local";
        const auto &[action_name, payload] = *spec;
        const auto id = automation_engine_.schedule_calendar(
            schedule, label_for(action_name, payload), action_name, payload);
        const auto item = automation_engine_.find(id);
        return {
            {{L"Automation #" + std::to_wstring(id) + L" scheduled " +
                  format_calendar_schedule(schedule) +
                  (item ? L"; next " + format_due_time(item->due_at) : L"") + L" -> " + action_name,
              ResultKind::success}},
            false};
    }
    return {{{
                L"Usage: auto [list|actions|status|show <id>|in <duration> <action> [payload]|at "
                L"<date> <time> <action> [payload]|every <duration> <action> [payload]|daily "
                L"<time> <action> [payload]|weekly <days> <time> <action> [payload]|policy <id> "
                L"...|enable <id>|disable <id>|cancel <id>|clear all]",
                ResultKind::warning,
            }},
            false};
}
CommandResult CommandEngine::plugin_command(std::wstring_view arguments) const {
    if (plugin_handler_) {
        return plugin_handler_(arguments);
    }
    return {{{L"Plugin management handler is unavailable.", ResultKind::error}}, false};
}
CommandResult CommandEngine::system_command(std::wstring_view arguments) const {
    if (!trim(arguments).empty()) {
        return {{{L"Usage: sys", ResultKind::warning}}, false};
    }
    const auto metrics = system_metrics_.snapshot();
    const auto runtime = runtime_scheduler_.stats();
    std::vector<ResultLine> lines;
    lines.push_back({L"Axiom system/runtime health", ResultKind::success});
    if (metrics.ready) {
        std::wostringstream cpu;
        cpu << std::fixed << std::setprecision(1) << metrics.cpu_percent;
        lines.push_back({L"CPU: " + cpu.str() + L"%", ResultKind::info});
    } else {
        lines.push_back({L"CPU: sampler warming up (two samples required)", ResultKind::warning});
    }
    const auto used_memory = metrics.physical_total_bytes >= metrics.physical_available_bytes
                                 ? metrics.physical_total_bytes - metrics.physical_available_bytes
                                 : 0;
    if (metrics.physical_total_bytes > 0) {
        lines.push_back({
            L"RAM: " + format_size(used_memory) + L" / " +
                format_size(metrics.physical_total_bytes) + L" available=" +
                format_size(metrics.physical_available_bytes),
            ResultKind::info,
        });
    }
    if (metrics.process_working_set_bytes > 0) {
        lines.push_back({L"Axiom working set: " + format_size(metrics.process_working_set_bytes),
                         ResultKind::info});
    }
    lines.push_back(
        {L"System uptime: " +
             format_remaining(std::chrono::system_clock::now() +
                              std::chrono::milliseconds{
                                  static_cast<std::chrono::milliseconds::rep>(metrics.uptime_ms)}),
         ResultKind::info});
    lines.push_back({
        L"Runtime tasks: active=" + std::to_wstring(runtime.active_tasks) + L" scheduled=" +
            std::to_wstring(runtime.total_scheduled) + L" executed=" +
            std::to_wstring(runtime.total_executed) + L" cancelled=" +
            std::to_wstring(runtime.total_cancelled) + L" failed=" +
            std::to_wstring(runtime.total_failed),
        ResultKind::info,
    });
    const auto executor = action_executor_.stats();
    lines.push_back({
        L"Action executor: workers=" + std::to_wstring(executor.worker_count) + L" active=" +
            std::to_wstring(executor.active_workers) + L" queued=" +
            std::to_wstring(executor.queued_tasks) + L"/" +
            std::to_wstring(executor.queue_capacity) + L" completed=" +
            std::to_wstring(executor.total_completed) + L" rejected=" +
            std::to_wstring(executor.total_rejected),
        executor.total_rejected == 0 ? ResultKind::info : ResultKind::warning,
    });
    lines.push_back({L"Active reminders: " + std::to_wstring(reminder_center_.active_count()),
                     ResultKind::info});
    const auto automations = automation_engine_.stats();
    lines.push_back({
        L"Automations: total=" + std::to_wstring(automations.total) + L" enabled=" +
            std::to_wstring(automations.enabled) + L" recurring=" +
            std::to_wstring(automations.recurring) + L" calendar=" +
            std::to_wstring(automations.calendar) + L" retry-policy=" +
            std::to_wstring(automations.retry_enabled) + L" retrying=" +
            std::to_wstring(automations.retrying) + L" unresolved=" +
            std::to_wstring(automations.unresolved_actions) + L" runs=" +
            std::to_wstring(automations.total_runs) + L" failures=" +
            std::to_wstring(automations.total_failures),
        ResultKind::info,
    });
    const auto notifications = notification_center_.stats();
    lines.push_back({
        L"Notifications: pending=" + std::to_wstring(notifications.pending) + L" retained=" +
            std::to_wstring(notifications.retained) + L" published=" +
            std::to_wstring(notifications.total_published) + L" dropped=" +
            std::to_wstring(notifications.total_dropped),
        ResultKind::info,
    });
    return {std::move(lines), false};
}
CommandResult CommandEngine::diagnostics_command(std::wstring_view arguments) const {
    const auto [raw_action, raw_value] = split_command(trim(arguments));
    const auto action = lower_copy(raw_action);
    const auto render_events = [](std::wstring title, const std::vector<DiagnosticEvent> &events) {
        std::vector<ResultLine> lines;
        lines.push_back({std::move(title), ResultKind::success});
        for (const auto &event : events) {
            ResultKind kind = ResultKind::info;
            if (event.severity == DiagnosticSeverity::warning) {
                kind = ResultKind::warning;
            } else if (event.severity == DiagnosticSeverity::error) {
                kind = ResultKind::error;
            }
            std::wstring duration;
            if (event.duration > std::chrono::milliseconds::zero()) {
                duration = L" " + std::to_wstring(event.duration.count()) + L"ms";
            }
            lines.push_back({
                L"#" + std::to_wstring(event.id) + L" " + format_due_time(event.occurred_at) +
                    L" [" + std::wstring{diagnostic_domain_name(event.domain)} + L"/" +
                    std::wstring{diagnostic_severity_name(event.severity)} + L"] " + event.subject +
                    duration + L" — " + event.message,
                kind,
            });
        }
        if (events.empty()) {
            lines.push_back({L"No matching diagnostics in this session.", ResultKind::info});
        }
        return CommandResult{std::move(lines), false};
    };
    if (action.empty() || action == L"status") {
        const auto diagnostics = execution_diagnostics_.stats();
        const auto executor = action_executor_.stats();
        return {{
                    {L"Execution diagnostics (session-scoped)", ResultKind::success},
                    {L"retained=" + std::to_wstring(diagnostics.retained) + L"/" +
                         std::to_wstring(diagnostics.capacity) + L" recorded=" +
                         std::to_wstring(diagnostics.total_recorded) + L" errors=" +
                         std::to_wstring(diagnostics.total_errors) + L" dropped-oldest=" +
                         std::to_wstring(diagnostics.dropped_oldest),
                     ResultKind::info},
                    {L"Action executor: workers=" + std::to_wstring(executor.worker_count) +
                         L" active=" + std::to_wstring(executor.active_workers) + L" queued=" +
                         std::to_wstring(executor.queued_tasks) + L"/" +
                         std::to_wstring(executor.queue_capacity) + L" submitted=" +
                         std::to_wstring(executor.total_submitted) + L" completed=" +
                         std::to_wstring(executor.total_completed) + L" failed=" +
                         std::to_wstring(executor.total_failed) + L" rejected=" +
                         std::to_wstring(executor.total_rejected),
                     ResultKind::info},
                    {L"Diagnostics intentionally omit action payloads and are not persisted across "
                     L"restarts.",
                     ResultKind::info},
                },
                false};
    }
    if (action == L"clear") {
        execution_diagnostics_.clear();
        return {{{L"Session execution diagnostics cleared.", ResultKind::success}}, false};
    }
    if (action == L"recent") {
        std::size_t limit = 30;
        if (!trim(raw_value).empty()) {
            const auto parsed = parse_u64(raw_value);
            if (!parsed || *parsed == 0 || *parsed > 200) {
                return {{{L"Usage: diag recent [1-200]", ResultKind::warning}}, false};
            }
            limit = static_cast<std::size_t>(*parsed);
        }
        return render_events(L"Recent execution diagnostics", execution_diagnostics_.recent(limit));
    }
    if (action == L"runtime") {
        return render_events(
            L"Runtime diagnostics",
            execution_diagnostics_.for_subject(DiagnosticDomain::runtime, L"executor", 50));
    }
    if (action == L"plugin" || action == L"action" || action == L"automation") {
        const auto subject = trim(raw_value);
        if (subject.empty()) {
            return {{{L"Usage: diag " + action + L" <subject>", ResultKind::warning}}, false};
        }
        DiagnosticDomain domain = DiagnosticDomain::plugin;
        if (action == L"action")
            domain = DiagnosticDomain::action;
        if (action == L"automation") {
            const auto id = parse_u64(subject);
            if (!id || *id == 0) {
                return {{{L"Usage: diag automation <id>", ResultKind::warning}}, false};
            }
            domain = DiagnosticDomain::automation;
        }
        return render_events(L"Diagnostics for " + action + L" " + subject,
                             execution_diagnostics_.for_subject(domain, subject, 100));
    }
    return {{{L"Usage: diag [status|recent [count]|runtime|plugin <id>|action <name>|automation "
              L"<id>|clear]",
              ResultKind::warning}},
            false};
}
CommandResult CommandEngine::journal_command(std::wstring_view arguments) const {
    const auto [raw_action, raw_value] = split_command(trim(arguments));
    const auto action = lower_copy(raw_action);
    const auto render_entries = [](std::wstring title, const std::vector<JournalEntry> &entries) {
        std::vector<ResultLine> lines;
        lines.push_back({std::move(title), ResultKind::success});
        for (const auto &entry : entries) {
            std::wstring tags;
            for (const auto &tag : entry.tags)
                tags += L" #" + tag;
            const auto kind = entry.kind == JournalEntryKind::note ? L"NOTE" : L"ACT";
            lines.push_back({
                L"#" + std::to_wstring(entry.id) + L" [" + kind + L"] " +
                    format_due_time(entry.created_at) + L" " + entry.text + tags,
                ResultKind::info,
            });
        }
        if (entries.empty())
            lines.push_back({L"No journal entries matched.", ResultKind::info});
        return CommandResult{std::move(lines), false};
    };
    if (action.empty() || action == L"today") {
        const auto [begin, end] = local_today_bounds();
        return render_entries(L"Today's journal", journal_store_.between(begin, end, 200));
    }
    if (action == L"recent")
        return render_entries(L"Recent journal", journal_store_.recent(50));
    if (action == L"add") {
        auto [text, tags] = journal_text_and_tags(raw_value);
        if (text.empty())
            return {{{L"Usage: note <text> [#tags]", ResultKind::warning}}, false};
        const auto id = journal_store_.add_note(std::move(text), std::move(tags));
        return {{{L"Journal note #" + std::to_wstring(id) + L" saved.", ResultKind::success}},
                false};
    }
    if (action == L"search") {
        if (trim(raw_value).empty())
            return {{{L"Usage: journal search <query>", ResultKind::warning}}, false};
        return render_entries(L"Journal search", journal_store_.search(raw_value, 50));
    }
    if (action == L"tag") {
        if (trim(raw_value).empty())
            return {{{L"Usage: journal tag <tag>", ResultKind::warning}}, false};
        return render_entries(L"Journal tag", journal_store_.tagged(raw_value, 50));
    }
    if (action == L"show") {
        const auto id = parse_u64(raw_value);
        if (!id || *id == 0)
            return {{{L"Usage: journal show <id>", ResultKind::warning}}, false};
        const auto item = journal_store_.find(*id);
        if (!item)
            return {{{L"Journal entry not found.", ResultKind::warning}}, false};
        std::vector<ResultLine> lines{
            {L"Journal #" + std::to_wstring(item->id), ResultKind::success},
            {L"time=" + format_due_time(item->created_at), ResultKind::info},
            {L"text=" + item->text, ResultKind::info}};
        if (!item->source.empty())
            lines.push_back({L"source=" + item->source, ResultKind::info});
        for (const auto &tag : item->tags)
            lines.push_back({L"tag=#" + tag, ResultKind::info});
        return {std::move(lines), false};
    }
    if (action == L"delete") {
        const auto id = parse_u64(raw_value);
        if (!id || *id == 0)
            return {{{L"Usage: journal delete <id>", ResultKind::warning}}, false};
        const bool removed = journal_store_.erase(*id);
        return {{{removed ? L"Journal entry deleted." : L"Journal entry not found.",
                  removed ? ResultKind::success : ResultKind::warning}},
                false};
    }
    if (action == L"clear-notes") {
        const auto count = journal_store_.clear_notes();
        return {{{L"Removed " + std::to_wstring(count) +
                      L" manual journal note(s); activity history was preserved.",
                  ResultKind::success}},
                false};
    }
    if (action == L"context") {
        if (journal_context_policy_ && !journal_context_policy_())
            return {
                {{L"Journal context access is disabled by privacy settings.", ResultKind::warning}},
                false};
        const auto items = journal_store_.context(raw_value, 20, 12000);
        std::vector<ResultLine> lines{{L"Journal context boundary", ResultKind::success}};
        for (const auto &item : items)
            lines.push_back({L"#" + std::to_wstring(item.id) + L" " +
                                 format_due_time(item.created_at) + L" " + item.text,
                             ResultKind::info});
        if (items.empty())
            lines.push_back({L"No context matched.", ResultKind::info});
        return {std::move(lines), false};
    }
    if (action == L"status") {
        const auto st = journal_store_.status();
        return {{{L"Journal: entries=" + std::to_wstring(st.entry_count) + L" notes=" +
                      std::to_wstring(st.note_count) + L" activities=" +
                      std::to_wstring(st.activity_count) + L" generation=" +
                      std::to_wstring(st.generation),
                  ResultKind::success},
                 {L"store=" + st.path.wstring(), ResultKind::info},
                 {L"recovered=" + std::wstring{st.recovered_from_alternate ? L"yes" : L"no"},
                  ResultKind::info}},
                false};
    }
    return {
        {{L"Usage: journal [today|recent|add|search|tag|show|delete|clear-notes|context|status]",
          ResultKind::warning}},
        false};
}
CommandResult CommandEngine::notification_command(std::wstring_view arguments) const {
    const auto [raw_action, raw_value] = split_command(trim(arguments));
    const auto action = lower_copy(raw_action);
    if (action.empty() || action == L"list") {
        const auto recent = notification_center_.recent(24);
        std::vector<ResultLine> lines;
        lines.push_back({L"Recent notifications", ResultKind::success});
        for (const auto &item : recent) {
            std::wstring level;
            switch (item.level) {
            case NotificationLevel::success:
                level = L"OK";
                break;
            case NotificationLevel::warning:
                level = L"WARN";
                break;
            case NotificationLevel::error:
                level = L"ERR";
                break;
            case NotificationLevel::info:
            default:
                level = L"INFO";
                break;
            }
            lines.push_back({
                L"#" + std::to_wstring(item.id) + L" [" + level + L"] " + item.title + L" — " +
                    item.body,
                ResultKind::info,
            });
        }
        if (recent.empty()) {
            lines.push_back({L"Notification history is empty.", ResultKind::info});
        }
        return {std::move(lines), false};
    }
    if (action == L"status") {
        const auto stats = notification_center_.stats();
        return {{{
                    L"Notifications: pending=" + std::to_wstring(stats.pending) + L" retained=" +
                        std::to_wstring(stats.retained) + L" published=" +
                        std::to_wstring(stats.total_published) + L" dropped=" +
                        std::to_wstring(stats.total_dropped),
                    ResultKind::success,
                }},
                false};
    }
    if (action == L"show") {
        const auto id = parse_u64(raw_value);
        if (!id || *id == 0) {
            return {{{L"Usage: notify show <id>", ResultKind::warning}}, false};
        }
        const auto item = notification_center_.find(*id);
        if (!item) {
            return {{{L"Notification not found: #" + std::to_wstring(*id), ResultKind::warning}},
                    false};
        }
        return {{{L"#" + std::to_wstring(item->id) + L" " + item->title, ResultKind::success},
                 {item->body, ResultKind::info}},
                false};
    }
    if (action == L"clear") {
        notification_center_.clear_history();
        return {{{L"Notification history cleared.", ResultKind::success}}, false};
    }
    return {{{L"Usage: notify [list|show <id>|clear|status]", ResultKind::warning}}, false};
}
CommandResult CommandEngine::execute(std::wstring_view input) const {
    const auto normalized = trim(input);
    if (normalized == L"?") {
        return help({});
    }
    if (normalized.starts_with(L"?")) {
        return help(trim(std::wstring_view{normalized}.substr(1)));
    }
    const auto [name, arguments] = split_command(normalized);
    if (name.empty()) {
        return {{{L"Type 'help' or '?' to browse commands.", ResultKind::info}}, false};
    }
    const auto *command = find(name);
    if (command == nullptr && plugin_host_.contains_command(name)) {
        const auto result = plugin_host_.invoke_command(name, arguments);
        return {{{result.message.empty()
                      ? (result.success ? L"Plugin command completed." : L"Plugin command failed.")
                      : result.message,
                  result.success ? ResultKind::success : ResultKind::error}},
                false};
    }
    if (command == nullptr) {
        return search_files(input);
    }
    try {
        return command->handler(arguments);
    } catch (const std::exception &exception) {
        std::wstring message{L"Command failed: "};
        const std::string narrow = exception.what();
        message.append(narrow.begin(), narrow.end());
        return {{{std::move(message), ResultKind::error}}, false};
    } catch (...) {
        return {{{L"Command failed with an unknown error.", ResultKind::error}}, false};
    }
}
App::App(HINSTANCE instance)
    : instance_{instance},
      watcher_resync_{watcher_health_,
                      [this](std::stop_token stop_token, std::uint64_t generation) {
                          return perform_watcher_resync(stop_token, generation);
                      }},
      startup_restore_recovery_{recover_default_local_data_before_store_load()},
      app_settings_store_{default_app_settings_path()},
      settings_{app_settings_store_.load(default_app_settings(), legacy_index_settings_path())},
      reminder_store_{default_reminder_store_path()},
      reminder_center_{runtime_scheduler_, &reminder_store_,
                       [this](const Reminder &reminder) { on_reminder_fired(reminder); }},
      system_metrics_{runtime_scheduler_}, notification_center_{128},
      journal_protector_{L"JournalLive"},
      journal_store_{default_journal_store_path(), settings_.journal_max_entries,
                     &journal_protector_, settings_.journal_live_protection_enabled},
      action_registry_{&execution_diagnostics_}, plugin_trust_store_{default_plugin_trust_path()},
      plugin_packages_{default_plugin_directory(), plugin_trust_store_},
      plugin_host_{action_registry_, &execution_diagnostics_},
      plugin_loader_{plugin_host_, &execution_diagnostics_},
      automation_store_{default_automation_store_path()},
      automation_engine_{runtime_scheduler_,
                         action_executor_,
                         automation_store_,
                         action_registry_,
                         &execution_diagnostics_,
                         [this](const Automation &automation, const ActionResult &result) {
                             on_automation_executed(automation, result);
                         }},
      command_engine_{
          file_index_,
          clipboard_store_,
          reminder_center_,
          runtime_scheduler_,
          action_executor_,
          execution_diagnostics_,
          system_metrics_,
          notification_center_,
          journal_store_,
          action_registry_,
          plugin_host_,
          automation_engine_,
          [this] { on_index_configuration_changed(); },
          [this](std::wstring_view arguments) { return index_recovery_command(arguments); },
          [this](std::wstring_view arguments) { return settings_command(arguments); },
          [this](std::wstring_view arguments) { return data_command(arguments); },
          [this](std::wstring_view arguments) { return plugin_management_command(arguments); },
          [this] { return settings_.journal_context_enabled; }} {
    refresh_theme_resources();
    register_window_class();
    create_window();
    add_tray_icon();
    file_index_.start(settings_.index_roots, settings_.excluded_directory_names);
    restart_file_watcher();
    register_builtin_actions();
    try {
        (void)plugin_trust_store_.load();
    } catch (const std::exception &exception) {
        [[maybe_unused]] const auto id = notification_center_.publish(
            NotificationLevel::error, L"Plugin trust",
            L"Plugin trust metadata is unreadable; no managed plugin will be loaded: " +
                widen_ascii(exception.what()));
    }
    if (settings_.plugins_enabled) {
        load_enabled_plugins();
    }
    (void)journal_store_.load();
    apply_journal_retention();
    (void)reminder_center_.restore();
    const auto automation_restore = automation_engine_.restore();
    if (automation_restore.unresolved_actions > 0) {
        [[maybe_unused]] const auto id = notification_center_.publish(
            NotificationLevel::warning, L"Automation restore",
            std::to_wstring(automation_restore.unresolved_actions) +
                L" automation(s) reference unavailable actions and were not armed.");
        PostMessageW(window_, notification_ready_message, 0, 0);
    }
    system_metrics_.start();
    create_controls();
    register_hotkey();
    std::wstring startup_error;
    if (!set_launch_at_startup(settings_.launch_at_startup, startup_error)) {
        [[maybe_unused]] const auto id = notification_center_.publish(
            NotificationLevel::warning, L"Startup setting",
            L"Could not synchronize Windows startup registration: " + startup_error);
        PostMessageW(window_, notification_ready_message, 0, 0);
    }
    if (!AddClipboardFormatListener(window_)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "AddClipboardFormatListener failed");
    }
    capture_clipboard_text();
}
App::~App() {
    watcher_resync_.stop();
    file_watcher_.stop();
    watcher_health_.stopped();
    file_index_.stop();
    automation_engine_.stop();
    action_executor_.stop(true);
    plugin_loader_.unload_all();
    reminder_center_.stop();
    system_metrics_.stop();
    runtime_scheduler_.stop();
    remove_tray_icon();
    if (window_ != nullptr) {
        RemoveClipboardFormatListener(window_);
        UnregisterHotKey(window_, hotkey_id);
    }
    if (font_ != nullptr) {
        DeleteObject(font_);
    }
    if (heading_font_ != nullptr) {
        DeleteObject(heading_font_);
    }
    if (background_brush_ != nullptr) {
        DeleteObject(background_brush_);
    }
    if (surface_brush_ != nullptr) {
        DeleteObject(surface_brush_);
    }
}
AppTheme App::effective_theme() const {
    if (settings_.theme == AppTheme::high_contrast || high_contrast_enabled()) {
        return AppTheme::high_contrast;
    }
    if (settings_.theme == AppTheme::system) {
        return windows_prefers_dark_apps() ? AppTheme::dark : AppTheme::light;
    }
    return settings_.theme;
}
void App::refresh_theme_resources() {
    const auto colors = palette_colors(effective_theme());
    palette_background_ = colors.background;
    palette_surface_ = colors.surface;
    palette_text_ = colors.text;
    palette_muted_ = colors.muted;
    palette_accent_ = colors.accent;
    palette_selection_ = colors.selection;
    palette_selection_text_ = colors.selection_text;
    palette_success_ = colors.success;
    palette_warning_ = colors.warning;
    palette_error_ = colors.error;
    if (background_brush_ != nullptr) {
        DeleteObject(background_brush_);
        background_brush_ = nullptr;
    }
    if (surface_brush_ != nullptr) {
        DeleteObject(surface_brush_);
        surface_brush_ = nullptr;
    }
    background_brush_ = CreateSolidBrush(palette_background_);
    surface_brush_ = CreateSolidBrush(palette_surface_);
    if (background_brush_ == nullptr || surface_brush_ == nullptr) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "CreateSolidBrush failed");
    }
}
void App::apply_theme() {
    refresh_theme_resources();
    if (edit_ != nullptr) {
        InvalidateRect(edit_, nullptr, TRUE);
    }
    if (results_ != nullptr) {
        InvalidateRect(results_, nullptr, TRUE);
    }
    if (window_ != nullptr) {
        RedrawWindow(window_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}
void App::register_window_class() {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &App::window_proc_thunk;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = axiom_window_class_name;
    if (RegisterClassExW(&window_class) == 0) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "RegisterClassExW failed");
    }
}
void App::create_window() {
    window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, axiom_window_class_name,
                              window_title, WS_POPUP | WS_BORDER, CW_USEDEFAULT, CW_USEDEFAULT,
                              palette_width, palette_height, nullptr, nullptr, instance_, this);
    if (window_ == nullptr) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "CreateWindowExW failed");
    }
    center_on_active_monitor();
}
void App::create_controls() {
    font_ = CreateFontW(-19, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    heading_font_ = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    edit_ = CreateWindowExW(0, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, margin,
                            margin, palette_width - margin * 2, edit_height, window_,
                            reinterpret_cast<HMENU>(edit_id), instance_, nullptr);
    results_ =
        CreateWindowExW(0, L"LISTBOX", L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL |
                            LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                        margin, margin + edit_height + 12, palette_width - margin * 2,
                        palette_height - (margin * 2 + edit_height + 12), window_,
                        reinterpret_cast<HMENU>(result_list_id), instance_, nullptr);
    if (edit_ == nullptr || results_ == nullptr) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "CreateWindowExW control creation failed");
    }
    if (font_ != nullptr) {
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(results_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    SendMessageW(results_, LB_SETITEMHEIGHT, 0, result_item_height);
    SendMessageW(edit_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Search files or type a command. Try '?' or Tab."));
    apply_theme();
    render_result(command_engine_.execute(L"help"));
}
void App::register_hotkey() {
    if (!RegisterHotKey(window_, hotkey_id, MOD_ALT | MOD_NOREPEAT, VK_SPACE)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "RegisterHotKey(Alt+Space) failed");
    }
}
void App::add_tray_icon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = tray_icon_id;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = tray_callback_message;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(data.szTip, L"Axiom Desktop", _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        throw std::runtime_error{"Failed to create Axiom tray icon."};
    }
    tray_icon_added_ = true;
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
}
void App::remove_tray_icon() noexcept {
    if (!tray_icon_added_ || window_ == nullptr) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = tray_icon_id;
    Shell_NotifyIconW(NIM_DELETE, &data);
    tray_icon_added_ = false;
}
void App::show_tray_menu() {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, tray_open_command_id, L"Open Axiom");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (settings_.launch_at_startup ? MF_CHECKED : MF_UNCHECKED),
                tray_startup_command_id, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, tray_exit_command_id, L"Exit");
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                        cursor.x, cursor.y, 0, window_, nullptr);
    DestroyMenu(menu);
    if (command == tray_open_command_id) {
        show_palette();
    } else if (command == tray_startup_command_id) {
        const auto result =
            settings_command(settings_.launch_at_startup ? L"startup off" : L"startup on");
        if (!result.lines.empty()) {
            [[maybe_unused]] const auto id = notification_center_.publish(
                result.lines.front().kind == ResultKind::error ? NotificationLevel::error
                                                               : NotificationLevel::success,
                L"Axiom Settings", result.lines.front().text);
            PostMessageW(window_, notification_ready_message, 0, 0);
        }
    } else if (command == tray_exit_command_id) {
        exit_requested_ = true;
        PostMessageW(window_, WM_CLOSE, 0, 0);
    }
}
void App::show_palette() {
    center_on_active_monitor();
    ShowWindow(window_, SW_SHOWNORMAL);
    SetForegroundWindow(window_);
    SetFocus(edit_);
    SendMessageW(edit_, EM_SETSEL, 0, -1);
    update_live_results();
}
void App::hide_palette() {
    KillTimer(window_, live_search_timer_id);
    ShowWindow(window_, SW_HIDE);
}
void App::execute_input() {
    KillTimer(window_, live_search_timer_id);
    const auto input = current_input();
    const auto name = split_command(input).first;
    if (!input.empty() && !command_engine_.is_command_name(name) && !input.starts_with(L"?")) {
        const auto suggestions = command_engine_.suggestions(input, 8);
        if (!suggestions.empty()) {
            set_input_text(suggestions.front().completion);
            update_live_results();
            return;
        }
        render_live_search(input);
        if (!live_result_paths_.empty() && activate_selected_result()) {
            return;
        }
    }
    const auto result = command_engine_.execute(input);
    render_result(result);
    if (result.request_exit) {
        exit_requested_ = true;
        PostMessageW(window_, WM_CLOSE, 0, 0);
        return;
    }
    SetFocus(edit_);
    SendMessageW(edit_, EM_SETSEL, 0, -1);
}
void App::set_input_text(std::wstring_view text) {
    SetWindowTextW(edit_, std::wstring{text}.c_str());
    SendMessageW(edit_, EM_SETSEL, static_cast<WPARAM>(text.size()),
                 static_cast<LPARAM>(text.size()));
}
std::wstring App::current_input() const {
    const int length = GetWindowTextLengthW(edit_);
    std::wstring input(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(edit_, input.data(), length + 1);
    }
    input.resize(static_cast<std::size_t>(length));
    return trim(input);
}
std::optional<std::wstring> App::current_live_file_query() const {
    const auto input = current_input();
    if (input.empty()) {
        return std::nullopt;
    }
    const auto [name, arguments] = split_command(input);
    if (equals_ignore_case(name, L"find")) {
        const auto query = trim(arguments);
        if (query.empty()) {
            return std::nullopt;
        }
        return query;
    }
    if (command_engine_.is_command_name(name)) {
        return std::nullopt;
    }
    return input;
}
void App::update_live_results() {
    live_result_paths_.clear();
    live_result_completions_.clear();
    const auto input = current_input();
    if (input.empty()) {
        render_result(command_engine_.execute(L"help"));
        return;
    }
    const auto suggestions = command_engine_.suggestions(input, 16);
    if (!suggestions.empty()) {
        SendMessageW(results_, LB_RESETCONTENT, 0, 0);
        live_result_completions_.reserve(suggestions.size());
        for (const auto &suggestion : suggestions) {
            const auto index = SendMessageW(results_, LB_ADDSTRING, 0,
                                            reinterpret_cast<LPARAM>(suggestion.display.c_str()));
            if (index != LB_ERR && index != LB_ERRSPACE) {
                SendMessageW(results_, LB_SETITEMDATA, static_cast<WPARAM>(index),
                             static_cast<LPARAM>(suggestion.kind));
                live_result_completions_.push_back(suggestion.completion);
            }
        }
        if (!live_result_completions_.empty()) {
            SendMessageW(results_, LB_SETCURSEL, 0, 0);
        }
        return;
    }
    if (const auto query = current_live_file_query()) {
        render_live_search(*query);
        return;
    }
    const auto name = split_command(input).first;
    if (command_engine_.is_command_name(name) || input.starts_with(L"?")) {
        render_result(command_engine_.execute(
            L"help " + trim(input.starts_with(L"?") ? std::wstring_view{input}.substr(1)
                                                    : std::wstring_view{name})));
    }
}
void App::render_live_search(std::wstring_view query) {
    const auto hits = file_index_.search(query, 32);
    const auto snapshot = file_index_.snapshot();
    SendMessageW(results_, LB_RESETCONTENT, 0, 0);
    live_result_paths_.clear();
    live_result_completions_.clear();
    live_result_paths_.reserve(hits.size());
    for (const auto &hit : hits) {
        std::wstring display = hit.is_directory ? L"[DIR] " : L"[FILE] ";
        display += hit.path.wstring();
        if (!hit.is_directory && hit.size > 0) {
            display += L" (" + format_size(hit.size) + L")";
        }
        const auto index =
            SendMessageW(results_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
        if (index != LB_ERR && index != LB_ERRSPACE) {
            SendMessageW(results_, LB_SETITEMDATA, static_cast<WPARAM>(index),
                         static_cast<LPARAM>(ResultKind::info));
            live_result_paths_.push_back(hit.path);
        }
    }
    if (!live_result_paths_.empty()) {
        SendMessageW(results_, LB_SETCURSEL, 0, 0);
        return;
    }
    std::wstring message;
    if (snapshot.state == IndexState::indexing) {
        message = L"[!] Indexing... no match yet for: " + std::wstring{query};
    } else if (snapshot.state == IndexState::failed) {
        message = L"[ERR] File index failed: " + snapshot.last_error;
    } else {
        message = L"No indexed file or folder matched: " + std::wstring{query};
    }
    const auto index =
        SendMessageW(results_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(message.c_str()));
    if (index != LB_ERR && index != LB_ERRSPACE) {
        const auto kind =
            snapshot.state == IndexState::failed
                ? ResultKind::error
                : (snapshot.state == IndexState::indexing ? ResultKind::warning : ResultKind::info);
        SendMessageW(results_, LB_SETITEMDATA, static_cast<WPARAM>(index),
                     static_cast<LPARAM>(kind));
    }
}
bool App::activate_selected_result() {
    LRESULT selected = SendMessageW(results_, LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR || selected < 0) {
        selected = 0;
    }
    if (!live_result_completions_.empty()) {
        if (static_cast<std::size_t>(selected) >= live_result_completions_.size()) {
            selected = 0;
        }
        set_input_text(live_result_completions_[static_cast<std::size_t>(selected)]);
        SetFocus(edit_);
        update_live_results();
        return true;
    }
    if (live_result_paths_.empty()) {
        return false;
    }
    if (static_cast<std::size_t>(selected) >= live_result_paths_.size()) {
        selected = 0;
    }
    const auto &path = live_result_paths_[static_cast<std::size_t>(selected)];
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(window_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        render_result(
            {{{L"ShellExecute failed for: " + path.wstring(), ResultKind::error}}, false});
        return false;
    }
    hide_palette();
    return true;
}
void App::render_result(const CommandResult &result) {
    live_result_paths_.clear();
    live_result_completions_.clear();
    SendMessageW(results_, LB_RESETCONTENT, 0, 0);
    for (const auto &line : result.lines) {
        std::wstring prefix;
        switch (line.kind) {
        case ResultKind::success:
            prefix = L"[OK] ";
            break;
        case ResultKind::warning:
            prefix = L"[!] ";
            break;
        case ResultKind::error:
            prefix = L"[ERR] ";
            break;
        case ResultKind::heading:
        case ResultKind::info:
        default:
            break;
        }
        const std::wstring display = prefix + line.text;
        const auto index =
            SendMessageW(results_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
        if (index != LB_ERR && index != LB_ERRSPACE) {
            SendMessageW(results_, LB_SETITEMDATA, static_cast<WPARAM>(index),
                         static_cast<LPARAM>(line.kind));
        }
    }
}
void App::capture_clipboard_text() {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        return;
    }
    if (!OpenClipboard(window_)) {
        return;
    }
    struct ClipboardCloser {
        ~ClipboardCloser() {
            CloseClipboard();
        }
    } closer;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data == nullptr) {
        return;
    }
    const auto *text = static_cast<const wchar_t *>(GlobalLock(data));
    if (text == nullptr) {
        return;
    }
    const std::wstring value{text};
    GlobalUnlock(data);
    [[maybe_unused]] const auto recorded = clipboard_store_.record_text(value);
}
void App::on_index_configuration_changed() {
    const auto snapshot = file_index_.snapshot();
    settings_.index_roots = snapshot.roots;
    settings_.excluded_directory_names = snapshot.excluded_directory_names;
    save_settings();
    restart_file_watcher();
}
void App::save_settings() {
    app_settings_store_.save(settings_);
}
CommandResult App::settings_command(std::wstring_view arguments) {
    const auto [raw_action, raw_value] = split_command(trim(arguments));
    const auto action = lower_copy(raw_action);
    const auto value = lower_copy(trim(raw_value));
    if (action.empty() || action == L"status") {
        std::vector<ResultLine> lines;
        lines.push_back({L"Axiom unified settings", ResultKind::success});
        lines.push_back({
            L"startup=" + std::wstring{settings_.launch_at_startup ? L"on" : L"off"} +
                L" registry=" + (startup_registration_matches() ? L"in-sync" : L"out-of-sync"),
            ResultKind::info,
        });
        lines.push_back({
            L"close-to-tray=" + std::wstring{settings_.close_to_tray ? L"on" : L"off"},
            ResultKind::info,
        });
        lines.push_back({
            L"notifications=" + std::wstring{settings_.notifications_enabled ? L"on" : L"off"},
            ResultKind::info,
        });
        lines.push_back({
            L"theme=" + std::wstring{app_theme_name(settings_.theme)} + L" effective=" +
                std::wstring{app_theme_name(effective_theme())},
            ResultKind::info,
        });
        lines.push_back({
            L"plugins=" + std::wstring{settings_.plugins_enabled ? L"on" : L"off"} +
                L" (disabled by default; loads only from the Axiom plugin directory)",
            ResultKind::info,
        });
        const auto journal_status = journal_store_.status();
        lines.push_back({
            L"journal-protection=" +
                std::wstring{settings_.journal_live_protection_enabled ? L"on" : L"off"} +
                L" effective=" +
                (journal_status.protected_at_rest ? L"AXJP-v1" : L"AXJR-v1 plaintext") +
                L" protector=" + journal_status.protector_name,
            journal_status.protected_at_rest ? ResultKind::info : ResultKind::warning,
        });
        lines.push_back({L"journal-max-entries=" + std::to_wstring(settings_.journal_max_entries) +
                             L" activity-retention-days=" +
                             std::to_wstring(settings_.journal_activity_retention_days) +
                             L" note-retention-days=" +
                             std::to_wstring(settings_.journal_note_retention_days),
                         ResultKind::info});
        lines.push_back(
            {L"settings-file=" + app_settings_store_.file_path().wstring(), ResultKind::info});
        return {std::move(lines), false};
    }
    if (action == L"theme") {
        const auto requested = parse_app_theme(value);
        if (!requested) {
            return {
                {
                    {L"Usage: settings theme "
                     L"<system|light|dark|oled|graphite|midnight|nord|high-contrast>",
                     ResultKind::warning},
                    {L"Themes: System, Light, Dark, OLED, Graphite, Midnight, Nord, High Contrast",
                     ResultKind::info},
                },
                false};
        }
        settings_.theme = *requested;
        save_settings();
        apply_theme();
        return {{
                    {L"Theme is now " + std::wstring{app_theme_name(settings_.theme)} +
                         L" (effective " + std::wstring{app_theme_name(effective_theme())} + L").",
                     ResultKind::success},
                },
                false};
    }
    const auto parse_toggle = [&]() -> std::optional<bool> {
        if (value == L"on" || value == L"true" || value == L"1") {
            return true;
        }
        if (value == L"off" || value == L"false" || value == L"0") {
            return false;
        }
        return std::nullopt;
    };
    const auto enabled = parse_toggle();
    if (!enabled) {
        return {{{
                    L"Usage: settings [status|startup <on|off>|tray <on|off>|notifications "
                    L"<on|off>|plugins <on|off>|journal-protection <on|off>|theme <name>]",
                    ResultKind::warning,
                }},
                false};
    }
    if (action == L"startup") {
        std::wstring error;
        if (!set_launch_at_startup(*enabled, error)) {
            return {{{L"Windows startup update failed: " + error, ResultKind::error}}, false};
        }
        settings_.launch_at_startup = *enabled;
        save_settings();
        return {{{
                    L"Start with Windows is now " + std::wstring{*enabled ? L"on" : L"off"} + L".",
                    ResultKind::success,
                }},
                false};
    }
    if (action == L"tray") {
        settings_.close_to_tray = *enabled;
        save_settings();
        return {{{
                    L"Close-to-tray is now " + std::wstring{*enabled ? L"on" : L"off"} + L".",
                    ResultKind::success,
                }},
                false};
    }
    if (action == L"notifications") {
        settings_.notifications_enabled = *enabled;
        save_settings();
        if (*enabled && notification_center_.stats().pending > 0) {
            PostMessageW(window_, notification_ready_message, 0, 0);
        }
        return {
            {{
                L"Desktop notifications are now " + std::wstring{*enabled ? L"on" : L"off"} + L".",
                ResultKind::success,
            }},
            false};
    }
    if (action == L"journal-protection") {
        if (*enabled == settings_.journal_live_protection_enabled &&
            journal_store_.status().protected_at_rest == *enabled) {
            return {{{L"Journal live-store protection is already " +
                          std::wstring{*enabled ? L"on" : L"off"} + L".",
                      ResultKind::info}},
                    false};
        }
        journal_store_.set_protection_enabled(*enabled);
        settings_.journal_live_protection_enabled = *enabled;
        save_settings();
        return {{{
                    *enabled ? L"Journal live store is now protected with Windows "
                               L"DPAPI/current-user AXJP-v1."
                             : L"Journal live-store protection is now off; journal.bin is "
                               L"plaintext AXJR-v1.",
                    *enabled ? ResultKind::success : ResultKind::warning,
                }},
                false};
    }
    if (action == L"plugins") {
        if (*enabled == settings_.plugins_enabled) {
            return {
                {{L"Plugin loading is already " + std::wstring{*enabled ? L"on" : L"off"} + L".",
                  ResultKind::info}},
                false};
        }
        settings_.plugins_enabled = *enabled;
        save_settings();
        if (!*enabled) {
            plugin_loader_.unload_all();
            reconcile_plugin_automations();
            return {{{L"Plugin loading is now off. Plugin automations remain enabled but are "
                      L"parked until their actions return.",
                      ResultKind::success}},
                    false};
        }
        load_enabled_plugins();
        reconcile_plugin_automations();
        return {{{L"Plugin loading is now on. Only individually enabled, verified managed plugins "
                  L"were considered.",
                  ResultKind::success}},
                false};
    }
    return {{{
                L"Usage: settings [status|startup <on|off>|tray <on|off>|notifications "
                L"<on|off>|plugins <on|off>|journal-protection <on|off>|theme <name>]",
                ResultKind::warning,
            }},
            false};
}
void App::reconcile_plugin_automations() {
    const auto report = automation_engine_.reconcile_action_availability();
    if (report.parked == 0 && report.armed == 0)
        return;
    [[maybe_unused]] const auto notification_id = notification_center_.publish(
        NotificationLevel::info, L"Plugin automations",
        std::to_wstring(report.parked) + L" parked, " + std::to_wstring(report.armed) +
            L" re-armed; unresolved=" + std::to_wstring(report.unresolved) + L".");
    PostMessageW(window_, notification_ready_message, 0, 0);
}
void App::load_enabled_plugins() {
    for (const auto &record : plugin_trust_store_.list()) {
        if (!record.enabled || record.quarantined)
            continue;
        const auto verification = plugin_packages_.verify(record);
        if (!verification.success) {
            try {
                (void)plugin_trust_store_.quarantine(record.id, verification.message);
            } catch (...) {
            }
            (void)plugin_loader_.unload_plugin(record.id);
            [[maybe_unused]] const auto diagnostic_id = execution_diagnostics_.record(
                DiagnosticSeverity::error, DiagnosticDomain::plugin, widen_ascii(record.id),
                L"package verification failed: " + widen_ascii(verification.message));
            [[maybe_unused]] const auto id = notification_center_.publish(
                NotificationLevel::error, L"Plugin quarantined",
                widen_ascii(record.id) + L": " + widen_ascii(verification.message));
            continue;
        }
        const auto loaded = plugin_loader_.load_manifest(verification.manifest_path);
        if (!loaded.success) {
            [[maybe_unused]] const auto id =
                notification_center_.publish(NotificationLevel::warning, L"Plugin load",
                                             widen_ascii(record.id) + L": " + loaded.message);
        }
    }
}
CommandResult App::plugin_management_command(std::wstring_view arguments) {
    const auto [raw_action, raw_rest] = split_command(trim(arguments));
    const auto action = lower_copy(raw_action);
    const auto rest = trim(raw_rest);
    const auto plugin_id = [&](std::wstring_view value) -> std::optional<std::string> {
        const auto ascii = narrow_ascii(trim(value));
        if (!ascii || !is_valid_plugin_id(*ascii))
            return std::nullopt;
        return ascii;
    };
    if (action.empty() || action == L"status" || action == L"list") {
        const auto records = plugin_trust_store_.list();
        const auto loaded = plugin_loader_.loaded_plugin_ids();
        const auto trust = plugin_trust_store_.status();
        std::vector<ResultLine> lines;
        lines.push_back({L"Managed plugins: " + std::to_wstring(records.size()) + L" | loaded=" +
                             std::to_wstring(loaded.size()) + L" | master=" +
                             std::wstring{settings_.plugins_enabled ? L"on" : L"off"},
                         ResultKind::success});
        lines.push_back({L"Trust file: " + trust.path.wstring() + L" | quarantined=" +
                             std::to_wstring(trust.quarantined) +
                             (trust.recovered_from_alternate ? L" | recovered-from-backup" : L""),
                         ResultKind::info});
        for (const auto &record : records) {
            const bool is_loaded = plugin_loader_.is_loaded(record.id);
            std::wstring flags =
                record.quarantined ? L"quarantined" : (record.enabled ? L"enabled" : L"disabled");
            if (is_loaded)
                flags += L",loaded";
            const auto pin = record.library_sha256.substr(0, 12);
            std::wstring line = widen_ascii(record.id) + L" | v" + widen_ascii(record.version) +
                                L" | " + flags + L" | sha256=" + widen_ascii(pin);
            if (record.quarantined && !record.quarantine_reason.empty()) {
                line += L" | " + widen_ascii(record.quarantine_reason);
            }
            if (const auto last_error = execution_diagnostics_.last_error(DiagnosticDomain::plugin,
                                                                          widen_ascii(record.id))) {
                line += L" | last-error=" + last_error->message;
            }
            lines.push_back(
                {std::move(line), record.quarantined ? ResultKind::warning : ResultKind::info});
        }
        if (records.empty()) {
            lines.push_back({L"Install with: plugin install <path-to-.axp>. Installation never "
                             L"auto-enables a plugin.",
                             ResultKind::info});
        }
        return {std::move(lines), false};
    }
    if (action == L"commands") {
        const auto commands = plugin_host_.list_commands();
        std::vector<ResultLine> lines{
            {L"Loaded plugin commands: " + std::to_wstring(commands.size()), ResultKind::success}};
        for (const auto &command : commands) {
            lines.push_back({command.usage + L" | " + widen_ascii(command.owner_plugin_id) +
                                 L" | " + command.summary,
                             ResultKind::info});
        }
        return {std::move(lines), false};
    }
    if (action == L"install") {
        if (rest.empty())
            return {{{L"Usage: plugin install <path-to-manifest.axp>", ResultKind::warning}},
                    false};
        const auto result = plugin_packages_.install(std::filesystem::path{std::wstring{rest}});
        return {{{widen_ascii(result.message),
                  result.success ? ResultKind::success : ResultKind::error}},
                false};
    }
    if (action == L"enable") {
        const auto id = plugin_id(rest);
        if (!id)
            return {{{L"Usage: plugin enable <plugin-id>", ResultKind::warning}}, false};
        const auto record = plugin_trust_store_.find(*id);
        if (!record)
            return {{{L"Plugin is not installed.", ResultKind::error}}, false};
        const auto verification = plugin_packages_.verify(*record);
        if (!verification.success) {
            (void)plugin_trust_store_.quarantine(*id, verification.message);
            (void)plugin_loader_.unload_plugin(*id);
            reconcile_plugin_automations();
            return {{{L"Plugin failed pinned verification and was quarantined: " +
                          widen_ascii(verification.message),
                      ResultKind::error}},
                    false};
        }
        (void)plugin_trust_store_.set_enabled(*id, true);
        std::vector<ResultLine> lines{
            {L"Plugin enabled in trust policy: " + widen_ascii(*id), ResultKind::success}};
        if (settings_.plugins_enabled) {
            const auto loaded = plugin_loader_.load_manifest(verification.manifest_path);
            lines.push_back(
                {loaded.message, loaded.success ? ResultKind::success : ResultKind::warning});
        } else {
            lines.push_back({L"Global plugin loading is off; the plugin will remain unloaded until "
                             L"settings plugins on.",
                             ResultKind::info});
        }
        reconcile_plugin_automations();
        return {std::move(lines), false};
    }
    if (action == L"disable") {
        const auto id = plugin_id(rest);
        if (!id)
            return {{{L"Usage: plugin disable <plugin-id>", ResultKind::warning}}, false};
        if (!plugin_trust_store_.find(*id))
            return {{{L"Plugin is not installed.", ResultKind::error}}, false};
        (void)plugin_trust_store_.set_enabled(*id, false);
        (void)plugin_loader_.unload_plugin(*id);
        reconcile_plugin_automations();
        return {
            {{L"Plugin disabled and unloaded. Dependent automations remain persisted but parked.",
              ResultKind::success}},
            false};
    }
    if (action == L"verify") {
        const auto target = lower_copy(rest);
        if (target.empty())
            return {{{L"Usage: plugin verify <plugin-id|all>", ResultKind::warning}}, false};
        std::vector<PluginTrustRecord> records;
        if (target == L"all") {
            records = plugin_trust_store_.list();
        } else {
            const auto id = plugin_id(rest);
            if (!id)
                return {{{L"Invalid plugin ID.", ResultKind::warning}}, false};
            const auto record = plugin_trust_store_.find(*id);
            if (!record)
                return {{{L"Plugin is not installed.", ResultKind::error}}, false};
            records.push_back(*record);
        }
        std::vector<ResultLine> lines;
        for (const auto &record : records) {
            const auto verification = plugin_packages_.verify(record);
            if (!verification.success) {
                (void)plugin_trust_store_.quarantine(record.id, verification.message);
                (void)plugin_loader_.unload_plugin(record.id);
                lines.push_back({widen_ascii(record.id) + L": quarantined - " +
                                     widen_ascii(verification.message),
                                 ResultKind::error});
            } else {
                lines.push_back(
                    {widen_ascii(record.id) + L": pinned package verified", ResultKind::success});
            }
        }
        reconcile_plugin_automations();
        if (lines.empty())
            lines.push_back({L"No installed plugins.", ResultKind::info});
        return {std::move(lines), false};
    }
    if (action == L"reload") {
        if (!settings_.plugins_enabled)
            return {{{L"Global plugin loading is off.", ResultKind::warning}}, false};
        const auto target = lower_copy(rest);
        if (target.empty())
            return {{{L"Usage: plugin reload <plugin-id|all>", ResultKind::warning}}, false};
        std::vector<PluginTrustRecord> records;
        if (target == L"all")
            records = plugin_trust_store_.list();
        else {
            const auto id = plugin_id(rest);
            if (!id)
                return {{{L"Invalid plugin ID.", ResultKind::warning}}, false};
            const auto record = plugin_trust_store_.find(*id);
            if (!record)
                return {{{L"Plugin is not installed.", ResultKind::error}}, false};
            records.push_back(*record);
        }
        std::vector<ResultLine> lines;
        for (const auto &record : records) {
            if (!record.enabled || record.quarantined)
                continue;
            const auto verification = plugin_packages_.verify(record);
            if (!verification.success) {
                (void)plugin_trust_store_.quarantine(record.id, verification.message);
                (void)plugin_loader_.unload_plugin(record.id);
                lines.push_back({widen_ascii(record.id) + L": quarantined - " +
                                     widen_ascii(verification.message),
                                 ResultKind::error});
                continue;
            }
            (void)plugin_loader_.unload_plugin(record.id);
            const auto loaded = plugin_loader_.load_manifest(verification.manifest_path);
            lines.push_back({widen_ascii(record.id) + L": " + loaded.message,
                             loaded.success ? ResultKind::success : ResultKind::warning});
        }
        reconcile_plugin_automations();
        if (lines.empty())
            lines.push_back({L"No enabled non-quarantined plugins matched.", ResultKind::info});
        return {std::move(lines), false};
    }
    if (action == L"remove") {
        const auto id = plugin_id(rest);
        if (!id)
            return {{{L"Usage: plugin remove <plugin-id>", ResultKind::warning}}, false};
        const auto record = plugin_trust_store_.find(*id);
        if (!record)
            return {{{L"Plugin is not installed.", ResultKind::error}}, false};
        if (record->enabled)
            (void)plugin_trust_store_.set_enabled(*id, false);
        (void)plugin_loader_.unload_plugin(*id);
        reconcile_plugin_automations();
        const auto result = plugin_packages_.remove(*id);
        return {{{widen_ascii(result.message),
                  result.success ? ResultKind::success : ResultKind::error}},
                false};
    }
    return {{{L"Usage: plugin [status|list|commands|install <manifest.axp>|enable <id>|disable "
              L"<id>|verify <id|all>|reload <id|all>|remove <id>]",
              ResultKind::warning}},
            false};
}
void App::apply_journal_retention() {
    const auto now = std::chrono::system_clock::now();
    if (settings_.journal_activity_retention_days != 0) {
        (void)journal_store_.prune_before(
            now - std::chrono::hours{24ull * settings_.journal_activity_retention_days},
            JournalEntryKind::activity);
    }
    if (settings_.journal_note_retention_days != 0) {
        (void)journal_store_.prune_before(
            now - std::chrono::hours{24ull * settings_.journal_note_retention_days},
            JournalEntryKind::note);
    }
}
CommandResult App::data_command(std::wstring_view arguments) {
    const auto [raw_action, raw_value] = split_command(trim(arguments));
    const auto action = lower_copy(raw_action);
    auto value = trim(raw_value);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
        value = value.substr(1, value.size() - 2);
    DpapiDataProtector protector;
    const std::vector<LocalDataFile> stores{
        {"settings.conf", app_settings_store_.file_path()},
        {"reminders.bin", default_reminder_store_path()},
        {"automations.bin", default_automation_store_path()},
        {"journal.bin", default_journal_store_path()},
    };
    if (action.empty() || action == L"status") {
        const auto journal = journal_store_.status();
        return {
            {
                {L"Axiom local-data privacy", ResultKind::success},
                {L"backup-protector=" + protector.name(), ResultKind::info},
                {L"journal-at-rest=" +
                     std::wstring{journal.protected_at_rest ? L"AXJP-v1 protected"
                                                            : L"AXJR-v1 plaintext"} +
                     L" requested=" + (journal.protection_requested ? L"protected" : L"plaintext") +
                     L" protector=" + journal.protector_name,
                 journal.protected_at_rest ? ResultKind::success : ResultKind::warning},
                {L"journal-migration=" +
                     std::wstring{journal.protection_migrated ? L"performed-this-session"
                                                              : L"none"} +
                     L" recovery=" +
                     (journal.recovered_from_alternate ? L"alternate-candidate" : L"primary"),
                 ResultKind::info},
                {L"journal-entries=" + std::to_wstring(journal.entry_count) + L" max=" +
                     std::to_wstring(settings_.journal_max_entries),
                 ResultKind::info},
                {L"activity-retention-days=" +
                     std::to_wstring(settings_.journal_activity_retention_days) +
                     L" note-retention-days=" +
                     std::to_wstring(settings_.journal_note_retention_days),
                 ResultKind::info},
                {L"context-access=" +
                     std::wstring{settings_.journal_context_enabled ? L"enabled" : L"disabled"},
                 ResultKind::info},
                {L"local-root=" + app_settings_store_.file_path().parent_path().wstring(),
                 ResultKind::info},
                {L"restore-recovery-startup=" +
                     std::wstring{startup_restore_recovery_.journal_found ? L"journal-processed"
                                                                          : L"no-journal"} +
                     L" rolled-back=" +
                     std::to_wstring(startup_restore_recovery_.recovery.files_rolled_back) +
                     L" cleanup=" +
                     std::to_wstring(startup_restore_recovery_.recovery.temporary_files_removed) +
                     L" orphan-journal-tmp=" +
                     (startup_restore_recovery_.orphan_journal_temporary_removed ? L"removed"
                                                                                 : L"none"),
                 ResultKind::info},
            },
            false};
    }
    if (action == L"support-bundle") {
        const auto parsed = parse_support_bundle_arguments(raw_value);
        if (!parsed) {
            return {{{L"Usage: data support-bundle <output-path> [messages] [sensitive]",
                      ResultKind::warning}},
                    false};
        }
        const auto diagnostics_stats = execution_diagnostics_.stats();
        const auto watcher = watcher_health_.snapshot();
        const auto index = file_index_.snapshot();
        std::vector<SupportField> fields{
            {"product-version", "0.16.0", false},
            {"product-state", "v16-operational-recovery-wip", false},
            {"watcher-health", watcher_health_state_name(watcher.state), false},
            {"watcher-roots", std::to_string(watcher.watched_roots), false},
            {"watcher-notifications", std::to_string(watcher.notifications), false},
            {"watcher-overflows", std::to_string(watcher.overflows), false},
            {"watcher-errors", std::to_string(watcher.errors), false},
            {"watcher-successful-resyncs", std::to_string(watcher.successful_resyncs), false},
            {"watcher-issue-generation", std::to_string(watcher.issue_generation), false},
            {"index-entries", std::to_string(index.entries), false},
            {"index-files", std::to_string(index.files), false},
            {"index-directories", std::to_string(index.directories), false},
            {"index-root-count", std::to_string(index.roots.size()), false},
            {"diagnostics-retained", std::to_string(diagnostics_stats.retained), false},
            {"diagnostics-capacity", std::to_string(diagnostics_stats.capacity), false},
            {"diagnostics-recorded", std::to_string(diagnostics_stats.total_recorded), false},
            {"diagnostics-errors", std::to_string(diagnostics_stats.total_errors), false},
            {"diagnostics-dropped-oldest", std::to_string(diagnostics_stats.dropped_oldest), false},
            {"settings-notifications", settings_.notifications_enabled ? "on" : "off", false},
            {"settings-plugins", settings_.plugins_enabled ? "on" : "off", false},
            {"settings-journal-context", settings_.journal_context_enabled ? "on" : "off", false},
            {"settings-journal-protection",
             settings_.journal_live_protection_enabled ? "on" : "off", false},
            {"restore-startup-journal-found",
             startup_restore_recovery_.journal_found ? "yes" : "no", false},
            {"restore-startup-rolled-back",
             std::to_string(startup_restore_recovery_.recovery.files_rolled_back), false},
            {"restore-startup-cleanup",
             std::to_string(startup_restore_recovery_.recovery.temporary_files_removed), false},
            {"local-data-root",
             wide_to_utf8(app_settings_store_.file_path().parent_path().wstring()), true},
            {"settings-file", wide_to_utf8(app_settings_store_.file_path().wstring()), true},
        };
        constexpr std::size_t support_bundle_root_limit = 32;
        const auto included_roots = std::min(index.roots.size(), support_bundle_root_limit);
        fields.push_back({"index-roots-included", std::to_string(included_roots), false});
        for (std::size_t i = 0; i < included_roots; ++i) {
            fields.push_back(
                {"index-root-" + std::to_string(i), wide_to_utf8(index.roots[i].wstring()), true});
        }
        std::vector<SupportDiagnostic> diagnostics;
        const auto recent = execution_diagnostics_.recent(256);
        diagnostics.reserve(recent.size());
        for (const auto &event : recent) {
            const auto unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     event.occurred_at.time_since_epoch())
                                     .count();
            diagnostics.push_back(SupportDiagnostic{
                event.id,
                unix_ms,
                wide_to_utf8(diagnostic_severity_name(event.severity)),
                wide_to_utf8(diagnostic_domain_name(event.domain)),
                wide_to_utf8(event.subject),
                wide_to_utf8(event.message),
            });
        }
        SupportBundleOptions options;
        options.include_diagnostic_messages = parsed->include_diagnostic_messages;
        options.include_sensitive_fields = parsed->include_sensitive_fields;
        options.max_diagnostics = 100;
        const auto report =
            SupportBundle::create(parsed->output_path, fields, diagnostics, options);
        std::vector<ResultLine> lines{
            {L"Support bundle created: " + parsed->output_path.wstring(), ResultKind::success},
            {L"fields=" + std::to_wstring(report.fields_written) + L" sensitive-omitted=" +
                 std::to_wstring(report.sensitive_fields_omitted) + L" diagnostics=" +
                 std::to_wstring(report.diagnostics_written) + L" messages-omitted=" +
                 std::to_wstring(report.diagnostic_messages_omitted) + L" bytes=" +
                 std::to_wstring(report.bytes_written),
             ResultKind::info},
            {L"Privacy: diagnostic messages and local/index paths are omitted unless explicitly "
             L"requested with `messages` / `sensitive`.",
             (parsed->include_diagnostic_messages || parsed->include_sensitive_fields)
                 ? ResultKind::warning
                 : ResultKind::info},
            {L"Raw action payloads, clipboard contents, journal entries, backup payload bytes and "
             L"plugin binaries are never bundle inputs.",
             ResultKind::info},
        };
        return {std::move(lines), false};
    }
    if (action == L"journal-protection") {
        if (value.empty() || lower_copy(value) == L"status") {
            const auto journal = journal_store_.status();
            return {{{
                        L"journal-protection=" +
                            std::wstring{journal.protected_at_rest ? L"on (AXJP-v1)"
                                                                   : L"off (AXJR-v1 plaintext)"} +
                            L" requested=" + (journal.protection_requested ? L"on" : L"off") +
                            L" protector=" + journal.protector_name,
                        journal.protected_at_rest ? ResultKind::success : ResultKind::warning,
                    }},
                    false};
        }
        return settings_command(L"journal-protection " + value);
    }
    if (action == L"backup" || action == L"backup-portable") {
        if (value.empty())
            return {{{L"Usage: data " + action + L" <archive-path>", ResultKind::warning}}, false};
        const bool protected_backup = action == L"backup";
        if (protected_backup && !protector.available())
            return {{{L"OS-protected backup requires Windows DPAPI.", ResultKind::error}}, false};
        std::vector<LocalDataBlob> overrides;
        if (!protected_backup) {
            overrides.push_back({"journal.bin", journal_store_.plaintext_snapshot_bytes()});
        }
        const auto info =
            LocalDataArchive::create(std::filesystem::path{value}, stores,
                                     protected_backup ? &protector : nullptr, overrides);
        return {{{L"Backup created: " + value, ResultKind::success},
                 {L"entries=" + std::to_wstring(info.entries.size()) + L" protection=" +
                      (info.protected_by_os ? L"DPAPI/current-user"
                                            : L"none; journal entry is portable plaintext AXJR-v1"),
                  info.protected_by_os ? ResultKind::info : ResultKind::warning}},
                false};
    }
    if (action == L"inspect") {
        if (value.empty())
            return {{{L"Usage: data inspect <archive-path>", ResultKind::warning}}, false};
        const auto info = LocalDataArchive::inspect(std::filesystem::path{value}, &protector);
        std::vector<ResultLine> lines{
            {L"Backup archive", ResultKind::success},
            {L"protection=" + std::wstring{info.protected_by_os ? L"DPAPI/current-user" : L"none"},
             ResultKind::info}};
        for (const auto &entry : info.entries)
            lines.push_back({std::wstring{entry.name.begin(), entry.name.end()} + L" " +
                                 format_size(entry.size),
                             ResultKind::info});
        return {std::move(lines), false};
    }
    if (action == L"restore") {
        if (value.empty())
            return {{{L"Usage: data restore <archive-path>", ResultKind::warning}}, false};
        (void)LocalDataArchive::inspect(std::filesystem::path{value}, &protector);
        automation_engine_.stop();
        reminder_center_.stop();
        try {
            const auto info =
                LocalDataArchive::restore(std::filesystem::path{value}, stores,
                                          default_restore_recovery_journal_path(), &protector);
            std::vector<ResultLine> lines{
                {L"Restore committed transactionally for " + std::to_wstring(info.entries.size()) +
                     L" file(s). Axiom will exit; restart to load restored state.",
                 ResultKind::success}};
            if (info.recovery_cleanup_pending) {
                lines.push_back({L"Restore data is committed; recovery metadata cleanup is pending "
                                 L"and will complete before stores load on next startup.",
                                 ResultKind::warning});
            }
            return {std::move(lines), true};
        } catch (...) {
            try {
                (void)reminder_center_.restore();
            } catch (...) {
            }
            try {
                (void)automation_engine_.restore();
            } catch (...) {
            }
            throw;
        }
    }
    if (action == L"journal-export" || action == L"journal-export-all") {
        if (value.empty())
            return {{{L"Usage: data " + action + L" <file.axj>", ResultKind::warning}}, false};
        const auto result = export_journal(journal_store_, std::filesystem::path{value},
                                           action == L"journal-export-all");
        return {{{L"Journal export created: entries=" + std::to_wstring(result.entries) +
                      L". Export is UTF-8 plaintext; protect it separately if sensitive.",
                  ResultKind::warning}},
                false};
    }
    if (action == L"journal-import") {
        if (value.empty())
            return {{{L"Usage: data journal-import <file.axj>", ResultKind::warning}}, false};
        const auto result = import_journal(journal_store_, std::filesystem::path{value});
        apply_journal_retention();
        return {{{L"Journal import complete: entries=" + std::to_wstring(result.entries) +
                      L" notes=" + std::to_wstring(result.notes) + L" activities=" +
                      std::to_wstring(result.activities),
                  ResultKind::success}},
                false};
    }
    if (action == L"context-access") {
        const auto lower = lower_copy(value);
        if (lower != L"on" && lower != L"off")
            return {{{L"Usage: data context-access <on|off>", ResultKind::warning}}, false};
        settings_.journal_context_enabled = lower == L"on";
        save_settings();
        return {
            {{L"Journal context access is now " +
                  std::wstring{settings_.journal_context_enabled ? L"enabled" : L"disabled"} + L".",
              ResultKind::success}},
            false};
    }
    if (action == L"retention") {
        const auto [raw_key, raw_number] = split_command(value);
        const auto key = lower_copy(raw_key);
        if (key.empty() || key == L"status")
            return data_command(L"status");
        const auto number = parse_u64(raw_number);
        if (!number || *number > 200000)
            return {{{L"Usage: data retention [max-entries 100..200000|activity-days "
                      L"0..3650|note-days 0..3650]",
                      ResultKind::warning}},
                    false};
        if (key == L"max-entries") {
            if (*number < 100)
                return {{{L"journal max-entries minimum is 100.", ResultKind::warning}}, false};
            settings_.journal_max_entries = static_cast<std::uint32_t>(*number);
            const auto pruned = journal_store_.set_max_entries(settings_.journal_max_entries);
            save_settings();
            return {{{L"Journal max entries updated; pruned=" + std::to_wstring(pruned),
                      ResultKind::success}},
                    false};
        }
        if ((key == L"activity-days" || key == L"note-days") && *number <= 3650) {
            if (key == L"activity-days")
                settings_.journal_activity_retention_days = static_cast<std::uint32_t>(*number);
            else
                settings_.journal_note_retention_days = static_cast<std::uint32_t>(*number);
            save_settings();
            apply_journal_retention();
            return {{{L"Journal retention updated.", ResultKind::success}}, false};
        }
        return {{{L"Usage: data retention [max-entries 100..200000|activity-days 0..3650|note-days "
                  L"0..3650]",
                  ResultKind::warning}},
                false};
    }
    return {{{L"Usage: data [status|support-bundle <output-path> [messages] "
              L"[sensitive]|backup|backup-portable|inspect|restore|journal-export|journal-export-"
              L"all|journal-import|journal-protection|context-access|retention]",
              ResultKind::warning}},
            false};
}
bool App::set_launch_at_startup(bool enabled, std::wstring &error_message) {
    constexpr wchar_t registry_path[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t value_name[] = L"AxiomDesktop";
    HKEY raw_key = nullptr;
    const LONG open_result =
        RegCreateKeyExW(HKEY_CURRENT_USER, registry_path, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &raw_key, nullptr);
    if (open_result != ERROR_SUCCESS) {
        error_message = L"RegCreateKeyExW error " + std::to_wstring(open_result);
        return false;
    }
    struct RegistryCloser {
        HKEY key{};
        ~RegistryCloser() {
            if (key != nullptr)
                RegCloseKey(key);
        }
    } closer{raw_key};
    LONG result = ERROR_SUCCESS;
    if (enabled) {
        const auto command = startup_command_line();
        if (command.empty()) {
            error_message = L"Could not resolve Axiom executable path.";
            return false;
        }
        result = RegSetValueExW(raw_key, value_name, 0, REG_SZ,
                                reinterpret_cast<const BYTE *>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(raw_key, value_name);
        if (result == ERROR_FILE_NOT_FOUND) {
            result = ERROR_SUCCESS;
        }
    }
    if (result != ERROR_SUCCESS) {
        error_message = L"Windows registry error " + std::to_wstring(result);
        return false;
    }
    return true;
}
bool App::startup_registration_matches() const {
    constexpr wchar_t registry_path[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t value_name[] = L"AxiomDesktop";
    HKEY raw_key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, registry_path, 0, KEY_QUERY_VALUE, &raw_key) !=
        ERROR_SUCCESS) {
        return !settings_.launch_at_startup;
    }
    struct RegistryCloser {
        HKEY key{};
        ~RegistryCloser() {
            if (key != nullptr)
                RegCloseKey(key);
        }
    } closer{raw_key};
    DWORD type = 0;
    DWORD bytes = 0;
    LONG result = RegQueryValueExW(raw_key, value_name, nullptr, &type, nullptr, &bytes);
    if (result == ERROR_FILE_NOT_FOUND) {
        return !settings_.launch_at_startup;
    }
    if (result != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t)) {
        return false;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    result = RegQueryValueExW(raw_key, value_name, nullptr, &type,
                              reinterpret_cast<BYTE *>(value.data()), &bytes);
    if (result != ERROR_SUCCESS) {
        return false;
    }
    if (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return settings_.launch_at_startup ? value == startup_command_line() : false;
}
void App::restart_file_watcher(bool recovery_restart) {
    const auto snapshot = file_index_.snapshot();
    if (!recovery_restart) {
        watcher_health_.started(snapshot.roots.size());
    }
    file_watcher_.start(
        snapshot.roots, [this](const FileChange &change) { handle_file_change(change); },
        [this](const WatcherIssue &issue) { handle_watcher_issue(issue); });
    watcher_health_.update_watched_roots(file_watcher_.watch_count());
}
void App::handle_file_change(const FileChange &change) {
    watcher_health_.note_notification();
    switch (change.kind) {
    case FileChangeKind::removed:
    case FileChangeKind::renamed_old:
        file_index_.remove_path(change.path);
        break;
    case FileChangeKind::added:
    case FileChangeKind::modified:
    case FileChangeKind::renamed_new:
        file_index_.refresh_path(change.path);
        break;
    }
}
void App::handle_watcher_issue(const WatcherIssue &issue) {
    const auto generation = issue.kind == WatcherIssueKind::overflow
                                ? watcher_health_.note_overflow(issue.error_code)
                                : watcher_health_.note_error(issue.error_code);

    if (!watcher_restart_in_progress_.load(std::memory_order_acquire)) {
        watcher_resync_.request(generation);
    }
}
bool App::perform_watcher_resync(std::stop_token stop_token, std::uint64_t issue_generation) {
    if (stop_token.stop_requested()) {
        return false;
    }

    const auto initial = watcher_health_.snapshot();
    if (initial.issue_generation != issue_generation) {
        return true;
    }

    if (initial.restart_required) {
        watcher_restart_in_progress_.store(true, std::memory_order_release);
        restart_file_watcher(true);
        watcher_restart_in_progress_.store(false, std::memory_order_release);

        if (stop_token.stop_requested()) {
            return false;
        }

        const auto after_restart = watcher_health_.snapshot();
        if (after_restart.issue_generation != issue_generation) {
            return true;
        }
        if (file_watcher_.watch_count() != file_index_.snapshot().roots.size()) {
            return false;
        }
        if (!watcher_health_.mark_restarted(issue_generation)) {
            return false;
        }
    }

    const auto scan_generation = file_index_.rebuild_tracked();
    return file_index_.wait_for_scan(scan_generation, stop_token) == IndexScanWaitResult::succeeded;
}
CommandResult App::index_recovery_command(std::wstring_view arguments) {
    const auto action = lower_copy(trim(arguments));
    if (action.empty() || action == L"status") {
        const auto health = watcher_health_.snapshot();
        const auto resync = watcher_resync_.stats();
        std::vector<ResultLine> lines;
        const auto health_kind =
            health.state == WatcherHealthState::healthy
                ? ResultKind::success
                : (health.state == WatcherHealthState::stopped ? ResultKind::info
                                                               : ResultKind::warning);
        lines.push_back({
            L"Watcher health: " + widen_ascii(watcher_health_state_name(health.state)),
            health_kind,
        });
        lines.push_back({
            L"Watcher roots=" + std::to_wstring(health.watched_roots) + L" notifications=" +
                std::to_wstring(health.notifications) + L" overflows=" +
                std::to_wstring(health.overflows) + L" errors=" + std::to_wstring(health.errors) +
                L" manual-resyncs=" + std::to_wstring(health.manual_resync_requests) +
                L" successful-resyncs=" + std::to_wstring(health.successful_resyncs),
            ResultKind::info,
        });
        lines.push_back({
            L"Watcher generation=" + std::to_wstring(health.issue_generation) + L" last-resynced=" +
                std::to_wstring(health.last_resynced_generation) + L" restart-required=" +
                (health.restart_required ? std::wstring{L"yes"} : std::wstring{L"no"}),
            ResultKind::info,
        });
        lines.push_back({
            L"Resync worker: " + std::wstring{resync.running ? L"running" : L"idle"} +
                L" requests=" + std::to_wstring(resync.requests) + L" attempts=" +
                std::to_wstring(resync.attempts) + L" completed=" +
                std::to_wstring(resync.completed) + L" failed=" + std::to_wstring(resync.failed) +
                L" superseded=" + std::to_wstring(resync.superseded) + L" pending-generation=" +
                std::to_wstring(resync.pending_generation),
            ResultKind::info,
        });
        if (health.last_error != 0) {
            lines.push_back({L"Last watcher error: " + std::to_wstring(health.last_error),
                             ResultKind::warning});
        }
        return {std::move(lines), false};
    }
    if (action == L"resync") {
        const auto generation = watcher_health_.request_resync();
        if (generation == 0) {
            return {{{L"Watcher resync is unavailable because no active roots are being watched.",
                      ResultKind::warning}},
                    false};
        }
        watcher_resync_.request(generation);
        return {{{
                    L"Watcher/index full resync requested for generation " +
                        std::to_wstring(generation) + L".",
                    ResultKind::success,
                }},
                false};
    }
    return {{{L"Usage: index [status|rebuild|resync|root|exclude]", ResultKind::warning}}, false};
}
void App::register_builtin_actions() {
    const auto require_registered = [](bool registered, const char *name) {
        if (!registered) {
            throw std::runtime_error{std::string{"Duplicate built-in automation action: "} + name};
        }
    };
    require_registered(
        action_registry_.register_action(
            L"notify",
            L"Publish a local Axiom desktop notification. Payload is the notification body.",
            [this](std::wstring_view payload) {
                const auto body = trim(payload);
                if (body.empty()) {
                    return ActionResult{false, L"notify action requires a non-empty payload."};
                }
                [[maybe_unused]] const auto id = notification_center_.publish(
                    NotificationLevel::info, L"Axiom Automation", body);
                if (window_ != nullptr) {
                    PostMessageW(window_, notification_ready_message, 0, 0);
                }
                return ActionResult{true, L"Notification queued."};
            }),
        "notify");
    require_registered(
        action_registry_.register_action(
            L"open",
            L"Open one explicit file, folder, document, or URL through Windows Shell; no shell "
            L"command expansion.",
            [](std::wstring_view payload) {
                const auto target = trim(payload);
                if (target.empty()) {
                    return ActionResult{false, L"open action requires a path or URL payload."};
                }
                const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
                    nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
                if (result <= 32) {
                    return ActionResult{false, L"ShellExecute failed with code " +
                                                   std::to_wstring(result) + L"."};
                }
                return ActionResult{true, L"Opened: " + target};
            }),
        "open");
    require_registered(
        action_registry_.register_action(
            L"index-rebuild", L"Request a background rebuild of the current Axiom file index.",
            [this](std::wstring_view payload) {
                if (!trim(payload).empty()) {
                    return ActionResult{false, L"index-rebuild does not accept a payload."};
                }
                file_index_.rebuild();
                return ActionResult{true, L"Index rebuild requested."};
            }),
        "index-rebuild");
}
void App::on_automation_executed(const Automation &automation, const ActionResult &result) {
    try {
        [[maybe_unused]] const auto journal_id = journal_store_.add_activity(
            L"automation",
            L"Automation #" + std::to_wstring(automation.id) + L" " + automation.name +
                (result.success ? std::wstring{L" completed"}
                                : std::wstring{L" failed: "} + result.message),
            {L"automation", result.success ? L"success" : L"failure"});
    } catch (...) {
    }
    if (result.success) {
        return;
    }
    [[maybe_unused]] const auto id = notification_center_.publish(
        NotificationLevel::error, L"Automation failed",
        L"#" + std::to_wstring(automation.id) + L" " + automation.name + L" — " + result.message);
    if (window_ != nullptr) {
        PostMessageW(window_, notification_ready_message, 0, 0);
    }
}
void App::on_reminder_fired(const Reminder &reminder) {
    try {
        [[maybe_unused]] const auto journal_id = journal_store_.add_activity(
            L"reminder", L"Reminder fired: " + reminder.message, {L"reminder"});
    } catch (...) {
    }
    [[maybe_unused]] const auto id = notification_center_.publish(
        NotificationLevel::info, L"Axiom Reminder",
        reminder.message + L" (#" + std::to_wstring(reminder.id) + L")");
    if (window_ != nullptr) {
        PostMessageW(window_, notification_ready_message, 0, 0);
    }
}
void App::pump_notifications() {
    KillTimer(window_, notification_pump_timer_id);
    auto pending = notification_center_.drain_pending(1);
    if (!pending.empty() && settings_.notifications_enabled) {
        show_notification(pending.front());
    }
    if (notification_center_.stats().pending > 0) {
        SetTimer(window_, notification_pump_timer_id, notification_pump_interval_ms, nullptr);
    }
}
void App::show_notification(const Notification &notification) {
    if (!tray_icon_added_) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = tray_icon_id;
    data.uFlags = NIF_INFO;
    wcsncpy_s(data.szInfoTitle, notification.title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, notification.body.c_str(), _TRUNCATE);
    data.uTimeout = 8000;
    switch (notification.level) {
    case NotificationLevel::error:
        data.dwInfoFlags = NIIF_ERROR;
        break;
    case NotificationLevel::warning:
        data.dwInfoFlags = NIIF_WARNING;
        break;
    case NotificationLevel::success:
    case NotificationLevel::info:
    default:
        data.dwInfoFlags = NIIF_INFO;
        break;
    }
    Shell_NotifyIconW(NIM_MODIFY, &data);
}
void App::center_on_active_monitor() {
    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    GetMonitorInfoW(monitor, &monitor_info);
    const RECT &area = monitor_info.rcWork;
    const int width = palette_width;
    const int height = palette_height;
    const int x = area.left + ((area.right - area.left) - width) / 2;
    const int y = area.top + ((area.bottom - area.top) - height) / 3;
    SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
}
LRESULT App::window_proc(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case notification_ready_message:
        pump_notifications();
        return 0;
    case axiom_activate_message:
        show_palette();
        return 0;
    case axiom_verification_shutdown_message:
        exit_requested_ = true;
        PostMessageW(window_, WM_CLOSE, 0, 0);
        return 0;
    case tray_callback_message: {
        const UINT event = LOWORD(static_cast<DWORD>(lparam));
        if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK || event == NIN_SELECT ||
            event == NIN_KEYSELECT) {
            show_palette();
            return 0;
        }
        if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
            show_tray_menu();
            return 0;
        }
        break;
    }
    case WM_ERASEBKGND: {
        RECT area{};
        GetClientRect(window_, &area);
        FillRect(reinterpret_cast<HDC>(wparam), &area, background_brush_);
        return 1;
    }
    case WM_CTLCOLOREDIT: {
        if (reinterpret_cast<HWND>(lparam) == edit_) {
            auto dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, palette_text_);
            SetBkColor(dc, palette_surface_);
            return reinterpret_cast<LRESULT>(surface_brush_);
        }
        break;
    }
    case WM_CTLCOLORLISTBOX: {
        if (reinterpret_cast<HWND>(lparam) == results_) {
            auto dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, palette_text_);
            SetBkColor(dc, palette_surface_);
            return reinterpret_cast<LRESULT>(surface_brush_);
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto *item = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
        if (item == nullptr || item->CtlID != result_list_id ||
            item->itemID == static_cast<UINT>(-1)) {
            break;
        }
        const bool selected = (item->itemState & ODS_SELECTED) != 0;
        const auto kind = static_cast<ResultKind>(item->itemData);
        const COLORREF background = selected ? palette_selection_ : palette_surface_;
        const COLORREF text_color = selected ? palette_selection_text_
                                             : (kind == ResultKind::heading   ? palette_accent_
                                                : kind == ResultKind::success ? palette_success_
                                                : kind == ResultKind::warning ? palette_warning_
                                                : kind == ResultKind::error   ? palette_error_
                                                                              : palette_text_);
        HBRUSH brush = CreateSolidBrush(background);
        if (brush != nullptr) {
            FillRect(item->hDC, &item->rcItem, brush);
            DeleteObject(brush);
        }
        const LRESULT length = SendMessageW(results_, LB_GETTEXTLEN, item->itemID, 0);
        if (length != LB_ERR) {
            std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
            SendMessageW(results_, LB_GETTEXT, item->itemID,
                         reinterpret_cast<LPARAM>(value.data()));
            value.resize(static_cast<std::size_t>(length));
            RECT text_area = item->rcItem;
            text_area.left += kind == ResultKind::heading ? 12 : 18;
            text_area.right -= 10;
            SetBkMode(item->hDC, TRANSPARENT);
            SetTextColor(item->hDC, text_color);
            const auto previous = SelectObject(
                item->hDC,
                kind == ResultKind::heading && heading_font_ != nullptr ? heading_font_ : font_);
            DrawTextW(item->hDC, value.c_str(), static_cast<int>(value.size()), &text_area,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (previous != nullptr) {
                SelectObject(item->hDC, previous);
            }
        }
        if ((item->itemState & ODS_FOCUS) != 0) {
            RECT focus = item->rcItem;
            InflateRect(&focus, -2, -2);
            DrawFocusRect(item->hDC, &focus);
        }
        return TRUE;
    }
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
        apply_theme();
        return 0;
    case WM_CLIPBOARDUPDATE:
        capture_clipboard_text();
        return 0;
    case WM_TIMECHANGE:
        (void)automation_engine_.refresh_calendar_schedules();
        return 0;
    case WM_HOTKEY:
        if (wparam == hotkey_id) {
            if (IsWindowVisible(window_)) {
                hide_palette();
            } else {
                show_palette();
            }
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE && IsWindowVisible(window_)) {
            hide_palette();
        }
        return 0;
    case WM_TIMER:
        if (wparam == live_search_timer_id) {
            KillTimer(window_, live_search_timer_id);
            update_live_results();
            return 0;
        }
        if (wparam == notification_pump_timer_id) {
            pump_notifications();
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wparam) == edit_id && HIWORD(wparam) == EN_CHANGE) {
            KillTimer(window_, live_search_timer_id);
            SetTimer(window_, live_search_timer_id, live_search_debounce_ms, nullptr);
            return 0;
        }
        if (LOWORD(wparam) == result_list_id && HIWORD(wparam) == LBN_DBLCLK) {
            [[maybe_unused]] const bool opened = activate_selected_result();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            hide_palette();
            return 0;
        }
        if (wparam == VK_RETURN && GetFocus() == edit_) {
            execute_input();
            return 0;
        }
        break;
    case WM_CLOSE:
        if (!exit_requested_ && settings_.close_to_tray) {
            hide_palette();
            return 0;
        }
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        remove_tray_icon();
        window_ = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}
LRESULT CALLBACK App::window_proc_thunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    App *self = nullptr;
    if (message == WM_NCCREATE) {
        const auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
        self = static_cast<App *>(create->lpCreateParams);
        self->window_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<App *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->window_proc(message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
int App::run() {
    MSG message{};
    while (true) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == 0) {
            return static_cast<int>(message.wParam);
        }
        if (result == -1) {
            return 1;
        }
        if (IsWindowVisible(window_) && edit_ != nullptr && message.message == WM_KEYDOWN) {
            if (message.hwnd == edit_) {
                if (message.wParam == VK_RETURN) {
                    execute_input();
                    continue;
                }
                if (message.wParam == VK_ESCAPE) {
                    hide_palette();
                    continue;
                }
                if (message.wParam == VK_TAB) {
                    if (const auto completion = command_engine_.complete(current_input())) {
                        set_input_text(*completion);
                        update_live_results();
                    }
                    continue;
                }
                if (message.wParam == VK_DOWN) {
                    KillTimer(window_, live_search_timer_id);
                    update_live_results();
                    if (SendMessageW(results_, LB_GETCOUNT, 0, 0) <= 0) {
                        continue;
                    }
                    SendMessageW(results_, LB_SETCURSEL, 0, 0);
                    SetFocus(results_);
                    continue;
                }
            } else if (message.hwnd == results_) {
                if (message.wParam == VK_RETURN) {
                    if (activate_selected_result()) {
                        continue;
                    }
                }
                if (message.wParam == VK_ESCAPE) {
                    hide_palette();
                    continue;
                }
                if (message.wParam == VK_UP && SendMessageW(results_, LB_GETCURSEL, 0, 0) <= 0) {
                    SetFocus(edit_);
                    continue;
                }
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
} // namespace axiom
