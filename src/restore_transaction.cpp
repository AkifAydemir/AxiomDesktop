#include "restore_transaction.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace axiom {
namespace {

constexpr std::string_view journal_magic = "AXRT1";
constexpr std::size_t max_transaction_files = 64;
constexpr std::size_t max_journal_bytes = 1024u * 1024u;
std::atomic<std::uint64_t> next_token{1};

struct ItemState {
    std::string name;
    std::filesystem::path destination;
    std::filesystem::path temporary;
    std::filesystem::path backup;
    bool prepared{};
    bool had_original{};
    bool backup_moved{};
    bool published{};
};

struct JournalState {
    std::string token;
    bool committed{};
    std::vector<ItemState> items;
};

[[nodiscard]] std::string make_token() {
    const auto ticks =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto sequence = next_token.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream stream;
    stream << std::hex << ticks << '-' << sequence;
    return stream.str();
}

[[nodiscard]] bool valid_token(std::string_view token) noexcept {
    const auto separator = token.find('-');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= token.size() ||
        token.find('-', separator + 1) != std::string_view::npos || token.size() > 64) {
        return false;
    }
    const auto is_hex = [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    };
    return std::all_of(token.begin(), token.begin() + static_cast<std::ptrdiff_t>(separator),
                       is_hex) &&
           std::all_of(token.begin() + static_cast<std::ptrdiff_t>(separator + 1), token.end(),
                       is_hex);
}

[[nodiscard]] std::filesystem::path normalized_absolute(const std::filesystem::path &path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error).lexically_normal();
    if (error) {
        throw std::system_error{error, "Unable to normalize restore transaction path"};
    }
    return absolute;
}

[[nodiscard]] char hex_digit(unsigned value) noexcept {
    return static_cast<char>(value < 10 ? '0' + value : 'a' + (value - 10));
}

[[nodiscard]] std::string hex_encode(std::string_view input) {
    std::string output;
    output.reserve(input.size() * 2);
    for (const unsigned char ch : input) {
        output.push_back(hex_digit((ch >> 4) & 0x0Fu));
        output.push_back(hex_digit(ch & 0x0Fu));
    }
    return output;
}

[[nodiscard]] unsigned hex_value(char ch) {
    if (ch >= '0' && ch <= '9')
        return static_cast<unsigned>(ch - '0');
    if (ch >= 'a' && ch <= 'f')
        return static_cast<unsigned>(ch - 'a' + 10);
    if (ch >= 'A' && ch <= 'F')
        return static_cast<unsigned>(ch - 'A' + 10);
    throw std::runtime_error{"Restore recovery journal contains invalid hex data."};
}

[[nodiscard]] std::string hex_decode(std::string_view input) {
    if ((input.size() % 2) != 0) {
        throw std::runtime_error{"Restore recovery journal contains truncated hex data."};
    }
    std::string output;
    output.reserve(input.size() / 2);
    for (std::size_t index = 0; index < input.size(); index += 2) {
        const auto value = (hex_value(input[index]) << 4) | hex_value(input[index + 1]);
        output.push_back(static_cast<char>(value));
    }
    return output;
}

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path &path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view value) {
    std::u8string utf8;
    utf8.reserve(value.size());
    for (const char ch : value) {
        utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(ch)));
    }
    return std::filesystem::path{utf8};
}

[[nodiscard]] std::filesystem::path sibling_artifact(const std::filesystem::path &destination,
                                                     std::string_view token,
                                                     std::string_view suffix) {
    auto name = destination.filename().wstring();
    std::wstring extra = L".axiom-restore.";
    extra.append(token.begin(), token.end());
    extra.push_back(L'.');
    extra.append(suffix.begin(), suffix.end());
    return destination.parent_path() / (name + extra);
}

void remove_expected_file_if_exists(const std::filesystem::path &path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory)
        return;
    if (error) {
        throw std::system_error{error, "Unable to inspect restore transaction artifact"};
    }
    if (status.type() == std::filesystem::file_type::not_found)
        return;
    if (status.type() != std::filesystem::file_type::regular) {
        throw std::runtime_error{"Restore transaction artifact is not a regular file: " +
                                 path.string()};
    }
    if (!std::filesystem::remove(path, error) || error) {
        if (error)
            throw std::system_error{error, "Unable to remove restore transaction artifact"};
        throw std::runtime_error{"Unable to remove restore transaction artifact: " + path.string()};
    }
}

