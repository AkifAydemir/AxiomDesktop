#include "index_scan_gate.hpp"

#include <algorithm>

namespace axiom {

IndexScanGate::IndexScanGate(std::size_t retained_outcomes)
    : retained_outcomes_{std::max<std::size_t>(1, retained_outcomes)} {}

std::uint64_t IndexScanGate::begin() noexcept {
    try {
        std::scoped_lock lock{mutex_};
        if (!outcomes_.empty() && outcomes_.back().state == State::pending) {
            outcomes_.back().state = State::superseded;
        }
        ++next_generation_;
        if (next_generation_ == 0)
            ++next_generation_;
        outcomes_.push_back(Outcome{next_generation_, State::pending});
        trim_locked();
        cv_.notify_all();
        return next_generation_;
    } catch (...) {
        return 0;
    }
}

void IndexScanGate::complete(std::uint64_t generation, bool success) noexcept {
    try {
        {
            std::scoped_lock lock{mutex_};
            auto *outcome = find_locked(generation);
            if (outcome == nullptr || outcome->state != State::pending)
                return;
            outcome->state = success ? State::succeeded : State::failed;
        }
        cv_.notify_all();
    } catch (...) {
    }
}

void IndexScanGate::cancel(std::uint64_t generation) noexcept {
    try {
        {
            std::scoped_lock lock{mutex_};
            auto *outcome = find_locked(generation);
            if (outcome == nullptr || outcome->state != State::pending)
                return;
            outcome->state = State::superseded;
        }
        cv_.notify_all();
    } catch (...) {
    }
}

IndexScanWaitResult IndexScanGate::wait(std::uint64_t generation,
                                        std::stop_token stop_token) const noexcept {
    try {
        std::unique_lock lock{mutex_};
        const auto resolved = [this, generation] {
            const auto *outcome = find_locked(generation);
            return outcome == nullptr || outcome->state != State::pending;
        };
        if (!cv_.wait(lock, stop_token, resolved)) {
            return IndexScanWaitResult::stopped;
        }

        const auto *outcome = find_locked(generation);
        if (outcome == nullptr)
            return IndexScanWaitResult::unknown_generation;
        switch (outcome->state) {
        case State::succeeded:
            return IndexScanWaitResult::succeeded;
        case State::failed:
            return IndexScanWaitResult::failed;
        case State::superseded:
            return IndexScanWaitResult::superseded;
        case State::pending:
            break;
        }
    } catch (...) {
    }
    return IndexScanWaitResult::unknown_generation;
}

std::uint64_t IndexScanGate::current_generation() const noexcept {
    std::scoped_lock lock{mutex_};
    return next_generation_;
}

IndexScanGate::Outcome *IndexScanGate::find_locked(std::uint64_t generation) noexcept {
    const auto found =
        std::find_if(outcomes_.begin(), outcomes_.end(), [generation](const Outcome &outcome) {
            return outcome.generation == generation;
        });
    return found == outcomes_.end() ? nullptr : &*found;
}

const IndexScanGate::Outcome *IndexScanGate::find_locked(std::uint64_t generation) const noexcept {
    const auto found =
        std::find_if(outcomes_.cbegin(), outcomes_.cend(), [generation](const Outcome &outcome) {
            return outcome.generation == generation;
        });
    return found == outcomes_.cend() ? nullptr : &*found;
}

void IndexScanGate::trim_locked() noexcept {
    while (outcomes_.size() > retained_outcomes_ && !outcomes_.empty() &&
           outcomes_.front().state != State::pending) {
        outcomes_.pop_front();
    }
}

} // namespace axiom
