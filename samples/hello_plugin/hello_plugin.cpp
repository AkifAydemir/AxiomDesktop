#include "plugin_api.hpp"
#include <string>
namespace {
std::string response;
AxiomPluginInvokeResultV1 AXIOM_PLUGIN_CALL hello_command(void *, const char *payload) {
    response = "Hello from Axiom plugin";
    if (payload != nullptr && *payload != '\0')
        response += ": " + std::string{payload};
    return {1, response.c_str()};
}
AxiomPluginInvokeResultV1 AXIOM_PLUGIN_CALL hello_action(void *, const char *payload) {
    response = "Plugin action received: ";
    response += payload != nullptr ? payload : "";
    return {1, response.c_str()};
}
} // namespace
AXIOM_PLUGIN_EXPORT const AxiomPluginDescriptorV1 *AXIOM_PLUGIN_CALL AxiomPlugin_QueryV1() {
    static const AxiomPluginDescriptorV1 descriptor{sizeof(AxiomPluginDescriptorV1),
                                                    AXIOM_PLUGIN_ABI_V1,
                                                    "axiom.sample.hello",
                                                    "Axiom Hello Plugin",
                                                    "1.0.0",
                                                    AXIOM_PLUGIN_CAP_ACTIONS |
                                                        AXIOM_PLUGIN_CAP_COMMANDS};
    return &descriptor;
}
AXIOM_PLUGIN_EXPORT int AXIOM_PLUGIN_CALL AxiomPlugin_LoadV1(const AxiomPluginHostV1 *host) {
    if (host == nullptr || host->abi_version != AXIOM_PLUGIN_ABI_V1)
        return 0;
    if (!host->register_command(host->host_context, "hello", "hello [text]", "Sample SDK command.",
                                &hello_command, nullptr))
        return 0;
    if (!host->register_action(host->host_context, "hello-action", "Sample SDK automation action.",
                               &hello_action, nullptr))
        return 0;
    return 1;
}
AXIOM_PLUGIN_EXPORT void AXIOM_PLUGIN_CALL AxiomPlugin_UnloadV1() {}
