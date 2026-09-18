#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace axiom {
enum class ClipboardKind {
    text,
    url,
    path,
    json,
    code,
};
[[nodiscard]] std::wstring_view to_string(ClipboardKind kind) noexcept;
struct ClipboardEntry {
    std::uint64_t id{};
    ClipboardKind kind{ClipboardKind::text};
    std::wstring text;
    std::chrono::system_clock::time_point captured_at{};
    std::uint32_t capture_count{1};
    bool pinned{false};
};
struct ClipboardStoreSnapshot {
    std::size_t entries{};
    std::size_t pinned{};
    std::size_t text_bytes{};
    std::uint64_t next_id{};
};
class ClipboardStore final {
  public:
    explicit ClipboardStore(std::size_t max_entries = 200,
                            std::size_t max_text_bytes = 2 * 1024 * 1024,
                            std::size_t max_item_text_bytes = 256 * 1024);
    ClipboardStore(const ClipboardStore &) = delete;
    ClipboardStore &operator=(const ClipboardStore &) = delete;
    [[nodiscard]] std::optional<std::uint64_t> record_text(std::wstring_view text);
    [[nodiscard]] std::vector<ClipboardEntry> recent(std::size_t limit = 20) const;
    [[nodiscard]] std::vector<ClipboardEntry> search(std::wstring_view query,
                                                     std::size_t limit = 20) const;
    [[nodiscard]] std::optional<ClipboardEntry> find(std::uint64_t id) const;
    [[nodiscard]] bool set_pinned(std::uint64_t id, bool pinned);
    [[nodiscard]] bool erase(std::uint64_t id);
    std::size_t clear_unpinned();
    void clear_all();
    [[nodiscard]] ClipboardStoreSnapshot snapshot() const;

  private:
    mutable std::mutex mutex_;
    std::deque<ClipboardEntry> entries_;
    std::size_t max_entries_{};
    std::size_t max_text_bytes_{};
    std::size_t max_item_text_bytes_{};
    std::size_t text_bytes_{};
    std::uint64_t next_id_{1};
    static ClipboardKind classify(std::wstring_view text);
    static std::size_t text_bytes(std::wstring_view text) noexcept;
    void enforce_limits_locked();
};
} // namespace axiom
