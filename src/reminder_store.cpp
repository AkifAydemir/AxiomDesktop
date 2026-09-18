#include "reminder_store.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
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
constexpr std::array<std::byte, 4> magic{std::byte{'A'}, std::byte{'X'}, std::byte{'R'},
                                         std::byte{'M'}};
constexpr std::uint32_t format_version = 1;
constexpr std::size_t max_store_bytes = 64u * 1024u * 1024u;
constexpr std::size_t max_records = 100000;
constexpr std::size_t max_message_bytes = 1024u * 1024u;
constexpr std::size_t header_bytes = 48;
struct Candidate final {
    ReminderStoreSnapshot snapshot;
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
        throw std::runtime_error("Reminder store truncated integral field");
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
        throw std::runtime_error("Reminder text contains an invalid Unicode codepoint");
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
                    throw std::runtime_error(
                        "Reminder text ends with an unpaired UTF-16 surrogate");
                }
                const auto low = static_cast<std::uint32_t>(input[++index]);
                if (low < 0xDC00u || low > 0xDFFFu) {
                    throw std::runtime_error(
                        "Reminder text contains an invalid UTF-16 surrogate pair");
                }
                codepoint = 0x10000u + ((codepoint - 0xD800u) << 10u) + (low - 0xDC00u);
            } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
                throw std::runtime_error("Reminder text contains an unpaired UTF-16 surrogate");
            }
        } else {
            if (codepoint >= 0xD800u && codepoint <= 0xDFFFu) {
                throw std::runtime_error("Reminder text contains a surrogate codepoint");
            }
        }
        append_utf8_codepoint(output, codepoint);
    }
    return output;
}
[[nodiscard]] std::uint32_t decode_utf8_codepoint(std::string_view input, std::size_t &offset) {
    if (offset >= input.size()) {
        throw std::runtime_error("Reminder store contains truncated UTF-8");
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
        throw std::runtime_error("Reminder store contains invalid UTF-8 prefix");
    }
    for (int index = 0; index < continuation_count; ++index) {
        if (offset >= input.size()) {
            throw std::runtime_error("Reminder store contains truncated UTF-8 sequence");
        }
        const auto byte = static_cast<unsigned char>(input[offset++]);
        if ((byte & 0xC0u) != 0x80u) {
            throw std::runtime_error("Reminder store contains invalid UTF-8 continuation byte");
        }
        codepoint = (codepoint << 6u) | (byte & 0x3Fu);
    }
    if (codepoint < minimum || codepoint > 0x10FFFFu ||
        (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
        throw std::runtime_error("Reminder store contains non-canonical UTF-8");
    }
    return codepoint;
}
[[nodiscard]] std::wstring utf8_to_wide(std::string_view input) {
    std::wstring output;
    std::size_t offset = 0;
    while (offset < input.size()) {
        const auto codepoint = decode_utf8_codepoint(input, offset);
        if constexpr (sizeof(wchar_t) == 2) {
            if (codepoint <= 0xFFFFu) {
                output.push_back(static_cast<wchar_t>(codepoint));
            } else {
                const auto adjusted = codepoint - 0x10000u;
                output.push_back(static_cast<wchar_t>(0xD800u + (adjusted >> 10u)));
                output.push_back(static_cast<wchar_t>(0xDC00u + (adjusted & 0x3FFu)));
            }
        } else {
            output.push_back(static_cast<wchar_t>(codepoint));
        }
    }
    return output;
}
[[nodiscard]] std::vector<std::byte> serialize_payload(std::span<const Reminder> reminders) {
    std::vector<std::byte> payload;
    for (const auto &reminder : reminders) {
        const auto message = wide_to_utf8(reminder.message);
        if (message.empty() || message.size() > max_message_bytes) {
            throw std::runtime_error("Reminder message cannot be empty or exceed 1 MiB");
        }
        const auto due_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                reminder.due_at.time_since_epoch())
                                .count();
        const auto repeat_ms = reminder.repeat_interval.count();
        if (repeat_ms < 0) {
            throw std::runtime_error("Reminder repeat interval cannot be negative");
        }
        append_le(payload, reminder.id);
        append_le(payload, static_cast<std::int64_t>(due_ms));
        append_le(payload, static_cast<std::int64_t>(repeat_ms));
        append_le(payload, reminder.fire_count);
        append_le(payload, static_cast<std::uint32_t>(message.size()));
        payload.insert(payload.end(), reinterpret_cast<const std::byte *>(message.data()),
                       reinterpret_cast<const std::byte *>(message.data() + message.size()));
    }
    return payload;
}
[[nodiscard]] std::vector<std::byte> serialize_file(std::uint64_t generation, ReminderId next_id,
                                                    std::span<const Reminder> reminders) {
    if (reminders.size() > max_records) {
        throw std::runtime_error("Reminder store exceeds record limit");
    }
    auto payload = serialize_payload(reminders);
    if (header_bytes + payload.size() > max_store_bytes) {
        throw std::runtime_error("Reminder store exceeds 64 MiB limit");
    }
    std::vector<std::byte> output;
    output.reserve(header_bytes + payload.size());
    output.insert(output.end(), magic.begin(), magic.end());
    append_le(output, format_version);
    append_le(output, generation);
    append_le(output, next_id);
    append_le(output, static_cast<std::uint64_t>(reminders.size()));
    append_le(output, static_cast<std::uint64_t>(payload.size()));
    append_le(output, crc32(payload));
    append_le(output, std::uint32_t{0});
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}
[[nodiscard]] Candidate parse_candidate(const std::filesystem::path &path) {
    std::ifstream stream{path, std::ios::binary | std::ios::ate};
    if (!stream) {
        throw std::system_error(errno, std::generic_category(), "Unable to open reminder store");
    }
    const auto end = stream.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > max_store_bytes) {
        throw std::runtime_error("Reminder store has an invalid file size");
    }
    const auto size = static_cast<std::size_t>(end);
    if (size < header_bytes) {
        throw std::runtime_error("Reminder store is too small");
    }
    std::vector<std::byte> bytes(size);
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        throw std::runtime_error("Unable to read complete reminder store");
    }
    if (!std::equal(magic.begin(), magic.end(), bytes.begin())) {
        throw std::runtime_error("Reminder store magic mismatch");
    }
    std::size_t offset = magic.size();
    const auto version = read_le<std::uint32_t>(bytes, offset);
    if (version != format_version) {
        throw std::runtime_error("Unsupported reminder store version");
    }
    ReminderStoreSnapshot snapshot;
    snapshot.generation = read_le<std::uint64_t>(bytes, offset);
    snapshot.next_id = read_le<ReminderId>(bytes, offset);
    const auto record_count = read_le<std::uint64_t>(bytes, offset);
    const auto payload_size = read_le<std::uint64_t>(bytes, offset);
    const auto expected_crc = read_le<std::uint32_t>(bytes, offset);
    (void)read_le<std::uint32_t>(bytes, offset);
    if (record_count > max_records || payload_size > max_store_bytes ||
        offset + payload_size != bytes.size()) {
        throw std::runtime_error("Reminder store header bounds are invalid");
    }
    const auto payload =
        std::span<const std::byte>{bytes}.subspan(offset, static_cast<std::size_t>(payload_size));
    if (crc32(payload) != expected_crc) {
        throw std::runtime_error("Reminder store CRC mismatch");
    }
    snapshot.reminders.reserve(static_cast<std::size_t>(record_count));
    std::size_t payload_offset = 0;
    ReminderId max_id = 0;
    for (std::uint64_t index = 0; index < record_count; ++index) {
        Reminder reminder;
        reminder.id = read_le<ReminderId>(payload, payload_offset);
        const auto due_ms = read_le<std::int64_t>(payload, payload_offset);
        const auto repeat_ms = read_le<std::int64_t>(payload, payload_offset);
        reminder.fire_count = read_le<std::uint64_t>(payload, payload_offset);
        const auto message_size = read_le<std::uint32_t>(payload, payload_offset);
        if (reminder.id == 0 || repeat_ms < 0 || message_size == 0 ||
            message_size > max_message_bytes) {
            throw std::runtime_error("Reminder store contains invalid record fields");
        }
        if (payload_offset > payload.size() || payload.size() - payload_offset < message_size) {
            throw std::runtime_error("Reminder store contains truncated reminder text");
        }
        const auto *message_data = reinterpret_cast<const char *>(payload.data() + payload_offset);
        reminder.message = utf8_to_wide(std::string_view{message_data, message_size});
        payload_offset += message_size;
        reminder.due_at = std::chrono::system_clock::time_point{std::chrono::milliseconds{due_ms}};
        reminder.repeat_interval = std::chrono::milliseconds{repeat_ms};
        max_id = std::max(max_id, reminder.id);
        snapshot.reminders.push_back(std::move(reminder));
    }
    if (payload_offset != payload.size()) {
        throw std::runtime_error("Reminder store contains trailing payload bytes");
    }
    std::sort(snapshot.reminders.begin(), snapshot.reminders.end(),
              [](const Reminder &lhs, const Reminder &rhs) { return lhs.id < rhs.id; });
    for (std::size_t index = 1; index < snapshot.reminders.size(); ++index) {
        if (snapshot.reminders[index - 1].id == snapshot.reminders[index].id) {
            throw std::runtime_error("Reminder store contains duplicate IDs");
        }
    }
    if (snapshot.next_id == 0 || snapshot.next_id <= max_id) {
        if (max_id == std::numeric_limits<ReminderId>::max()) {
            throw std::runtime_error("Reminder store exhausted reminder IDs");
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
                                "Open reminder store for flush failed");
    }
    const BOOL flushed = FlushFileBuffers(file);
    const DWORD error = flushed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!flushed) {
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "FlushFileBuffers failed");
    }
