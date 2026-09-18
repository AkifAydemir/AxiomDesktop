#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
namespace axiom {
enum class NotificationLevel {
    info,
    success,
    warning,
    error,
};
struct Notification {
    std::uint64_t id{};
    NotificationLevel level{NotificationLevel::info};
    std::wstring title;
    std::wstring body;
    std::chrono::system_clock::time_point created_at{};
};
struct NotificationStats {
    std::size_t pending{};
    std::size_t retained{};
    std::uint64_t total_published{};
    std::uint64_t total_dropped{};
};
class NotificationCenter final {
  public:
    explicit NotificationCenter(std::size_t retention_limit = 128);
    [[nodiscard]] std::uint64_t publish(NotificationLevel level, std::wstring title,
                                        std::wstring body);
    [[nodiscard]] std::vector<Notification> drain_pending(std::size_t limit = 32);
    [[nodiscard]] std::vector<Notification> recent(std::size_t limit = 32) const;
    [[nodiscard]] std::optional<Notification> find(std::uint64_t id) const;
    [[nodiscard]] NotificationStats stats() const;
    void clear_history();

  private:
    mutable std::mutex mutex_;
    std::size_t retention_limit_{};
    std::uint64_t next_id_{1};
    std::uint64_t total_published_{};
    std::uint64_t total_dropped_{};
    std::deque<Notification> pending_;
    std::deque<Notification> history_;
};
} // namespace axiom
