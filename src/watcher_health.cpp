#include "watcher_health.hpp"

namespace axiom {

void WatcherHealth::started(std::size_t watched_roots) noexcept {
    std::scoped_lock lock{mutex_};
    snapshot_.watched_roots = watched_roots;
    snapshot_.state =
        watched_roots == 0 ? WatcherHealthState::stopped : WatcherHealthState::healthy;
    snapshot_.last_error = 0;
    snapshot_.restart_required = false;
}

void WatcherHealth::update_watched_roots(std::size_t watched_roots) noexcept {
    std::scoped_lock lock{mutex_};
    snapshot_.watched_roots = watched_roots;
    if (watched_roots == 0 && snapshot_.state != WatcherHealthState::degraded &&
        snapshot_.state != WatcherHealthState::resync_required) {
        snapshot_.state = WatcherHealthState::stopped;
    }
}

void WatcherHealth::stopped() noexcept {
    std::scoped_lock lock{mutex_};
    snapshot_.state = WatcherHealthState::stopped;
    snapshot_.watched_roots = 0;
    snapshot_.last_error = 0;
    snapshot_.restart_required = false;
}

void WatcherHealth::note_notification() noexcept {
    std::scoped_lock lock{mutex_};
    ++snapshot_.notifications;
}

std::uint64_t WatcherHealth::note_issue_locked(bool overflow, std::uint32_t error_code) noexcept {
    if (overflow) {
        ++snapshot_.overflows;
    } else {
        ++snapshot_.errors;
        snapshot_.restart_required = true;
    }

    ++snapshot_.issue_generation;
    snapshot_.last_error = error_code;
    if (snapshot_.watched_roots != 0) {
        if (overflow) {
            snapshot_.state = WatcherHealthState::resync_required;
        } else if (snapshot_.state != WatcherHealthState::resync_required) {
            // A generic watcher error means the watch is degraded, but it must
            // not erase an already-observed notification-loss condition.
            snapshot_.state = WatcherHealthState::degraded;
        }
    }
    return snapshot_.issue_generation;
}

std::uint64_t WatcherHealth::note_overflow(std::uint32_t error_code) noexcept {
    std::scoped_lock lock{mutex_};
    return note_issue_locked(true, error_code);
}

std::uint64_t WatcherHealth::note_error(std::uint32_t error_code) noexcept {
    std::scoped_lock lock{mutex_};
    return note_issue_locked(false, error_code);
}

std::uint64_t WatcherHealth::request_resync() noexcept {
    std::scoped_lock lock{mutex_};
    if (snapshot_.watched_roots == 0) {
        return 0;
    }

    ++snapshot_.manual_resync_requests;
    ++snapshot_.issue_generation;
    snapshot_.state = WatcherHealthState::resync_required;
    snapshot_.last_error = 0;
    return snapshot_.issue_generation;
}

bool WatcherHealth::mark_restarted(std::uint64_t observed_issue_generation) noexcept {
    std::scoped_lock lock{mutex_};
    if (snapshot_.watched_roots == 0 || observed_issue_generation != snapshot_.issue_generation ||
        !snapshot_.restart_required) {
        return false;
    }

    snapshot_.restart_required = false;
    return true;
}

bool WatcherHealth::mark_resynced(std::uint64_t observed_issue_generation) noexcept {
    std::scoped_lock lock{mutex_};
    if (snapshot_.watched_roots == 0 ||
        (snapshot_.state != WatcherHealthState::resync_required &&
         snapshot_.state != WatcherHealthState::degraded) ||
        observed_issue_generation != snapshot_.issue_generation || snapshot_.restart_required) {
        return false;
    }

    ++snapshot_.successful_resyncs;
    snapshot_.last_resynced_generation = observed_issue_generation;
    snapshot_.state = WatcherHealthState::healthy;
    snapshot_.last_error = 0;
    return true;
}

WatcherHealthSnapshot WatcherHealth::snapshot() const noexcept {
    std::scoped_lock lock{mutex_};
    return snapshot_;
}

const char *watcher_health_state_name(WatcherHealthState state) noexcept {
    switch (state) {
    case WatcherHealthState::stopped:
        return "stopped";
    case WatcherHealthState::healthy:
        return "healthy";
    case WatcherHealthState::degraded:
        return "degraded";
    case WatcherHealthState::resync_required:
        return "resync-required";
    }
    return "unknown";
}

} // namespace axiom
