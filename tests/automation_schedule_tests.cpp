#include "automation_schedule.hpp"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace {
using namespace std::chrono_literals;
void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}
std::chrono::system_clock::time_point local_time(int year, int month, int day, int hour, int minute,
                                                 int second) {
    std::tm value{};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_sec = second;
    value.tm_isdst = -1;
    const auto raw = std::mktime(&value);
    if (raw == static_cast<std::time_t>(-1)) {
        throw std::runtime_error{"mktime failed in automation schedule test"};
    }
    return std::chrono::system_clock::from_time_t(raw);
}
std::tm local_parts(std::chrono::system_clock::time_point value) {
    const auto raw = std::chrono::system_clock::to_time_t(value);
    std::tm result{};
#ifdef _WIN32
    if (localtime_s(&result, &raw) != 0)
        throw std::runtime_error{"localtime_s failed"};
#else
    if (localtime_r(&raw, &result) == nullptr)
        throw std::runtime_error{"localtime_r failed"};
#endif
    return result;
}
#ifndef _WIN32
class ScopedTimezone final {
  public:
    explicit ScopedTimezone(const char *zone) {
        if (const char *current = std::getenv("TZ")) {
            previous_ = current;
            had_previous_ = true;
        }
        if (setenv("TZ", zone, 1) != 0) {
            throw std::runtime_error{"setenv(TZ) failed"};
        }
        tzset();
    }
    ~ScopedTimezone() {
        if (had_previous_) {
            (void)setenv("TZ", previous_.c_str(), 1);
        } else {
            (void)unsetenv("TZ");
        }
        tzset();
    }

  private:
    std::string previous_;
    bool had_previous_{};
};
#endif
} // namespace
int main() {
    try {
        axiom::AutomationCalendarSchedule daily;
        daily.weekday_mask = 0x7F;
        daily.hour = 9;
        daily.minute = 30;
        daily.time_zone = L"local";
        expect(axiom::valid_calendar_schedule(daily), "daily local schedule should validate");
        const auto monday_morning = local_time(2026, 1, 5, 8, 0, 0);
        const auto daily_next = axiom::next_calendar_occurrence(daily, monday_morning);
        expect(daily_next.has_value(), "daily schedule should resolve next occurrence");
        const auto daily_parts = local_parts(*daily_next);
        expect(daily_parts.tm_year == 126 && daily_parts.tm_mon == 0 && daily_parts.tm_mday == 5,
               "daily occurrence should remain on same local date");
        expect(daily_parts.tm_hour == 9 && daily_parts.tm_min == 30,
               "daily occurrence should preserve requested local wall time");
        axiom::AutomationCalendarSchedule tuesday = daily;
        tuesday.weekday_mask = static_cast<std::uint8_t>(1u << 2); // Tuesday
        const auto weekly_next = axiom::next_calendar_occurrence(tuesday, monday_morning);
        expect(weekly_next.has_value(), "weekly schedule should resolve next occurrence");
        const auto weekly_parts = local_parts(*weekly_next);
        expect(weekly_parts.tm_wday == 2 && weekly_parts.tm_mday == 6,
               "weekday mask should advance to selected local weekday");
        auto unsupported_zone = daily;
        unsupported_zone.time_zone = L"Europe/Istanbul";
        expect(!axiom::valid_calendar_schedule(unsupported_zone),
               "v15 should fail closed for unsupported explicit non-local zones");
#ifdef _WIN32
        TIME_ZONE_INFORMATION time_zone{};
        const DWORD time_zone_state = GetTimeZoneInformation(&time_zone);
        expect(time_zone_state != TIME_ZONE_ID_INVALID, "GetTimeZoneInformation failed");
        const bool exercise_dst = time_zone.DaylightDate.wMonth != 0;
        if (!exercise_dst && std::getenv("AXIOM_REQUIRE_DST") != nullptr) {
            throw std::runtime_error{"Current Windows time zone does not expose DST transitions"};
        }
        if (exercise_dst) {
#else
        {
            ScopedTimezone timezone{"America/New_York"};
#endif
            axiom::AutomationCalendarSchedule spring_gap;
            spring_gap.weekday_mask = static_cast<std::uint8_t>(1u << 0); // Sunday
            spring_gap.hour = 2;
            spring_gap.minute = 30;
            spring_gap.time_zone = L"local";
            const auto before_gap = local_time(2026, 3, 8, 0, 0, 0);
            const auto after_gap = axiom::next_calendar_occurrence(spring_gap, before_gap);
            expect(after_gap.has_value(), "DST-gap schedule should resolve a later valid Sunday");
            const auto gap_parts = local_parts(*after_gap);
            expect(gap_parts.tm_year == 126 && gap_parts.tm_mon == 2 && gap_parts.tm_mday == 15,
                   "nonexistent spring-forward wall time must be skipped, not shifted");
            expect(gap_parts.tm_hour == 2 && gap_parts.tm_min == 30,
                   "post-gap occurrence must preserve requested wall-clock time");
            axiom::AutomationCalendarSchedule fall_back = spring_gap;
            fall_back.hour = 1;
            fall_back.minute = 30;
            const auto before_fall_back = local_time(2026, 11, 1, 0, 0, 0);
            const auto ambiguous = axiom::next_calendar_occurrence(fall_back, before_fall_back);
            expect(ambiguous.has_value(), "fall-back wall time should resolve exactly once");
            const auto ambiguous_parts = local_parts(*ambiguous);
            expect(ambiguous_parts.tm_year == 126 && ambiguous_parts.tm_mon == 10 &&
                       ambiguous_parts.tm_mday == 1 && ambiguous_parts.tm_hour == 1 &&
                       ambiguous_parts.tm_min == 30,
                   "fall-back occurrence should preserve requested ambiguous local wall time");
        }
#ifdef _WIN32
        else {
            std::cout << "AxiomAutomationScheduleTests: DST scenarios skipped because the current "
                         "Windows time zone has no DST rules\n";
        }
#endif
        axiom::AutomationFailurePolicy invalid_none;
        invalid_none.disable_on_exhaustion = true;
        expect(!axiom::valid_failure_policy(invalid_none),
               "retry-disabled policy must reject meaningless exhaustion behavior");
        axiom::AutomationFailurePolicy fixed;
        fixed.kind = axiom::AutomationRetryKind::fixed;
        fixed.max_retries = 3;
        fixed.initial_delay = 5s;
        fixed.max_delay = 5s;
        expect(axiom::valid_failure_policy(fixed), "fixed retry policy should validate");
        expect(axiom::retry_delay_for(fixed, 3) == 5s, "fixed retry delay should remain constant");
        axiom::AutomationFailurePolicy exponential;
        exponential.kind = axiom::AutomationRetryKind::exponential;
        exponential.max_retries = 8;
        exponential.initial_delay = 2s;
        exponential.max_delay = 10s;
        expect(axiom::valid_failure_policy(exponential),
               "exponential retry policy should validate");
        expect(axiom::retry_delay_for(exponential, 1) == 2s,
               "first backoff should use initial delay");
        expect(axiom::retry_delay_for(exponential, 2) == 4s, "second backoff should double");
        expect(axiom::retry_delay_for(exponential, 4) == 10s, "backoff should clamp to max delay");
        std::cout << "AxiomAutomationScheduleTests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "AxiomAutomationScheduleTests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
