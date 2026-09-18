#include "plugin_host.hpp"
#include <algorithm>
#include <chrono>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <codecvt>
#include <locale>
#endif
#include <memory>
#include <stdexcept>
#include <utility>
namespace axiom {
namespace {
[[nodiscard]] std::wstring utf8_to_wide(const char *text) {
    if (text == nullptr) {
        return {};
    }
#ifdef _WIN32
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (size <= 0)
        throw std::runtime_error{"Invalid UTF-8 text."};
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, output.data(), size) <= 0) {
        throw std::runtime_error{"Invalid UTF-8 text."};
    }
    output.pop_back();
    return output;
#else
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    return converter.from_bytes(text);
#endif
}
[[nodiscard]] std::string wide_to_utf8(std::wstring_view text) {
#ifdef _WIN32
    if (text.empty())
        return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        throw std::runtime_error{"Invalid UTF-16 text."};
    std::string output(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), output.data(), size, nullptr,
                            nullptr) <= 0) {
        throw std::runtime_error{"Invalid UTF-16 text."};
    }
    return output;
#else
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    return converter.to_bytes(text.data(), text.data() + text.size());
#endif
}
[[nodiscard]] std::wstring canonical_command(std::wstring_view value) {
    std::wstring output;
    output.reserve(value.size());
    for (const wchar_t ch : value) {
        if (ch >= L'A' && ch <= L'Z') {
            output.push_back(static_cast<wchar_t>(ch - L'A' + L'a'));
        } else {
            output.push_back(ch);
        }
    }
    return output;
}
[[nodiscard]] bool valid_extension_name(std::wstring_view value) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    for (const wchar_t ch : value) {
        const bool alpha = (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
        const bool digit = ch >= L'0' && ch <= L'9';
        if (!alpha && !digit && ch != L'-' && ch != L'_') {
            return false;
        }
    }
    return true;
}
} // namespace
struct PluginHost::RegistrationScope::Context {
    PluginHost *host{};
    std::string plugin_id;
    std::uint64_t capabilities{};
    std::size_t actions{};
    std::size_t commands{};
    std::shared_ptr<CallbackGate> gate;
};
PluginHost::PluginHost(ActionRegistry &actions, ExecutionDiagnostics *diagnostics)
    : actions_{actions}, diagnostics_{diagnostics} {}
