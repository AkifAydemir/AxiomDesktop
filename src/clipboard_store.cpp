#include "clipboard_store.hpp"
#include <algorithm>
#include <cwctype>
#include <limits>
#include <utility>
namespace axiom {
namespace {
[[nodiscard]] std::wstring trim_copy(std::wstring_view input) {
    const auto is_space = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    std::size_t first = 0;
    while (first < input.size() && is_space(input[first])) {
        ++first;
    }
    std::size_t last = input.size();
    while (last > first && is_space(input[last - 1])) {
        --last;
    }
    return std::wstring{input.substr(first, last - first)};
}
[[nodiscard]] std::wstring lower_copy(std::wstring_view value) {
    std::wstring output{value};
    std::transform(output.begin(), output.end(), output.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return output;
}
[[nodiscard]] bool starts_with_ignore_case(std::wstring_view value, std::wstring_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::towlower(value[i]) != std::towlower(prefix[i])) {
            return false;
        }
    }
    return true;
}
[[nodiscard]] bool looks_like_path(std::wstring_view value) {
    if (value.size() >= 3 && std::iswalpha(value[0]) != 0 && value[1] == L':' &&
        (value[2] == L'\\' || value[2] == L'/')) {
        return true;
    }
    return starts_with_ignore_case(value, L"\\\\") || starts_with_ignore_case(value, L"~/") ||
           starts_with_ignore_case(value, L"./") || starts_with_ignore_case(value, L"../");
}
[[nodiscard]] bool looks_like_json(std::wstring_view value) {
    if (value.size() < 2) {
        return false;
    }
    const bool object = value.front() == L'{' && value.back() == L'}';
    const bool array = value.front() == L'[' && value.back() == L']';
    if (!object && !array) {
        return false;
    }
    if (object) {
        return value.find(L':') != std::wstring_view::npos;
    }
    return value.find(L',') != std::wstring_view::npos ||
           value.find(L'{') != std::wstring_view::npos ||
           value.find(L'"') != std::wstring_view::npos;
}
[[nodiscard]] bool looks_like_code(std::wstring_view value) {
    constexpr std::wstring_view markers[] = {
        L"#include", L"namespace ", L"class ",  L"struct ",   L"template<", L"template <", L"std::",
        L"public:",  L"private:",   L"return ", L"function ", L"const ",    L"auto ",
    };
    for (const auto marker : markers) {
        if (value.find(marker) != std::wstring_view::npos) {
            return true;
        }
    }
    const auto semicolon = value.find(L';');
    const auto open_brace = value.find(L'{');
    const auto close_brace = value.find(L'}');
    return semicolon != std::wstring_view::npos && open_brace != std::wstring_view::npos &&
           close_brace != std::wstring_view::npos;
}
[[nodiscard]] int search_score(const ClipboardEntry &entry, std::wstring_view lowered_query) {
    const auto lowered = lower_copy(entry.text);
    const auto position = lowered.find(lowered_query);
    if (position == std::wstring::npos) {
        return std::numeric_limits<int>::min();
    }
    int score = 1000;
    if (position == 0) {
        score += 300;
    } else {
        score -= static_cast<int>(std::min<std::size_t>(position, 200));
    }
    if (entry.pinned) {
        score += 100;
    }
    score += static_cast<int>(std::min<std::uint32_t>(entry.capture_count, 20));
    return score;
}
} // namespace
std::wstring_view to_string(ClipboardKind kind) noexcept {
    switch (kind) {
    case ClipboardKind::url:
        return L"url";
    case ClipboardKind::path:
        return L"path";
    case ClipboardKind::json:
        return L"json";
    case ClipboardKind::code:
        return L"code";
    case ClipboardKind::text:
    default:
        return L"text";
    }
}
ClipboardStore::ClipboardStore(std::size_t max_entries, std::size_t max_text_bytes,
                               std::size_t max_item_text_bytes)
    : max_entries_{std::max<std::size_t>(max_entries, 1)},
      max_text_bytes_{std::max<std::size_t>(max_text_bytes, sizeof(wchar_t))},
      max_item_text_bytes_{std::max<std::size_t>(max_item_text_bytes, sizeof(wchar_t))} {}
