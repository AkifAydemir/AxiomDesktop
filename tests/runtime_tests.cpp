#include "runtime.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
namespace {
using namespace std::chrono_literals;
[[noreturn]] void fail(const char *message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}
void require(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}
std::filesystem::path unique_store_path(const char *label) {
    auto path = std::filesystem::temp_directory_path();
    path /= std::string{"axiom-runtime-"} + label + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin";
    return path;
}
void cleanup_store(const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    auto temp = path;
    temp += ".tmp";
    std::filesystem::remove(temp, error);
    auto backup = path;
    backup += ".bak";
    std::filesystem::remove(backup, error);
}
void test_scheduler_basics() {
    axiom::RuntimeScheduler scheduler;
    std::mutex mutex;
    std::condition_variable cv;
    int one_shot_count = 0;
    const auto one_shot = scheduler.schedule_after(25ms, [&] {
        {
            std::scoped_lock lock{mutex};
            ++one_shot_count;
        }
        cv.notify_all();
    });
    require(one_shot != 0, "one-shot task should have a stable id");
    {
        std::unique_lock lock{mutex};
        require(cv.wait_for(lock, 500ms, [&] { return one_shot_count == 1; }),
                "one-shot task should fire");
    }
    std::atomic_int cancelled_count{0};
    const auto cancelled = scheduler.schedule_after(120ms, [&] { ++cancelled_count; });
    require(scheduler.cancel(cancelled), "scheduled task should cancel");
    std::this_thread::sleep_for(160ms);
    require(cancelled_count.load() == 0, "cancelled task must not execute");
    std::mutex repeating_mutex;
    std::condition_variable repeating_cv;
    int repeating_count = 0;
    const auto repeating = scheduler.schedule_every(5ms, 20ms, [&] {
        {
            std::scoped_lock lock{repeating_mutex};
            ++repeating_count;
        }
        repeating_cv.notify_all();
    });
    {
        std::unique_lock lock{repeating_mutex};
        require(repeating_cv.wait_for(lock, 500ms, [&] { return repeating_count >= 3; }),
                "repeating task should execute multiple times");
    }
    require(scheduler.cancel(repeating), "repeating task should cancel");
    int repeated = 0;
    {
        std::scoped_lock lock{repeating_mutex};
        repeated = repeating_count;
    }
    std::this_thread::sleep_for(60ms);
    {
        std::scoped_lock lock{repeating_mutex};
        require(repeating_count == repeated, "repeating task should stop after cancellation");
    }
    std::atomic_int burst_count{0};
    for (int i = 0; i < 200; ++i) {
        [[maybe_unused]] const auto task = scheduler.schedule_after(0ms, [&] { ++burst_count; });
    }
    const auto burst_deadline = std::chrono::steady_clock::now() + 1s;
    while (burst_count.load() != 200 && std::chrono::steady_clock::now() < burst_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    require(burst_count.load() == 200, "scheduler should drain immediate burst tasks");
    [[maybe_unused]] const auto throwing =
        scheduler.schedule_after(0ms, [] { throw std::runtime_error("expected test failure"); });
    std::this_thread::sleep_for(30ms);
    const auto stats = scheduler.stats();
    require(stats.total_scheduled >= 203, "runtime should track scheduled tasks");
    require(stats.total_executed >= 202, "runtime should track executed callbacks");
    require(stats.total_cancelled >= 2, "runtime should track cancellations");
    require(stats.total_failed >= 1,
            "runtime should contain callback exceptions and track failures");
    scheduler.stop();
}
void test_bounded_executor() {
    axiom::BoundedExecutor executor{1, 1};
    std::mutex mutex;
    std::condition_variable cv;
    bool first_started = false;
    bool release_first = false;
    std::atomic_int completed{0};
    require(executor.submit([&] {
        {
            std::unique_lock lock{mutex};
            first_started = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release_first; });
        }
        ++completed;
    }),
            "first executor task should be accepted");
    {
        std::unique_lock lock{mutex};
        require(cv.wait_for(lock, 500ms, [&] { return first_started; }),
                "first executor task should start");
    }
    require(executor.submit([&] { ++completed; }),
            "second executor task should fill the bounded queue");
    require(!executor.submit([&] { ++completed; }),
            "third executor task should be rejected when queue is full");
    const auto saturated = executor.stats();
    require(saturated.worker_count == 1, "executor should report worker count");
    require(saturated.active_workers == 1, "executor should report active worker");
    require(saturated.queued_tasks == 1, "executor should report queued task");
    require(saturated.total_rejected >= 1, "executor should report queue rejection");
    {
        std::scoped_lock lock{mutex};
        release_first = true;
    }
    cv.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (completed.load() != 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    require(completed.load() == 2, "accepted executor tasks should complete");
    require(executor.submit([] { throw std::runtime_error{"expected executor failure"}; }),
            "throwing executor task should be accepted");
    const auto failure_deadline = std::chrono::steady_clock::now() + 1s;
    while (executor.stats().total_failed == 0 &&
           std::chrono::steady_clock::now() < failure_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    require(executor.stats().total_failed >= 1,
            "throwing callback should be contained and counted");
    require(executor.submit([&] { ++completed; }),
            "executor worker should accept work after a throwing callback");
    const auto recovery_deadline = std::chrono::steady_clock::now() + 1s;
    while (completed.load() != 3 && std::chrono::steady_clock::now() < recovery_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    require(completed.load() == 3, "executor worker should survive callback exceptions");
    executor.stop(true);
    const auto finished = executor.stats();
    require(finished.total_submitted == 4, "executor submitted count mismatch");
    require(finished.total_completed == 4, "executor completed count mismatch");
    require(finished.total_failed >= 1, "executor should count callback exceptions");
}
void test_in_memory_reminders() {
    axiom::RuntimeScheduler scheduler;
    std::mutex fired_mutex;
    std::condition_variable fired_cv;
    axiom::Reminder fired_reminder;
    bool fired = false;
    axiom::ReminderCenter reminders{scheduler, [&](const axiom::Reminder &reminder) {
                                        {
                                            std::scoped_lock lock{fired_mutex};
                                            fired_reminder = reminder;
                                            fired = true;
                                        }
                                        fired_cv.notify_all();
                                    }};
    bool short_repeat_rejected = false;
    try {
        [[maybe_unused]] const auto invalid = reminders.schedule_every(50ms, L"too fast");
    } catch (const std::invalid_argument &) {
        short_repeat_rejected = true;
    }
    require(short_repeat_rejected, "user reminder recurrence below one second should be rejected");
    const auto first = reminders.schedule_after(35ms, L"drink water");
    const auto second = reminders.schedule_after(300ms, L"cancel me");
    require(first != second, "reminders should have stable unique ids");
    require(reminders.active_count() == 2, "two reminders should be active");
    require(reminders.cancel(second), "second reminder should cancel");
    {
        std::unique_lock lock{fired_mutex};
        require(fired_cv.wait_for(lock, 500ms, [&] { return fired; }), "reminder should fire");
    }
    require(fired_reminder.id == first, "fired reminder id should match");
    require(fired_reminder.message == L"drink water", "fired reminder message should match");
    require(reminders.active_count() == 0, "fired one-shot should leave active set");
    {
        std::scoped_lock lock{fired_mutex};
        fired = false;
    }
    const auto immediate = reminders.schedule_after(0ms, L"immediate");
    {
        std::unique_lock lock{fired_mutex};
        require(fired_cv.wait_for(lock, 500ms, [&] { return fired; }),
                "zero-delay reminder should not race registration");
    }
    require(fired_reminder.id == immediate, "zero-delay reminder id should match");
    reminders.stop();
    scheduler.stop();
}
void test_persistent_restore_and_recurrence() {
    const auto path = unique_store_path("restore");
    cleanup_store(path);
    axiom::ReminderId persisted_one_shot = 0;
    axiom::ReminderId persisted_recurring = 0;
    try {
        {
            axiom::RuntimeScheduler scheduler;
            axiom::ReminderStore store{path};
            axiom::ReminderCenter reminders{scheduler, &store, {}};
            persisted_one_shot =
                reminders.schedule_at(std::chrono::system_clock::now() + 10s, L"survive restart");
            persisted_recurring = reminders.schedule_every(2s, L"repeat after restart");
            require(store.status().record_count == 2,
                    "persistent store should contain scheduled reminders");
            reminders.stop();
            scheduler.stop();
        }
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<axiom::Reminder> fired;
        {
            axiom::RuntimeScheduler scheduler;
            axiom::ReminderStore store{path};
            axiom::ReminderCenter reminders{scheduler, &store,
                                            [&](const axiom::Reminder &reminder) {
                                                {
                                                    std::scoped_lock lock{mutex};
                                                    fired.push_back(reminder);
                                                }
                                                cv.notify_all();
                                            }};
            const auto report = reminders.restore();
            require(report.restored == 2, "restart should restore two reminders");
            require(report.overdue == 0, "future reminders should not be overdue");
            const auto restored = reminders.snapshot();
            require(restored.size() == 2, "restored center should expose two reminders");
            require((restored[0].id == persisted_one_shot || restored[1].id == persisted_one_shot),
                    "one-shot ID must survive restart");
            require(
                (restored[0].id == persisted_recurring || restored[1].id == persisted_recurring),
                "recurring ID must survive restart");
            require(reminders.cancel(persisted_one_shot),
                    "restored one-shot should cancel by stable id");
            require(reminders.cancel(persisted_recurring),
                    "restored recurring should cancel by stable id");
            require(store.status().record_count == 0, "cancellation should persist immediately");
            reminders.stop();
            scheduler.stop();
        }
        // Seed an overdue one-shot and recurring reminder to exercise restart catch-up.
        {
            axiom::ReminderStore store{path};
            const auto now = std::chrono::system_clock::now();
            const std::vector<axiom::Reminder> seeded{
                {41, now - 4s, 0ms, 0, L"missed one-shot"},
                {42, now - 5s, 2s, 7, L"missed recurring"},
            };
            store.save(43, seeded);
        }
        {
            axiom::RuntimeScheduler scheduler;
            axiom::ReminderStore store{path};
            axiom::ReminderCenter reminders{scheduler, &store,
                                            [&](const axiom::Reminder &reminder) {
                                                {
                                                    std::scoped_lock lock{mutex};
                                                    fired.push_back(reminder);
                                                }
                                                cv.notify_all();
                                            }};
            const auto report = reminders.restore();
            require(report.restored == 2, "overdue restore should load both reminders");
            require(report.overdue == 2, "overdue restore should count overdue records");
            {
                std::unique_lock lock{mutex};
                require(cv.wait_for(lock, 800ms,
                                    [&] {
                                        bool one_shot_seen = false;
                                        bool recurring_seen = false;
                                        for (const auto &reminder : fired) {
                                            one_shot_seen = one_shot_seen || reminder.id == 41;
                                            recurring_seen = recurring_seen || reminder.id == 42;
                                        }
                                        return one_shot_seen && recurring_seen;
                                    }),
                        "overdue reminders should fire once promptly after restore");
            }
            const auto active = reminders.snapshot();
            require(active.size() == 1,
                    "overdue one-shot should be removed while recurring remains");
            require(active.front().id == 42, "overdue recurring reminder should remain active");
            require(active.front().fire_count == 8,
                    "recurring fire count should increment after catch-up");
            require(active.front().due_at > std::chrono::system_clock::now(),
                    "recurring catch-up should advance to a future occurrence");
            require(store.load().reminders.size() == 1,
                    "post-fire state should persist immediately");
            reminders.clear();
            require(store.status().record_count == 0, "clear should persist empty reminder set");
            reminders.stop();
            scheduler.stop();
        }
        cleanup_store(path);
    } catch (...) {
        cleanup_store(path);
        throw;
    }
}
} // namespace
int main() {
    test_scheduler_basics();
    test_bounded_executor();
    test_in_memory_reminders();
    test_persistent_restore_and_recurrence();
    std::cout << "Axiom runtime tests passed\n";
    return 0;
}
