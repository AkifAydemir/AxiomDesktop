#include "notification_center.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>
namespace axiom {
NotificationCenter::NotificationCenter(std::size_t retention_limit)
    : retention_limit_{retention_limit} {
    if (retention_limit_ == 0) {
        throw std::invalid_argument{"NotificationCenter retention limit must be non-zero."};
    }
}
std::uint64_t NotificationCenter::publish(NotificationLevel level, std::wstring title,
                                          std::wstring body) {
    if (title.empty()) {
        title = L"Axiom";
    }
    std::scoped_lock lock{mutex_};
    Notification notification{
        next_id_++, level, std::move(title), std::move(body), std::chrono::system_clock::now(),
    };
    const auto id = notification.id;
    pending_.push_back(notification);
    history_.push_front(std::move(notification));
    ++total_published_;
    while (history_.size() > retention_limit_) {
        history_.pop_back();
        ++total_dropped_;
    }
    while (pending_.size() > retention_limit_) {
        pending_.pop_front();
        ++total_dropped_;
    }
    return id;
}
std::vector<Notification> NotificationCenter::drain_pending(std::size_t limit) {
    std::vector<Notification> output;
    if (limit == 0) {
        return output;
    }
    std::scoped_lock lock{mutex_};
    const auto count = std::min(limit, pending_.size());
    output.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        output.push_back(std::move(pending_.front()));
        pending_.pop_front();
    }
    return output;
}
std::vector<Notification> NotificationCenter::recent(std::size_t limit) const {
    std::scoped_lock lock{mutex_};
    const auto count = std::min(limit, history_.size());
    return {history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(count)};
}
std::optional<Notification> NotificationCenter::find(std::uint64_t id) const {
    std::scoped_lock lock{mutex_};
    const auto it = std::find_if(history_.begin(), history_.end(),
                                 [id](const Notification &item) { return item.id == id; });
    return it == history_.end() ? std::nullopt : std::optional<Notification>{*it};
}
NotificationStats NotificationCenter::stats() const {
    std::scoped_lock lock{mutex_};
    return NotificationStats{
        pending_.size(),
        history_.size(),
        total_published_,
        total_dropped_,
    };
}
void NotificationCenter::clear_history() {
    std::scoped_lock lock{mutex_};
    history_.clear();
}
} // namespace axiom
