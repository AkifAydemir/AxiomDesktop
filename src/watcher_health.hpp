#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace axiom {

enum class WatcherHealthState : std::uint8_t {
    stopped,
    healthy,
    degraded,
    resync_required,
};

struct WatcherHealthSnapshot {
    WatcherHealthState state{WatcherHealthState::stopped};
    std::size_t watched_roots{};
    std::uint64_t notifications{};
    std::uint64_t overflows{};
    std::uint64_t errors{};
    std::uint64_t successful_resyncs{};
    std::uint64_t manual_resync_requests{};
    std::uint64_t issue_generation{};
    std::uint64_t last_resynced_generation{};
    std::uint32_t last_error{};
    bool restart_required{};
};

class WatcherHealth final {
  public:
    void started(std::size_t watched_roots) noexcept;
    void update_watched_roots(std::size_t watched_roots) noexcept;
    void stopped() noexcept;

    void note_notification() noexcept;
    [[nodiscard]] std::uint64_t note_overflow(std::uint32_t error_code) noexcept;
    [[nodiscard]] std::uint64_t note_error(std::uint32_t error_code) noexcept;
    [[nodiscard]] std::uint64_t request_resync() noexcept;

    // Generic watcher failures can terminate a root watch. Recovery must explicitly
    // confirm that the watch set was restarted before a full resync may restore
    // healthy state. Overflow-only loss does not require a restart.
    [[nodiscard]] bool mark_restarted(std::uint64_t observed_issue_generation) noexcept;

    // Clears resync-required only if no newer watcher loss/error was observed
    // while the full index resync was running.
    [[nodiscard]] bool mark_resynced(std::uint64_t observed_issue_generation) noexcept;

    [[nodiscard]] WatcherHealthSnapshot snapshot() const noexcept;

  private:
    [[nodiscard]] std::uint64_t note_issue_locked(bool overflow, std::uint32_t error_code) noexcept;

    mutable std::mutex mutex_;
    WatcherHealthSnapshot snapshot_{};
};

[[nodiscard]] const char *watcher_health_state_name(WatcherHealthState state) noexcept;

} // namespace axiom
