#pragma once
#include "execution_diagnostics.hpp"
#include <functional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
namespace axiom {
struct ActionResult {
    bool success{false};
    std::wstring message;
};
struct ActionDescriptor {
    std::wstring name;
    std::wstring summary;
    std::wstring owner;
};
class ActionRegistry final {
  public:
    using Handler = std::function<ActionResult(std::wstring_view)>;
    explicit ActionRegistry(ExecutionDiagnostics *diagnostics = nullptr) noexcept;
    [[nodiscard]] bool register_action(std::wstring name, std::wstring summary, Handler handler,
                                       std::wstring owner = L"builtin");
    [[nodiscard]] bool unregister_action(std::wstring_view name);
    [[nodiscard]] bool contains(std::wstring_view name) const;
    [[nodiscard]] std::vector<ActionDescriptor> list() const;
    [[nodiscard]] ActionResult invoke(std::wstring_view name,
                                      std::wstring_view payload) const noexcept;

  private:
    struct Entry {
        ActionDescriptor descriptor;
        Handler handler;
    };
    ExecutionDiagnostics *diagnostics_{};
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::wstring, Entry> entries_;
};
} // namespace axiom
