#include "plugin_host.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
namespace {
using namespace std::chrono_literals;
struct BlockingContext {
    std::promise<void> entered;
    std::shared_future<void> release;
};
AxiomPluginInvokeResultV1 AXIOM_PLUGIN_CALL blocking(void *raw, const char *) {
    auto *context = static_cast<BlockingContext *>(raw);
    context->entered.set_value();
    context->release.wait();
    return {1, "done"};
}
AxiomPluginInvokeResultV1 AXIOM_PLUGIN_CALL echo(void *, const char *payload) {
    static std::string result;
    result = std::string{"echo:"} + (payload ? payload : "");
    return {1, result.c_str()};
}
void expect(bool value, const char *message) {
    if (!value)
        throw std::runtime_error{message};
}
void run_tests() {
    axiom::ExecutionDiagnostics diagnostics{64};
    axiom::ActionRegistry actions{&diagnostics};
    axiom::PluginHost host{actions, &diagnostics};
    host.reserve_command_name(L"help");
    axiom::PluginHost::RegistrationScope scope{
        host, "sample", AXIOM_PLUGIN_CAP_ACTIONS | AXIOM_PLUGIN_CAP_COMMANDS};
    const auto *api = scope.c_host();
    api->log(api->host_context, 2, "sample warning");
    expect(api->register_action(api->host_context, "sample-action", "sample", &echo, nullptr) == 1,
           "action registration failed");
    expect(api->register_command(api->host_context, "samplecmd", "samplecmd <text>", "sample",
                                 &echo, nullptr) == 1,
           "command registration failed");
    expect(scope.registered_actions() == 1 && scope.registered_commands() == 1,
           "registration counts wrong");
    expect(api->register_command(api->host_context, "help", "help", "collision", &echo, nullptr) ==
               0,
           "reserved host command must not be shadowed");
    expect(actions.invoke(L"sample-action", L"ok").success, "plugin action invocation failed");
    const auto command = host.invoke_command(L"samplecmd", L"hello");
    expect(command.success && command.message == L"echo:hello", "plugin command invocation failed");
    const auto plugin_events =
        diagnostics.for_subject(axiom::DiagnosticDomain::plugin, L"sample", 20);
    expect(plugin_events.size() >= 3,
           "plugin diagnostics should include log/action/command events");
    bool warning_seen = false;
    bool command_seen = false;
    for (const auto &event : plugin_events) {
        warning_seen = warning_seen || event.message.find(L"sample warning") != std::wstring::npos;
        command_seen =
            command_seen || event.message.find(L"command=samplecmd") != std::wstring::npos;
    }
    expect(warning_seen, "plugin host log callback should feed diagnostics");
    expect(command_seen, "plugin command execution should feed diagnostics");
    host.unregister_plugin("sample");
    expect(!actions.contains(L"sample-action"), "plugin action was not unregistered");
    expect(!host.contains_command(L"samplecmd"), "plugin command was not unregistered");
    axiom::PluginHost::RegistrationScope denied{host, "denied", AXIOM_PLUGIN_CAP_COMMANDS};
    expect(denied.c_host()->register_action(denied.c_host()->host_context, "nope", "nope", &echo,
                                            nullptr) == 0,
           "capability gate failed");
    // Unload must wait until an already-running plugin callback leaves the DLL.
    std::promise<void> release_promise;
    BlockingContext blocking_context;
    blocking_context.release = release_promise.get_future().share();
    auto entered = blocking_context.entered.get_future();
    axiom::PluginHost::RegistrationScope blocking_scope{host, "blocking", AXIOM_PLUGIN_CAP_ACTIONS};
    expect(blocking_scope.c_host()->register_action(blocking_scope.c_host()->host_context,
                                                    "blocking-action", "blocking", &blocking,
                                                    &blocking_context) == 1,
           "blocking action registration failed");
    std::thread invocation{[&] { (void)actions.invoke(L"blocking-action", L""); }};
    expect(entered.wait_for(1s) == std::future_status::ready, "blocking callback did not enter");
    auto unload = std::async(std::launch::async, [&] { host.unregister_plugin("blocking"); });
    expect(unload.wait_for(40ms) == std::future_status::timeout,
           "unregister returned while callback was active");
    release_promise.set_value();
    invocation.join();
    expect(unload.wait_for(1s) == std::future_status::ready,
           "unregister did not finish after callback returned");
    unload.get();
    expect(!actions.contains(L"blocking-action"), "blocking action survived unregister");
}
} // namespace
int main() {
    try {
        run_tests();
        std::cout << "AxiomPluginHostTests: PASS\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "AxiomPluginHostTests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
