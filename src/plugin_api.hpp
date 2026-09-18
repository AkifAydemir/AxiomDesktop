#pragma once
#include <cstddef>
#include <cstdint>
#if defined(_WIN32)
#define AXIOM_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#define AXIOM_PLUGIN_CALL __cdecl
#else
#define AXIOM_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#define AXIOM_PLUGIN_CALL
#endif
inline constexpr std::uint32_t AXIOM_PLUGIN_ABI_V1 = 0x00010000u;
enum AxiomPluginCapabilityV1 : std::uint64_t {
    AXIOM_PLUGIN_CAP_ACTIONS = 1ull << 0,
    AXIOM_PLUGIN_CAP_COMMANDS = 1ull << 1,
};
struct AxiomPluginInvokeResultV1 {
    int success;
    const char *message_utf8;
};
// Callback result strings remain plugin-owned and must stay valid until the callback returns to the
// host. Plugins must not throw exceptions across this ABI.
using AxiomPluginInvokeFnV1 =
    AxiomPluginInvokeResultV1(AXIOM_PLUGIN_CALL *)(void *plugin_context, const char *payload_utf8);
struct AxiomPluginDescriptorV1 {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
    const char *plugin_id;
    const char *display_name;
    const char *plugin_version;
    std::uint64_t requested_capabilities;
};
// The host table is valid only during AxiomPlugin_LoadV1. Plugins must not retain this pointer.
struct AxiomPluginHostV1 {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
    void *host_context;
    int(AXIOM_PLUGIN_CALL *register_action)(void *host_context, const char *name_utf8,
                                            const char *summary_utf8,
                                            AxiomPluginInvokeFnV1 callback, void *plugin_context);
    int(AXIOM_PLUGIN_CALL *register_command)(void *host_context, const char *name_utf8,
                                             const char *usage_utf8, const char *summary_utf8,
                                             AxiomPluginInvokeFnV1 callback, void *plugin_context);
    void(AXIOM_PLUGIN_CALL *log)(void *host_context, int level, const char *message_utf8);
};
using AxiomPluginQueryFnV1 = const AxiomPluginDescriptorV1 *(AXIOM_PLUGIN_CALL *)();
using AxiomPluginLoadFnV1 = int(AXIOM_PLUGIN_CALL *)(const AxiomPluginHostV1 *host);
using AxiomPluginUnloadFnV1 = void(AXIOM_PLUGIN_CALL *)();
#define AXIOM_PLUGIN_QUERY_SYMBOL "AxiomPlugin_QueryV1"
#define AXIOM_PLUGIN_LOAD_SYMBOL "AxiomPlugin_LoadV1"
#define AXIOM_PLUGIN_UNLOAD_SYMBOL "AxiomPlugin_UnloadV1"
