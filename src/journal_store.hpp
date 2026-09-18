#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace axiom {
class DataProtector;
using JournalEntryId = std::uint64_t;
enum class JournalEntryKind : std::uint8_t {
    note = 1,
    activity = 2,
};
struct JournalEntry {
    JournalEntryId id{};
    JournalEntryKind kind{JournalEntryKind::note};
    std::chrono::system_clock::time_point created_at{};
    std::wstring source;
    std::wstring text;
    std::vector<std::wstring> tags;
};
struct JournalContextItem {
    JournalEntryId id{};
    std::chrono::system_clock::time_point created_at{};
    JournalEntryKind kind{JournalEntryKind::note};
    std::wstring text;
    std::vector<std::wstring> tags;
};
struct JournalStoreSnapshot {
    std::uint64_t generation{};
    JournalEntryId next_id{1};
    std::vector<JournalEntry> entries;
};
struct JournalStoreStatus {
    std::filesystem::path path;
    std::uint64_t generation{};
    std::size_t entry_count{};
    std::size_t note_count{};
    std::size_t activity_count{};
    bool recovered_from_alternate{};
    bool protection_requested{};
    bool protected_at_rest{};
    bool protection_migrated{};
    bool protector_available{};
    std::wstring protector_name;
};
class JournalStore final {
  public:
    explicit JournalStore(std::filesystem::path path, std::size_t max_entries = 50000,
                          const DataProtector *protector = nullptr,
                          bool protection_enabled = false);
    JournalStore(const JournalStore &) = delete;
    JournalStore &operator=(const JournalStore &) = delete;
    [[nodiscard]] JournalStoreSnapshot load();
    [[nodiscard]] JournalEntryId
    add_note(std::wstring text, std::vector<std::wstring> tags = {},
             std::chrono::system_clock::time_point created_at = std::chrono::system_clock::now());
    [[nodiscard]] JournalEntryId add_activity(
        std::wstring source, std::wstring text, std::vector<std::wstring> tags = {},
        std::chrono::system_clock::time_point created_at = std::chrono::system_clock::now());
    [[nodiscard]] std::optional<JournalEntry> find(JournalEntryId id) const;
    [[nodiscard]] std::vector<JournalEntry> recent(std::size_t limit = 50) const;
    [[nodiscard]] std::vector<JournalEntry> between(std::chrono::system_clock::time_point begin,
                                                    std::chrono::system_clock::time_point end,
                                                    std::size_t limit = 200) const;
    [[nodiscard]] std::vector<JournalEntry> search(std::wstring_view query,
                                                   std::size_t limit = 50) const;
    [[nodiscard]] std::vector<JournalEntry> tagged(std::wstring_view tag,
                                                   std::size_t limit = 50) const;
    [[nodiscard]] std::vector<JournalContextItem> context(std::wstring_view query,
                                                          std::size_t limit = 20,
                                                          std::size_t max_characters = 12000) const;
    [[nodiscard]] bool erase(JournalEntryId id);
    std::size_t clear_notes();
    std::size_t prune_before(std::chrono::system_clock::time_point cutoff,
                             std::optional<JournalEntryKind> kind = std::nullopt);
    std::size_t set_max_entries(std::size_t max_entries);
    std::size_t import_entries(std::vector<JournalEntry> entries);
    void set_protection_enabled(bool enabled);
    [[nodiscard]] std::vector<std::byte> plaintext_snapshot_bytes() const;
    [[nodiscard]] JournalStoreStatus status() const;
    [[nodiscard]] const std::filesystem::path &path() const noexcept {
        return path_;
    }
    static std::vector<std::wstring> normalize_tags(std::span<const std::wstring> tags);

  private:
    std::filesystem::path path_;
    std::size_t max_entries_{};
    mutable std::mutex mutex_;
    std::uint64_t generation_{};
    JournalEntryId next_id_{1};
    std::vector<JournalEntry> entries_;
    bool recovered_from_alternate_{};
    const DataProtector *protector_{};
    bool protection_enabled_{};
    bool protected_at_rest_{};
    bool protection_migrated_{};
    bool skip_primary_rotation_once_{};
    [[nodiscard]] JournalEntryId add_entry(JournalEntry entry);
    void save_locked();
    void enforce_limit_locked();
};
} // namespace axiom