#else
    const int file = ::open(path.c_str(), O_RDONLY);
    if (file < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "Open reminder store for fsync failed");
    }
    const int result = ::fsync(file);
    const int error = errno;
    ::close(file);
    if (result != 0) {
        throw std::system_error(error, std::generic_category(), "fsync reminder store failed");
    }
#endif
}
void write_file(const std::filesystem::path &path, std::span<const std::byte> bytes) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        throw std::runtime_error("Unable to create reminder store temp file");
    }
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) {
        throw std::runtime_error("Unable to write reminder store temp file");
    }
    stream.close();
    durable_flush(path);
}
} // namespace
ReminderStore::ReminderStore(std::filesystem::path path) : path_{std::move(path)} {
    if (path_.empty()) {
        throw std::invalid_argument("Reminder store path must not be empty");
    }
}
ReminderStoreSnapshot ReminderStore::load() {
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
            throw std::runtime_error("No valid reminder store snapshot found; last error: " +
                                     last_error);
        }
        generation_ = 0;
        record_count_ = 0;
        recovered_from_alternate_ = false;
        return {};
    }
    generation_ = best->snapshot.generation;
    record_count_ = best->snapshot.reminders.size();
    recovered_from_alternate_ = best->path != path_;
    return best->snapshot;
}
void ReminderStore::save(ReminderId next_id, std::span<const Reminder> reminders) {
    std::scoped_lock lock{mutex_};
    if (next_id == 0) {
        throw std::invalid_argument("Reminder next ID must not be zero");
    }
    const auto next_generation = generation_ + 1;
    const auto bytes = serialize_file(next_generation, next_id, reminders);
    std::error_code error;
    const auto parent = path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            throw std::system_error(error, "Unable to create reminder store directory");
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
            throw std::system_error(error, "Unable to rotate reminder store backup");
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
        throw std::system_error(error, "Unable to publish reminder store snapshot");
    }
    std::filesystem::remove(backup, error);
    generation_ = next_generation;
    record_count_ = reminders.size();
    recovered_from_alternate_ = false;
}
ReminderStoreStatus ReminderStore::status() const {
    std::scoped_lock lock{mutex_};
    return ReminderStoreStatus{path_, generation_, record_count_, recovered_from_alternate_};
}
} // namespace axiom