[[nodiscard]] bool expected_regular_file_exists(const std::filesystem::path &path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        status.type() == std::filesystem::file_type::not_found) {
        return false;
    }
    if (error) {
        throw std::system_error{error, "Unable to inspect restore transaction file"};
    }
    if (status.type() != std::filesystem::file_type::regular) {
        throw std::runtime_error{"Restore transaction path is not a regular file: " +
                                 path.string()};
    }
    return true;
}

void prepare_recovery_slot(const std::filesystem::path &recovery_journal_path) {
    std::error_code error;
    if (std::filesystem::exists(recovery_journal_path, error)) {
        if (error)
            throw std::system_error{error, "Unable to inspect restore recovery journal"};
        throw std::runtime_error{"A restore recovery journal already exists. Recover it before "
                                 "starting another restore."};
    }
    if (error)
        throw std::system_error{error, "Unable to inspect restore recovery journal"};

    auto temporary = recovery_journal_path;
    temporary += L".tmp";
    remove_expected_file_if_exists(temporary);
}

void durable_flush_file(const std::filesystem::path &path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Unable to open restore transaction file for durable flush");
    }
    const BOOL flushed = FlushFileBuffers(handle);
    const DWORD error = flushed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!flushed) {
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "Unable to durably flush restore transaction file");
    }
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        throw std::system_error{errno, std::generic_category(),
                                "Unable to open restore transaction file for durable flush"};
    }
    const int result = ::fsync(descriptor);
    const int error = errno;
    ::close(descriptor);
    if (result != 0) {
        throw std::system_error{error, std::generic_category(),
                                "Unable to durably flush restore transaction file"};
    }
#endif
}

void atomic_replace_file(const std::filesystem::path &source,
                         const std::filesystem::path &destination) {
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Unable to atomically publish restore recovery journal");
    }
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (error) {
        throw std::system_error{error, "Unable to atomically publish restore recovery journal"};
    }
#endif
}

void write_journal(const std::filesystem::path &path, const JournalState &state) {
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            throw std::system_error{error, "Unable to create restore recovery journal directory"};
        }
    }

    auto temporary = path;
    temporary += L".tmp";

    std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
    if (!stream) {
        throw std::runtime_error{"Unable to create restore recovery journal."};
    }

    stream << journal_magic << '\n';
    stream << "phase=" << (state.committed ? "committed" : "committing") << '\n';
    stream << "token=" << state.token << '\n';
    stream << "count=" << state.items.size() << '\n';
    for (const auto &item : state.items) {
        stream << "item=" << hex_encode(item.name) << '|'
               << hex_encode(path_to_utf8(item.destination)) << '|'
               << hex_encode(path_to_utf8(item.temporary)) << '|'
               << hex_encode(path_to_utf8(item.backup)) << '|' << (item.prepared ? '1' : '0') << '|'
               << (item.had_original ? '1' : '0') << '|' << (item.backup_moved ? '1' : '0') << '|'
               << (item.published ? '1' : '0') << '\n';
    }
    stream.flush();
    if (!stream) {
        throw std::runtime_error{"Unable to flush restore recovery journal."};
    }
    stream.close();
    durable_flush_file(temporary);
    atomic_replace_file(temporary, path);
}

