#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace axiom {
struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string library;
    std::uint32_t abi_version{0};
    std::uint64_t capabilities{0};
    std::filesystem::path source_path;
};
struct PluginManifestResult {
    bool success{false};
    PluginManifest manifest;
    std::string error;
};
[[nodiscard]] PluginManifestResult parse_plugin_manifest(const std::filesystem::path &path);
[[nodiscard]] std::vector<std::filesystem::path>
discover_plugin_manifests(const std::filesystem::path &directory);
[[nodiscard]] bool is_valid_plugin_id(std::string_view value) noexcept;
} // namespace axiom
