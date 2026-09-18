#pragma once
#ifdef _WIN32
#include "plugin_host.hpp"
#include "plugin_manifest.hpp"
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>
namespace axiom {
struct PluginLoadIssue {
    std::filesystem::path manifest_path;
    std::wstring message;
};
struct PluginLoadSummary {
    std::size_t loaded{};
    std::vector<PluginLoadIssue> issues;
};
struct PluginLoadResult {
    bool success{};
    std::string plugin_id;
    std::filesystem::path manifest_path;
    std::wstring message;
};
class PluginLoaderWin32 final {
  public:
    explicit PluginLoaderWin32(PluginHost &host, ExecutionDiagnostics *diagnostics = nullptr);
    ~PluginLoaderWin32();
    PluginLoaderWin32(const PluginLoaderWin32 &) = delete;
    PluginLoaderWin32 &operator=(const PluginLoaderWin32 &) = delete;
    [[nodiscard]] PluginLoadResult load_manifest(const std::filesystem::path &manifest_path);
    [[nodiscard]] bool unload_plugin(std::string_view plugin_id) noexcept;
    [[nodiscard]] bool is_loaded(std::string_view plugin_id) const noexcept;
    [[nodiscard]] std::vector<std::string> loaded_plugin_ids() const;
    void unload_all() noexcept;

  private:
    struct LoadedModule {
        std::string plugin_id;
        HMODULE module{};
        AxiomPluginUnloadFnV1 unload{};
    };
    PluginHost &host_;
    ExecutionDiagnostics *diagnostics_{};
    std::vector<LoadedModule> modules_;
};
} // namespace axiom
#endif
