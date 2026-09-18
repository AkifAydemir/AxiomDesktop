#pragma once
#include "action_registry.hpp"
#include "execution_diagnostics.hpp"
#include "plugin_api.hpp"
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace axiom {
struct PluginCommandDescriptor {
    std::wstring name;
    std::wstring usage;
    std::wstring summary;
    std::string owner_plugin_id;
};
struct PluginCommandResult {
    bool success{false};
    std::wstring message;
};
struct LoadedPluginInfo {
    std::string id;
    std::wstring name;
    std::string version;
    std::uint64_t capabilities{0};
    std::size_t action_count{0};
    std::size_t command_count{0};
};
class PluginHost final {
  public:
    explicit PluginHost(ActionRegistry &actions, ExecutionDiagnostics *diagnostics = nullptr);
    class RegistrationScope final {
      public:
        RegistrationScope(PluginHost &host, std::string plugin_id, std::uint64_t capabilities);
        ~RegistrationScope();
        [[nodiscard]] const AxiomPluginHostV1 *c_host() noexcept;
        [[nodiscard]] std::size_t registered_actions() const noexcept;
        [[nodiscard]] std::size_t registered_commands() const noexcept;

      private:
        struct Context;
        Context *context_ptr_{};
        AxiomPluginHostV1 host_api_{};
        std::unique_ptr<Context> context_;
    };
    void reserve_command_name(std::wstring name);
    [[nodiscard]] bool contains_command(std::wstring_view name) const;
    [[nodiscard]] std::vector<PluginCommandDescriptor> list_commands() const;
    [[nodiscard]] PluginCommandResult invoke_command(std::wstring_view name,
                                                     std::wstring_view arguments) const noexcept;
    void mark_plugin_loaded(LoadedPluginInfo info);
    [[nodiscard]] std::vector<LoadedPluginInfo> list_plugins() const;
    // Removes host-visible callbacks, rejects new callback leases and waits for
    // already-running callbacks to return before the DLL may be unloaded.
    void unregister_plugin(std::string_view plugin_id) noexcept;

  private:
    struct CallbackGate {
        std::mutex mutex;
        std::condition_variable idle;
        bool accepting{true};
        std::size_t active{};
    };
    struct CommandEntry {
        PluginCommandDescriptor descriptor;
        AxiomPluginInvokeFnV1 callback{};
        void *plugin_context{};
        std::shared_ptr<CallbackGate> gate;
    };
    ActionRegistry &actions_;
    ExecutionDiagnostics *diagnostics_{};
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::wstring, CommandEntry> commands_;
    std::unordered_set<std::wstring> reserved_command_names_;
    std::unordered_map<std::string, std::vector<std::wstring>> action_names_by_plugin_;
    std::unordered_map<std::string, std::vector<std::wstring>> command_names_by_plugin_;
    std::unordered_map<std::string, LoadedPluginInfo> loaded_plugins_;
    std::unordered_map<std::string, std::shared_ptr<CallbackGate>> callback_gates_;
    [[nodiscard]] std::shared_ptr<CallbackGate> callback_gate_for(std::string_view plugin_id);
    [[nodiscard]] static AxiomPluginInvokeResultV1
    invoke_callback(const std::shared_ptr<CallbackGate> &gate, AxiomPluginInvokeFnV1 callback,
                    void *plugin_context, const std::string &payload) noexcept;
    [[nodiscard]] bool register_action_from_plugin(std::string_view plugin_id,
                                                   std::uint64_t capabilities,
                                                   const char *name_utf8, const char *summary_utf8,
                                                   AxiomPluginInvokeFnV1 callback,
                                                   void *plugin_context,
                                                   const std::shared_ptr<CallbackGate> &gate);
    [[nodiscard]] bool
    register_command_from_plugin(std::string_view plugin_id, std::uint64_t capabilities,
                                 const char *name_utf8, const char *usage_utf8,
                                 const char *summary_utf8, AxiomPluginInvokeFnV1 callback,
                                 void *plugin_context, const std::shared_ptr<CallbackGate> &gate);
};
} // namespace axiom
