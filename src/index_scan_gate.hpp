#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stop_token>

namespace axiom {

enum class IndexScanWaitResult : std::uint8_t {
    succeeded,
    failed,
    superseded,
    stopped,
    unknown_generation,
};

class IndexScanGate final {
  public:
    explicit IndexScanGate(std::size_t retained_outcomes = 64);

    [[nodiscard]] std::uint64_t begin() noexcept;
    void complete(std::uint64_t generation, bool success) noexcept;
    void cancel(std::uint64_t generation) noexcept;

    [[nodiscard]] IndexScanWaitResult wait(std::uint64_t generation,
                                           std::stop_token stop_token = {}) const noexcept;

    [[nodiscard]] std::uint64_t current_generation() const noexcept;

  private:
    enum class State : std::uint8_t { pending, succeeded, failed, superseded };
    struct Outcome {
        std::uint64_t generation{};
        State state{State::pending};
    };

    [[nodiscard]] Outcome *find_locked(std::uint64_t generation) noexcept;
    [[nodiscard]] const Outcome *find_locked(std::uint64_t generation) const noexcept;
    void trim_locked() noexcept;

    const std::size_t retained_outcomes_;
    mutable std::mutex mutex_;
    mutable std::condition_variable_any cv_;
    std::deque<Outcome> outcomes_;
    std::uint64_t next_generation_{};
};

} // namespace axiom
