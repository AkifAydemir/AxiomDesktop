#include "action_registry.hpp"
#include "automation_engine.hpp"
#include "automation_store.hpp"
#include "runtime.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>
namespace {
using namespace std::chrono_literals;
void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}
std::filesystem::path unique_test_path() {
    auto path = std::filesystem::temp_directory_path();
    path /= "axiom-automation-engine-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin";
    return path;
}
void cleanup(const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    auto temp = path;
    temp += ".tmp";
    std::filesystem::remove(temp, error);
    auto backup = path;
    backup += ".bak";
    std::filesystem::remove(backup, error);
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
bool within_one_second(std::chrono::system_clock::time_point lhs,
                       std::chrono::system_clock::time_point rhs) {
    auto delta = lhs - rhs;
    if (delta < std::chrono::system_clock::duration::zero()) {
        delta = -delta;
    }
    return delta <= 1s;
}
void run_tests() {
    const auto path = unique_test_path();
    cleanup(path);
    try {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<std::wstring> executions;
        axiom::ExecutionDiagnostics diagnostics{128};
        axiom::ActionRegistry actions{&diagnostics};
        expect(actions.register_action(L"capture", L"Capture payload for tests.",
                                       [&](std::wstring_view payload) {
                                           {
                                               std::scoped_lock lock{mutex};
                                               executions.emplace_back(payload);
                                           }
                                           cv.notify_all();
                                           return axiom::ActionResult{true, L"captured"};
                                       }),
               "capture action should register");
        expect(actions.register_action(L"fail", L"Always fail.",
                                       [](std::wstring_view) {
                                           return axiom::ActionResult{false,
                                                                      L"intentional failure"};
                                       }),
               "failure action should register");
        std::atomic<int> flaky_attempts{0};
        expect(actions.register_action(L"flaky", L"Fail twice then succeed.",
                                       [&](std::wstring_view) {
                                           const auto attempt = ++flaky_attempts;
                                           return attempt < 3
                                                      ? axiom::ActionResult{false,
                                                                            L"transient failure"}
                                                      : axiom::ActionResult{true, L"recovered"};
                                       }),
               "flaky action should register");
        std::atomic<int> retry_once_attempts{0};
        expect(actions.register_action(L"retry-once", L"Fail once then succeed.",
                                       [&](std::wstring_view) {
                                           const auto attempt = ++retry_once_attempts;
                                           return attempt == 1
                                                      ? axiom::ActionResult{false, L"retry once"}
                                                      : axiom::ActionResult{true, L"recovered"};
                                       }),
               "retry-once action should register");
        std::atomic_bool slow_action_done{false};
        expect(actions.register_action(L"slow", L"Sleep to verify scheduler isolation.",
                                       [&](std::wstring_view) {
                                           std::this_thread::sleep_for(180ms);
                                           slow_action_done.store(true);
                                           return axiom::ActionResult{true, L"slow completed"};
                                       }),
               "slow action should register");
        axiom::RuntimeScheduler scheduler;
        axiom::BoundedExecutor executor{2, 16};
        axiom::AutomationStore store{path};
        axiom::AutomationEngine engine{scheduler, executor, store, actions, &diagnostics};
        const auto restored = engine.restore();
        expect(restored.restored == 0, "new automation engine should restore empty");
        std::atomic_bool scheduler_tick{false};
        [[maybe_unused]] const auto slow_id =
            engine.schedule_after(0ms, L"slow isolation", L"slow", L"");
        [[maybe_unused]] const auto timing_task =
            scheduler.schedule_after(25ms, [&] { scheduler_tick.store(true); });
        const auto timing_deadline = std::chrono::steady_clock::now() + 120ms;
        while (!scheduler_tick.load() && std::chrono::steady_clock::now() < timing_deadline) {
            std::this_thread::sleep_for(1ms);
        }
        expect(scheduler_tick.load(), "slow action should not block scheduler timing worker");
        expect(!slow_action_done.load(),
               "scheduler timing task should run before slow action completes");
        const auto slow_deadline = std::chrono::steady_clock::now() + 1s;
        while (!slow_action_done.load() && std::chrono::steady_clock::now() < slow_deadline) {
            std::this_thread::sleep_for(1ms);
        }
        expect(slow_action_done.load(),
               "slow action should eventually complete on execution worker");
        const auto one_shot = engine.schedule_after(30ms, L"one shot", L"capture", L"first");
        {
            std::unique_lock lock{mutex};
            expect(cv.wait_for(lock, 2s, [&] { return executions.size() >= 1; }),
                   "one-shot automation should execute");
        }
        expect(executions.front() == L"first", "one-shot payload should reach action");
        expect(!engine.find(one_shot).has_value(),
               "one-shot automation should be removed before action execution");
        const auto recurring = engine.schedule_every(35ms, L"repeat", L"capture", L"tick");
        {
            std::unique_lock lock{mutex};
            expect(cv.wait_for(lock, 2s, [&] { return executions.size() >= 3; }),
                   "recurring automation should execute repeatedly");
        }
        expect(engine.set_enabled(recurring, false), "recurring automation should disable");
        const auto completion_deadline = std::chrono::steady_clock::now() + 1s;
        while (std::chrono::steady_clock::now() < completion_deadline) {
            const auto current = engine.find(recurring);
            if (current && current->run_count >= 2)
                break;
            std::this_thread::sleep_for(1ms);
        }
        const auto disabled = engine.find(recurring);
        expect(disabled && !disabled->enabled, "disabled automation should remain persistent");
        expect(disabled->run_count >= 2,
               "recurring run count should be retained after in-flight completion");
        const auto fail_id = engine.schedule_every(40ms, L"failure test", L"fail", L"");
        const auto failure_completion_deadline = std::chrono::steady_clock::now() + 1s;
        while (std::chrono::steady_clock::now() < failure_completion_deadline) {
            const auto current = engine.find(fail_id);
            if (current && current->failure_count >= 1)
                break;
            std::this_thread::sleep_for(1ms);
        }
        expect(engine.set_enabled(fail_id, false), "failing recurring automation should disable");
        const auto failed = engine.find(fail_id);
        expect(failed && failed->failure_count >= 1, "automation failures should be counted");
        expect(!failed->last_success, "last success flag should reflect failure");
        expect(failed->last_error == L"intentional failure",
               "last failure diagnostic should persist in memory");
        const auto automation_error =
            diagnostics.last_error(axiom::DiagnosticDomain::automation, std::to_wstring(fail_id));
        expect(automation_error.has_value(),
               "automation failure should be present in session diagnostics");
        expect(automation_error->message.find(L"intentional failure") != std::wstring::npos,
               "automation diagnostic should retain failure reason");
        // Calendar schedules persist explicit local wall-clock semantics rather than a 24h
        // interval.
        axiom::AutomationCalendarSchedule calendar;
        calendar.weekday_mask = 0x7Fu;
        calendar.hour = 23;
        calendar.minute = 59;
        calendar.second = 0;
        calendar.time_zone = L"local";
        const auto calendar_id =
            engine.schedule_calendar(calendar, L"daily local", L"capture", L"calendar");
        const auto calendar_item = engine.find(calendar_id);
        expect(calendar_item &&
                   calendar_item->schedule_kind == axiom::AutomationScheduleKind::local_calendar,
               "calendar automation should retain schedule kind");
        expect(calendar_item->due_at > std::chrono::system_clock::now(),
               "calendar automation should resolve a future local occurrence");
        expect(engine.stats().calendar >= 1, "engine stats should report calendar schedules");
        expect(engine.cancel(calendar_id), "calendar automation cleanup failed");
        // One-shot transient failure retries are durable records between attempts and disappear
        // after success.
        const auto flaky_id = engine.schedule_at(std::chrono::system_clock::now() + 40ms,
                                                 L"flaky one-shot", L"flaky", L"");
        axiom::AutomationFailurePolicy flaky_policy;
        flaky_policy.kind = axiom::AutomationRetryKind::fixed;
        flaky_policy.max_retries = 3;
        flaky_policy.initial_delay = 80ms;
        flaky_policy.max_delay = 80ms;
        expect(engine.set_failure_policy(flaky_id, flaky_policy),
               "one-shot retry policy should apply");
        const auto retry_pending_deadline = std::chrono::steady_clock::now() + 1s;
        bool saw_retry_pending = false;
        while (std::chrono::steady_clock::now() < retry_pending_deadline) {
            const auto current = engine.find(flaky_id);
            if (current && current->retry_attempt >= 1) {
                saw_retry_pending = true;
                expect(current->failure_count >= 1,
                       "retrying one-shot should persist failure metadata");
                expect(engine.stats().retrying >= 1,
                       "engine stats should expose pending retry state");
                break;
            }
            std::this_thread::sleep_for(2ms);
        }
        expect(saw_retry_pending, "one-shot failure should persist a pending retry");
        const auto flaky_done_deadline = std::chrono::steady_clock::now() + 2s;
        while (flaky_attempts.load() < 3 &&
               std::chrono::steady_clock::now() < flaky_done_deadline) {
            std::this_thread::sleep_for(2ms);
        }
        expect(flaky_attempts.load() == 3,
               "fixed retry policy should reach eventual successful attempt");
        for (int i = 0; i < 100 && engine.find(flaky_id).has_value(); ++i)
            std::this_thread::sleep_for(2ms);
        expect(!engine.find(flaky_id).has_value(), "successful retried one-shot should be removed");
        // Exhaustion policy can disable a recurring automation without preempting the running
        // callback.
        const auto disable_on_failure =
            engine.schedule_every(30ms, L"disable after retry", L"fail", L"");
        axiom::AutomationFailurePolicy disable_policy;
        disable_policy.kind = axiom::AutomationRetryKind::fixed;
        disable_policy.max_retries = 1;
        disable_policy.initial_delay = 20ms;
        disable_policy.max_delay = 20ms;
        disable_policy.disable_on_exhaustion = true;
        expect(engine.set_failure_policy(disable_on_failure, disable_policy),
               "disable-on-exhaustion policy should apply");
        const auto disabled_deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < disabled_deadline) {
            const auto current = engine.find(disable_on_failure);
            if (current && !current->enabled && current->failure_count >= 2)
                break;
            std::this_thread::sleep_for(2ms);
        }
        const auto exhausted = engine.find(disable_on_failure);
        expect(exhausted && !exhausted->enabled,
               "recurring automation should disable after retry exhaustion");
        expect(exhausted->failure_count >= 2 && exhausted->retry_attempt == 0,
               "exhausted automation should retain failures and clear transient retry state");
        expect(engine.cancel(disable_on_failure), "exhausted recurring cleanup failed");
        // If retry backoff crosses one or more regular fixed-interval occurrences,
        // a successful retry resumes at the next future regular occurrence instead
        // of immediately replaying a stale due time.
        const auto retry_resume = engine.schedule_every(80ms, L"retry resume", L"retry-once", L"");
        axiom::AutomationFailurePolicy resume_policy;
        resume_policy.kind = axiom::AutomationRetryKind::fixed;
        resume_policy.max_retries = 1;
        resume_policy.initial_delay = 220ms;
        resume_policy.max_delay = 220ms;
        expect(engine.set_failure_policy(retry_resume, resume_policy),
               "retry-resume policy should apply");
        const auto resume_deadline = std::chrono::steady_clock::now() + 2s;
        bool saw_future_resume = false;
        while (std::chrono::steady_clock::now() < resume_deadline) {
            const auto current = engine.find(retry_resume);
            if (current && current->run_count >= 2 && current->retry_attempt == 0) {
                saw_future_resume = current->due_at > std::chrono::system_clock::now();
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        expect(retry_once_attempts.load() >= 2,
               "retry-resume action should execute the retry attempt");
        expect(saw_future_resume,
               "post-retry recurring schedule must skip stale regular occurrences");
        expect(engine.cancel(retry_resume), "retry-resume automation cleanup failed");
        engine.stop();
        executor.stop(true);
        scheduler.stop();
        // Restart: disabled records survive with stable IDs and execution metadata.
        axiom::RuntimeScheduler scheduler2;
        axiom::BoundedExecutor executor2{2, 16};
        axiom::AutomationStore store2{path};
        axiom::AutomationEngine engine2{scheduler2, executor2, store2, actions, &diagnostics};
        const auto report = engine2.restore();
        expect(report.restored == 2, "restart should restore remaining recurring automations");
        expect(report.disabled == 2, "disabled automations should remain disabled after restart");
        const auto restored_repeat = engine2.find(recurring);
        expect(restored_repeat && restored_repeat->id == recurring,
               "automation IDs should remain stable across restart");
        expect(restored_repeat->run_count >= 2, "run metadata should survive restart");
        engine2.clear();
        expect(engine2.snapshot().empty(), "clear should remove all automations");
        engine2.stop();
        executor2.stop(true);
        scheduler2.stop();
        // Unknown actions stay persisted but are not armed on restore.
        axiom::Automation missing;
        missing.id = 77;
        missing.enabled = true;
        missing.due_at = std::chrono::system_clock::now() - 1s;
        missing.name = L"missing action";
        missing.action_name = L"plugin-action";
        axiom::AutomationStore store3{path};
        store3.save(78, std::span<const axiom::Automation>{&missing, 1});
        axiom::RuntimeScheduler scheduler3;
        axiom::BoundedExecutor executor3{2, 16};
        axiom::AutomationEngine engine3{scheduler3, executor3, store3, actions, &diagnostics};
        const auto unresolved = engine3.restore();
        expect(unresolved.restored == 1, "unresolved automation should still restore");
        expect(unresolved.unresolved_actions == 1, "restore should report missing action");
        expect(engine3.stats().unresolved_actions == 1,
               "engine stats should report unresolved action");
        std::this_thread::sleep_for(80ms);
        expect(engine3.find(77).has_value(), "unresolved automation should not fire and disappear");
        std::atomic<int> plugin_runs{0};
        expect(actions.register_action(L"plugin-action", L"temporary plugin action",
                                       [&](std::wstring_view) {
                                           ++plugin_runs;
                                           return axiom::ActionResult{true, L"ok"};
                                       }),
               "plugin action should register");
        const auto available = engine3.reconcile_action_availability();
        expect(available.armed == 1 && available.unresolved == 0,
               "reconcile should arm restored automation when action returns");
        for (int i = 0; i < 100 && plugin_runs.load() == 0; ++i)
            std::this_thread::sleep_for(10ms);
        expect(plugin_runs.load() == 1,
               "restored automation did not run after plugin action returned");
        expect(!engine3.find(77).has_value(),
               "restored one-shot automation should disappear after execution");
        const auto plugin_recurring =
            engine3.schedule_every(120ms, L"plugin recurring", L"plugin-action", L"");
        expect(actions.unregister_action(L"plugin-action"), "plugin action should unregister");
        const auto parked = engine3.reconcile_action_availability();
        expect(parked.parked == 1 && parked.unresolved == 1,
               "reconcile should park plugin automation when action disappears");
        std::this_thread::sleep_for(180ms);
        expect(plugin_runs.load() == 1, "parked plugin automation unexpectedly executed");
        expect(actions.register_action(L"plugin-action", L"temporary plugin action",
                                       [&](std::wstring_view) {
                                           ++plugin_runs;
                                           return axiom::ActionResult{true, L"ok"};
                                       }),
               "plugin action should re-register");
        const auto rearmed = engine3.reconcile_action_availability();
        expect(rearmed.armed == 1 && rearmed.unresolved == 0,
               "reconcile should re-arm plugin automation when action returns");
        for (int i = 0; i < 100 && plugin_runs.load() < 2; ++i)
            std::this_thread::sleep_for(10ms);
        expect(plugin_runs.load() >= 2, "re-armed plugin automation did not resume");
        expect(engine3.cancel(plugin_recurring), "plugin recurring cleanup failed");
        engine3.stop();
        executor3.stop(true);
        scheduler3.stop();
        cleanup(path);
    } catch (...) {
        cleanup(path);
        throw;
    }
}
void test_calendar_restore_rebases_derived_due_state() {
    const auto path = unique_test_path();
    cleanup(path);
    try {
        axiom::ActionRegistry actions;
        expect(actions.register_action(
                   L"noop", L"Calendar restore test action.",
                   [](std::wstring_view) { return axiom::ActionResult{true, L"ok"}; }),
               "calendar restore action should register");
        const auto now = std::chrono::system_clock::now();
        const auto future_local = local_parts(now + 2h);
        axiom::AutomationCalendarSchedule schedule;
        schedule.weekday_mask = 0x7Fu;
        schedule.hour = static_cast<std::uint8_t>(future_local.tm_hour);
        schedule.minute = static_cast<std::uint8_t>(future_local.tm_min);
        schedule.second = static_cast<std::uint8_t>(future_local.tm_sec);
        schedule.time_zone = L"local";
        const auto expected = axiom::next_calendar_occurrence(schedule, now);
        expect(expected.has_value(), "calendar restore fixture should resolve expected due");
        axiom::Automation active;
        active.id = 1;
        active.enabled = true;
        active.due_at = now + 72h; // Deliberately stale absolute cache.
        active.schedule_kind = axiom::AutomationScheduleKind::local_calendar;
        active.calendar_schedule = schedule;
        active.name = L"active local calendar";
        active.action_name = L"noop";
        axiom::Automation disabled = active;
        disabled.id = 2;
        disabled.enabled = false;
        disabled.due_at = now - 24h;
        disabled.name = L"disabled local calendar";
        axiom::Automation retrying = active;
        retrying.id = 3;
        retrying.due_at = now + 30min; // Retry timer itself remains absolute/durable.
        retrying.name = L"retrying local calendar";
        retrying.failure_policy.kind = axiom::AutomationRetryKind::fixed;
        retrying.failure_policy.max_retries = 2;
        retrying.failure_policy.initial_delay = 30min;
        retrying.failure_policy.max_delay = 30min;
        retrying.retry_attempt = 1;
        retrying.retry_resume_at = now + 96h; // Deliberately stale local-calendar resume cache.
        std::vector<axiom::Automation> records{active, disabled, retrying};
        axiom::AutomationStore writer{path};
        writer.save(4, records);
        axiom::RuntimeScheduler scheduler;
        axiom::BoundedExecutor executor{1, 8};
        axiom::AutomationStore reader{path};
        axiom::AutomationEngine engine{scheduler, executor, reader, actions};
        const auto report = engine.restore();
        expect(report.restored == 3, "calendar restore should retain all records");
        expect(report.calendar_due_rebased == 1,
               "enabled calendar due cache should rebase on restore");
        expect(report.calendar_retry_resumes_rebased == 1,
               "calendar retry resume cache should rebase on restore");
        expect(report.calendar_resolution_failures == 0,
               "valid calendar restore should not fail resolution");
        const auto restored_active = engine.find(1);
        expect(restored_active && within_one_second(restored_active->due_at, *expected),
               "calendar restore must derive due_at from current local wall-clock metadata");
        const auto restored_retry = engine.find(3);
        expect(restored_retry && restored_retry->retry_resume_at &&
                   within_one_second(*restored_retry->retry_resume_at, *expected),
               "pending retry must resume against the current local calendar after restart");
        expect(restored_retry && within_one_second(restored_retry->due_at, retrying.due_at),
               "restore must not rewrite the active retry timer itself");
        const auto restored_disabled = engine.find(2);
        expect(restored_disabled && !restored_disabled->enabled && restored_disabled->due_at <= now,
               "disabled calendar record should remain inert until explicitly enabled");
        expect(engine.set_enabled(2, true),
               "disabled calendar should enable when action is available");
        const auto enabled = engine.find(2);
        expect(enabled && enabled->enabled && enabled->due_at > std::chrono::system_clock::now(),
               "enabling a stale local calendar must rebase it to a future local occurrence");
        expect(within_one_second(enabled->due_at, *expected),
               "enabled local calendar should use the same derived wall-clock occurrence");
        engine.stop();
        executor.stop(true);
        scheduler.stop();
        cleanup(path);
    } catch (...) {
        cleanup(path);
        throw;
    }
}
void test_engine_stop_waits_for_inflight() {
    const auto path = unique_test_path();
    cleanup(path);
    try {
        std::mutex mutex;
        std::condition_variable cv;
        bool started = false;
        std::atomic_bool finished{false};
        axiom::ActionRegistry actions;
        expect(actions.register_action(L"slow-stop", L"lifecycle barrier test",
                                       [&](std::wstring_view) {
                                           {
                                               std::scoped_lock lock{mutex};
                                               started = true;
                                           }
                                           cv.notify_all();
                                           std::this_thread::sleep_for(120ms);
                                           finished.store(true);
                                           return axiom::ActionResult{true, L"done"};
                                       }),
               "slow-stop action should register");
        axiom::RuntimeScheduler scheduler;
        axiom::BoundedExecutor executor{1, 8};
        axiom::AutomationStore store{path};
        {
            axiom::AutomationEngine engine{scheduler, executor, store, actions};
            [[maybe_unused]] const auto id =
                engine.schedule_every(1ms, L"stop barrier", L"slow-stop", L"");
            std::unique_lock lock{mutex};
            expect(cv.wait_for(lock, 1s, [&] { return started; }),
                   "slow recurring action should start");
        }
        expect(finished.load(),
               "AutomationEngine destructor must wait for in-flight executor work");
        executor.stop(true);
        scheduler.stop();
        cleanup(path);
    } catch (...) {
        cleanup(path);
        throw;
    }
}
} // namespace
int main() {
    try {
        run_tests();
        test_calendar_restore_rebases_derived_due_state();
        test_engine_stop_waits_for_inflight();
        std::cout << "AxiomAutomationEngineTests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "AxiomAutomationEngineTests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