[[nodiscard]] std::vector<std::string> split(std::string_view value, char delimiter) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(delimiter, start);
        if (end == std::string_view::npos) {
            parts.emplace_back(value.substr(start));
            break;
        }
        parts.emplace_back(value.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

[[nodiscard]] bool parse_flag(std::string_view value) {
    if (value == "1")
        return true;
    if (value == "0")
        return false;
    throw std::runtime_error{"Restore recovery journal contains invalid flag."};
}

[[nodiscard]] JournalState read_journal(const std::filesystem::path &path) {
    std::ifstream stream{path, std::ios::binary | std::ios::ate};
    if (!stream) {
        throw std::runtime_error{"Unable to open restore recovery journal."};
    }
    const auto end = stream.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > max_journal_bytes) {
        throw std::runtime_error{"Restore recovery journal exceeds size limit."};
    }
    stream.seekg(0);

    std::string line;
    if (!std::getline(stream, line) || line != journal_magic) {
        throw std::runtime_error{"Invalid restore recovery journal magic."};
    }

    JournalState state;
    std::size_t expected_count = std::numeric_limits<std::size_t>::max();
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.starts_with("phase=")) {
            const auto phase = std::string_view{line}.substr(6);
            if (phase == "committed")
                state.committed = true;
            else if (phase == "committing")
                state.committed = false;
            else
                throw std::runtime_error{"Restore recovery journal has invalid phase."};
        } else if (line.starts_with("token=")) {
            state.token = line.substr(6);
        } else if (line.starts_with("count=")) {
            try {
                std::size_t consumed{};
                const auto parsed = std::stoull(line.substr(6), &consumed, 10);
                if (consumed != line.size() - 6 || parsed > max_transaction_files) {
                    throw std::runtime_error{"bad count"};
                }
                expected_count = static_cast<std::size_t>(parsed);
            } catch (...) {
                throw std::runtime_error{"Restore recovery journal has invalid item count."};
            }
        } else if (line.starts_with("item=")) {
            const auto fields = split(std::string_view{line}.substr(5), '|');
            if (fields.size() != 8) {
                throw std::runtime_error{"Restore recovery journal item is malformed."};
            }
            ItemState item;
            item.name = hex_decode(fields[0]);
            item.destination = path_from_utf8(hex_decode(fields[1]));
            item.temporary = path_from_utf8(hex_decode(fields[2]));
            item.backup = path_from_utf8(hex_decode(fields[3]));
            item.prepared = parse_flag(fields[4]);
            item.had_original = parse_flag(fields[5]);
            item.backup_moved = parse_flag(fields[6]);
            item.published = parse_flag(fields[7]);
            state.items.push_back(std::move(item));
        }
    }

    if (state.token.empty() || expected_count == std::numeric_limits<std::size_t>::max() ||
        state.items.size() != expected_count) {
        throw std::runtime_error{"Restore recovery journal is incomplete."};
    }
    return state;
}

void validate_recovery_state(JournalState &state,
                             const std::filesystem::path &recovery_journal_path,
                             std::span<const std::filesystem::path> allowed_destinations) {
    if (!valid_token(state.token)) {
        throw std::runtime_error{"Restore recovery journal contains an invalid transaction token."};
    }
    if (allowed_destinations.empty()) {
        throw std::invalid_argument{"Restore recovery requires an explicit destination allowlist."};
    }

    std::unordered_set<std::string> allowed;
    allowed.reserve(allowed_destinations.size());
    for (const auto &destination : allowed_destinations) {
        if (destination.empty()) {
            throw std::invalid_argument{"Restore recovery allowlist contains an empty path."};
        }
        allowed.emplace(path_to_utf8(normalized_absolute(destination)));
    }

    const auto journal = normalized_absolute(recovery_journal_path);
    std::unordered_set<std::string> seen_names;
    std::unordered_set<std::string> seen_destinations;
    for (auto &item : state.items) {
        if (item.name.empty() || item.name.size() > 256 || !seen_names.emplace(item.name).second) {
            throw std::runtime_error{
                "Restore recovery journal contains an invalid or duplicate item name."};
        }
        if (!item.prepared && (item.had_original || item.backup_moved || item.published)) {
            throw std::runtime_error{"Restore recovery journal contains inconsistent item state."};
        }
        if (item.backup_moved && !item.had_original) {
            throw std::runtime_error{
                "Restore recovery journal contains an impossible backup state."};
        }
        if (state.committed && !item.published) {
            throw std::runtime_error{
                "Committed restore recovery journal contains an unpublished item."};
        }

        item.destination = normalized_absolute(item.destination);
        item.temporary = normalized_absolute(item.temporary);
        item.backup = normalized_absolute(item.backup);
        const auto destination_key = path_to_utf8(item.destination);
        if (!seen_destinations.emplace(destination_key).second) {
            throw std::runtime_error{"Restore recovery journal contains duplicate destinations."};
        }
        if (!allowed.contains(destination_key)) {
            throw std::runtime_error{"Restore recovery journal references a destination outside "
                                     "the explicit allowlist."};
        }

        const auto expected_temporary = sibling_artifact(item.destination, state.token, "new");
        const auto expected_backup = sibling_artifact(item.destination, state.token, "bak");
        if (item.temporary != expected_temporary || item.backup != expected_backup) {
            throw std::runtime_error{"Restore recovery journal contains forged artifact paths."};
        }
        if (item.destination == journal || item.temporary == journal || item.backup == journal) {
            throw std::runtime_error{
                "Restore recovery journal collides with a transaction data path."};
        }
    }
}

