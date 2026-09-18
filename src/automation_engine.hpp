#pragma once
#include "action_registry.hpp"
#include "automation_schedule.hpp"
#include "automation_store.hpp"
#include "runtime.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace axiom {
struct AutomationRestoreReport {
    std::size_t restored{};
    std::size_t overdue{};
    std::size_t disabled{};
    std::size_t unresolved_actions{};
    std::size_t calendar_due_rebased{};
    std::size_t calendar_retry_resumes_rebased{};
    std::size_t calendar_resolution_failures{};
};
struct AutomationActionReconcileReport {
    std::size_t parked{};
    std::size_t armed{};
    std::size_t unresolved{};
};
struct AutomationCalendarRefreshReport {
    std::size_t due_rebased{};
    std::size_t retry_resume_rebased{};
    std::size_t parked{};
};
struct AutomationEngineStats {
    std::size_t total{};
    std::size_t enabled{};
    std::size_t disabled{};
    std::size_t recurring{};
    std::size_t calendar{};
    std::size_t retry_enabled{};
    std::size_t retrying{};
    std::size_t unresolved_actions{};
    std::uint64_t total_runs{};
    std::uint64_t total_failures{};
};
class AutomationEngine final {
  public:
    struct State;
    using Duration = RuntimeScheduler::Duration;
    using ExecutionCallback = std::function<void(const Automation &, const ActionResult &)>;
    AutomationEngine(RuntimeScheduler &scheduler, BoundedExecutor &executor, AutomationStore &store,
                     ActionRegistry &actions, ExecutionDiagnostics *diagnostics = nullptr,
                     ExecutionCallback execution_callback = {});
    ~AutomationEngine();
    AutomationEngine(const AutomationEngine &) = delete;
    AutomationEngine &operator=(const AutomationEngine &) = delete;
    [[nodiscard]] AutomationId schedule_after(Duration delay, std::wstring name,
                                              std::wstring action_name,
                                              std::wstring action_payload = {});
    [[nodiscard]] AutomationId schedule_at(std::chrono::system_clock::time_point due_at,
                                           std::wstring name, std::wstring action_name,
                                           std::wstring action_payload = {});
    [[nodiscard]] AutomationId schedule_every(Duration interval, std::wstring name,
                                              std::wstring action_name,
                                              std::wstring action_payload = {});
    [[nodiscard]] AutomationId schedule_calendar(AutomationCalendarSchedule schedule,
                                                 std::wstring name, std::wstring action_name,
                                                 std::wstring action_payload = {});
    [[nodiscard]] bool cancel(AutomationId id);
    [[nodiscard]] bool set_failure_policy(AutomationId id, AutomationFailurePolicy policy);
    [[nodiscard]] bool set_enabled(AutomationId id, bool enabled);
    void clear();
    void stop() noexcept;
    void set_execution_callback(ExecutionCallback callback);
    [[nodiscard]] AutomationRestoreReport restore();
    [[nodiscard]] AutomationActionReconcileReport reconcile_action_availability();
    [[nodiscard]] AutomationCalendarRefreshReport refresh_calendar_schedules();
    [[nodiscard]] std::vector<Automation> snapshot() const;
    [[nodiscard]] std::optional<Automation> find(AutomationId id) const;
    [[nodiscard]] AutomationEngineStats stats() const;
    [[nodiscard]] AutomationStoreStatus persistence_status() const;
    [[nodiscard]] std::string last_persistence_error() const;

  private:
    RuntimeScheduler &scheduler_;
    BoundedExecutor &executor_;
    AutomationStore &store_;
    ActionRegistry &actions_;
    std::shared_ptr<State> state_;
};
} // namespace axiom
