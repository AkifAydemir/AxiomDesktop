#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace axiom {
enum class DiagnosticSeverity : std::uint8_t {
    info,
    warning,
    error,
};
enum class DiagnosticDomain : std::uint8_t {
    runtime,
    action,
    automation,
    plugin,
};
struct DiagnosticEvent {
    std::uint64_t id{};
    std::chrono::system_clock::time_point occurred_at{};
    DiagnosticSeverity severity{DiagnosticSeverity::info};
    DiagnosticDomain domain{DiagnosticDomain::runtime};
    std::wstring subject;
    std::wstring message;
    std::chrono::milliseconds duration{};
};
struct DiagnosticsStats {
    std::size_t retained{};
    std::size_t capacity{};
    std::uint64_t total_recorded{};
    std::uint64_t total_errors{};
    std::uint64_t dropped_oldest{};
};
class ExecutionDiagnostics final {
  public:
    explicit ExecutionDiagnostics(std::size_t capacity = 512);
    [[nodiscard]] std::uint64_t record(DiagnosticSeverity severity, DiagnosticDomain domain,
                                       std::wstring subject, std::wstring message,
                                       std::chrono::milliseconds duration = {});
    [[nodiscard]] std::vector<DiagnosticEvent> recent(std::size_t limit = 50) const;
    [[nodiscard]] std::vector<DiagnosticEvent>
    for_subject(DiagnosticDomain domain, std::wstring_view subject, std::size_t limit = 50) const;
    [[nodiscard]] std::optional<DiagnosticEvent> last_error(DiagnosticDomain domain,
                                                            std::wstring_view subject = {}) const;
    [[nodiscard]] DiagnosticsStats stats() const noexcept;
    void clear() noexcept;

  private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<DiagnosticEvent> events_;
    std::uint64_t next_id_{1};
    std::uint64_t total_recorded_{};
    std::uint64_t total_errors_{};
    std::uint64_t dropped_oldest_{};
};
[[nodiscard]] std::wstring_view diagnostic_severity_name(DiagnosticSeverity severity) noexcept;
[[nodiscard]] std::wstring_view diagnostic_domain_name(DiagnosticDomain domain) noexcept;
} // namespace axiom
