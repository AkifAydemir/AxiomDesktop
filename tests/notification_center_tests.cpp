#include "notification_center.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>
namespace {
void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}
void run_tests() {
    axiom::NotificationCenter center{3};
    const auto first = center.publish(axiom::NotificationLevel::info, L"One", L"Body 1");
    const auto second = center.publish(axiom::NotificationLevel::warning, L"Two", L"Body 2");
    const auto third = center.publish(axiom::NotificationLevel::error, L"Three", L"Body 3");
    expect(first == 1 && second == 2 && third == 3, "notification IDs should be monotonic");
    expect(center.stats().pending == 3, "published notifications should become pending");
    const auto drained = center.drain_pending(2);
    expect(drained.size() == 2, "drain should honor limit");
    expect(drained[0].id == first && drained[1].id == second, "pending delivery should be FIFO");
    expect(center.stats().pending == 1, "drain should remove delivered notifications");
    const auto fourth = center.publish(axiom::NotificationLevel::success, L"Four", L"Body 4");
    expect(fourth == 4, "IDs should remain monotonic after drains");
    const auto recent = center.recent(10);
    expect(recent.size() == 3, "history should respect retention limit");
    expect(recent.front().id == fourth, "history should be newest-first");
    expect(!center.find(first).has_value(), "evicted history entry should not be discoverable");
    expect(center.find(third).has_value(), "retained history entry should be discoverable");
    expect(center.stats().total_dropped >= 1, "retention eviction should increment drop count");
    center.clear_history();
    expect(center.recent().empty(), "clear_history should clear retained history");
    expect(center.stats().pending == 2, "clearing history should not discard pending delivery");
}
} // namespace
int main() {
    try {
        run_tests();
        std::cout << "AxiomNotificationCenterTests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "AxiomNotificationCenterTests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