std::optional<std::uint64_t> ClipboardStore::record_text(std::wstring_view text) {
    auto normalized = trim_copy(text);
    if (normalized.empty()) {
        return std::nullopt;
    }
    const auto bytes = text_bytes(normalized);
    if (bytes > max_item_text_bytes_ || bytes > max_text_bytes_) {
        return std::nullopt;
    }
    std::scoped_lock lock{mutex_};
    const auto duplicate =
        std::find_if(entries_.begin(), entries_.end(),
                     [&](const ClipboardEntry &entry) { return entry.text == normalized; });
    if (duplicate != entries_.end()) {
        ClipboardEntry refreshed = std::move(*duplicate);
        entries_.erase(duplicate);
        refreshed.captured_at = std::chrono::system_clock::now();
        if (refreshed.capture_count != std::numeric_limits<std::uint32_t>::max()) {
            ++refreshed.capture_count;
        }
        const auto id = refreshed.id;
        entries_.push_front(std::move(refreshed));
        return id;
    }
    ClipboardEntry entry;
    entry.id = next_id_++;
    entry.kind = classify(normalized);
    entry.text = std::move(normalized);
    entry.captured_at = std::chrono::system_clock::now();
    text_bytes_ += bytes;
    const auto id = entry.id;
    entries_.push_front(std::move(entry));
    enforce_limits_locked();
    return id;
}
std::vector<ClipboardEntry> ClipboardStore::recent(std::size_t limit) const {
    std::scoped_lock lock{mutex_};
    const auto count = std::min(limit, entries_.size());
    std::vector<ClipboardEntry> output;
    output.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        output.push_back(entries_[i]);
    }
    return output;
}
std::vector<ClipboardEntry> ClipboardStore::search(std::wstring_view query,
                                                   std::size_t limit) const {
    const auto normalized = lower_copy(trim_copy(query));
    if (normalized.empty() || limit == 0) {
        return {};
    }
    struct Candidate {
        ClipboardEntry entry;
        int score{};
        std::size_t recency{};
    };
    std::scoped_lock lock{mutex_};
    std::vector<Candidate> candidates;
    candidates.reserve(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const int score = search_score(entries_[i], normalized);
        if (score == std::numeric_limits<int>::min()) {
            continue;
        }
        candidates.push_back({entries_[i], score, i});
    }
    const auto candidate_order = [](const Candidate &lhs, const Candidate &rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        return lhs.recency < rhs.recency;
    };
    if (candidates.size() > limit) {
        std::partial_sort(candidates.begin(),
                          candidates.begin() + static_cast<std::ptrdiff_t>(limit), candidates.end(),
                          candidate_order);
        candidates.resize(limit);
    } else {
        std::sort(candidates.begin(), candidates.end(), candidate_order);
    }
    std::vector<ClipboardEntry> output;
    output.reserve(candidates.size());
    for (auto &candidate : candidates) {
        output.push_back(std::move(candidate.entry));
    }
    return output;
}
std::optional<ClipboardEntry> ClipboardStore::find(std::uint64_t id) const {
    std::scoped_lock lock{mutex_};
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const ClipboardEntry &entry) { return entry.id == id; });
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return *it;
}
bool ClipboardStore::set_pinned(std::uint64_t id, bool pinned) {
    std::scoped_lock lock{mutex_};
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const ClipboardEntry &entry) { return entry.id == id; });
    if (it == entries_.end()) {
        return false;
    }
    it->pinned = pinned;
    enforce_limits_locked();
    return true;
}
bool ClipboardStore::erase(std::uint64_t id) {
    std::scoped_lock lock{mutex_};
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const ClipboardEntry &entry) { return entry.id == id; });
    if (it == entries_.end()) {
        return false;
    }
    text_bytes_ -= text_bytes(it->text);
    entries_.erase(it);
    return true;
}
std::size_t ClipboardStore::clear_unpinned() {
    std::scoped_lock lock{mutex_};
    std::size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->pinned) {
            ++it;
            continue;
        }
        text_bytes_ -= text_bytes(it->text);
        it = entries_.erase(it);
        ++removed;
    }
    return removed;
}
void ClipboardStore::clear_all() {
    std::scoped_lock lock{mutex_};
    entries_.clear();
    text_bytes_ = 0;
}
ClipboardStoreSnapshot ClipboardStore::snapshot() const {
    std::scoped_lock lock{mutex_};
    const auto pinned = static_cast<std::size_t>(
        std::count_if(entries_.begin(), entries_.end(),
                      [](const ClipboardEntry &entry) { return entry.pinned; }));
    return {
        entries_.size(),
        pinned,
        text_bytes_,
        next_id_,
    };
}
ClipboardKind ClipboardStore::classify(std::wstring_view text) {
    const auto normalized = trim_copy(text);
    if (starts_with_ignore_case(normalized, L"https://") ||
        starts_with_ignore_case(normalized, L"http://")) {
        return ClipboardKind::url;
    }
    if (looks_like_path(normalized)) {
        return ClipboardKind::path;
    }
    if (looks_like_json(normalized)) {
        return ClipboardKind::json;
    }
    if (looks_like_code(normalized)) {
        return ClipboardKind::code;
    }
    return ClipboardKind::text;
}
std::size_t ClipboardStore::text_bytes(std::wstring_view text) noexcept {
    return text.size() * sizeof(wchar_t);
}
void ClipboardStore::enforce_limits_locked() {
    const auto over_limits = [&] {
        return entries_.size() > max_entries_ || text_bytes_ > max_text_bytes_;
    };
    while (over_limits()) {
        auto removable = std::find_if(entries_.rbegin(), entries_.rend(),
                                      [](const ClipboardEntry &entry) { return !entry.pinned; });
        if (removable == entries_.rend()) {
            break;
        }
        const auto erase_it = std::next(removable).base();
        text_bytes_ -= text_bytes(erase_it->text);
        entries_.erase(erase_it);
    }
}
} // namespace axiom