void copy_file_contents(const std::filesystem::path &source,
                        const std::filesystem::path &destination) {
    std::ifstream input{source, std::ios::binary};
    if (!input) {
        throw std::runtime_error{"Unable to open staged restore file: " + source.string()};
    }
    std::ofstream output{destination, std::ios::binary | std::ios::trunc};
    if (!output) {
        throw std::runtime_error{"Unable to create restore temporary file: " +
                                 destination.string()};
    }
    output << input.rdbuf();
    output.flush();
    if (!input.eof() && input.fail()) {
        throw std::runtime_error{"Unable to read staged restore file completely."};
    }
    if (!output) {
        throw std::runtime_error{"Unable to write restore temporary file completely."};
    }
    output.close();
    durable_flush_file(destination);
}

void write_blob_contents(std::span<const std::byte> data,
                         const std::filesystem::path &destination) {
    std::ofstream output{destination, std::ios::binary | std::ios::trunc};
    if (!output) {
        throw std::runtime_error{"Unable to create restore temporary file: " +
                                 destination.string()};
    }
    if (!data.empty()) {
        output.write(reinterpret_cast<const char *>(data.data()),
                     static_cast<std::streamsize>(data.size()));
    }
    output.flush();
    if (!output) {
        throw std::runtime_error{"Unable to write restore temporary file completely."};
    }
    output.close();
    durable_flush_file(destination);
}

[[nodiscard]] RestoreRecoveryResult rollback_state(JournalState &state,
                                                   const std::filesystem::path &journal_path) {
    RestoreRecoveryResult result;
    std::string first_error;

    for (auto it = state.items.rbegin(); it != state.items.rend(); ++it) {
        auto &item = *it;
        try {
            std::error_code error;
            const bool backup_exists = expected_regular_file_exists(item.backup);
            const bool destination_exists = expected_regular_file_exists(item.destination);
            const bool temporary_exists = expected_regular_file_exists(item.temporary);

            if (backup_exists) {
                if (destination_exists) {
                    if (!std::filesystem::remove(item.destination, error) || error) {
                        if (error) {
                            throw std::system_error{
                                error, "Unable to remove published restore file during rollback"};
                        }
                        throw std::runtime_error{
                            "Unable to remove published restore file during rollback"};
                    }
                }
                std::filesystem::rename(item.backup, item.destination, error);
                if (error) {
                    throw std::system_error{error,
                                            "Unable to restore original file during rollback"};
                }
                ++result.files_rolled_back;
            } else if (item.prepared && !item.had_original &&
                       (item.published || (!temporary_exists && destination_exists))) {
                if (!std::filesystem::remove(item.destination, error) || error) {
                    if (error) {
                        throw std::system_error{
                            error, "Unable to remove newly-created restore file during rollback"};
                    }
                    throw std::runtime_error{
                        "Unable to remove newly-created restore file during rollback"};
                }
                ++result.files_rolled_back;
            }

            if (temporary_exists) {
                if (!std::filesystem::remove(item.temporary, error) || error) {
                    if (error) {
                        throw std::system_error{
                            error, "Unable to remove restore temporary file during rollback"};
                    }
                    throw std::runtime_error{
                        "Unable to remove restore temporary file during rollback"};
                }
                ++result.temporary_files_removed;
            }
        } catch (const std::exception &exception) {
            if (first_error.empty())
                first_error = exception.what();
        }
    }

    if (!first_error.empty()) {
        throw std::runtime_error{"Restore rollback is incomplete: " + first_error +
                                 ". Recovery journal preserved at " + journal_path.string()};
    }

    std::error_code error;
    std::filesystem::remove(journal_path, error);
    if (error) {
        throw std::system_error{error,
                                "Rollback succeeded but recovery journal could not be removed"};
    }
    auto temporary_journal = journal_path;
    temporary_journal += L".tmp";
    std::filesystem::remove(temporary_journal, error);
    result.journal_removed = true;
    return result;
}

