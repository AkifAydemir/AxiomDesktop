#include "support_bundle.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_set>

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

constexpr std::size_t max_fields = 128;
constexpr std::size_t max_diagnostics = 256;
constexpr std::size_t max_key_bytes = 96;
constexpr std::size_t max_value_bytes = 4096;
constexpr std::size_t max_subject_bytes = 512;
constexpr std::size_t max_message_bytes = 4096;

[[nodiscard]] std::string clean_text(std::string_view input, std::size_t max_bytes) {
    std::string output;
    output.reserve(std::min(input.size(), max_bytes));
    for (const unsigned char ch : input) {
        if (output.size() >= max_bytes)
            break;
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            output.push_back(' ');
        } else if (ch < 0x20u || ch == 0x7Fu) {
            output.push_back('?');
        } else {
            output.push_back(static_cast<char>(ch));
        }
    }
    return output;
}

[[nodiscard]] bool valid_key(std::string_view key) {
    if (key.empty() || key.size() > max_key_bytes)
        return false;
    for (const unsigned char ch : key) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
        if (!ok)
            return false;
    }
    return true;
}

std::atomic<std::uint64_t> next_temp_id{1};

[[nodiscard]] std::filesystem::path unique_temporary_path(const std::filesystem::path &path) {
    const auto ticks =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto sequence = next_temp_id.fetch_add(1, std::memory_order_relaxed);
    auto temporary = path;
    temporary += L".tmp." + std::to_wstring(ticks) + L"." + std::to_wstring(sequence);
    return temporary;
}

void write_exclusive_durable(const std::filesystem::path &path, std::string_view bytes) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error{static_cast<int>(GetLastError()), std::system_category(),
                                "Unable to create exclusive support bundle temporary"};
    }

    std::size_t offset = 0;
    bool success = true;
    DWORD failure = ERROR_SUCCESS;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1024u * 1024u));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) ||
            written != chunk) {
            success = false;
            failure = GetLastError();
            break;
        }
        offset += written;
    }
    if (success && !FlushFileBuffers(handle)) {
        success = false;
        failure = GetLastError();
    }
    CloseHandle(handle);
    if (!success) {
        DeleteFileW(path.c_str());
        throw std::system_error{static_cast<int>(failure), std::system_category(),
                                "Unable to write support bundle temporary"};
    }
#else
    const int flags = O_WRONLY | O_CREAT | O_EXCL
#ifdef O_CLOEXEC
                      | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                      | O_NOFOLLOW
#endif
        ;
    const int fd = ::open(path.c_str(), flags, 0600);
    if (fd < 0) {
        throw std::system_error{errno, std::generic_category(),
                                "Unable to create exclusive support bundle temporary"};
    }

    std::size_t offset = 0;
    int failure = 0;
    while (offset < bytes.size()) {
        const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            failure = errno;
            break;
        }
        if (written == 0) {
            failure = EIO;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (failure == 0 && ::fsync(fd) != 0)
        failure = errno;
    if (::close(fd) != 0 && failure == 0)
        failure = errno;
    if (failure != 0) {
        ::unlink(path.c_str());
        throw std::system_error{failure, std::generic_category(),
                                "Unable to write support bundle temporary"};
    }
#endif
}

void replace_atomic(const std::filesystem::path &temporary, const std::filesystem::path &path) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error{static_cast<int>(GetLastError()), std::system_category(),
                                "Unable to publish support bundle"};
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        throw std::system_error{error, "Unable to publish support bundle"};
    }
#endif
}

void publish_atomic(const std::filesystem::path &path, const std::string &bytes) {
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            throw std::system_error{error, "Unable to create support bundle directory"};
        }
    }

    const auto temporary = unique_temporary_path(path);
    try {
        write_exclusive_durable(temporary, bytes);
        replace_atomic(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

} // namespace

SupportBundleReport SupportBundle::create(const std::filesystem::path &output_path,
                                          std::span<const SupportField> fields,
                                          std::span<const SupportDiagnostic> diagnostics,
                                          SupportBundleOptions options) {
    if (output_path.empty()) {
        throw std::invalid_argument{"Support bundle output path must not be empty."};
    }
    if (fields.size() > max_fields) {
        throw std::invalid_argument{"Support bundle field count exceeds limit."};
    }
    options.max_diagnostics = std::min(options.max_diagnostics, max_diagnostics);

    SupportBundleReport report;
    std::string output;
    output.reserve(8192);
    output += "AXIOM-SUPPORT-BUNDLE v1\n";
    output += "privacy=explicit-opt-in\n";
    output += "sensitive-fields=";
    output += options.include_sensitive_fields ? "included\n" : "omitted\n";
    output += "diagnostic-messages=";
    output += options.include_diagnostic_messages ? "included\n" : "omitted\n";
    output += "\n[metadata]\n";

    std::unordered_set<std::string> field_keys;
    field_keys.reserve(fields.size());
    for (const auto &field : fields) {
        if (!valid_key(field.key)) {
            throw std::invalid_argument{"Support bundle field key is invalid: " + field.key};
        }
        if (!field_keys.emplace(field.key).second) {
            throw std::invalid_argument{"Support bundle field key is duplicated: " + field.key};
        }
        if (field.sensitive && !options.include_sensitive_fields) {
            ++report.sensitive_fields_omitted;
            continue;
        }
        output += field.key;
        output += '=';
        output += clean_text(field.value, max_value_bytes);
        output += '\n';
        ++report.fields_written;
    }

    output += "\n[diagnostics]\n";
    const auto count = std::min(diagnostics.size(), options.max_diagnostics);
    for (std::size_t index = 0; index < count; ++index) {
        const auto &diagnostic = diagnostics[index];
        output += "id=" + std::to_string(diagnostic.id);
        output += " time-ms=" + std::to_string(diagnostic.occurred_at_unix_ms);
        output += " severity=" + clean_text(diagnostic.severity, 32);
        output += " domain=" + clean_text(diagnostic.domain, 64);
        output += " subject=" + clean_text(diagnostic.subject, max_subject_bytes);
        if (options.include_diagnostic_messages) {
            output += " message=" + clean_text(diagnostic.message, max_message_bytes);
        } else if (!diagnostic.message.empty()) {
            ++report.diagnostic_messages_omitted;
        }
        output += '\n';
        ++report.diagnostics_written;
    }

    output += "\n[redaction-summary]\n";
    output += "sensitive-fields-omitted=" + std::to_string(report.sensitive_fields_omitted) + '\n';
    output +=
        "diagnostic-messages-omitted=" + std::to_string(report.diagnostic_messages_omitted) + '\n';
    output += "note=Raw action payloads, clipboard contents and journal entries are not part of "
              "this bundle contract.\n";

    publish_atomic(output_path, output);
    std::error_code error;
    report.bytes_written = std::filesystem::file_size(output_path, error);
    if (error) {
        throw std::system_error{error, "Unable to stat created support bundle"};
    }
    return report;
}

} // namespace axiom
