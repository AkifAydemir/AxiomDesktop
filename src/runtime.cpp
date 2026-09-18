#include "runtime.hpp"
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>
namespace axiom {
struct RuntimeScheduler::Task final {
    RuntimeTaskId id{};
    std::chrono::steady_clock::time_point deadline{};
    Duration interval{};
    Callback callback;
    std::atomic_bool cancelled{false};
};
bool RuntimeScheduler::TaskLater::operator()(const std::shared_ptr<Task> &lhs,
                                             const std::shared_ptr<Task> &rhs) const noexcept {
    if (lhs->deadline != rhs->deadline) {
        return lhs->deadline > rhs->deadline;
    }
    return lhs->id > rhs->id;
}
RuntimeScheduler::RuntimeScheduler()
    : worker_{[this](std::stop_token stop_token) { run(stop_token); }} {}
RuntimeScheduler::~RuntimeScheduler() {
    stop();
}
RuntimeTaskId RuntimeScheduler::schedule_after(Duration delay, Callback callback) {
    return schedule_impl(delay, Duration::zero(), std::move(callback));
}
RuntimeTaskId RuntimeScheduler::schedule_every(Duration initial_delay, Duration interval,
                                               Callback callback) {
    if (interval <= Duration::zero()) {
        throw std::invalid_argument("RuntimeScheduler repeating interval must be positive");
    }
    return schedule_impl(initial_delay, interval, std::move(callback));
}
RuntimeTaskId RuntimeScheduler::schedule_impl(Duration delay, Duration interval,
                                              Callback callback) {
    if (!callback) {
        throw std::invalid_argument("RuntimeScheduler callback must not be empty");
    }
    if (delay < Duration::zero()) {
        delay = Duration::zero();
    }
    std::scoped_lock lock{mutex_};
    if (stopped_) {
        throw std::runtime_error("RuntimeScheduler is stopped");
    }
    auto task = std::make_shared<Task>();
    task->id = next_id_++;
    task->deadline = std::chrono::steady_clock::now() + delay;
    task->interval = interval;
    task->callback = std::move(callback);
    tasks_.emplace(task->id, task);
    queue_.push(task);
    ++total_scheduled_;
    ++revision_;
    cv_.notify_all();
    return task->id;
}
bool RuntimeScheduler::cancel(RuntimeTaskId id) noexcept {
    std::scoped_lock lock{mutex_};
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return false;
    }
    if (!it->second->cancelled.exchange(true)) {
        ++total_cancelled_;
    }
    tasks_.erase(it);
    ++revision_;
    cv_.notify_all();
    return true;
}
void RuntimeScheduler::stop() noexcept {
    {
        std::scoped_lock lock{mutex_};
        if (stopped_) {
            return;
        }
        stopped_ = true;
        for (auto &[id, task] : tasks_) {
            (void)id;
            if (!task->cancelled.exchange(true)) {
                ++total_cancelled_;
            }
        }
        tasks_.clear();
        ++revision_;
    }
    worker_.request_stop();
    cv_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}
