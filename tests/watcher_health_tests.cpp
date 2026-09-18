#include "watcher_health.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

void lifecycle_and_resync() {
    axiom::WatcherHealth health;

    auto snapshot = health.snapshot();
    expect(snapshot.state == axiom::WatcherHealthState::stopped,
           "new watcher health must start stopped");
    expect(snapshot.watched_roots == 0, "new watcher health must have zero roots");

    health.started(2);
    health.note_notification();
    health.note_notification();
    snapshot = health.snapshot();
    expect(snapshot.state == axiom::WatcherHealthState::healthy, "started watcher must be healthy");
    expect(snapshot.watched_roots == 2, "started watcher must expose root count");
    expect(snapshot.notifications == 2, "notification count mismatch");

    const auto generation = health.note_overflow(1022);
    snapshot = health.snapshot();
    expect(snapshot.state == axiom::WatcherHealthState::resync_required,
           "overflow must require resync");
    expect(snapshot.overflows == 1, "overflow count mismatch");
    expect(snapshot.issue_generation == generation, "overflow must advance issue generation");
    expect(snapshot.last_error == 1022, "overflow error code mismatch");

    health.note_notification();
    snapshot = health.snapshot();
    expect(snapshot.state == axiom::WatcherHealthState::resync_required,
           "later notifications must not clear a required resync");

    expect(health.mark_resynced(generation), "matching resync generation must restore health");
    snapshot = health.snapshot();
    expect(snapshot.state == axiom::WatcherHealthState::healthy,
           "successful resync must restore health");
    expect(snapshot.successful_resyncs == 1, "resync count mismatch");
    expect(snapshot.last_resynced_generation == generation, "resync generation mismatch");
    expect(snapshot.last_error == 0, "successful resync must clear last error");

    const auto error_generation = health.note_error(5);
    snapshot = health.snapshot();
    expect(snapshot.state == axiom::WatcherHealthState::degraded,
           "watcher error must report degraded health");
    expect(snapshot.errors == 1, "error count mismatch");
    expect(snapshot.last_error == 5, "watcher error code mismatch");
    expect(snapshot.restart_required, "watcher error must require watch restart");
    expect(error_generation > generation, "issue generation must be monotonic");

    health.stopped();
    snapshot = health.snapshot();
    expect(snapshot.state == axiom::WatcherHealthState::stopped,
           "stopped watcher must report stopped");
    expect(snapshot.watched_roots == 0, "stopped watcher must clear root count");
    expect(snapshot.notifications == 3 && snapshot.overflows == 1 && snapshot.errors == 1,
           "stopping must preserve session counters");
}

void newer_issue_prevents_false_healthy_state() {
    axiom::WatcherHealth health;
    health.started(1);

    const auto first_generation = health.note_overflow(1022);
    const auto second_generation = health.note_error(5);
    expect(second_generation > first_generation, "new watcher issue must advance generation");

    expect(!health.mark_resynced(first_generation),
           "resync started before a newer issue must not clear health");
    auto snapshot = health.snapshot();
    expect(snapshot.state == axiom::WatcherHealthState::resync_required,
           "newer issue must keep resync-required state");
    expect(snapshot.successful_resyncs == 0, "stale resync completion must not count as success");
    expect(snapshot.last_error == 5, "stale resync completion must preserve newest error");

    expect(!health.mark_resynced(second_generation),
           "generic watcher error must require restart before resync can restore health");
    expect(health.snapshot().restart_required,
           "generic watcher error must expose restart-required");
    expect(health.mark_restarted(second_generation),
           "matching watcher restart must clear restart-required");
    expect(health.mark_resynced(second_generation),
           "latest generation can clear resync-required after restart");
    snapshot = health.snapshot();
    expect(snapshot.state == axiom::WatcherHealthState::healthy,
           "latest generation resync must restore health");
    expect(snapshot.successful_resyncs == 1, "latest generation resync must count once");
}

void zero_root_start_is_stopped() {
    axiom::WatcherHealth health;
    health.started(0);
    auto snapshot = health.snapshot();
    expect(snapshot.state == axiom::WatcherHealthState::stopped,
           "zero-root start must remain stopped");

    const auto generation = health.note_overflow(1022);
    snapshot = health.snapshot();
    expect(snapshot.state == axiom::WatcherHealthState::stopped,
           "overflow without active roots must not invent a running watcher");
    expect(snapshot.overflows == 1, "overflow should remain observable while stopped");
    expect(!health.mark_resynced(generation), "resync while stopped must not be accepted");
    snapshot = health.snapshot();
    expect(snapshot.successful_resyncs == 0,
           "resync while stopped must not increment success count");

    health.started(2);
    const auto degraded_generation = health.note_error(5);
    health.update_watched_roots(1);
    snapshot = health.snapshot();
    expect(snapshot.watched_roots == 1, "root update must expose actual active watcher count");
    expect(snapshot.state == axiom::WatcherHealthState::degraded,
           "root-count correction must not erase an issue observed during watcher startup");
    expect(!health.mark_resynced(degraded_generation),
           "degraded watcher cannot become healthy before its watch is restarted");
    expect(health.mark_restarted(degraded_generation),
           "matching restart must satisfy the degraded watcher recovery contract");
    expect(health.mark_resynced(degraded_generation),
           "manual recovery can clear a degraded watcher after restart + verified resync");
}

void manual_resync_and_zero_root_failure_state() {
    axiom::WatcherHealth health;
    health.started(2);

    const auto manual_generation = health.request_resync();
    auto snapshot = health.snapshot();
    expect(manual_generation != 0, "manual resync with active roots must create a generation");
    expect(snapshot.manual_resync_requests == 1, "manual resync counter mismatch");
    expect(snapshot.state == axiom::WatcherHealthState::resync_required,
           "manual resync must require a full resync");
    expect(health.mark_resynced(manual_generation),
           "manual resync generation must be acknowledgeable");

    const auto error_generation = health.note_error(5);
    health.update_watched_roots(0);
    snapshot = health.snapshot();
    expect(snapshot.state == axiom::WatcherHealthState::degraded,
           "failed recovery with zero active roots must remain degraded");
    expect(snapshot.restart_required,
           "failed recovery with zero active roots must keep restart-required");
    expect(!health.mark_resynced(error_generation),
           "zero-root degraded watcher must not become healthy without restart");

    health.stopped();
    expect(health.request_resync() == 0,
           "manual resync while stopped must not invent an active watcher generation");
}

void names_are_stable() {
    expect(std::string_view{axiom::watcher_health_state_name(axiom::WatcherHealthState::stopped)} ==
               "stopped",
           "stopped name mismatch");
    expect(std::string_view{axiom::watcher_health_state_name(axiom::WatcherHealthState::healthy)} ==
               "healthy",
           "healthy name mismatch");
    expect(std::string_view{axiom::watcher_health_state_name(
               axiom::WatcherHealthState::degraded)} == "degraded",
           "degraded name mismatch");
    expect(std::string_view{axiom::watcher_health_state_name(
               axiom::WatcherHealthState::resync_required)} == "resync-required",
           "resync-required name mismatch");
}

} // namespace

int main() {
    try {
        lifecycle_and_resync();
        newer_issue_prevents_false_healthy_state();
        zero_root_start_is_stopped();
        manual_resync_and_zero_root_failure_state();
        names_are_stable();
        std::cout << "watcher_health_tests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "watcher_health_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
