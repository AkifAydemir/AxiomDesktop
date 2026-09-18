#include "action_registry.hpp"
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <mutex>
#include <stdexcept>
#include <utility>
namespace axiom {
namespace {
[[nodiscard]] std::wstring canonical_name(std::wstring_view value) {
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
[[nodiscard]] bool valid_name(std::wstring_view value) noexcept {
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
[[nodiscard]] std::wstring exception_message(const std::exception &exception) {
    const std::string narrow = exception.what();
    return {narrow.begin(), narrow.end()};
}
} // namespace
ActionRegistry::ActionRegistry(ExecutionDiagnostics *diagnostics) noexcept
    : diagnostics_{diagnostics} {}
bool ActionRegistry::register_action(std::wstring name, std::wstring summary, Handler handler,
                                     std::wstring owner) {
    if (!valid_name(name)) {
        throw std::invalid_argument{"Action name must contain only ASCII letters, digits, '-' or "
                                    "'_' and be <= 64 characters."};
    }
    if (!handler) {
        throw std::invalid_argument{"Action handler must not be empty."};
    }
    name = canonical_name(name);
    std::unique_lock lock{mutex_};
    return entries_
        .emplace(name, Entry{ActionDescriptor{name, std::move(summary), std::move(owner)},
                             std::move(handler)})
        .second;
}
bool ActionRegistry::unregister_action(std::wstring_view name) {
    const auto canonical = canonical_name(name);
    std::unique_lock lock{mutex_};
    return entries_.erase(canonical) != 0;
}
bool ActionRegistry::contains(std::wstring_view name) const {
    const auto canonical = canonical_name(name);
    std::shared_lock lock{mutex_};
    return entries_.contains(canonical);
}
std::vector<ActionDescriptor> ActionRegistry::list() const {
    std::shared_lock lock{mutex_};
    std::vector<ActionDescriptor> output;
    output.reserve(entries_.size());
    for (const auto &[name, entry] : entries_) {
        (void)name;
        output.push_back(entry.descriptor);
    }
    std::sort(output.begin(), output.end(),
              [](const ActionDescriptor &lhs, const ActionDescriptor &rhs) {
                  return lhs.name < rhs.name;
              });
    return output;
}
ActionResult ActionRegistry::invoke(std::wstring_view name,
                                    std::wstring_view payload) const noexcept {
    Handler handler;
    ActionDescriptor descriptor;
    {
        const auto canonical = canonical_name(name);
        std::shared_lock lock{mutex_};
        const auto it = entries_.find(canonical);
        if (it == entries_.end()) {
            if (diagnostics_ != nullptr) {
                try {
                    [[maybe_unused]] const auto diagnostic_id = diagnostics_->record(
                        DiagnosticSeverity::error, DiagnosticDomain::action, canonical,
                        L"Action invocation failed: action is not registered.");
                } catch (...) {
                }
            }
            return {false, L"Unknown automation action: " + canonical};
        }
        handler = it->second.handler;
        descriptor = it->second.descriptor;
    }
    const auto started = std::chrono::steady_clock::now();
    ActionResult result;
    try {
        result = handler(payload);
        if (result.message.empty()) {
            result.message = result.success ? L"Action completed." : L"Action failed.";
        }
    } catch (const std::exception &exception) {
        result = {false, L"Action threw: " + exception_message(exception)};
    } catch (...) {
        result = {false, L"Action failed with an unknown exception."};
    }
    if (diagnostics_ != nullptr) {
        try {
            const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            std::wstring message = L"owner=" + descriptor.owner + L"; " +
                                   (result.success ? std::wstring{L"completed"}
                                                   : std::wstring{L"failed: "} + result.message);
            [[maybe_unused]] const auto diagnostic_id = diagnostics_->record(
                result.success ? DiagnosticSeverity::info : DiagnosticSeverity::error,
                DiagnosticDomain::action, descriptor.name, message, duration);
            constexpr std::wstring_view plugin_prefix{L"plugin:"};
            if (descriptor.owner.starts_with(plugin_prefix)) {
                [[maybe_unused]] const auto plugin_diagnostic_id = diagnostics_->record(
                    result.success ? DiagnosticSeverity::info : DiagnosticSeverity::error,
                    DiagnosticDomain::plugin, descriptor.owner.substr(plugin_prefix.size()),
                    L"action=" + descriptor.name + L"; " +
                        (result.success ? std::wstring{L"completed"}
                                        : std::wstring{L"failed: "} + result.message),
                    duration);
            }
        } catch (...) {
        }
    }
    return result;
}
} // namespace axiom
