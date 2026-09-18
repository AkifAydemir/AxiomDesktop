#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <vector>
namespace axiom {
using ReminderId = std::uint64_t;
struct Reminder {
    ReminderId id{};
    std::chrono::system_clock::time_point due_at{};
    std::chrono::milliseconds repeat_interval{};
    std::uint64_t fire_count{};
    std::wstring message;
    [[nodiscard]] bool recurring() const noexcept {
        return repeat_interval > std::chrono::milliseconds::zero();
    }
};
struct ReminderStoreSnapshot {
    std::uint64_t generation{};
    ReminderId next_id{1};
    std::vector<Reminder> reminders;
};
struct ReminderStoreStatus {
    std::filesystem::path path;
    std::uint64_t generation{};
    std::size_t record_count{};
    bool recovered_from_alternate{};
};
class ReminderStore final {
  public:
    explicit ReminderStore(std::filesystem::path path);
    ReminderStore(const ReminderStore &) = delete;
    ReminderStore &operator=(const ReminderStore &) = delete;
    [[nodiscard]] ReminderStoreSnapshot load();
    void save(ReminderId next_id, std::span<const Reminder> reminders);
    [[nodiscard]] ReminderStoreStatus status() const;
    [[nodiscard]] const std::filesystem::path &path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::uint64_t generation_{};
    std::size_t record_count_{};
    bool recovered_from_alternate_{};
};
} // namespace axiom
