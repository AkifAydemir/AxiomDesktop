#include "automation_engine.hpp"
#include "automation_schedule.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
namespace axiom {
struct AutomationEngine::State final {
    struct Entry final {
        Automation automation;
        RuntimeTaskId runtime_task{};
        std::uint64_t arm_generation{};
    };
    RuntimeScheduler *scheduler{};
    BoundedExecutor *executor{};
    AutomationStore *store{};
    ActionRegistry *actions{};
    ExecutionDiagnostics *diagnostics{};
    mutable std::mutex mutex;
    std::condition_variable work_idle;
    std::mutex mutation_mutex;
    std::size_t in_flight{};
    std::unordered_map<AutomationId, Entry> entries;
    ExecutionCallback execution_callback;
    AutomationId next_id{1};
    bool stopped{};
    std::string last_persistence_error;
};
namespace {
[[nodiscard]] RuntimeScheduler::Duration delay_until(std::chrono::system_clock::time_point due_at) {
    const auto now = std::chrono::system_clock::now();
    if (due_at <= now) {
        return RuntimeScheduler::Duration::zero();
    }
    return std::chrono::duration_cast<RuntimeScheduler::Duration>(due_at - now);
}
[[nodiscard]] std::chrono::system_clock::time_point
next_fixed_occurrence(std::chrono::system_clock::time_point previous_due,
                      std::chrono::milliseconds interval,
                      std::chrono::system_clock::time_point now) {
    if (interval <= std::chrono::milliseconds::zero()) {
        return previous_due;
    }
    auto next = previous_due + interval;
    if (next > now) {
        return next;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - next);
    const auto skipped = elapsed.count() / interval.count() + 1;
    return next + interval * skipped;
}
[[nodiscard]] std::optional<std::chrono::system_clock::time_point>
next_regular_occurrence(const Automation &automation, std::chrono::system_clock::time_point now) {
    if (automation.schedule_kind == AutomationScheduleKind::fixed_interval) {
        return next_fixed_occurrence(automation.due_at, automation.repeat_interval, now);
    }
    if (automation.schedule_kind == AutomationScheduleKind::local_calendar) {
        return next_calendar_occurrence(automation.calendar_schedule, now);
    }
    return std::nullopt;
}
[[nodiscard]] bool rebase_local_calendar_state(Automation &automation,
                                               std::chrono::system_clock::time_point now) {
    if (automation.schedule_kind != AutomationScheduleKind::local_calendar) {
        return true;
    }
    const auto next = next_calendar_occurrence(automation.calendar_schedule, now);
    if (!next) {
        return false;
    }
    if (automation.retry_pending()) {
        automation.retry_resume_at = *next;
    } else {
        automation.due_at = *next;
    }
    return true;
}
void validate_automation_input(const ActionRegistry &actions, std::wstring_view name,
                               std::wstring_view action_name) {
    if (name.empty()) {
        throw std::invalid_argument{"Automation name must not be empty."};
    }
    if (name.size() > 512) {
        throw std::invalid_argument{"Automation name must be <= 512 characters."};
    }
    if (!actions.contains(action_name)) {
        throw std::invalid_argument{"Automation action is not registered."};
    }
}
[[nodiscard]] std::pair<AutomationId, std::vector<Automation>>
persistent_snapshot(const std::shared_ptr<AutomationEngine::State> &state) {
    std::pair<AutomationId, std::vector<Automation>> snapshot;
    std::scoped_lock lock{state->mutex};
    snapshot.first = state->next_id;
    snapshot.second.reserve(state->entries.size());
    for (const auto &[id, entry] : state->entries) {
        (void)id;
        snapshot.second.push_back(entry.automation);
    }
    std::sort(snapshot.second.begin(), snapshot.second.end(),
              [](const Automation &lhs, const Automation &rhs) { return lhs.id < rhs.id; });
    return snapshot;
}
void persist_state(const std::shared_ptr<AutomationEngine::State> &state) {
    AutomationStore *store = nullptr;
    {
        std::scoped_lock lock{state->mutex};
        store = state->store;
    }
    if (store == nullptr) {
        return;
    }
    const auto [next_id, automations] = persistent_snapshot(state);
    store->save(next_id, automations);
    std::scoped_lock lock{state->mutex};
    state->last_persistence_error.clear();
}
void record_persistence_error(const std::shared_ptr<AutomationEngine::State> &state,
                              const std::exception &exception) noexcept {
    std::scoped_lock lock{state->mutex};
    state->last_persistence_error = exception.what();
}
struct ExecutionLeaseToken final {
    std::shared_ptr<AutomationEngine::State> state;
    ~ExecutionLeaseToken() {
        if (!state) {
            return;
        }
        bool notify = false;
        {
            std::scoped_lock lock{state->mutex};
            if (state->in_flight != 0) {
                --state->in_flight;
            }
            notify = state->in_flight == 0;
        }
        if (notify) {
            state->work_idle.notify_all();
        }
    }
};
void arm_automation(const std::weak_ptr<AutomationEngine::State> &weak_state, AutomationId id);
void complete_automation_execution(const std::weak_ptr<AutomationEngine::State> &weak_state,
                                   Automation fired, bool recurring, ActionResult result,
                                   std::chrono::milliseconds duration, bool retryable = true) {
    const auto state = weak_state.lock();
    if (!state) {
        return;
    }
    const bool was_retry = fired.retry_pending();
    fired.run_count += 1;
    fired.last_run_at = std::chrono::system_clock::now();
    fired.last_success = result.success;
    if (!result.success) {
        fired.failure_count += 1;
        fired.consecutive_failures += 1;
        fired.last_error = result.message;
    } else {
        fired.consecutive_failures = 0;
        fired.retry_attempt = 0;
        fired.retry_resume_at.reset();
        fired.last_error.clear();
    }
    if (state->diagnostics != nullptr) {
        try {
            std::wstring message = L"#" + std::to_wstring(fired.id) + L" " + fired.name +
                                   L"; action=" + fired.action_name + L"; " +
                                   (result.success ? std::wstring{L"completed"}
                                                   : std::wstring{L"failed: "} + result.message);
            [[maybe_unused]] const auto diagnostic_id = state->diagnostics->record(
                result.success ? DiagnosticSeverity::info : DiagnosticSeverity::error,
                DiagnosticDomain::automation, std::to_wstring(fired.id), std::move(message),
                duration);
        } catch (...) {
        }
    }
    bool should_arm = false;
    bool retry_scheduled = false;
    std::chrono::milliseconds retry_delay{};
    bool persist_after_completion = recurring;
    {
        std::scoped_lock mutation_lock{state->mutation_mutex};
        {
            std::scoped_lock lock{state->mutex};
            const bool can_retry = !result.success && retryable &&
                                   fired.failure_policy.retries_enabled() &&
                                   fired.retry_attempt < fired.failure_policy.max_retries;
            if (can_retry) {
                const auto next_attempt = fired.retry_attempt + 1;
                retry_delay = retry_delay_for(fired.failure_policy, next_attempt);
                fired.retry_attempt = next_attempt;
                fired.due_at = std::chrono::system_clock::now() + retry_delay;
                if (recurring) {
                    const auto it = state->entries.find(fired.id);
                    if (it != state->entries.end()) {
                        fired.retry_resume_at = it->second.automation.due_at;
                        const bool enabled = it->second.automation.enabled;
                        const auto runtime_task = it->second.runtime_task;
                        const auto arm_generation = it->second.arm_generation;
                        it->second.automation = fired;
                        it->second.automation.enabled = enabled;
                        it->second.runtime_task = runtime_task;
                        it->second.arm_generation = arm_generation;
                        should_arm = enabled;
                        retry_scheduled = true;
                    }
                } else {
                    fired.retry_resume_at.reset();
                    state->entries[fired.id] = AutomationEngine::State::Entry{fired};
                    should_arm = fired.enabled;
                    retry_scheduled = true;
                    persist_after_completion = true;
                }
            } else if (recurring) {
                const auto it = state->entries.find(fired.id);
                if (it != state->entries.end()) {
                    it->second.automation.run_count = fired.run_count;
                    it->second.automation.failure_count = fired.failure_count;
                    it->second.automation.consecutive_failures = fired.consecutive_failures;
                    it->second.automation.last_run_at = fired.last_run_at;
                    it->second.automation.last_success = fired.last_success;
                    it->second.automation.last_error = fired.last_error;
                    it->second.automation.retry_attempt = 0;
                    it->second.automation.retry_resume_at.reset();
                    if (!result.success && fired.failure_policy.disable_on_exhaustion &&
                        fired.failure_policy.retries_enabled()) {
                        it->second.automation.enabled = false;
                    }
                    if (was_retry && it->second.automation.enabled &&
                        it->second.automation.due_at <= std::chrono::system_clock::now()) {
                        const auto now = std::chrono::system_clock::now();
                        const auto next_due = next_regular_occurrence(it->second.automation, now);
                        if (next_due) {
                            it->second.automation.due_at = *next_due;
                        } else {
                            it->second.automation.enabled = false;
                            if (state->diagnostics != nullptr) {
                                try {
                                    [[maybe_unused]] const auto diagnostic_id =
                                        state->diagnostics->record(
                                            DiagnosticSeverity::error, DiagnosticDomain::automation,
                                            std::to_wstring(fired.id),
                                            L"Recurring automation disabled because its post-retry "
                                            L"schedule could not be resolved.");
                                } catch (...) {
                                }
                            }
                        }
                    }
                    should_arm = it->second.automation.enabled;
                }
            }
        }
        if (persist_after_completion) {
            try {
                persist_state(state);
            } catch (const std::exception &exception) {
                record_persistence_error(state, exception);
                should_arm = false;
                if (state->diagnostics != nullptr) {
                    try {
                        [[maybe_unused]] const auto diagnostic_id = state->diagnostics->record(
                            DiagnosticSeverity::error, DiagnosticDomain::automation,
                            std::to_wstring(fired.id),
                            L"Post-execution automation state could not be persisted.");
                    } catch (...) {
                    }
                }
            }
        }
    }
    if (retry_scheduled && state->diagnostics != nullptr) {
        try {
            [[maybe_unused]] const auto diagnostic_id = state->diagnostics->record(
                DiagnosticSeverity::warning, DiagnosticDomain::automation,
                std::to_wstring(fired.id),
                L"Retry " + std::to_wstring(fired.retry_attempt) + L"/" +
                    std::to_wstring(fired.failure_policy.max_retries) + L" scheduled in " +
                    std::to_wstring(retry_delay.count()) + L" ms.");
        } catch (...) {
        }
    }
    if (should_arm) {
        arm_automation(weak_state, fired.id);
    }
    AutomationEngine::ExecutionCallback callback;
    {
        std::scoped_lock lock{state->mutex};
        callback = state->execution_callback;
    }
    if (callback) {
        try {
            callback(fired, result);
        } catch (...) {
            if (state->diagnostics != nullptr) {
                try {
                    [[maybe_unused]] const auto diagnostic_id = state->diagnostics->record(
                        DiagnosticSeverity::error, DiagnosticDomain::automation,
                        std::to_wstring(fired.id),
                        L"Automation completion observer threw an exception; execution state was "
                        L"already committed.");
                } catch (...) {
                }
            }
        }
    }
}
void execute_automation(const std::weak_ptr<AutomationEngine::State> &weak_state, Automation fired,
                        bool recurring) {
    const auto state = weak_state.lock();
    if (!state) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    ActionResult result;
    if (state->actions == nullptr) {
        result = {false, L"Automation action registry is unavailable."};
    } else {
        result = state->actions->invoke(fired.action_name, fired.action_payload);
    }
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    complete_automation_execution(weak_state, std::move(fired), recurring, std::move(result),
                                  duration);
}
void fire_automation(const std::weak_ptr<AutomationEngine::State> &weak_state, AutomationId id,
                     std::uint64_t arm_generation) {
    const auto state = weak_state.lock();
    if (!state) {
        return;
    }
    Automation fired;
    bool recurring = false;
    bool preflight_persisted = true;
    auto execution_lease = std::make_shared<ExecutionLeaseToken>();
    {
        std::scoped_lock mutation_lock{state->mutation_mutex};
        {
            std::scoped_lock lock{state->mutex};
            if (state->stopped) {
                return;
            }
            const auto it = state->entries.find(id);
            if (it == state->entries.end() || it->second.arm_generation != arm_generation ||
                !it->second.automation.enabled) {
                return;
            }
            fired = it->second.automation;
            recurring = fired.recurring();
            ++state->in_flight;
            execution_lease->state = state;
            if (recurring) {
                if (fired.retry_pending() && fired.retry_resume_at) {
                    it->second.automation.due_at = *fired.retry_resume_at;
                } else {
                    const auto next_due =
                        next_regular_occurrence(fired, std::chrono::system_clock::now());
                    if (next_due) {
                        it->second.automation.due_at = *next_due;
                    } else {
                        it->second.automation.enabled = false;
                        if (state->diagnostics != nullptr) {
                            try {
                                [[maybe_unused]] const auto diagnostic_id =
                                    state->diagnostics->record(
                                        DiagnosticSeverity::error, DiagnosticDomain::automation,
                                        std::to_wstring(fired.id),
                                        L"Recurring automation disabled because its next calendar "
                                        L"occurrence could not be resolved.");
                            } catch (...) {
                            }
                        }
                    }
                }
                it->second.automation.retry_attempt = 0;
                it->second.automation.retry_resume_at.reset();
                it->second.runtime_task = 0;
            } else {
                state->entries.erase(it);
            }
        }
        try {
            persist_state(state);
        } catch (const std::exception &exception) {
            preflight_persisted = false;
            record_persistence_error(state, exception);
        }
    }
    if (!preflight_persisted) {
        complete_automation_execution(
            weak_state, std::move(fired), recurring,
            ActionResult{
                false, L"Automation action skipped because durable state could not be committed."},
            {}, false);
        return;
    }
    if (state->executor == nullptr) {
        complete_automation_execution(
            weak_state, std::move(fired), recurring,
            ActionResult{false, L"Automation execution boundary is unavailable."}, {}, false);
        return;
    }
    Automation rejected = fired;
    const bool queued = state->executor->submit(
        [weak_state, fired = std::move(fired), recurring, execution_lease]() mutable {
            execute_automation(weak_state, std::move(fired), recurring);
        });
    if (!queued) {
        if (state->diagnostics != nullptr) {
            try {
                [[maybe_unused]] const auto diagnostic_id = state->diagnostics->record(
                    DiagnosticSeverity::error, DiagnosticDomain::runtime, L"executor",
                    L"Rejected automation #" + std::to_wstring(id) +
                        L" because the action execution queue is full or stopping.");
            } catch (...) {
            }
        }
        complete_automation_execution(
            weak_state, std::move(rejected), recurring,
            ActionResult{false, L"Automation execution queue is full or stopping."}, {}, true);
    }
}
void arm_automation(const std::weak_ptr<AutomationEngine::State> &weak_state, AutomationId id) {
    const auto state = weak_state.lock();
    if (!state) {
        return;
    }
    std::chrono::system_clock::time_point due_at;
    std::uint64_t arm_generation = 0;
    {
        std::scoped_lock lock{state->mutex};
        if (state->stopped || state->scheduler == nullptr || state->actions == nullptr) {
            return;
        }
        const auto it = state->entries.find(id);
        if (it == state->entries.end() || !it->second.automation.enabled ||
            !state->actions->contains(it->second.automation.action_name)) {
            return;
        }
        due_at = it->second.automation.due_at;
        arm_generation = ++it->second.arm_generation;
    }
    const auto task =
        state->scheduler->schedule_after(delay_until(due_at), [weak_state, id, arm_generation] {
            fire_automation(weak_state, id, arm_generation);
        });
    bool keep_task = false;
    {
        std::scoped_lock lock{state->mutex};
        const auto it = state->entries.find(id);
        if (!state->stopped && it != state->entries.end() && it->second.automation.enabled &&
            it->second.arm_generation == arm_generation) {
            it->second.runtime_task = task;
            keep_task = true;
        }
    }
    if (!keep_task) {
        (void)state->scheduler->cancel(task);
    }
}
} // namespace
AutomationEngine::AutomationEngine(RuntimeScheduler &scheduler, BoundedExecutor &executor,
                                   AutomationStore &store, ActionRegistry &actions,
                                   ExecutionDiagnostics *diagnostics,
                                   ExecutionCallback execution_callback)
    : scheduler_{scheduler}, executor_{executor}, store_{store}, actions_{actions},
      state_{std::make_shared<State>()} {
    state_->scheduler = &scheduler_;
    state_->executor = &executor_;
    state_->store = &store_;
    state_->actions = &actions_;
    state_->diagnostics = diagnostics;
    state_->execution_callback = std::move(execution_callback);
}
AutomationEngine::~AutomationEngine() {
    stop();
    state_.reset();
}
AutomationId AutomationEngine::schedule_after(Duration delay, std::wstring name,
                                              std::wstring action_name,
                                              std::wstring action_payload) {
    if (delay < Duration::zero()) {
        delay = Duration::zero();
    }
    return schedule_at(std::chrono::system_clock::now() + delay, std::move(name),
                       std::move(action_name), std::move(action_payload));
}
AutomationId AutomationEngine::schedule_at(std::chrono::system_clock::time_point due_at,
                                           std::wstring name, std::wstring action_name,
                                           std::wstring action_payload) {
    validate_automation_input(actions_, name, action_name);
    AutomationId id = 0;
    {
        std::scoped_lock mutation_lock{state_->mutation_mutex};
        {
            std::scoped_lock lock{state_->mutex};
            if (state_->stopped) {
                throw std::runtime_error{"AutomationEngine is stopped."};
            }
            if (state_->next_id == 0 ||
                state_->next_id == std::numeric_limits<AutomationId>::max()) {
                throw std::overflow_error{"Automation IDs exhausted."};
            }
            id = state_->next_id++;
            Automation automation;
            automation.id = id;
            automation.due_at = due_at;
            automation.schedule_kind = AutomationScheduleKind::one_shot;
            automation.name = std::move(name);
            automation.action_name = std::move(action_name);
            automation.action_payload = std::move(action_payload);
            state_->entries.emplace(id, State::Entry{std::move(automation)});
        }
        try {
            persist_state(state_);
        } catch (...) {
            std::scoped_lock lock{state_->mutex};
            state_->entries.erase(id);
            if (state_->next_id == id + 1) {
                state_->next_id = id;
            }
            throw;
        }
    }
    arm_automation(state_, id);
    return id;
}
AutomationId AutomationEngine::schedule_every(Duration interval, std::wstring name,
                                              std::wstring action_name,
                                              std::wstring action_payload) {
    if (interval <= Duration::zero()) {
        throw std::invalid_argument{"Automation recurring interval must be positive."};
    }
    validate_automation_input(actions_, name, action_name);
    AutomationId id = 0;
    {
        std::scoped_lock mutation_lock{state_->mutation_mutex};
        {
            std::scoped_lock lock{state_->mutex};
            if (state_->stopped) {
                throw std::runtime_error{"AutomationEngine is stopped."};
            }
            if (state_->next_id == 0 ||
                state_->next_id == std::numeric_limits<AutomationId>::max()) {
                throw std::overflow_error{"Automation IDs exhausted."};
            }
            id = state_->next_id++;
            Automation automation;
            automation.id = id;
            automation.due_at = std::chrono::system_clock::now() + interval;
            automation.schedule_kind = AutomationScheduleKind::fixed_interval;
            automation.repeat_interval = interval;
            automation.name = std::move(name);
            automation.action_name = std::move(action_name);
            automation.action_payload = std::move(action_payload);
            state_->entries.emplace(id, State::Entry{std::move(automation)});
        }
        try {
            persist_state(state_);
        } catch (...) {
            std::scoped_lock lock{state_->mutex};
            state_->entries.erase(id);
            if (state_->next_id == id + 1) {
                state_->next_id = id;
            }
            throw;
        }
    }
    arm_automation(state_, id);
    return id;
}
AutomationId AutomationEngine::schedule_calendar(AutomationCalendarSchedule schedule,
                                                 std::wstring name, std::wstring action_name,
                                                 std::wstring action_payload) {
    std::string schedule_error;
    if (!valid_calendar_schedule(schedule, &schedule_error)) {
        throw std::invalid_argument{schedule_error};
    }
    validate_automation_input(actions_, name, action_name);
    const auto due_at = next_calendar_occurrence(schedule, std::chrono::system_clock::now());
    if (!due_at) {
        throw std::runtime_error{"Unable to resolve the next local calendar occurrence."};
    }
    AutomationId id = 0;
    {
        std::scoped_lock mutation_lock{state_->mutation_mutex};
        {
            std::scoped_lock lock{state_->mutex};
            if (state_->stopped) {
                throw std::runtime_error{"AutomationEngine is stopped."};
            }
            if (state_->next_id == 0 ||
                state_->next_id == std::numeric_limits<AutomationId>::max()) {
                throw std::overflow_error{"Automation IDs exhausted."};
            }
            id = state_->next_id++;
            Automation automation;
            automation.id = id;
            automation.due_at = *due_at;
            automation.schedule_kind = AutomationScheduleKind::local_calendar;
            automation.calendar_schedule = std::move(schedule);
            automation.name = std::move(name);
            automation.action_name = std::move(action_name);
            automation.action_payload = std::move(action_payload);
            state_->entries.emplace(id, State::Entry{std::move(automation)});
        }
        try {
            persist_state(state_);
        } catch (...) {
            std::scoped_lock lock{state_->mutex};
            state_->entries.erase(id);
            if (state_->next_id == id + 1) {
                state_->next_id = id;
            }
            throw;
        }
    }
    arm_automation(state_, id);
    return id;
}
bool AutomationEngine::cancel(AutomationId id) {
    RuntimeTaskId runtime_task = 0;
    State::Entry removed;
    {
        std::scoped_lock mutation_lock{state_->mutation_mutex};
        {
            std::scoped_lock lock{state_->mutex};
            const auto it = state_->entries.find(id);
            if (it == state_->entries.end()) {
                return false;
            }
            removed = it->second;
            runtime_task = removed.runtime_task;
            state_->entries.erase(it);
        }
        try {
            persist_state(state_);
        } catch (...) {
            std::scoped_lock lock{state_->mutex};
            state_->entries.emplace(id, std::move(removed));
            throw;
        }
    }
    if (runtime_task != 0) {
        (void)scheduler_.cancel(runtime_task);
    }
    return true;
}
bool AutomationEngine::set_failure_policy(AutomationId id, AutomationFailurePolicy policy) {
    std::string policy_error;
    if (!valid_failure_policy(policy, &policy_error)) {
        throw std::invalid_argument{policy_error};
    }
    State::Entry previous_entry;
    {
        std::scoped_lock mutation_lock{state_->mutation_mutex};
        {
            std::scoped_lock lock{state_->mutex};
            const auto it = state_->entries.find(id);
            if (it == state_->entries.end()) {
                return false;
            }
            previous_entry = it->second;
            if (it->second.automation.retry_pending() &&
                policy.max_retries < it->second.automation.retry_attempt) {
                throw std::invalid_argument{
                    "Cannot reduce retry policy below an already-pending retry attempt."};
            }
            it->second.automation.failure_policy = policy;
        }
        try {
            persist_state(state_);
        } catch (...) {
            std::scoped_lock lock{state_->mutex};
            const auto it = state_->entries.find(id);
            if (it != state_->entries.end()) {
                it->second = previous_entry;
            }
            throw;
        }
    }
    return true;
}
bool AutomationEngine::set_enabled(AutomationId id, bool enabled) {
    RuntimeTaskId runtime_task = 0;
    bool changed = false;
    State::Entry previous_entry;
    {
        std::scoped_lock mutation_lock{state_->mutation_mutex};
        {
            std::scoped_lock lock{state_->mutex};
            const auto it = state_->entries.find(id);
            if (it == state_->entries.end()) {
                return false;
            }
            if (enabled && !actions_.contains(it->second.automation.action_name)) {
                throw std::runtime_error{
                    "Cannot enable automation because its action is unavailable."};
            }
            if (it->second.automation.enabled == enabled) {
                return true;
            }
            previous_entry = it->second;
            if (enabled &&
                it->second.automation.schedule_kind == AutomationScheduleKind::local_calendar &&
                !rebase_local_calendar_state(it->second.automation,
                                             std::chrono::system_clock::now())) {
                throw std::runtime_error{"Cannot enable local-calendar automation because its next "
                                         "occurrence could not be resolved."};
            }
            it->second.automation.enabled = enabled;
            runtime_task = it->second.runtime_task;
            if (!enabled) {
                it->second.runtime_task = 0;
                ++it->second.arm_generation;
            }
            changed = true;
        }
        try {
            persist_state(state_);
        } catch (...) {
            std::scoped_lock lock{state_->mutex};
            const auto it = state_->entries.find(id);
            if (it != state_->entries.end()) {
                it->second = previous_entry;
            }
            throw;
        }
    }
    if (changed && !enabled && runtime_task != 0) {
        (void)scheduler_.cancel(runtime_task);
    } else if (changed && enabled) {
        arm_automation(state_, id);
    }
    return true;
}
void AutomationEngine::clear() {
    std::vector<RuntimeTaskId> tasks;
    std::unordered_map<AutomationId, State::Entry> previous;
    {
        std::scoped_lock mutation_lock{state_->mutation_mutex};
        {
            std::scoped_lock lock{state_->mutex};
            previous = state_->entries;
            tasks.reserve(previous.size());
            for (const auto &[id, entry] : previous) {
                (void)id;
                if (entry.runtime_task != 0) {
                    tasks.push_back(entry.runtime_task);
                }
            }
            state_->entries.clear();
        }
        try {
            persist_state(state_);
        } catch (...) {
            std::scoped_lock lock{state_->mutex};
            state_->entries = std::move(previous);
            throw;
        }
    }
    for (const auto task : tasks) {
        (void)scheduler_.cancel(task);
    }
}
void AutomationEngine::stop() noexcept {
    std::vector<RuntimeTaskId> tasks;
    {
        std::scoped_lock lock{state_->mutex};
        if (!state_->stopped) {
            state_->stopped = true;
            for (auto &[id, entry] : state_->entries) {
                (void)id;
                ++entry.arm_generation;
                if (entry.runtime_task != 0) {
                    tasks.push_back(entry.runtime_task);
                    entry.runtime_task = 0;
                }
            }
        }
    }
    for (const auto task : tasks) {
        (void)scheduler_.cancel(task);
    }
    try {
        std::unique_lock lock{state_->mutex};
        state_->work_idle.wait(lock, [this] { return state_->in_flight == 0; });
    } catch (...) {
    }
}
void AutomationEngine::set_execution_callback(ExecutionCallback callback) {
    std::scoped_lock lock{state_->mutex};
    state_->execution_callback = std::move(callback);
}
AutomationActionReconcileReport AutomationEngine::reconcile_action_availability() {
    AutomationActionReconcileReport report;
    std::vector<RuntimeTaskId> to_cancel;
    std::vector<AutomationId> to_arm;
    {
        std::scoped_lock lock{state_->mutex};
        if (state_->stopped) {
            return report;
        }
        const auto now = std::chrono::system_clock::now();
        for (auto &[id, entry] : state_->entries) {
            if (!entry.automation.enabled) {
                continue;
            }
            const bool available = actions_.contains(entry.automation.action_name);
            if (!available) {
                ++report.unresolved;
                if (entry.runtime_task != 0) {
                    to_cancel.push_back(entry.runtime_task);
                    entry.runtime_task = 0;
                    ++entry.arm_generation;
                    ++report.parked;
                }
            } else if (entry.runtime_task == 0) {
                if (entry.automation.schedule_kind == AutomationScheduleKind::local_calendar &&
                    !rebase_local_calendar_state(entry.automation, now)) {
                    if (state_->diagnostics != nullptr) {
                        try {
                            [[maybe_unused]] const auto diagnostic_id = state_->diagnostics->record(
                                DiagnosticSeverity::error, DiagnosticDomain::automation,
                                std::to_wstring(id),
                                L"Local-calendar automation could not resolve a future occurrence "
                                L"while re-arming.");
                        } catch (...) {
                        }
                    }
                    continue;
                }
                to_arm.push_back(id);
                ++report.armed;
            }
        }
    }
    for (const auto task : to_cancel) {
        (void)scheduler_.cancel(task);
    }
    for (const auto id : to_arm) {
        arm_automation(state_, id);
    }
    return report;
}
AutomationCalendarRefreshReport AutomationEngine::refresh_calendar_schedules() {
    AutomationCalendarRefreshReport report;
    std::vector<RuntimeTaskId> to_cancel;
    std::vector<AutomationId> to_arm;
    {
        std::scoped_lock lock{state_->mutex};
        if (state_->stopped) {
            return report;
        }
        const auto now = std::chrono::system_clock::now();
        for (auto &[id, entry] : state_->entries) {
            if (!entry.automation.enabled ||
                entry.automation.schedule_kind != AutomationScheduleKind::local_calendar) {
                continue;
            }
            const auto next = next_calendar_occurrence(entry.automation.calendar_schedule, now);
            if (!next) {
                if (entry.runtime_task != 0) {
                    to_cancel.push_back(entry.runtime_task);
                    entry.runtime_task = 0;
                    ++entry.arm_generation;
                }
                ++report.parked;
                if (state_->diagnostics != nullptr) {
                    try {
                        [[maybe_unused]] const auto diagnostic_id = state_->diagnostics->record(
                            DiagnosticSeverity::error, DiagnosticDomain::automation,
                            std::to_wstring(id),
                            L"Local-calendar automation was parked because a future occurrence "
                            L"could not be resolved after a time change.");
                    } catch (...) {
                    }
                }
                continue;
            }
            if (entry.automation.retry_pending()) {
                if (!entry.automation.retry_resume_at ||
                    *entry.automation.retry_resume_at != *next) {
                    entry.automation.retry_resume_at = *next;
                    ++report.retry_resume_rebased;
                }
                continue;
            }
            if (entry.automation.due_at == *next) {
                continue;
            }
            entry.automation.due_at = *next;
            ++report.due_rebased;
            if (entry.runtime_task != 0) {
                to_cancel.push_back(entry.runtime_task);
                entry.runtime_task = 0;
                ++entry.arm_generation;
            }
            if (actions_.contains(entry.automation.action_name)) {
                to_arm.push_back(id);
            }
        }
    }
    for (const auto task : to_cancel) {
        (void)scheduler_.cancel(task);
    }
    for (const auto id : to_arm) {
        arm_automation(state_, id);
    }
    if (state_->diagnostics != nullptr &&
        (report.due_rebased != 0 || report.retry_resume_rebased != 0 || report.parked != 0)) {
        try {
            [[maybe_unused]] const auto diagnostic_id = state_->diagnostics->record(
                report.parked == 0 ? DiagnosticSeverity::info : DiagnosticSeverity::warning,
                DiagnosticDomain::automation, L"calendar",
                L"Local-calendar schedules refreshed: due=" + std::to_wstring(report.due_rebased) +
                    L", retry-resume=" + std::to_wstring(report.retry_resume_rebased) +
                    L", parked=" + std::to_wstring(report.parked) + L".");
        } catch (...) {
        }
    }
    return report;
}
AutomationRestoreReport AutomationEngine::restore() {
    const auto loaded = store_.load();
    AutomationRestoreReport report;
    std::vector<AutomationId> to_arm;
    {
        std::scoped_lock mutation_lock{state_->mutation_mutex};
        std::scoped_lock lock{state_->mutex};
        if (state_->stopped) {
            throw std::runtime_error{"AutomationEngine is stopped."};
        }
        if (!state_->entries.empty()) {
            throw std::runtime_error{"AutomationEngine restore requires an empty engine."};
        }
        state_->next_id = loaded.next_id == 0 ? 1 : loaded.next_id;
        const auto now = std::chrono::system_clock::now();
        for (auto automation : loaded.automations) {
            if (automation.enabled &&
                automation.schedule_kind == AutomationScheduleKind::local_calendar) {
                const bool retry_pending = automation.retry_pending();
                if (rebase_local_calendar_state(automation, now)) {
                    if (retry_pending) {
                        ++report.calendar_retry_resumes_rebased;
                    } else {
                        ++report.calendar_due_rebased;
                    }
                } else {
                    automation.enabled = false;
                    ++report.calendar_resolution_failures;
                }
            }
            state_->entries.emplace(automation.id, State::Entry{automation});
            ++report.restored;
            if (!automation.enabled) {
                ++report.disabled;
                continue;
            }
            if (!actions_.contains(automation.action_name)) {
                ++report.unresolved_actions;
                continue;
            }
            if (automation.due_at <= now) {
                ++report.overdue;
            }
            to_arm.push_back(automation.id);
        }
    }
    for (const auto id : to_arm) {
        arm_automation(state_, id);
    }
    return report;
}
std::vector<Automation> AutomationEngine::snapshot() const {
    std::scoped_lock lock{state_->mutex};
    std::vector<Automation> output;
    output.reserve(state_->entries.size());
    for (const auto &[id, entry] : state_->entries) {
        (void)id;
        output.push_back(entry.automation);
    }
    std::sort(output.begin(), output.end(), [](const Automation &lhs, const Automation &rhs) {
        if (lhs.due_at != rhs.due_at) {
            return lhs.due_at < rhs.due_at;
        }
        return lhs.id < rhs.id;
    });
    return output;
}
std::optional<Automation> AutomationEngine::find(AutomationId id) const {
    std::scoped_lock lock{state_->mutex};
    const auto it = state_->entries.find(id);
    if (it == state_->entries.end()) {
        return std::nullopt;
    }
    return it->second.automation;
}
AutomationEngineStats AutomationEngine::stats() const {
    std::scoped_lock lock{state_->mutex};
    AutomationEngineStats output;
    output.total = state_->entries.size();
    for (const auto &[id, entry] : state_->entries) {
        (void)id;
        const auto &automation = entry.automation;
        if (automation.enabled)
            ++output.enabled;
        else
            ++output.disabled;
        if (automation.recurring())
            ++output.recurring;
        if (automation.schedule_kind == AutomationScheduleKind::local_calendar)
            ++output.calendar;
        if (automation.failure_policy.retries_enabled())
            ++output.retry_enabled;
        if (automation.retry_pending())
            ++output.retrying;
        if (!actions_.contains(automation.action_name))
            ++output.unresolved_actions;
        output.total_runs += automation.run_count;
        output.total_failures += automation.failure_count;
    }
    return output;
}
AutomationStoreStatus AutomationEngine::persistence_status() const {
    return store_.status();
}
std::string AutomationEngine::last_persistence_error() const {
    std::scoped_lock lock{state_->mutex};
    return state_->last_persistence_error;
}
} // namespace axiom