template <typename Plan>
[[nodiscard]] std::vector<ItemState>
prepare_items_common(std::span<const Plan> files, std::string_view token,
                     const std::filesystem::path &recovery_journal_path) {
    if (files.empty()) {
        throw std::invalid_argument{"Restore transaction requires at least one file."};
    }
    if (files.size() > max_transaction_files) {
        throw std::invalid_argument{"Restore transaction exceeds file-count limit."};
    }
    if (recovery_journal_path.empty()) {
        throw std::invalid_argument{"Restore transaction requires a recovery journal path."};
    }

    std::unordered_set<std::string> names;
    std::unordered_set<std::string> destinations;
    std::vector<ItemState> items;
    items.reserve(files.size());

    std::error_code error;
    const auto journal_absolute =
        std::filesystem::absolute(recovery_journal_path, error).lexically_normal();
    if (error) {
        throw std::system_error{error, "Unable to normalize restore recovery journal path"};
    }

    for (const auto &file : files) {
        if (file.name.empty()) {
            throw std::invalid_argument{"Restore transaction file name must not be empty."};
        }
        if (!names.emplace(file.name).second) {
            throw std::invalid_argument{"Restore transaction contains duplicate file names."};
        }

        const auto destination =
            std::filesystem::absolute(file.destination_path, error).lexically_normal();
        if (error) {
            throw std::system_error{error, "Unable to normalize restore destination"};
        }
        const auto destination_key = path_to_utf8(destination);
        if (!destinations.emplace(destination_key).second) {
            throw std::invalid_argument{"Restore transaction contains duplicate destinations."};
        }
        if (destination == journal_absolute) {
            throw std::invalid_argument{
                "Restore destination must not overwrite the recovery journal."};
        }

        ItemState item;
        item.name = file.name;
        item.destination = destination;
        item.temporary = sibling_artifact(destination, token, "new");
        item.backup = sibling_artifact(destination, token, "bak");
        items.push_back(std::move(item));
    }
    return items;
}

template <typename PrepareTemporary>
[[nodiscard]] RestoreTransactionResult
commit_items(std::vector<ItemState> items, std::string token,
             const std::filesystem::path &recovery_journal_path,
             PrepareTemporary &&prepare_temporary) {
    JournalState state;
    state.token = std::move(token);
    state.items = std::move(items);
    write_journal(recovery_journal_path, state);

    RestoreTransactionResult result;
    try {
        for (std::size_t index = 0; index < state.items.size(); ++index) {
            auto &item = state.items[index];
            std::error_code error;
            const auto parent = item.destination.parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, error);
                if (error) {
                    throw std::system_error{error,
                                            "Unable to create restore destination directory"};
                }
            }

            const bool exists = std::filesystem::exists(item.destination, error) && !error;
            if (error) {
                throw std::system_error{error, "Unable to inspect restore destination"};
            }
            if (exists && !std::filesystem::is_regular_file(item.destination, error)) {
                throw std::runtime_error{"Restore destination is not a regular file: " +
                                         item.destination.string()};
            }
            if (error) {
                throw std::system_error{error, "Unable to inspect restore destination type"};
            }

            item.prepared = true;
            item.had_original = exists;
            write_journal(recovery_journal_path, state);

            remove_expected_file_if_exists(item.temporary);
            remove_expected_file_if_exists(item.backup);
            prepare_temporary(index, item.temporary);

            if (exists) {
                std::filesystem::rename(item.destination, item.backup, error);
                if (error) {
                    throw std::system_error{
                        error, "Unable to preserve original file before restore commit"};
                }
                item.backup_moved = true;
                ++result.existing_files_replaced;
                write_journal(recovery_journal_path, state);
            } else {
                ++result.new_files_created;
            }

            std::filesystem::rename(item.temporary, item.destination, error);
            if (error) {
                throw std::system_error{error, "Unable to publish staged restore file"};
            }
            item.published = true;
            ++result.files_committed;
            write_journal(recovery_journal_path, state);
        }

        // The durable committed marker is the transaction commit point. Backups are
        // intentionally retained until after this marker reaches disk so a crash can
        // never be misclassified as an incomplete transaction after rollback material
        // has already been deleted.
        state.committed = true;
        write_journal(recovery_journal_path, state);
    } catch (const std::exception &exception) {
        const std::string original = exception.what();
        try {
            (void)rollback_state(state, recovery_journal_path);
        } catch (const std::exception &rollback_exception) {
            throw std::runtime_error{"Restore transaction failed: " + original +
                                     "; rollback also failed: " + rollback_exception.what()};
        }
        throw std::runtime_error{"Restore transaction failed and was rolled back: " + original};
    }

    // After the committed marker is durable, cleanup failure is not permission to
    // roll back successfully published user data. Preserve the committed journal so
    // recover_interrupted() can finish artifact cleanup on the next startup.
    try {
        for (const auto &item : state.items) {
            remove_expected_file_if_exists(item.backup);
            remove_expected_file_if_exists(item.temporary);
        }

        std::error_code error;
        std::filesystem::remove(recovery_journal_path, error);
        if (error) {
            throw std::system_error{error,
                                    "Restore committed but recovery journal could not be removed"};
        }
    } catch (...) {
        result.recovery_cleanup_pending = true;
    }
    return result;
}

} // namespace

