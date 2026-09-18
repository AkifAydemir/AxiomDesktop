#pragma once
#include "reminder_store.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
namespace axiom {
using RuntimeTaskId = std::uint64_t;
struct RuntimeStats {
    std::size_t active_tasks{};
    std::uint64_t total_scheduled{};
    std::uint64_t total_executed{};
    std::uint64_t total_cancelled{};
    std::uint64_t total_failed{};
};
class RuntimeScheduler final {
  public:
    using Duration = std::chrono::milliseconds;
    using Callback = std::function<void()>;
    RuntimeScheduler();
    ~RuntimeScheduler();
    RuntimeScheduler(const RuntimeScheduler &) = delete;
    RuntimeScheduler &operator=(const RuntimeScheduler &) = delete;
    [[nodiscard]] RuntimeTaskId schedule_after(Duration delay, Callback callback);
    [[nodiscard]] RuntimeTaskId schedule_every(Duration initial_delay, Duration interval,
                                               Callback callback);
    [[nodiscard]] bool cancel(RuntimeTaskId id) noexcept;
    void stop() noexcept;
    [[nodiscard]] RuntimeStats stats() const noexcept;

  private:
    struct Task;
    struct TaskLater {
        bool operator()(const std::shared_ptr<Task> &lhs,
                        const std::shared_ptr<Task> &rhs) const noexcept;
    };
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::priority_queue<std::shared_ptr<Task>, std::vector<std::shared_ptr<Task>>, TaskLater>
        queue_;
    std::unordered_map<RuntimeTaskId, std::shared_ptr<Task>> tasks_;
    std::jthread worker_;
    RuntimeTaskId next_id_{1};
    std::uint64_t revision_{};
    std::uint64_t total_scheduled_{};
    std::uint64_t total_executed_{};
    std::uint64_t total_cancelled_{};
    std::uint64_t total_failed_{};
    bool stopped_{};
    [[nodiscard]] RuntimeTaskId schedule_impl(Duration delay, Duration interval, Callback callback);
    void run(std::stop_token stop_token);
    void prune_cancelled_locked();
};
struct ExecutorStats {
    std::size_t worker_count{};
    std::size_t active_workers{};
    std::size_t queued_tasks{};
    std::size_t queue_capacity{};
    std::uint64_t total_submitted{};
    std::uint64_t total_completed{};
    std::uint64_t total_failed{};
    std::uint64_t total_rejected{};
};
class BoundedExecutor final {
  public:
    using Callback = std::function<void()>;
    explicit BoundedExecutor(std::size_t worker_count = 2, std::size_t queue_capacity = 64);
    ~BoundedExecutor();
    BoundedExecutor(const BoundedExecutor &) = delete;
    BoundedExecutor &operator=(const BoundedExecutor &) = delete;
    [[nodiscard]] bool submit(Callback callback) noexcept;
    void stop(bool drain = true) noexcept;
    [[nodiscard]] ExecutorStats stats() const noexcept;

  private:
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<Callback> queue_;
    std::vector<std::jthread> workers_;
    const std::size_t queue_capacity_;
    std::size_t active_workers_{};
    std::uint64_t total_submitted_{};
    std::uint64_t total_completed_{};
    std::uint64_t total_failed_{};
    std::uint64_t total_rejected_{};
    bool accepting_{true};
    bool drain_on_stop_{true};
    bool stopped_{};
    void run(std::stop_token stop_token) noexcept;
};
struct ReminderRestoreReport {
    std::size_t restored{};
    std::size_t overdue{};
};
class ReminderCenter final {
  public:
    struct State; // opaque implementation state; definition remains private to runtime.cpp
    using Duration = RuntimeScheduler::Duration;
    using FiredCallback = std::function<void(const Reminder &)>;
    explicit ReminderCenter(RuntimeScheduler &scheduler, FiredCallback fired_callback = {});
    ReminderCenter(RuntimeScheduler &scheduler, ReminderStore *store, FiredCallback fired_callback);
    ~ReminderCenter();
    ReminderCenter(const ReminderCenter &) = delete;
    ReminderCenter &operator=(const ReminderCenter &) = delete;
    [[nodiscard]] ReminderId schedule_after(Duration delay, std::wstring message);
    [[nodiscard]] ReminderId schedule_at(std::chrono::system_clock::time_point due_at,
                                         std::wstring message);
    [[nodiscard]] ReminderId schedule_every(Duration interval, std::wstring message);
    [[nodiscard]] bool cancel(ReminderId id);
    void clear();
    void stop() noexcept;
    void set_fired_callback(FiredCallback callback);
    [[nodiscard]] ReminderRestoreReport restore();
    [[nodiscard]] std::vector<Reminder> snapshot() const;
    [[nodiscard]] std::size_t active_count() const noexcept;
    [[nodiscard]] std::optional<ReminderStoreStatus> persistence_status() const;
    [[nodiscard]] std::string last_persistence_error() const;

  private:
    RuntimeScheduler &scheduler_;
    std::shared_ptr<State> state_;
};
} // namespace axiom