std::shared_ptr<PluginHost::CallbackGate>
PluginHost::callback_gate_for(std::string_view plugin_id) {
    std::unique_lock lock{mutex_};
    auto &gate = callback_gates_[std::string{plugin_id}];
    if (!gate) {
        gate = std::make_shared<CallbackGate>();
    }
    {
        std::scoped_lock gate_lock{gate->mutex};
        if (gate->active != 0) {
            throw std::runtime_error{"Plugin callback gate is still active during registration."};
        }
        gate->accepting = true;
    }
    return gate;
}
AxiomPluginInvokeResultV1 PluginHost::invoke_callback(const std::shared_ptr<CallbackGate> &gate,
                                                      AxiomPluginInvokeFnV1 callback,
                                                      void *plugin_context,
                                                      const std::string &payload) noexcept {
    if (!gate || callback == nullptr) {
        return {0, "Plugin callback is unavailable."};
    }
    {
        std::unique_lock lock{gate->mutex};
        if (!gate->accepting) {
            return {0, "Plugin is unloading or disabled."};
        }
        ++gate->active;
    }
    struct Release final {
        std::shared_ptr<CallbackGate> gate;
        ~Release() {
            std::unique_lock lock{gate->mutex};
            if (gate->active != 0) {
                --gate->active;
            }
            const bool became_idle = gate->active == 0;
            lock.unlock();
            if (became_idle) {
                gate->idle.notify_all();
            }
        }
    } release{gate};
    try {
        return callback(plugin_context, payload.c_str());
    } catch (...) {
        return {0, "Plugin callback raised an exception across the ABI boundary."};
    }
}
PluginHost::RegistrationScope::RegistrationScope(PluginHost &host, std::string plugin_id,
                                                 std::uint64_t capabilities)
    : context_{std::make_unique<Context>()} {
    context_->host = &host;
    context_->plugin_id = std::move(plugin_id);
    context_->capabilities = capabilities;
    context_->gate = host.callback_gate_for(context_->plugin_id);
    context_ptr_ = context_.get();
    host_api_.struct_size = sizeof(AxiomPluginHostV1);
    host_api_.abi_version = AXIOM_PLUGIN_ABI_V1;
    host_api_.host_context = context_ptr_;
    host_api_.register_action = [](void *raw, const char *name, const char *summary,
                                   AxiomPluginInvokeFnV1 callback, void *plugin_context) -> int {
        try {
            auto &context = *static_cast<Context *>(raw);
            const bool registered = context.host->register_action_from_plugin(
                context.plugin_id, context.capabilities, name, summary, callback, plugin_context,
                context.gate);
            if (registered) {
                ++context.actions;
            }
            return registered ? 1 : 0;
        } catch (...) {
            return 0;
        }
    };
    host_api_.register_command = [](void *raw, const char *name, const char *usage,
                                    const char *summary, AxiomPluginInvokeFnV1 callback,
                                    void *plugin_context) -> int {
        try {
            auto &context = *static_cast<Context *>(raw);
            const bool registered = context.host->register_command_from_plugin(
                context.plugin_id, context.capabilities, name, usage, summary, callback,
                plugin_context, context.gate);
            if (registered) {
                ++context.commands;
            }
            return registered ? 1 : 0;
        } catch (...) {
            return 0;
        }
    };
    host_api_.log = [](void *raw, int level, const char *message) {
        try {
            auto &context = *static_cast<Context *>(raw);
            if (context.host->diagnostics_ == nullptr) {
                return;
            }
            DiagnosticSeverity severity = DiagnosticSeverity::info;
            if (level >= 3) {
                severity = DiagnosticSeverity::error;
            } else if (level == 2) {
                severity = DiagnosticSeverity::warning;
            }
            std::wstring text;
            try {
                text = utf8_to_wide(message);
            } catch (...) {
                text = L"Plugin emitted a log message that was not valid UTF-8.";
                severity = DiagnosticSeverity::warning;
            }
            [[maybe_unused]] const auto id = context.host->diagnostics_->record(
                severity, DiagnosticDomain::plugin, utf8_to_wide(context.plugin_id.c_str()),
                std::move(text));
        } catch (...) {
        }
    };
}
PluginHost::RegistrationScope::~RegistrationScope() = default;
const AxiomPluginHostV1 *PluginHost::RegistrationScope::c_host() noexcept {
    return &host_api_;
}
std::size_t PluginHost::RegistrationScope::registered_actions() const noexcept {
    return context_->actions;
}
std::size_t PluginHost::RegistrationScope::registered_commands() const noexcept {
    return context_->commands;
}
bool PluginHost::register_action_from_plugin(std::string_view plugin_id, std::uint64_t capabilities,
                                             const char *name_utf8, const char *summary_utf8,
                                             AxiomPluginInvokeFnV1 callback, void *plugin_context,
                                             const std::shared_ptr<CallbackGate> &gate) {
    if ((capabilities & AXIOM_PLUGIN_CAP_ACTIONS) == 0 || callback == nullptr) {
        return false;
    }
    const auto name = utf8_to_wide(name_utf8);
    const auto summary = utf8_to_wide(summary_utf8);
    if (!valid_extension_name(name)) {
        return false;
    }
    const bool registered = actions_.register_action(
        name, summary,
        [callback, plugin_context, gate](std::wstring_view payload) {
            const auto utf8 = wide_to_utf8(payload);
            const auto result = PluginHost::invoke_callback(gate, callback, plugin_context, utf8);
            return ActionResult{result.success != 0, utf8_to_wide(result.message_utf8)};
        },
        L"plugin:" + utf8_to_wide(std::string{plugin_id}.c_str()));
    if (!registered) {
        return false;
    }
    std::unique_lock lock{mutex_};
    action_names_by_plugin_[std::string{plugin_id}].push_back(name);
    return true;
}
bool PluginHost::register_command_from_plugin(std::string_view plugin_id,
                                              std::uint64_t capabilities, const char *name_utf8,
                                              const char *usage_utf8, const char *summary_utf8,
                                              AxiomPluginInvokeFnV1 callback, void *plugin_context,
                                              const std::shared_ptr<CallbackGate> &gate) {
    if ((capabilities & AXIOM_PLUGIN_CAP_COMMANDS) == 0 || callback == nullptr) {
        return false;
    }
    auto name = canonical_command(utf8_to_wide(name_utf8));
    if (!valid_extension_name(name)) {
        return false;
    }
    CommandEntry entry{
        {name, utf8_to_wide(usage_utf8), utf8_to_wide(summary_utf8), std::string{plugin_id}},
        callback,
        plugin_context,
        gate,
    };
    std::unique_lock lock{mutex_};
    if (reserved_command_names_.contains(name)) {
        return false;
    }
    if (!commands_.emplace(name, std::move(entry)).second) {
        return false;
    }
    command_names_by_plugin_[std::string{plugin_id}].push_back(name);
    return true;
}
void PluginHost::reserve_command_name(std::wstring name) {
    name = canonical_command(name);
    if (!valid_extension_name(name)) {
        return;
    }
    std::unique_lock lock{mutex_};
    reserved_command_names_.insert(std::move(name));
}
bool PluginHost::contains_command(std::wstring_view name) const {
    std::shared_lock lock{mutex_};
    return commands_.contains(canonical_command(name));
}
std::vector<PluginCommandDescriptor> PluginHost::list_commands() const {
    std::shared_lock lock{mutex_};
    std::vector<PluginCommandDescriptor> output;
    output.reserve(commands_.size());
    for (const auto &[name, entry] : commands_) {
        (void)name;
        output.push_back(entry.descriptor);
    }
    std::sort(output.begin(), output.end(),
              [](const PluginCommandDescriptor &lhs, const PluginCommandDescriptor &rhs) {
                  return lhs.name < rhs.name;
              });
    return output;
}
PluginCommandResult PluginHost::invoke_command(std::wstring_view name,
                                               std::wstring_view arguments) const noexcept {
    CommandEntry entry;
    {
        std::shared_lock lock{mutex_};
        const auto it = commands_.find(canonical_command(name));
        if (it == commands_.end()) {
            return {false, L"Unknown plugin command."};
        }
        entry = it->second;
    }
    const auto started = std::chrono::steady_clock::now();
    PluginCommandResult output;
    try {
        const auto payload = wide_to_utf8(arguments);
        const auto result =
            invoke_callback(entry.gate, entry.callback, entry.plugin_context, payload);
        output = {result.success != 0, utf8_to_wide(result.message_utf8)};
    } catch (...) {
        output = {false, L"Plugin command conversion failed."};
    }
    if (diagnostics_ != nullptr) {
        try {
            const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            std::wstring subject = utf8_to_wide(entry.descriptor.owner_plugin_id.c_str());
            std::wstring message = L"command=" + entry.descriptor.name + L"; " +
                                   (output.success ? std::wstring{L"completed"}
                                                   : std::wstring{L"failed: "} + output.message);
            [[maybe_unused]] const auto diagnostic_id = diagnostics_->record(
                output.success ? DiagnosticSeverity::info : DiagnosticSeverity::error,
                DiagnosticDomain::plugin, std::move(subject), std::move(message), duration);
        } catch (...) {
        }
    }
    return output;
}
void PluginHost::mark_plugin_loaded(LoadedPluginInfo info) {
    if (info.id.empty()) {
        return;
    }
    std::unique_lock lock{mutex_};
    loaded_plugins_.insert_or_assign(info.id, std::move(info));
}
std::vector<LoadedPluginInfo> PluginHost::list_plugins() const {
    std::shared_lock lock{mutex_};
    std::vector<LoadedPluginInfo> output;
    output.reserve(loaded_plugins_.size());
    for (const auto &[id, info] : loaded_plugins_) {
        (void)id;
        output.push_back(info);
    }
    std::sort(
        output.begin(), output.end(),
        [](const LoadedPluginInfo &lhs, const LoadedPluginInfo &rhs) { return lhs.id < rhs.id; });
    return output;
}
void PluginHost::unregister_plugin(std::string_view plugin_id) noexcept {
    try {
        const std::string id{plugin_id};
        std::vector<std::wstring> actions;
        std::vector<std::wstring> commands;
        std::shared_ptr<CallbackGate> gate;
        {
            std::unique_lock lock{mutex_};
            if (const auto gate_it = callback_gates_.find(id); gate_it != callback_gates_.end()) {
                gate = gate_it->second;
                std::scoped_lock gate_lock{gate->mutex};
                gate->accepting = false;
            }
            if (auto it = action_names_by_plugin_.find(id); it != action_names_by_plugin_.end()) {
                actions = std::move(it->second);
                action_names_by_plugin_.erase(it);
            }
            if (auto it = command_names_by_plugin_.find(id); it != command_names_by_plugin_.end()) {
                commands = std::move(it->second);
                command_names_by_plugin_.erase(it);
            }
            for (const auto &name : commands) {
                commands_.erase(name);
            }
            loaded_plugins_.erase(id);
        }
        for (const auto &name : actions) {
            (void)actions_.unregister_action(name);
        }
        if (gate) {
            std::unique_lock gate_lock{gate->mutex};
            gate->idle.wait(gate_lock, [&] { return gate->active == 0; });
        }
        std::unique_lock lock{mutex_};
        const auto it = callback_gates_.find(id);
        if (it != callback_gates_.end() && it->second == gate) {
            callback_gates_.erase(it);
        }
    } catch (...) {
    }
}
} // namespace axiom
