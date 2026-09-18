#pragma once
#include "automation_store.hpp"
#include <chrono>
#include <optional>
#include <string>
namespace axiom {
[[nodiscard]] bool valid_calendar_schedule(const AutomationCalendarSchedule &schedule,
                                           std::string *error = nullptr) noexcept;
[[nodiscard]] std::optional<std::chrono::system_clock::time_point>
next_calendar_occurrence(const AutomationCalendarSchedule &schedule,
                         std::chrono::system_clock::time_point after);
[[nodiscard]] bool valid_failure_policy(const AutomationFailurePolicy &policy,
                                        std::string *error = nullptr) noexcept;
[[nodiscard]] std::chrono::milliseconds retry_delay_for(const AutomationFailurePolicy &policy,
                                                        std::uint32_t retry_number) noexcept;
} // namespace axiom
