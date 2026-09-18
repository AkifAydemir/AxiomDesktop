#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace axiom {
using AutomationId = std::uint64_t;
enum class AutomationScheduleKind : std::uint32_t {
    one_shot = 0,
    fixed_interval = 1,
    local_calendar = 2,
};
struct AutomationCalendarSchedule {
    // Bit 0 = Sunday ... bit 6 = Saturday, matching std::tm::tm_wday.
    std::uint8_t weekday_mask{0x7Fu};
    std::uint8_t hour{};
    std::uint8_t minute{};
    std::uint8_t second{};
    std::wstring time_zone{L"local"};
};
enum class AutomationRetryKind : std::uint32_t {
    none = 0,
    fixed = 1,
    exponential = 2,
};
struct AutomationFailurePolicy {
    AutomationRetryKind kind{AutomationRetryKind::none};
    std::uint32_t max_retries{};
    std::chrono::milliseconds initial_delay{};
    std::chrono::milliseconds max_delay{};
    bool disable_on_exhaustion{};
    [[nodiscard]] bool retries_enabled() const noexcept {
        return kind != AutomationRetryKind::none && max_retries != 0;
    }
};
struct Automation {
    AutomationId id{};
    bool enabled{true};
    std::chrono::system_clock::time_point due_at{};
    AutomationScheduleKind schedule_kind{AutomationScheduleKind::one_shot};
    std::chrono::milliseconds repeat_interval{};
    AutomationCalendarSchedule calendar_schedule{};
    AutomationFailurePolicy failure_policy{};
    std::uint32_t retry_attempt{};
    std::uint32_t consecutive_failures{};
    std::optional<std::chrono::system_clock::time_point> retry_resume_at;
    std::uint64_t run_count{};
    std::uint64_t failure_count{};
    std::optional<std::chrono::system_clock::time_point> last_run_at;
    bool last_success{true};
    std::wstring name;
    std::wstring action_name;
    std::wstring action_payload;
    std::wstring last_error;
    [[nodiscard]] bool recurring() const noexcept {
        return schedule_kind != AutomationScheduleKind::one_shot;
    }
    [[nodiscard]] bool retry_pending() const noexcept {
        return retry_attempt != 0;
    }
};
struct AutomationStoreSnapshot {
    std::uint64_t generation{};
    AutomationId next_id{1};
    std::vector<Automation> automations;
};
struct AutomationStoreStatus {
    std::filesystem::path path;
    std::uint64_t generation{};
    std::size_t record_count{};
    bool recovered_from_alternate{};
};
class AutomationStore final {
  public:
    explicit AutomationStore(std::filesystem::path path);
    AutomationStore(const AutomationStore &) = delete;
    AutomationStore &operator=(const AutomationStore &) = delete;
    [[nodiscard]] AutomationStoreSnapshot load();
    void save(AutomationId next_id, std::span<const Automation> automations);
    [[nodiscard]] AutomationStoreStatus status() const;
    [[nodiscard]] const std::filesystem::path &path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::uint64_t generation_{};
    std::size_t record_count_{};
    bool recovered_from_alternate_{};
};
} // namespace axiom
