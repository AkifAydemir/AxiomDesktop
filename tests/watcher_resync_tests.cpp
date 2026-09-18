#include "watcher_resync.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

bool wait_until(const std::function<bool()> &predicate, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

void successful_request_restores_health() {
    axiom::WatcherHealth health;
    health.started(1);
    const auto generation = health.note_overflow(1022);

    std::atomic<std::uint64_t> observed{};
    axiom::WatcherResyncCoordinator coordinator{health, [&](std::stop_token, std::uint64_t value) {
                                                    observed.store(value);
                                                    return true;
                                                }};

    coordinator.request(generation);
    expect(wait_until([&] { return coordinator.stats().completed == 1; }),
           "successful resync should complete");
    expect(observed.load() == generation, "resync callback generation mismatch");
    expect(health.snapshot().state == axiom::WatcherHealthState::healthy,
           "successful resync must restore watcher health");
    coordinator.stop();
}

void newer_issue_is_coalesced_and_retried() {
    axiom::WatcherHealth health;
    health.started(1);
    const auto first = health.note_overflow(1022);

    std::mutex mutex;
    std::condition_variable entered;
    bool first_entered = false;
    bool release_first = false;
    std::atomic<int> calls{};
    std::atomic<std::uint64_t> last_generation{};

    axiom::WatcherResyncCoordinator coordinator{
        health, [&](std::stop_token stop_token, std::uint64_t generation) {
            const int call = ++calls;
            last_generation.store(generation);
            if (call == 1) {
                std::unique_lock lock{mutex};
                first_entered = true;
                entered.notify_one();
                while (!release_first && !stop_token.stop_requested()) {
                    entered.wait_for(lock, 5ms);
                }
            }
            if (call > 1) {
                (void)health.mark_restarted(generation);
            }
            return !stop_token.stop_requested();
        }};

    coordinator.request(first);
    {
        std::unique_lock lock{mutex};
        expect(entered.wait_for(lock, 1s, [&] { return first_entered; }),
               "first resync did not start");
    }

    const auto second = health.note_error(5);
    const auto third = health.note_overflow(1022);
    coordinator.request(second);
    coordinator.request(third);

    {
        std::scoped_lock lock{mutex};
        release_first = true;
    }
    entered.notify_all();

    expect(wait_until([&] { return coordinator.stats().completed == 1; }),
           "latest coalesced resync should complete");
    const auto stats = coordinator.stats();
    expect(stats.superseded == 1, "older in-flight resync should be superseded once");
    expect(stats.attempts == 2, "new issues should coalesce into one additional full resync");
    expect(last_generation.load() == third, "coalesced retry must use newest issue generation");
    expect(health.snapshot().last_resynced_generation == third,
           "health must acknowledge newest issue generation");
    coordinator.stop();
}

void failure_remains_explicitly_resync_required() {
    axiom::WatcherHealth health;
    health.started(1);
    const auto generation = health.note_overflow(1022);

    axiom::WatcherResyncCoordinator coordinator{
        health, [](std::stop_token, std::uint64_t) { return false; }};
    coordinator.request(generation);

    expect(wait_until([&] { return coordinator.stats().failed == 1; }),
           "failed resync should be counted");
    expect(health.snapshot().state == axiom::WatcherHealthState::resync_required,
           "failed resync must remain visible");
    coordinator.stop();
}

void stop_is_cooperative() {
    axiom::WatcherHealth health;
    health.started(1);
    const auto generation = health.note_error(5);
    std::atomic<bool> entered{};

    axiom::WatcherResyncCoordinator coordinator{health, [&](std::stop_token token, std::uint64_t) {
                                                    entered.store(true);
                                                    while (!token.stop_requested()) {
                                                        std::this_thread::sleep_for(1ms);
                                                    }
                                                    return false;
                                                }};
    coordinator.request(generation);
    expect(wait_until([&] { return entered.load(); }), "cooperative-stop callback did not start");
    coordinator.stop();
    expect(!coordinator.stats().running, "coordinator must not remain running after stop");
}

} // namespace

int main() {
    try {
        successful_request_restores_health();
        newer_issue_is_coalesced_and_retried();
        failure_remains_explicitly_resync_required();
        stop_is_cooperative();
        std::cout << "watcher_resync_tests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "watcher_resync_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
