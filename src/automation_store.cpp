#include "automation_store.hpp"
#include "automation_schedule.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace axiom {
namespace {
constexpr std::array<std::byte, 4> magic{std::byte{'A'}, std::byte{'X'}, std::byte{'A'},
                                         std::byte{'U'}};
constexpr std::uint32_t format_version = 2;
constexpr std::uint32_t legacy_format_version = 1;
constexpr std::size_t max_store_bytes = 64u * 1024u * 1024u;
constexpr std::size_t max_records = 10000;
constexpr std::size_t max_name_bytes = 16u * 1024u;
constexpr std::size_t max_payload_bytes = 1024u * 1024u;
constexpr std::size_t max_error_bytes = 64u * 1024u;
constexpr std::size_t header_bytes = 48;
constexpr std::int64_t no_last_run = std::numeric_limits<std::int64_t>::min();
struct Candidate final {
    AutomationStoreSnapshot snapshot;
    std::filesystem::path path;
};
[[nodiscard]] std::filesystem::path temp_path(const std::filesystem::path &path) {
    auto result = path;
    result += L".tmp";
    return result;
}
[[nodiscard]] std::filesystem::path backup_path(const std::filesystem::path &path) {
    auto result = path;
    result += L".bak";
    return result;
}
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto byte : bytes) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<unsigned char>(byte));
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}
template <typename T>
    requires std::is_integral_v<T>
void append_le(std::vector<std::byte> &output, T value) {
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned raw = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>((raw >> (index * 8u)) & 0xFFu));
    }
}
template <typename T>
    requires std::is_integral_v<T>