RuntimeStats RuntimeScheduler::stats() const noexcept {
    std::scoped_lock lock{mutex_};
    return RuntimeStats{
        tasks_.size(), total_scheduled_, total_executed_, total_cancelled_, total_failed_,
    };
}
void RuntimeScheduler::prune_cancelled_locked() {
    while (!queue_.empty() && queue_.top()->cancelled.load()) {
        queue_.pop();
    }
}
void RuntimeScheduler::run(std::stop_token stop_token) {
    std::unique_lock lock{mutex_};
    while (!stop_token.stop_requested()) {
        prune_cancelled_locked();
        if (queue_.empty()) {
            cv_.wait(lock, stop_token, [this] { return stopped_ || !queue_.empty(); });
            if (stopped_ || stop_token.stop_requested()) {
                break;
            }
            continue;
        }
        const auto deadline = queue_.top()->deadline;
        const auto observed_revision = revision_;
        const bool changed = cv_.wait_until(lock, stop_token, deadline, [this, observed_revision] {
            return stopped_ || revision_ != observed_revision;
        });
        if (stopped_ || stop_token.stop_requested()) {
            break;
        }
        if (changed) {
            continue;
        }
        prune_cancelled_locked();
        if (queue_.empty()) {
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        if (queue_.top()->deadline > now) {
            continue;
        }
        auto task = queue_.top();
        queue_.pop();
        if (task->cancelled.load()) {
            continue;
        }
        lock.unlock();
        bool failed = false;
        try {
            task->callback();
        } catch (...) {
            failed = true;
        }
        lock.lock();
        ++total_executed_;
        if (failed) {
            ++total_failed_;
        }
        if (task->cancelled.load() || stopped_) {
            tasks_.erase(task->id);
            continue;
        }
        if (task->interval > Duration::zero()) {
            task->deadline = std::chrono::steady_clock::now() + task->interval;
            queue_.push(task);
            ++revision_;
            continue;
        }
        tasks_.erase(task->id);
    }
}
BoundedExecutor::BoundedExecutor(std::size_t worker_count, std::size_t queue_capacity)
    : queue_capacity_{queue_capacity} {
    if (worker_count == 0) {
        throw std::invalid_argument("BoundedExecutor worker_count must be positive");
    }
    if (queue_capacity_ == 0) {
        throw std::invalid_argument("BoundedExecutor queue_capacity must be positive");
    }
    workers_.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this](std::stop_token stop_token) { run(stop_token); });
    }
}
BoundedExecutor::~BoundedExecutor() {
    stop(true);
}
bool BoundedExecutor::submit(Callback callback) noexcept {
    try {
        if (!callback) {
            std::scoped_lock lock{mutex_};
            ++total_rejected_;
            return false;
        }
        {
            std::scoped_lock lock{mutex_};
            if (!accepting_ || stopped_ || queue_.size() >= queue_capacity_) {
                ++total_rejected_;
                return false;
            }
            queue_.push_back(std::move(callback));
            ++total_submitted_;
        }
        cv_.notify_one();
        return true;
    } catch (...) {
        return false;
    }
}
void BoundedExecutor::stop(bool drain) noexcept {
    {
        std::scoped_lock lock{mutex_};
        if (stopped_) {
            return;
        }
        accepting_ = false;
        drain_on_stop_ = drain;
        if (!drain_on_stop_) {
            total_rejected_ += queue_.size();
            queue_.clear();
        }
        stopped_ = true;
    }
    cv_.notify_all();
    if (!drain) {
        for (auto &worker : workers_) {
            worker.request_stop();
        }
    }
    for (auto &worker : workers_) {
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
            worker.join();
        }
    }
}
ExecutorStats BoundedExecutor::stats() const noexcept {
    std::scoped_lock lock{mutex_};
    return ExecutorStats{
        workers_.size(),  active_workers_,  queue_.size(), queue_capacity_,
        total_submitted_, total_completed_, total_failed_, total_rejected_,
    };
}
void BoundedExecutor::run(std::stop_token stop_token) noexcept {
    while (true) {
        Callback callback;
        {
            std::unique_lock lock{mutex_};
            cv_.wait(lock, stop_token, [this] { return stopped_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stopped_ || stop_token.stop_requested()) {
                    return;
                }
                continue;
            }
            if (stop_token.stop_requested() && !drain_on_stop_) {
                return;
            }
            callback = std::move(queue_.front());
            queue_.pop_front();
            ++active_workers_;
        }
        bool failed = false;
        try {
            callback();
        } catch (...) {
            failed = true;
        }
        {
            std::scoped_lock lock{mutex_};
            if (active_workers_ != 0) {
                --active_workers_;
            }
            ++total_completed_;
            if (failed) {
                ++total_failed_;
            }
            if (stopped_ && drain_on_stop_ && queue_.empty() && active_workers_ == 0) {
                cv_.notify_all();
            }
        }
    }
}
struct ReminderCenter::State final {
    struct Entry final {
        Reminder reminder;
        RuntimeTaskId runtime_task{};
        std::uint64_t arm_generation{};
    };
    RuntimeScheduler *scheduler{};
    ReminderStore *store{};
    mutable std::mutex mutex;
    std::mutex mutation_mutex;
    std::unordered_map<ReminderId, Entry> entries;
    FiredCallback fired_callback;
    ReminderId next_id{1};
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
[[nodiscard]] std::pair<ReminderId, std::vector<Reminder>>
persistent_snapshot(const std::shared_ptr<ReminderCenter::State> &state) {
    std::pair<ReminderId, std::vector<Reminder>> snapshot;
    std::scoped_lock lock{state->mutex};
    snapshot.first = state->next_id;
    snapshot.second.reserve(state->entries.size());
    for (const auto &[id, entry] : state->entries) {
        (void)id;
        snapshot.second.push_back(entry.reminder);
    }
    std::sort(snapshot.second.begin(), snapshot.second.end(),
              [](const Reminder &lhs, const Reminder &rhs) { return lhs.id < rhs.id; });
    return snapshot;
}
void persist_state(const std::shared_ptr<ReminderCenter::State> &state) {
    ReminderStore *store = nullptr;
    {
        std::scoped_lock lock{state->mutex};
        store = state->store;
    }
    if (store == nullptr) {
        return;
    }
    const auto [next_id, reminders] = persistent_snapshot(state);
    store->save(next_id, reminders);
    std::scoped_lock lock{state->mutex};
    state->last_persistence_error.clear();
}
void record_persistence_error(const std::shared_ptr<ReminderCenter::State> &state,
                              const std::exception &exception) noexcept {
    std::scoped_lock lock{state->mutex};
    state->last_persistence_error = exception.what();
}
void arm_reminder(const std::weak_ptr<ReminderCenter::State> &weak_state, ReminderId id);
void fire_reminder(const std::weak_ptr<ReminderCenter::State> &weak_state, ReminderId id,
                   std::uint64_t arm_generation) {
    const auto state = weak_state.lock();
    if (!state) {
        return;
    }
    Reminder fired;
    ReminderCenter::FiredCallback callback;
    bool recurring = false;
    {
        std::scoped_lock mutation_lock{state->mutation_mutex};
        {
            std::scoped_lock lock{state->mutex};
            if (state->stopped) {
                return;
            }
            const auto it = state->entries.find(id);
            if (it == state->entries.end() || it->second.arm_generation != arm_generation) {
                return;
            }
            fired = it->second.reminder;
            callback = state->fired_callback;
            recurring = fired.recurring();
            if (recurring) {
                it->second.reminder.fire_count += 1;
                it->second.reminder.due_at = next_fixed_occurrence(
                    fired.due_at, fired.repeat_interval, std::chrono::system_clock::now());
                it->second.runtime_task = 0;
            } else {
                state->entries.erase(it);
            }
        }
        try {
            persist_state(state);
        } catch (const std::exception &exception) {
            record_persistence_error(state, exception);
        }
    }
    if (callback) {
        callback(fired);
    }
    if (recurring) {
        arm_reminder(weak_state, id);
    }
}
void arm_reminder(const std::weak_ptr<ReminderCenter::State> &weak_state, ReminderId id) {
    const auto state = weak_state.lock();
    if (!state) {
        return;
    }
    std::chrono::system_clock::time_point due_at;
    std::uint64_t arm_generation = 0;
    {
        std::scoped_lock lock{state->mutex};
        if (state->stopped || state->scheduler == nullptr) {
            return;
        }
        const auto it = state->entries.find(id);
        if (it == state->entries.end()) {
            return;
        }
        due_at = it->second.reminder.due_at;
        arm_generation = ++it->second.arm_generation;
    }
    const auto task =
        state->scheduler->schedule_after(delay_until(due_at), [weak_state, id, arm_generation] {
            fire_reminder(weak_state, id, arm_generation);
        });
    bool keep_task = false;
    {
        std::scoped_lock lock{state->mutex};
        const auto it = state->entries.find(id);
        if (!state->stopped && it != state->entries.end() &&
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
ReminderCenter::ReminderCenter(RuntimeScheduler &scheduler, FiredCallback fired_callback)
    : ReminderCenter(scheduler, nullptr, std::move(fired_callback)) {}
ReminderCenter::ReminderCenter(RuntimeScheduler &scheduler, ReminderStore *store,
                               FiredCallback fired_callback)
    : scheduler_{scheduler}, state_{std::make_shared<State>()} {
    state_->scheduler = &scheduler_;
    state_->store = store;
    state_->fired_callback = std::move(fired_callback);
}
ReminderCenter::~ReminderCenter() {
    stop();
    state_.reset();
}
ReminderId ReminderCenter::schedule_after(Duration delay, std::wstring message) {
    if (delay < Duration::zero()) {
        delay = Duration::zero();
    }
    return schedule_at(std::chrono::system_clock::now() + delay, std::move(message));
}
ReminderId ReminderCenter::schedule_at(std::chrono::system_clock::time_point due_at,
                                       std::wstring message) {
    if (message.empty()) {
        throw std::invalid_argument("Reminder message must not be empty");
    }
    ReminderId id = 0;
    {
        std::scoped_lock mutation_lock{state_->mutation_mutex};
        ReminderId previous_next = 0;
        {
            std::scoped_lock lock{state_->mutex};
            if (state_->stopped) {
                throw std::runtime_error("ReminderCenter is stopped");
            }
            previous_next = state_->next_id;
            id = state_->next_id++;
            state_->entries.emplace(
                id, State::Entry{Reminder{id, due_at, {}, 0, std::move(message)}, 0, 0});
        }
        try {
            persist_state(state_);
        } catch (...) {
            std::scoped_lock lock{state_->mutex};
            state_->entries.erase(id);
            state_->next_id = previous_next;
            throw;
        }
    }
    try {
        arm_reminder(state_, id);
    } catch (...) {
        try {
            (void)cancel(id);
        } catch (...) {
        }
        throw;
    }
    return id;
}
ReminderId ReminderCenter::schedule_every(Duration interval, std::wstring message) {
    if (interval < std::chrono::seconds{1}) {
        throw std::invalid_argument("Reminder repeat interval must be at least one second");
    }
    if (message.empty()) {
        throw std::invalid_argument("Reminder message must not be empty");
    }
    ReminderId id = 0;
    {
        std::scoped_lock mutation_lock{state_->mutation_mutex};
        ReminderId previous_next = 0;
        {
            std::scoped_lock lock{state_->mutex};
            if (state_->stopped) {
                throw std::runtime_error("ReminderCenter is stopped");
            }
            previous_next = state_->next_id;
            id = state_->next_id++;
            state_->entries.emplace(
                id, State::Entry{Reminder{id, std::chrono::system_clock::now() + interval, interval,
                                          0, std::move(message)},
                                 0, 0});
        }
        try {
            persist_state(state_);
        } catch (...) {
            std::scoped_lock lock{state_->mutex};
            state_->entries.erase(id);
            state_->next_id = previous_next;
            throw;
        }
    }
    try {
        arm_reminder(state_, id);
    } catch (...) {
        try {
            (void)cancel(id);
        } catch (...) {
        }
        throw;
    }
    return id;
}
bool ReminderCenter::cancel(ReminderId id) {
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
            runtime_task = it->second.runtime_task;
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
void ReminderCenter::clear() {
    std::vector<RuntimeTaskId> tasks;
    std::unordered_map<ReminderId, State::Entry> removed;
    {
        std::scoped_lock mutation_lock{state_->mutation_mutex};
        {
            std::scoped_lock lock{state_->mutex};
            removed = state_->entries;
            tasks.reserve(removed.size());
            for (const auto &[id, entry] : removed) {
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
            state_->entries = std::move(removed);
            throw;
        }
    }
    for (const auto task : tasks) {
        (void)scheduler_.cancel(task);
    }
}
void ReminderCenter::stop() noexcept {
    std::vector<RuntimeTaskId> tasks;
    {
        std::scoped_lock mutation_lock{state_->mutation_mutex};
        std::scoped_lock lock{state_->mutex};
        if (state_->stopped) {
            return;
        }
        state_->stopped = true;
        tasks.reserve(state_->entries.size());
        for (const auto &[id, entry] : state_->entries) {
            (void)id;
            if (entry.runtime_task != 0) {
                tasks.push_back(entry.runtime_task);
            }
        }
        state_->entries.clear();
    }
    for (const auto task : tasks) {
        (void)scheduler_.cancel(task);
    }
}
void ReminderCenter::set_fired_callback(FiredCallback callback) {
    std::scoped_lock lock{state_->mutex};
    state_->fired_callback = std::move(callback);
}
ReminderRestoreReport ReminderCenter::restore() {
    ReminderStore *store = nullptr;
    {
        std::scoped_lock lock{state_->mutex};
        store = state_->store;
    }
    if (store == nullptr) {
        return {};
    }
    const auto snapshot = store->load();
    ReminderRestoreReport report;
    std::vector<ReminderId> ids;
    {
        std::scoped_lock mutation_lock{state_->mutation_mutex};
        std::scoped_lock lock{state_->mutex};
        if (state_->stopped) {
            throw std::runtime_error("ReminderCenter is stopped");
        }
        if (!state_->entries.empty()) {
            throw std::runtime_error("ReminderCenter restore requires an empty center");
        }
        state_->next_id = snapshot.next_id;
        state_->last_persistence_error.clear();
        ids.reserve(snapshot.reminders.size());
        const auto now = std::chrono::system_clock::now();
        for (const auto &reminder : snapshot.reminders) {
            if (reminder.message.empty() || reminder.id == 0 ||
                reminder.repeat_interval < Duration::zero()) {
                continue;
            }
            state_->entries.emplace(reminder.id, State::Entry{reminder, 0, 0});
            ids.push_back(reminder.id);
            ++report.restored;
            if (reminder.due_at <= now) {
                ++report.overdue;
            }
        }
    }
    for (const auto id : ids) {
        arm_reminder(state_, id);
    }
    return report;
}
std::vector<Reminder> ReminderCenter::snapshot() const {
    std::vector<Reminder> reminders;
    {
        std::scoped_lock lock{state_->mutex};
        reminders.reserve(state_->entries.size());
        for (const auto &[id, entry] : state_->entries) {
            (void)id;
            reminders.push_back(entry.reminder);
        }
    }
    std::sort(reminders.begin(), reminders.end(), [](const Reminder &lhs, const Reminder &rhs) {
        if (lhs.due_at != rhs.due_at) {
            return lhs.due_at < rhs.due_at;
        }
        return lhs.id < rhs.id;
    });
    return reminders;
}
std::size_t ReminderCenter::active_count() const noexcept {
    std::scoped_lock lock{state_->mutex};
    return state_->entries.size();
}
std::optional<ReminderStoreStatus> ReminderCenter::persistence_status() const {
    ReminderStore *store = nullptr;
    {
        std::scoped_lock lock{state_->mutex};
        store = state_->store;
    }
    if (store == nullptr) {
        return std::nullopt;
    }
    return store->status();
}
std::string ReminderCenter::last_persistence_error() const {
    std::scoped_lock lock{state_->mutex};
    return state_->last_persistence_error;
}
} // namespace axiom
