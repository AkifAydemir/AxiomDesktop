#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace axiom {

struct SupportField {
    std::string key;
    std::string value;
    bool sensitive{};
};

struct SupportDiagnostic {
    std::uint64_t id{};
    std::int64_t occurred_at_unix_ms{};
    std::string severity;
    std::string domain;
    std::string subject;
    std::string message;
};

struct SupportBundleOptions {
    bool include_sensitive_fields{};
    bool include_diagnostic_messages{};
    std::size_t max_diagnostics{100};
};

struct SupportBundleReport {
    std::size_t fields_written{};
    std::size_t sensitive_fields_omitted{};
    std::size_t diagnostics_written{};
    std::size_t diagnostic_messages_omitted{};
    std::uintmax_t bytes_written{};
};

class SupportBundle final {
  public:
    [[nodiscard]] static SupportBundleReport create(const std::filesystem::path &output_path,
                                                    std::span<const SupportField> fields,
                                                    std::span<const SupportDiagnostic> diagnostics,
                                                    SupportBundleOptions options = {});
};

} // namespace axiom
