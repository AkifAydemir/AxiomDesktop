#include "execution_diagnostics.hpp"
#include <chrono>
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
    axiom::ExecutionDiagnostics diagnostics{3};
    [[maybe_unused]] const auto first =
        diagnostics.record(axiom::DiagnosticSeverity::info, axiom::DiagnosticDomain::plugin,
                           L"plugin.alpha", L"loaded");
    [[maybe_unused]] const auto second =
        diagnostics.record(axiom::DiagnosticSeverity::error, axiom::DiagnosticDomain::action,
                           L"sync", L"failed", std::chrono::milliseconds{17});
    [[maybe_unused]] const auto third =
        diagnostics.record(axiom::DiagnosticSeverity::warning, axiom::DiagnosticDomain::plugin,
                           L"plugin.alpha", L"slow callback");
    [[maybe_unused]] const auto fourth =
        diagnostics.record(axiom::DiagnosticSeverity::error, axiom::DiagnosticDomain::plugin,
                           L"plugin.beta", L"load failed");
    const auto stats = diagnostics.stats();
    expect(stats.capacity == 3, "diagnostic capacity should be retained");
    expect(stats.retained == 3, "bounded history should retain only capacity events");
    expect(stats.total_recorded == 4, "total recorded counter should be monotonic");
    expect(stats.total_errors == 2, "error counter should include discarded history");
    expect(stats.dropped_oldest == 1, "oldest event should be dropped at capacity");
    const auto recent = diagnostics.recent(2);
    expect(recent.size() == 2, "recent should obey limit");
    expect(recent[0].subject == L"plugin.beta", "recent should be newest-first");
    expect(recent[1].subject == L"plugin.alpha", "recent ordering mismatch");
    const auto alpha =
        diagnostics.for_subject(axiom::DiagnosticDomain::plugin, L"PLUGIN.ALPHA", 10);
    expect(alpha.size() == 1,
           "subject filtering should be case insensitive and respect bounded retention");
    expect(alpha.front().severity == axiom::DiagnosticSeverity::warning, "filtered event mismatch");
    const auto action_error = diagnostics.last_error(axiom::DiagnosticDomain::action, L"sync");
    expect(action_error.has_value(), "last action error should be available");
    expect(action_error->duration == std::chrono::milliseconds{17}, "duration should be retained");
    const auto plugin_error = diagnostics.last_error(axiom::DiagnosticDomain::plugin);
    expect(plugin_error && plugin_error->subject == L"plugin.beta",
           "domain-wide last error mismatch");
    diagnostics.clear();
    expect(diagnostics.recent().empty(), "clear should remove retained session history");
    expect(diagnostics.stats().total_recorded == 4, "clear should not reset lifetime counters");
}
} // namespace
int main() {
    try {
        run_tests();
        std::cout << "AxiomExecutionDiagnosticsTests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "AxiomExecutionDiagnosticsTests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