RestoreTransactionResult
RestoreTransaction::commit(std::span<const RestoreFilePlan> files,
                           const std::filesystem::path &recovery_journal_path) {
    const auto token = make_token();
    auto items = prepare_items_common(files, token, recovery_journal_path);
    prepare_recovery_slot(recovery_journal_path);
    std::vector<std::filesystem::path> staged;
    staged.reserve(files.size());
    std::error_code error;
    for (const auto &file : files) {
        auto path = std::filesystem::absolute(file.staged_path, error).lexically_normal();
        if (error || !std::filesystem::is_regular_file(path, error) || error) {
            throw std::runtime_error{"Restore transaction staged file is missing or invalid: " +
                                     file.staged_path.string()};
        }
        staged.push_back(std::move(path));
    }

    return commit_items(std::move(items), token, recovery_journal_path,
                        [&staged](std::size_t index, const std::filesystem::path &temporary) {
                            copy_file_contents(staged[index], temporary);
                        });
}

RestoreTransactionResult
RestoreTransaction::commit(std::span<const RestoreBlobPlan> files,
                           const std::filesystem::path &recovery_journal_path) {
    const auto token = make_token();
    auto items = prepare_items_common(files, token, recovery_journal_path);
    prepare_recovery_slot(recovery_journal_path);
    return commit_items(std::move(items), token, recovery_journal_path,
                        [files](std::size_t index, const std::filesystem::path &temporary) {
                            write_blob_contents(files[index].data, temporary);
                        });
}

RestoreRecoveryResult RestoreTransaction::recover_interrupted(
    const std::filesystem::path &recovery_journal_path,
    std::span<const std::filesystem::path> allowed_destinations) {
    std::error_code error;
    if (!std::filesystem::exists(recovery_journal_path, error) || error) {
        if (error) {
            throw std::system_error{error, "Unable to inspect restore recovery journal"};
        }
        return {};
    }

    auto state = read_journal(recovery_journal_path);
    validate_recovery_state(state, recovery_journal_path, allowed_destinations);
    if (state.committed) {
        RestoreRecoveryResult result;
        for (const auto &item : state.items) {
            if (expected_regular_file_exists(item.temporary)) {
                remove_expected_file_if_exists(item.temporary);
                ++result.temporary_files_removed;
            }
            if (expected_regular_file_exists(item.backup)) {
                remove_expected_file_if_exists(item.backup);
            }
        }
        std::filesystem::remove(recovery_journal_path, error);
        if (error) {
            throw std::system_error{error, "Unable to remove committed restore recovery journal"};
        }
        auto temporary_journal = recovery_journal_path;
        temporary_journal += L".tmp";
        if (expected_regular_file_exists(temporary_journal)) {
            remove_expected_file_if_exists(temporary_journal);
            ++result.temporary_files_removed;
        }
        result.journal_removed = true;
        return result;
    }

    return rollback_state(state, recovery_journal_path);
}

} // namespace axiom