[[nodiscard]] T read_le(std::span<const std::byte> bytes, std::size_t &offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
        throw std::runtime_error{"Automation store contains a truncated integral field."};
    }
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned raw{};
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        raw |= static_cast<Unsigned>(std::to_integer<unsigned char>(bytes[offset + index]))
               << (index * 8u);
    }
    offset += sizeof(T);
    return static_cast<T>(raw);
}
void append_utf8_codepoint(std::string &output, std::uint32_t codepoint) {
    if (codepoint <= 0x7Fu) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFu) {
        output.push_back(static_cast<char>(0xC0u | (codepoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint <= 0xFFFFu) {
        output.push_back(static_cast<char>(0xE0u | (codepoint >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint <= 0x10FFFFu) {
        output.push_back(static_cast<char>(0xF0u | (codepoint >> 18u)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else {
        throw std::runtime_error{"Automation text contains an invalid Unicode codepoint."};
    }
}
[[nodiscard]] std::string wide_to_utf8(std::wstring_view input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        std::uint32_t codepoint = static_cast<std::uint32_t>(input[index]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
                if (index + 1 >= input.size()) {
                    throw std::runtime_error{
                        "Automation text ends with an unpaired UTF-16 surrogate."};
                }
                const auto low = static_cast<std::uint32_t>(input[++index]);
                if (low < 0xDC00u || low > 0xDFFFu) {
                    throw std::runtime_error{
                        "Automation text contains an invalid UTF-16 surrogate pair."};
                }
                codepoint = 0x10000u + ((codepoint - 0xD800u) << 10u) + (low - 0xDC00u);
            } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
                throw std::runtime_error{"Automation text contains an unpaired UTF-16 surrogate."};
            }
        } else if (codepoint >= 0xD800u && codepoint <= 0xDFFFu) {
            throw std::runtime_error{"Automation text contains a surrogate codepoint."};
        }
        append_utf8_codepoint(output, codepoint);
    }
    return output;
}
[[nodiscard]] std::uint32_t decode_utf8_codepoint(std::string_view input, std::size_t &offset) {
    if (offset >= input.size()) {
        throw std::runtime_error{"Automation store contains truncated UTF-8."};
    }
    const auto first = static_cast<unsigned char>(input[offset++]);
    if (first <= 0x7Fu) {
        return first;
    }
    int continuation_count = 0;
    std::uint32_t codepoint = 0;
    std::uint32_t minimum = 0;
    if ((first & 0xE0u) == 0xC0u) {
        continuation_count = 1;
        codepoint = first & 0x1Fu;
        minimum = 0x80u;
    } else if ((first & 0xF0u) == 0xE0u) {
        continuation_count = 2;
        codepoint = first & 0x0Fu;
        minimum = 0x800u;
    } else if ((first & 0xF8u) == 0xF0u) {
        continuation_count = 3;
        codepoint = first & 0x07u;
        minimum = 0x10000u;
    } else {
        throw std::runtime_error{"Automation store contains an invalid UTF-8 prefix."};
    }
    for (int index = 0; index < continuation_count; ++index) {
        if (offset >= input.size()) {
            throw std::runtime_error{"Automation store contains a truncated UTF-8 sequence."};
        }
        const auto byte = static_cast<unsigned char>(input[offset++]);
        if ((byte & 0xC0u) != 0x80u) {
            throw std::runtime_error{
                "Automation store contains an invalid UTF-8 continuation byte."};
        }
        codepoint = (codepoint << 6u) | (byte & 0x3Fu);
    }
    if (codepoint < minimum || codepoint > 0x10FFFFu ||
        (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
        throw std::runtime_error{"Automation store contains non-canonical UTF-8."};
    }
    return codepoint;
}
[[nodiscard]] std::wstring utf8_to_wide(std::string_view input) {
    std::wstring output;
    std::size_t offset = 0;
    while (offset < input.size()) {
        auto codepoint = decode_utf8_codepoint(input, offset);
        if constexpr (sizeof(wchar_t) == 2) {
            if (codepoint <= 0xFFFFu) {
                output.push_back(static_cast<wchar_t>(codepoint));
            } else {
                codepoint -= 0x10000u;
                output.push_back(static_cast<wchar_t>(0xD800u + (codepoint >> 10u)));
                output.push_back(static_cast<wchar_t>(0xDC00u + (codepoint & 0x3FFu)));
            }
        } else {
            output.push_back(static_cast<wchar_t>(codepoint));
        }
    }
    return output;
}
void append_text(std::vector<std::byte> &payload, std::wstring_view text, std::size_t limit,
                 const char *field) {
    const auto utf8 = wide_to_utf8(text);
    if (utf8.size() > limit) {
        throw std::runtime_error{std::string{"Automation "} + field +
                                 " exceeds its storage limit."};
    }
    append_le(payload, static_cast<std::uint32_t>(utf8.size()));
    payload.insert(payload.end(), reinterpret_cast<const std::byte *>(utf8.data()),
                   reinterpret_cast<const std::byte *>(utf8.data() + utf8.size()));
}
[[nodiscard]] std::wstring read_text(std::span<const std::byte> payload, std::size_t &offset,
                                     std::size_t limit, const char *field) {
    const auto size = read_le<std::uint32_t>(payload, offset);
    if (size > limit || offset > payload.size() || payload.size() - offset < size) {
        throw std::runtime_error{std::string{"Automation store contains invalid "} + field +
                                 " bytes."};
    }
    const auto *data = reinterpret_cast<const char *>(payload.data() + offset);
    auto result = utf8_to_wide(std::string_view{data, size});
    offset += size;
    return result;
}
[[nodiscard]] std::vector<std::byte> serialize_payload(std::span<const Automation> automations) {
    std::vector<std::byte> payload;
    for (const auto &automation : automations) {
        if (automation.id == 0 || automation.name.empty() || automation.action_name.empty()) {
            throw std::runtime_error{"Automation ID, name, and action name must be non-empty."};
        }
        if (automation.repeat_interval < std::chrono::milliseconds::zero()) {
            throw std::runtime_error{"Automation repeat interval cannot be negative."};
        }
        if (automation.schedule_kind == AutomationScheduleKind::fixed_interval &&
            automation.repeat_interval <= std::chrono::milliseconds::zero()) {
            throw std::runtime_error{"Fixed-interval automation requires a positive interval."};
        }
        if (automation.schedule_kind != AutomationScheduleKind::fixed_interval &&
            automation.repeat_interval != std::chrono::milliseconds::zero()) {
            throw std::runtime_error{
                "Only fixed-interval automations may persist a repeat interval."};
        }
        if (automation.schedule_kind == AutomationScheduleKind::local_calendar &&
            !valid_calendar_schedule(automation.calendar_schedule)) {
            throw std::runtime_error{"Automation calendar schedule is invalid."};
        }
        if (automation.schedule_kind != AutomationScheduleKind::one_shot &&
            automation.schedule_kind != AutomationScheduleKind::fixed_interval &&
            automation.schedule_kind != AutomationScheduleKind::local_calendar) {
            throw std::runtime_error{"Automation schedule kind is invalid."};
        }
        if (!valid_failure_policy(automation.failure_policy)) {
            throw std::runtime_error{"Automation failure policy is invalid."};
        }
        if (automation.retry_attempt > automation.failure_policy.max_retries) {
            throw std::runtime_error{"Automation retry attempt exceeds configured retry limit."};
        }
        if (automation.retry_resume_at &&
            (!automation.recurring() || !automation.retry_pending())) {
            throw std::runtime_error{"Retry resume time requires an active recurring retry."};
        }
        if (automation.retry_pending() && automation.recurring() && !automation.retry_resume_at) {
            throw std::runtime_error{"Recurring retry state requires a retry resume time."};
        }
        const auto due_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                automation.due_at.time_since_epoch())
                                .count();
        const auto repeat_ms = automation.repeat_interval.count();
        const auto last_ms = automation.last_run_at
                                 ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                       automation.last_run_at->time_since_epoch())
                                       .count()
                                 : no_last_run;
        std::uint32_t flags = 0;
        if (automation.enabled)
            flags |= 0x1u;
        if (automation.last_success)
            flags |= 0x2u;
        append_le(payload, automation.id);
        append_le(payload, static_cast<std::int64_t>(due_ms));
        append_le(payload, static_cast<std::int64_t>(repeat_ms));
        append_le(payload, automation.run_count);
        append_le(payload, automation.failure_count);
        append_le(payload, static_cast<std::int64_t>(last_ms));
        append_le(payload, flags);
        append_text(payload, automation.name, max_name_bytes, "name");
        append_text(payload, automation.action_name, max_name_bytes, "action name");
        append_text(payload, automation.action_payload, max_payload_bytes, "action payload");
        append_text(payload, automation.last_error, max_error_bytes, "last error");
        append_le(payload, static_cast<std::uint32_t>(automation.schedule_kind));
        append_le(payload, static_cast<std::uint32_t>(automation.calendar_schedule.weekday_mask));
        append_le(payload, static_cast<std::uint32_t>(automation.calendar_schedule.hour));
        append_le(payload, static_cast<std::uint32_t>(automation.calendar_schedule.minute));
        append_le(payload, static_cast<std::uint32_t>(automation.calendar_schedule.second));
        append_text(payload, automation.calendar_schedule.time_zone, 128u, "calendar time zone");
        append_le(payload, static_cast<std::uint32_t>(automation.failure_policy.kind));
        append_le(payload, automation.failure_policy.max_retries);
        append_le(payload,
                  static_cast<std::int64_t>(automation.failure_policy.initial_delay.count()));
        append_le(payload, static_cast<std::int64_t>(automation.failure_policy.max_delay.count()));
        append_le(payload, static_cast<std::uint32_t>(
                               automation.failure_policy.disable_on_exhaustion ? 1u : 0u));
        append_le(payload, automation.retry_attempt);
        append_le(payload, automation.consecutive_failures);
        const auto retry_resume_ms = automation.retry_resume_at
                                         ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                               automation.retry_resume_at->time_since_epoch())
                                               .count()
                                         : no_last_run;
        append_le(payload, static_cast<std::int64_t>(retry_resume_ms));
    }
    return payload;
}
[[nodiscard]] std::vector<std::byte> serialize_file(std::uint64_t generation, AutomationId next_id,
                                                    std::span<const Automation> automations) {
    if (automations.size() > max_records) {
        throw std::runtime_error{"Automation store exceeds record limit."};
    }
    auto payload = serialize_payload(automations);
    if (header_bytes + payload.size() > max_store_bytes) {
        throw std::runtime_error{"Automation store exceeds 64 MiB limit."};
    }
    std::vector<std::byte> output;
    output.reserve(header_bytes + payload.size());
    output.insert(output.end(), magic.begin(), magic.end());
    append_le(output, format_version);
    append_le(output, generation);
    append_le(output, next_id);
    append_le(output, static_cast<std::uint64_t>(automations.size()));
    append_le(output, static_cast<std::uint64_t>(payload.size()));
    append_le(output, crc32(payload));
    append_le(output, std::uint32_t{0});
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}
[[nodiscard]] Candidate parse_candidate(const std::filesystem::path &path) {
    std::ifstream stream{path, std::ios::binary | std::ios::ate};
    if (!stream) {
        throw std::system_error(errno, std::generic_category(), "Unable to open automation store");
    }
    const auto end = stream.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > max_store_bytes) {
        throw std::runtime_error{"Automation store has an invalid file size."};
    }
    const auto size = static_cast<std::size_t>(end);
    if (size < header_bytes) {
        throw std::runtime_error{"Automation store is too small."};
    }
    std::vector<std::byte> bytes(size);
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        throw std::runtime_error{"Unable to read complete automation store."};
    }
    if (!std::equal(magic.begin(), magic.end(), bytes.begin())) {
        throw std::runtime_error{"Automation store magic mismatch."};
    }
    std::size_t offset = magic.size();
    const auto version = read_le<std::uint32_t>(bytes, offset);
    if (version != legacy_format_version && version != format_version) {
        throw std::runtime_error{"Unsupported automation store version."};
    }
    AutomationStoreSnapshot snapshot;
    snapshot.generation = read_le<std::uint64_t>(bytes, offset);
    snapshot.next_id = read_le<AutomationId>(bytes, offset);
    const auto record_count = read_le<std::uint64_t>(bytes, offset);
    const auto payload_size = read_le<std::uint64_t>(bytes, offset);
    const auto expected_crc = read_le<std::uint32_t>(bytes, offset);
    (void)read_le<std::uint32_t>(bytes, offset);
    if (record_count > max_records || payload_size > max_store_bytes ||
        offset + payload_size != bytes.size()) {
        throw std::runtime_error{"Automation store header bounds are invalid."};
    }
    const auto payload =
        std::span<const std::byte>{bytes}.subspan(offset, static_cast<std::size_t>(payload_size));
    if (crc32(payload) != expected_crc) {
        throw std::runtime_error{"Automation store CRC mismatch."};
    }
    snapshot.automations.reserve(static_cast<std::size_t>(record_count));
    std::size_t payload_offset = 0;
    AutomationId max_id = 0;
    for (std::uint64_t index = 0; index < record_count; ++index) {
        Automation automation;
        automation.id = read_le<AutomationId>(payload, payload_offset);
        const auto due_ms = read_le<std::int64_t>(payload, payload_offset);
        const auto repeat_ms = read_le<std::int64_t>(payload, payload_offset);
        automation.run_count = read_le<std::uint64_t>(payload, payload_offset);
        automation.failure_count = read_le<std::uint64_t>(payload, payload_offset);
        const auto last_ms = read_le<std::int64_t>(payload, payload_offset);
        const auto flags = read_le<std::uint32_t>(payload, payload_offset);
        automation.name = read_text(payload, payload_offset, max_name_bytes, "name");
        automation.action_name = read_text(payload, payload_offset, max_name_bytes, "action name");
        automation.action_payload =
            read_text(payload, payload_offset, max_payload_bytes, "action payload");
        automation.last_error = read_text(payload, payload_offset, max_error_bytes, "last error");
        if (automation.id == 0 || repeat_ms < 0 || automation.name.empty() ||
            automation.action_name.empty()) {
            throw std::runtime_error{"Automation store contains invalid record fields."};
        }
        automation.enabled = (flags & 0x1u) != 0;
        automation.last_success = (flags & 0x2u) != 0;
        automation.due_at =
            std::chrono::system_clock::time_point{std::chrono::milliseconds{due_ms}};
        automation.repeat_interval = std::chrono::milliseconds{repeat_ms};
        if (last_ms != no_last_run) {
            automation.last_run_at =
                std::chrono::system_clock::time_point{std::chrono::milliseconds{last_ms}};
        }
        if (version == legacy_format_version) {
            automation.schedule_kind = repeat_ms > 0 ? AutomationScheduleKind::fixed_interval
                                                     : AutomationScheduleKind::one_shot;
        } else {
            const auto schedule_kind = read_le<std::uint32_t>(payload, payload_offset);
            if (schedule_kind >
                static_cast<std::uint32_t>(AutomationScheduleKind::local_calendar)) {
                throw std::runtime_error{"Automation store contains an unknown schedule kind."};
            }
            automation.schedule_kind = static_cast<AutomationScheduleKind>(schedule_kind);
            const auto weekday_mask = read_le<std::uint32_t>(payload, payload_offset);
            const auto calendar_hour = read_le<std::uint32_t>(payload, payload_offset);
            const auto calendar_minute = read_le<std::uint32_t>(payload, payload_offset);
            const auto calendar_second = read_le<std::uint32_t>(payload, payload_offset);
            if (weekday_mask > 0x7Fu || calendar_hour > 23u || calendar_minute > 59u ||
                calendar_second > 59u) {
                throw std::runtime_error{"Automation store contains out-of-range calendar fields."};
            }
            automation.calendar_schedule.weekday_mask = static_cast<std::uint8_t>(weekday_mask);
            automation.calendar_schedule.hour = static_cast<std::uint8_t>(calendar_hour);
            automation.calendar_schedule.minute = static_cast<std::uint8_t>(calendar_minute);
            automation.calendar_schedule.second = static_cast<std::uint8_t>(calendar_second);
            automation.calendar_schedule.time_zone =
                read_text(payload, payload_offset, 128u, "calendar time zone");
            const auto retry_kind = read_le<std::uint32_t>(payload, payload_offset);
            if (retry_kind > static_cast<std::uint32_t>(AutomationRetryKind::exponential)) {
                throw std::runtime_error{"Automation store contains an unknown retry kind."};
            }
            automation.failure_policy.kind = static_cast<AutomationRetryKind>(retry_kind);
            automation.failure_policy.max_retries = read_le<std::uint32_t>(payload, payload_offset);
            automation.failure_policy.initial_delay =
                std::chrono::milliseconds{read_le<std::int64_t>(payload, payload_offset)};
            automation.failure_policy.max_delay =
                std::chrono::milliseconds{read_le<std::int64_t>(payload, payload_offset)};
            automation.failure_policy.disable_on_exhaustion =
                read_le<std::uint32_t>(payload, payload_offset) != 0;
            automation.retry_attempt = read_le<std::uint32_t>(payload, payload_offset);
            automation.consecutive_failures = read_le<std::uint32_t>(payload, payload_offset);
            const auto retry_resume_ms = read_le<std::int64_t>(payload, payload_offset);
            if (retry_resume_ms != no_last_run) {
                automation.retry_resume_at = std::chrono::system_clock::time_point{
                    std::chrono::milliseconds{retry_resume_ms}};
            }
            if (automation.schedule_kind == AutomationScheduleKind::fixed_interval) {
                if (automation.repeat_interval <= std::chrono::milliseconds::zero()) {
                    throw std::runtime_error{
                        "Automation store fixed schedule has an invalid interval."};
                }
            } else if (automation.repeat_interval != std::chrono::milliseconds::zero()) {
                throw std::runtime_error{
                    "Automation store non-fixed schedule contains a repeat interval."};
            }
            if (automation.schedule_kind == AutomationScheduleKind::local_calendar &&
                !valid_calendar_schedule(automation.calendar_schedule)) {
                throw std::runtime_error{"Automation store contains an invalid calendar schedule."};
            }
            if (!valid_failure_policy(automation.failure_policy) ||
                automation.retry_attempt > automation.failure_policy.max_retries ||
                (automation.retry_resume_at &&
                 (!automation.recurring() || !automation.retry_pending())) ||
                (automation.retry_pending() && automation.recurring() &&
                 !automation.retry_resume_at)) {
                throw std::runtime_error{"Automation store contains invalid failure/retry state."};
            }
        }
        max_id = std::max(max_id, automation.id);
        snapshot.automations.push_back(std::move(automation));
    }
    if (payload_offset != payload.size()) {
        throw std::runtime_error{"Automation store contains trailing payload bytes."};
    }
    std::sort(snapshot.automations.begin(), snapshot.automations.end(),
              [](const Automation &lhs, const Automation &rhs) { return lhs.id < rhs.id; });
    for (std::size_t index = 1; index < snapshot.automations.size(); ++index) {
        if (snapshot.automations[index - 1].id == snapshot.automations[index].id) {
            throw std::runtime_error{"Automation store contains duplicate IDs."};
        }
    }
    if (snapshot.next_id == 0 || snapshot.next_id <= max_id) {
        if (max_id == std::numeric_limits<AutomationId>::max()) {
            throw std::runtime_error{"Automation store exhausted automation IDs."};
        }
        snapshot.next_id = max_id + 1;
    }
    return Candidate{std::move(snapshot), path};
}
void durable_flush(const std::filesystem::path &path) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Open automation store for flush failed");
    }
    const BOOL flushed = FlushFileBuffers(file);
    const DWORD error = flushed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!flushed) {
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "FlushFileBuffers automation store failed");
    }
