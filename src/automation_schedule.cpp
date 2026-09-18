#include "automation_schedule.hpp"
#include <algorithm>
#include <ctime>
#include <limits>
namespace axiom {
namespace {
constexpr auto max_retry_delay = std::chrono::hours{24 * 30};
constexpr std::uint32_t max_retry_count = 32;
[[nodiscard]] bool localtime_safe(std::time_t raw, std::tm &output) noexcept {
#ifdef _WIN32
    return localtime_s(&output, &raw) == 0;
#else
    return localtime_r(&raw, &output) != nullptr;
#endif
}
void set_error(std::string *error, const char *message) noexcept {
    if (error != nullptr) {
        try {
            *error = message;
        } catch (...) {
        }
    }
}
[[nodiscard]] bool same_wall_time(const std::tm &value, int year, int month, int day, int hour,
                                  int minute, int second) noexcept {
    return value.tm_year == year && value.tm_mon == month && value.tm_mday == day &&
           value.tm_hour == hour && value.tm_min == minute && value.tm_sec == second;
}
} // namespace
bool valid_calendar_schedule(const AutomationCalendarSchedule &schedule,
                             std::string *error) noexcept {
    if (schedule.time_zone != L"local") {
        set_error(error, "v15 calendar schedules support the explicit local time zone only.");
        return false;
    }
    if ((schedule.weekday_mask & 0x7Fu) == 0 || (schedule.weekday_mask & ~0x7Fu) != 0) {
        set_error(error, "Calendar weekday mask must select at least one valid weekday.");
        return false;
    }
    if (schedule.hour > 23 || schedule.minute > 59 || schedule.second > 59) {
        set_error(error, "Calendar wall-clock time is invalid.");
        return false;
    }
    return true;
}
std::optional<std::chrono::system_clock::time_point>
next_calendar_occurrence(const AutomationCalendarSchedule &schedule,
                         std::chrono::system_clock::time_point after) {
    if (!valid_calendar_schedule(schedule)) {
        return std::nullopt;
    }
    const auto raw_after = std::chrono::system_clock::to_time_t(after);
    std::tm base{};
    if (!localtime_safe(raw_after, base)) {
        return std::nullopt;
    }
    // Search two weeks so a DST-invalid wall time on the only selected weekday can
    // be skipped safely and still find the following valid occurrence.
    for (int day_offset = 0; day_offset < 14; ++day_offset) {
        std::tm noon = base;
        noon.tm_hour = 12;
        noon.tm_min = 0;
        noon.tm_sec = 0;
        noon.tm_isdst = -1;
        noon.tm_mday += day_offset;
        const auto normalized_raw = std::mktime(&noon);
        if (normalized_raw == static_cast<std::time_t>(-1)) {
            continue;
        }
        std::tm normalized{};
        if (!localtime_safe(normalized_raw, normalized)) {
            continue;
        }
        const auto weekday_bit = static_cast<std::uint8_t>(1u << normalized.tm_wday);
        if ((schedule.weekday_mask & weekday_bit) == 0) {
            continue;
        }
        const int expected_year = normalized.tm_year;
        const int expected_month = normalized.tm_mon;
        const int expected_day = normalized.tm_mday;
        std::tm candidate = normalized;
        candidate.tm_hour = schedule.hour;
        candidate.tm_min = schedule.minute;
        candidate.tm_sec = schedule.second;
        candidate.tm_isdst = -1;
        const auto candidate_raw = std::mktime(&candidate);
        if (candidate_raw == static_cast<std::time_t>(-1)) {
            continue;
        }
        std::tm round_trip{};
        if (!localtime_safe(candidate_raw, round_trip) ||
            !same_wall_time(round_trip, expected_year, expected_month, expected_day, schedule.hour,
                            schedule.minute, schedule.second)) {
            // Spring-forward gaps are skipped instead of silently shifting the
            // requested wall-clock time. Fall-back ambiguity is resolved once by
            // the platform mktime/localtime policy for the local time zone.
            continue;
        }
        const auto candidate_time = std::chrono::system_clock::from_time_t(candidate_raw);
        if (candidate_time > after) {
            return candidate_time;
        }
    }
    return std::nullopt;
}
bool valid_failure_policy(const AutomationFailurePolicy &policy, std::string *error) noexcept {
    if (policy.kind == AutomationRetryKind::none) {
        if (policy.max_retries != 0 || policy.initial_delay != std::chrono::milliseconds::zero() ||
            policy.max_delay != std::chrono::milliseconds::zero() || policy.disable_on_exhaustion) {
            set_error(
                error,
                "Retry-disabled policy must not carry retry count/delays/exhaustion behavior.");
            return false;
        }
        return true;
    }
    if (policy.kind != AutomationRetryKind::fixed &&
        policy.kind != AutomationRetryKind::exponential) {
        set_error(error, "Unknown automation retry policy kind.");
        return false;
    }
    if (policy.max_retries == 0 || policy.max_retries > max_retry_count) {
        set_error(error, "Automation retry count must be between 1 and 32.");
        return false;
    }
    if (policy.initial_delay <= std::chrono::milliseconds::zero() ||
        policy.initial_delay > max_retry_delay) {
        set_error(error, "Automation retry initial delay must be positive and <= 30 days.");
        return false;
    }
    if (policy.max_delay < policy.initial_delay || policy.max_delay > max_retry_delay) {
        set_error(error, "Automation retry max delay must be >= initial delay and <= 30 days.");
        return false;
    }
    if (policy.kind == AutomationRetryKind::fixed && policy.max_delay != policy.initial_delay) {
        set_error(error, "Fixed retry policy requires max delay to equal initial delay.");
        return false;
    }
    return true;
}
std::chrono::milliseconds retry_delay_for(const AutomationFailurePolicy &policy,
                                          std::uint32_t retry_number) noexcept {
    if (!policy.retries_enabled() || retry_number == 0) {
        return std::chrono::milliseconds::zero();
    }
    if (policy.kind == AutomationRetryKind::fixed) {
        return policy.initial_delay;
    }
    auto value = policy.initial_delay.count();
    const auto cap = policy.max_delay.count();
    for (std::uint32_t step = 1; step < retry_number && value < cap; ++step) {
        if (value > cap / 2) {
            value = cap;
            break;
        }
        value *= 2;
    }
    return std::chrono::milliseconds{std::min(value, cap)};
}
} // namespace axiom
