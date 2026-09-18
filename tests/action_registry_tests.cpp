#include "action_registry.hpp"
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
    axiom::ExecutionDiagnostics diagnostics{32};
    axiom::ActionRegistry registry{&diagnostics};
    expect(registry.register_action(L"notify", L"Publish a notification.",
                                    [](std::wstring_view payload) {
                                        return axiom::ActionResult{true, L"notified: " +
                                                                             std::wstring{payload}};
                                    }),
           "first registration should succeed");
    expect(registry.contains(L"NOTIFY"), "action names should be case insensitive");
    const auto invoked = registry.invoke(L"Notify", L"hello");
    expect(invoked.success, "registered action should execute");
    expect(invoked.message == L"notified: hello", "payload should reach action handler");
    expect(!registry.register_action(
               L"NOTIFY", L"duplicate",
               [](std::wstring_view) { return axiom::ActionResult{true, L"x"}; }),
           "duplicate canonical action names should be rejected");
    expect(registry.register_action(
               L"throws", L"test exception containment",
               [](std::wstring_view) -> axiom::ActionResult { throw std::runtime_error{"boom"}; }),
           "throwing action should register");
    const auto contained = registry.invoke(L"throws", L"");
    expect(!contained.success, "action exceptions should be contained");
    expect(contained.message.find(L"boom") != std::wstring::npos,
           "contained exception should be diagnostic");
    expect(registry.register_action(
               L"plugin-fail", L"plugin diagnostic test",
               [](std::wstring_view) { return axiom::ActionResult{false, L"plugin failed"}; },
               L"plugin:axiom.test"),
           "plugin-owned action should register");
    const auto plugin_failed = registry.invoke(L"plugin-fail", L"secret-payload");
    expect(!plugin_failed.success, "plugin-owned failure should propagate");
    const auto action_error =
        diagnostics.last_error(axiom::DiagnosticDomain::action, L"plugin-fail");
    expect(action_error.has_value(), "action failure should be captured in diagnostics");
    expect(action_error->message.find(L"secret-payload") == std::wstring::npos,
           "diagnostics must not log action payloads");
    const auto plugin_error =
        diagnostics.last_error(axiom::DiagnosticDomain::plugin, L"axiom.test");
    expect(plugin_error.has_value(),
           "plugin-owned action failure should also be visible per plugin");
    const auto unknown = registry.invoke(L"missing", L"");
    expect(!unknown.success, "unknown actions should fail without throwing");
    const auto actions = registry.list();
    expect(actions.size() == 3, "registry list should expose registered actions");
    expect(actions[0].name == L"notify" && actions[1].name == L"plugin-fail" &&
               actions[2].name == L"throws",
           "registry list should be sorted");
    expect(registry.unregister_action(L"NOTIFY"), "unregister should be case insensitive");
    expect(!registry.contains(L"notify"), "unregistered action should disappear");
}
} // namespace
int main() {
    try {
        run_tests();
        std::cout << "AxiomActionRegistryTests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "AxiomActionRegistryTests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
