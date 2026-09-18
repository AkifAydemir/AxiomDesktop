#pragma once
#include "plugin_trust_store.hpp"
#include <filesystem>
#include <string>
#include <string_view>
namespace axiom {
struct PluginPackageResult {
    bool success{false};
    std::string plugin_id;
    std::string message;
};
struct PluginVerificationResult {
    bool success{false};
    std::filesystem::path manifest_path;
    std::filesystem::path library_path;
    std::string manifest_sha256;
    std::string library_sha256;
    std::string message;
};
class PluginPackageManager final {
  public:
    PluginPackageManager(std::filesystem::path root, PluginTrustStore &trust_store);
    [[nodiscard]] PluginPackageResult install(const std::filesystem::path &manifest_path);
    [[nodiscard]] PluginPackageResult remove(std::string_view plugin_id);
    [[nodiscard]] PluginVerificationResult verify(const PluginTrustRecord &record) const;
    [[nodiscard]] std::filesystem::path plugin_directory(std::string_view plugin_id) const;
    [[nodiscard]] const std::filesystem::path &root() const noexcept;

  private:
    std::filesystem::path root_;
    PluginTrustStore &trust_store_;
};
} // namespace axiom