#else
    const int file = ::open(path.c_str(), O_RDONLY);
    if (file < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "Open automation store for fsync failed");
    }
    const int result = ::fsync(file);
    const int error = errno;
    ::close(file);
    if (result != 0) {
        throw std::system_error(error, std::generic_category(), "fsync automation store failed");
    }
#endif
}
void write_file(const std::filesystem::path &path, std::span<const std::byte> bytes) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        throw std::runtime_error{"Unable to create automation store temp file."};
    }
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) {
        throw std::runtime_error{"Unable to write automation store temp file."};
    }
    stream.close();
    durable_flush(path);
}
} // namespace
AutomationStore::AutomationStore(std::filesystem::path path) : path_{std::move(path)} {
    if (path_.empty()) {
        throw std::invalid_argument{"Automation store path must not be empty."};
    }
}
AutomationStoreSnapshot AutomationStore::load() {
    std::scoped_lock lock{mutex_};
    const auto temp = temp_path(path_);
    const auto backup = backup_path(path_);
    const std::array candidates{path_, temp, backup};
    std::optional<Candidate> best;
    bool any_candidate_exists = false;
    std::string last_error;
    for (const auto &candidate_path : candidates) {
        std::error_code error;
        if (!std::filesystem::exists(candidate_path, error) || error) {
            continue;
        }
        any_candidate_exists = true;
        try {
            auto candidate = parse_candidate(candidate_path);
            if (!best || candidate.snapshot.generation > best->snapshot.generation) {
                best = std::move(candidate);
            }
        } catch (const std::exception &exception) {
            last_error = exception.what();
        }
    }
    if (!best) {
        if (any_candidate_exists) {
            throw std::runtime_error{"No valid automation store snapshot found; last error: " +
                                     last_error};
        }
        generation_ = 0;
        record_count_ = 0;
        recovered_from_alternate_ = false;
        return {};
    }
    generation_ = best->snapshot.generation;
    record_count_ = best->snapshot.automations.size();
    recovered_from_alternate_ = best->path != path_;
    return best->snapshot;
}
void AutomationStore::save(AutomationId next_id, std::span<const Automation> automations) {
    std::scoped_lock lock{mutex_};
    if (next_id == 0) {
        throw std::invalid_argument{"Automation next ID must not be zero."};
    }
    // A fresh store object may be asked to save before load(). Discover the
    // highest durable generation first so a new snapshot never regresses and
    // loses to an older .bak/.tmp candidate during recovery.
    if (generation_ == 0) {
        const auto temp = temp_path(path_);
        const auto backup = backup_path(path_);
        const std::array candidates{path_, temp, backup};
        for (const auto &candidate_path : candidates) {
            std::error_code candidate_error;
            if (!std::filesystem::exists(candidate_path, candidate_error) || candidate_error) {
                continue;
            }
            try {
                const auto candidate = parse_candidate(candidate_path);
                generation_ = std::max(generation_, candidate.snapshot.generation);
            } catch (...) {
                // save() still replaces corrupt candidates through the normal
                // temp/backup commit path; only valid generations participate.
            }
        }
    }
    const auto next_generation = generation_ + 1;
    const auto bytes = serialize_file(next_generation, next_id, automations);
    std::error_code error;
    const auto parent = path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            throw std::system_error(error, "Unable to create automation store directory");
        }
    }
    const auto temp = temp_path(path_);
    const auto backup = backup_path(path_);
    std::filesystem::remove(temp, error);
    error.clear();
    write_file(temp, bytes);
    std::filesystem::remove(backup, error);
    error.clear();
    if (std::filesystem::exists(path_, error) && !error) {
        std::filesystem::rename(path_, backup, error);
        if (error) {
            std::filesystem::remove(temp);
            throw std::system_error(error, "Unable to rotate automation store backup");
        }
    }
    error.clear();
    std::filesystem::rename(temp, path_, error);
    if (error) {
        std::error_code restore_error;
        if (!std::filesystem::exists(path_, restore_error) &&
            std::filesystem::exists(backup, restore_error)) {
            std::filesystem::rename(backup, path_, restore_error);
        }
        std::filesystem::remove(temp);
        throw std::system_error(error, "Unable to install automation store snapshot");
    }
    generation_ = next_generation;
    record_count_ = automations.size();
    recovered_from_alternate_ = false;
}
AutomationStoreStatus AutomationStore::status() const {
    std::scoped_lock lock{mutex_};
    return AutomationStoreStatus{path_, generation_, record_count_, recovered_from_alternate_};
}
} // namespace axiom
