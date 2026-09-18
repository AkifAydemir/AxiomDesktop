#include "watcher_resync.hpp"

#include <algorithm>
#include <utility>

namespace axiom {

WatcherResyncCoordinator::WatcherResyncCoordinator(WatcherHealth &health, ResyncCallback callback)
    : health_{health}, callback_{std::move(callback)},
      worker_{[this](std::stop_token stop_token) { run(stop_token); }} {}

WatcherResyncCoordinator::~WatcherResyncCoordinator() {
    stop();
}

void WatcherResyncCoordinator::request(std::uint64_t issue_generation) noexcept {
    if (issue_generation == 0) {
        return;
    }

    try {
        {
            std::scoped_lock lock{mutex_};
            if (stopped_) {
                return;
            }
            ++stats_.requests;
            stats_.pending_generation = std::max(stats_.pending_generation, issue_generation);
        }
        cv_.notify_one();
    } catch (...) {
    }
}

void WatcherResyncCoordinator::request_current() noexcept {
    request(health_.snapshot().issue_generation);
}

void WatcherResyncCoordinator::stop() noexcept {
    {
        std::scoped_lock lock{mutex_};
        if (stopped_) {
            return;
        }
        stopped_ = true;
    }

    worker_.request_stop();
    cv_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

WatcherResyncStats WatcherResyncCoordinator::stats() const noexcept {
    std::scoped_lock lock{mutex_};
    return stats_;
}

void WatcherResyncCoordinator::run(std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested()) {
        std::uint64_t generation = 0;
        {
            std::unique_lock lock{mutex_};
            cv_.wait(lock, stop_token,
                     [this] { return stopped_ || stats_.pending_generation != 0; });
            if (stopped_ || stop_token.stop_requested()) {
                break;
            }
            generation = stats_.pending_generation;
            stats_.pending_generation = 0;
            stats_.running = true;
            ++stats_.attempts;
        }

        bool success = false;
        try {
            success = callback_ && callback_(stop_token, generation);
        } catch (...) {
            success = false;
        }

        const bool accepted = success && health_.mark_resynced(generation);

        {
            std::scoped_lock lock{mutex_};
            stats_.running = false;
            if (accepted) {
                ++stats_.completed;
            } else if (!success) {
                ++stats_.failed;
            } else {
                ++stats_.superseded;
            }

            // New notification-loss generations are queued by request().
            // Do not synthesize retries from health state here: a persistent
            // watcher/open failure during recovery must remain explicitly
            // degraded/resync-required rather than becoming a hot retry loop.
        }

        cv_.notify_one();
    }

    std::scoped_lock lock{mutex_};
    stats_.running = false;
}

} // namespace axiom
