#include "index_scan_gate.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace {

void expect(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error{message};
}

void success_and_failure_are_generation_specific() {
    axiom::IndexScanGate gate;
    const auto first = gate.begin();
    expect(first != 0, "first generation must be non-zero");

    auto waiter = std::async(std::launch::async, [&] { return gate.wait(first); });
    std::this_thread::sleep_for(2ms);
    gate.complete(first, true);
    expect(waiter.get() == axiom::IndexScanWaitResult::succeeded,
           "completed scan must wake as succeeded");

    const auto second = gate.begin();
    gate.complete(second, false);
    expect(gate.wait(second) == axiom::IndexScanWaitResult::failed,
           "failed scan must remain distinguishable from success");
}

void newer_scan_supersedes_old_waiter() {
    axiom::IndexScanGate gate;
    const auto first = gate.begin();
    auto waiter = std::async(std::launch::async, [&] { return gate.wait(first); });

    const auto second = gate.begin();
    expect(second > first, "scan generations must increase");
    expect(waiter.get() == axiom::IndexScanWaitResult::superseded,
           "replacement scan must release the old waiter");

    gate.complete(first, true);
    expect(gate.wait(first) == axiom::IndexScanWaitResult::superseded,
           "late old-worker completion must not rewrite superseded state");

    gate.complete(second, true);
    expect(gate.wait(second) == axiom::IndexScanWaitResult::succeeded,
           "replacement scan must complete independently");
}

void stop_token_releases_wait_without_mutating_scan() {
    axiom::IndexScanGate gate;
    const auto generation = gate.begin();
    std::stop_source source;
    auto waiter =
        std::async(std::launch::async, [&] { return gate.wait(generation, source.get_token()); });
    std::this_thread::sleep_for(2ms);
    source.request_stop();
    expect(waiter.get() == axiom::IndexScanWaitResult::stopped,
           "stop token must cooperatively release scan wait");

    gate.complete(generation, true);
    expect(gate.wait(generation) == axiom::IndexScanWaitResult::succeeded,
           "cancelling one waiter must not cancel the underlying scan");
}

void retention_is_bounded() {
    axiom::IndexScanGate gate{3};
    std::uint64_t first = 0;
    for (int i = 0; i < 8; ++i) {
        const auto generation = gate.begin();
        if (i == 0)
            first = generation;
        gate.complete(generation, true);
    }
    expect(gate.wait(first) == axiom::IndexScanWaitResult::unknown_generation,
           "old completed generations must be pruned");
    expect(gate.current_generation() >= 8, "generation must remain monotonic after pruning");
}

} // namespace

int main() {
    try {
        success_and_failure_are_generation_specific();
        newer_scan_supersedes_old_waiter();
        stop_token_releases_wait_without_mutating_scan();
        retention_is_bounded();
        std::cout << "index_scan_gate_tests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "index_scan_gate_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
