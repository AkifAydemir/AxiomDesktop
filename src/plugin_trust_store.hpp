#pragma once
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace axiom {
struct PluginTrustRecord {
    std::string id;
    std::string version;
    std::string library;
    bool enabled{false};
    bool quarantined{false};
    std::string manifest_sha256;
    std::string library_sha256;
    std::string quarantine_reason;
};
struct PluginTrustStatus {
    std::filesystem::path path;
    std::size_t records{};
    std::size_t enabled{};
    std::size_t quarantined{};
    bool recovered_from_alternate{false};
};
class PluginTrustStore final {
  public:
    explicit PluginTrustStore(std::filesystem::path path);
    [[nodiscard]] std::vector<PluginTrustRecord> load();
    void save(std::vector<PluginTrustRecord> records);
    [[nodiscard]] std::vector<PluginTrustRecord> list() const;
    [[nodiscard]] std::optional<PluginTrustRecord> find(std::string_view id) const;
    void upsert(PluginTrustRecord record);
    [[nodiscard]] bool set_enabled(std::string_view id, bool enabled);
    [[nodiscard]] bool quarantine(std::string_view id, std::string reason);
    [[nodiscard]] bool erase(std::string_view id);
    [[nodiscard]] PluginTrustStatus status() const;
    [[nodiscard]] const std::filesystem::path &path() const noexcept;

  private:
    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::vector<PluginTrustRecord> records_;
    bool recovered_from_alternate_{false};
    void commit_locked(std::vector<PluginTrustRecord> records);
};
} // namespace axiom
