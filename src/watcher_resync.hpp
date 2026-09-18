#pragma once

#include "watcher_health.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace axiom {

struct WatcherResyncStats {
    std::uint64_t requests{};
    std::uint64_t attempts{};
    std::uint64_t completed{};
    std::uint64_t failed{};
    std::uint64_t superseded{};
    std::uint64_t pending_generation{};
    bool running{};
};

class WatcherResyncCoordinator final {
  public:
    using ResyncCallback = std::function<bool(std::stop_token, std::uint64_t)>;

    WatcherResyncCoordinator(WatcherHealth &health, ResyncCallback callback);
    ~WatcherResyncCoordinator();

    WatcherResyncCoordinator(const WatcherResyncCoordinator &) = delete;
    WatcherResyncCoordinator &operator=(const WatcherResyncCoordinator &) = delete;

    void request(std::uint64_t issue_generation) noexcept;
    void request_current() noexcept;
    void stop() noexcept;

    [[nodiscard]] WatcherResyncStats stats() const noexcept;

  private:
    void run(std::stop_token stop_token) noexcept;

    WatcherHealth &health_;
    ResyncCallback callback_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    WatcherResyncStats stats_{};
    bool stopped_{};
    std::jthread worker_;
};

} // namespace axiom
