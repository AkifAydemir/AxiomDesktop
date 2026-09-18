#include "file_index.hpp"
#include <algorithm>
#include <cwctype>
#include <optional>
#include <system_error>
#include <utility>
namespace axiom {
namespace {
[[nodiscard]] std::wstring lower_copy(std::wstring_view value) {
    std::wstring output{value};
    std::transform(output.begin(), output.end(), output.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return output;
}
[[nodiscard]] std::wstring trim(std::wstring_view input) {
    const auto is_space = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    auto first = input.begin();
    while (first != input.end() && is_space(*first)) {
        ++first;
    }
    auto last = input.end();
    while (last != first && is_space(*(last - 1))) {
        --last;
    }
    return {first, last};
}
[[nodiscard]] std::wstring normalized_path_key(const std::filesystem::path &path) {
    return lower_copy(path.lexically_normal().generic_wstring());
}
[[nodiscard]] std::filesystem::path normalize_root(std::filesystem::path root) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(root, error);
    if (!error) {
        root = std::move(absolute);
    }
    return root.lexically_normal();
}
[[nodiscard]] std::vector<std::wstring> normalize_exclusions(std::vector<std::wstring> exclusions) {
    std::vector<std::wstring> normalized;
    normalized.reserve(exclusions.size());
    for (auto &exclusion : exclusions) {
        auto value = lower_copy(trim(exclusion));
        if (value.empty() || value.find_first_of(L"/\\") != std::wstring::npos) {
            continue;
        }
        if (std::find(normalized.begin(), normalized.end(), value) == normalized.end()) {
            normalized.push_back(std::move(value));
        }
    }
    std::sort(normalized.begin(), normalized.end());
    return normalized;
}
[[nodiscard]] std::vector<std::wstring> split_terms(std::wstring_view query) {
    std::vector<std::wstring> terms;
    const auto normalized = lower_copy(trim(query));
    std::size_t cursor = 0;
    while (cursor < normalized.size()) {
        while (cursor < normalized.size() && std::iswspace(normalized[cursor]) != 0) {
            ++cursor;
        }
        if (cursor >= normalized.size()) {
            break;
        }
        const std::size_t begin = cursor;
        while (cursor < normalized.size() && std::iswspace(normalized[cursor]) == 0) {
            ++cursor;
        }
        terms.emplace_back(normalized.substr(begin, cursor - begin));
    }
    return terms;
}
[[nodiscard]] std::optional<int> subsequence_score(std::wstring_view haystack,
                                                   std::wstring_view needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t needle_index = 0;
    int gaps = 0;
    int first_position = -1;
    int previous_position = -1;
    for (std::size_t i = 0; i < haystack.size() && needle_index < needle.size(); ++i) {
        if (haystack[i] != needle[needle_index]) {
            continue;
        }
        const int current = static_cast<int>(i);
        if (first_position < 0) {
            first_position = current;
        }
        if (previous_position >= 0) {
            gaps += current - previous_position - 1;
        }
        previous_position = current;
        ++needle_index;
    }
    if (needle_index != needle.size()) {
        return std::nullopt;
    }
    return 120 - std::min(gaps * 3, 90) - std::min(first_position, 25);
}
[[nodiscard]] std::optional<int> score_term(std::wstring_view name, std::wstring_view path,
                                            std::wstring_view term) {
    if (term.empty()) {
        return 0;
    }
    if (name == term) {
        return 1000;
    }
    if (name.starts_with(term)) {
        return 760 - static_cast<int>(std::min<std::size_t>(name.size() - term.size(), 80));
    }
    if (const auto position = name.find(term); position != std::wstring_view::npos) {
        return 560 - static_cast<int>(std::min<std::size_t>(position * 3, 180));
    }
    if (const auto fuzzy = subsequence_score(name, term)) {
        return *fuzzy;
    }
    if (const auto position = path.find(term); position != std::wstring_view::npos) {
        return 90 - static_cast<int>(std::min<std::size_t>(position / 8, 60));
    }
    return std::nullopt;
}
[[nodiscard]] bool is_builtin_exclusion(std::wstring_view name) {
    return name == L".git" || name == L".svn" || name == L"node_modules" ||
           name == L"$recycle.bin" || name == L"system volume information";
}
[[nodiscard]] bool
should_skip_directory(const std::filesystem::path &path,
                      const std::vector<std::wstring> &excluded_directory_names) {
    const auto name = lower_copy(path.filename().wstring());
    if (is_builtin_exclusion(name)) {
        return true;
    }
    return std::find(excluded_directory_names.begin(), excluded_directory_names.end(), name) !=
           excluded_directory_names.end();
}
[[nodiscard]] bool is_same_or_descendant(std::wstring_view candidate, std::wstring_view base) {
    if (candidate == base) {
        return true;
    }
    if (candidate.size() <= base.size() || !candidate.starts_with(base)) {
        return false;
    }
    return candidate[base.size()] == L'/';
}
[[nodiscard]] bool path_is_inside_roots(const std::filesystem::path &path,
                                        const std::vector<std::filesystem::path> &roots) {
    const auto key = normalized_path_key(path);
    return std::any_of(roots.begin(), roots.end(), [&](const auto &root) {
        return is_same_or_descendant(key, normalized_path_key(root));
    });
}
[[nodiscard]] bool
path_has_excluded_component(const std::filesystem::path &path,
                            const std::vector<std::wstring> &excluded_directory_names) {
    for (const auto &component : path) {
        const auto name = lower_copy(component.wstring());
        if (is_builtin_exclusion(name) ||
            std::find(excluded_directory_names.begin(), excluded_directory_names.end(), name) !=
                excluded_directory_names.end()) {
            return true;
        }
    }
    return false;
}
} // namespace
std::optional<FileIndex::Record>
FileIndex::make_record(const std::filesystem::directory_entry &entry) {
    std::error_code error;
    const bool is_directory = entry.is_directory(error);
    if (error) {
        return std::nullopt;
    }
    const bool is_file = entry.is_regular_file(error);
    if (error || (!is_directory && !is_file)) {
        return std::nullopt;
    }
    std::uintmax_t size = 0;
    if (is_file) {
        size = entry.file_size(error);
        if (error) {
            size = 0;
        }
    }
    const auto path = entry.path().lexically_normal();
    return Record{
        path, lower_copy(path.filename().wstring()), normalized_path_key(path), is_directory, size,
    };
}
void FileIndex::append_subtree(const std::filesystem::path &root,
                               const std::vector<std::wstring> &excluded_directory_names,
                               std::vector<Record> &output) {
    std::error_code error;
    const std::filesystem::directory_entry root_entry{root, error};
    if (error) {
        return;
    }
    if (root_entry.is_directory(error) && !error &&
        should_skip_directory(root, excluded_directory_names)) {
        return;
    }
    error.clear();
    if (const auto record = make_record(root_entry)) {
        output.push_back(*record);
    }
    if (!root_entry.is_directory(error) || error) {
        return;
    }
    std::filesystem::recursive_directory_iterator iterator{
        root, std::filesystem::directory_options::skip_permission_denied, error};
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        if (error) {
            error.clear();
            iterator.increment(error);
            continue;
        }
        if (iterator->is_directory(error) && !error &&
            should_skip_directory(iterator->path(), excluded_directory_names)) {
            iterator.disable_recursion_pending();
            iterator.increment(error);
            continue;
        }
        error.clear();
        if (const auto record = make_record(*iterator)) {
            output.push_back(*record);
        }
        iterator.increment(error);
    }
}
void FileIndex::recalculate_counts(const std::vector<Record> &records, std::size_t &files,
                                   std::size_t &directories) {
    files = 0;
    directories = 0;
    for (const auto &record : records) {
        if (record.is_directory) {
            ++directories;
        } else {
            ++files;
        }
    }
}
FileIndex::~FileIndex() {
    stop();
}
void FileIndex::start(std::vector<std::filesystem::path> roots,
                      std::vector<std::wstring> excluded_directory_names) {
    std::vector<std::filesystem::path> normalized_roots;
    normalized_roots.reserve(roots.size());
    for (auto &root : roots) {
        if (root.empty()) {
            continue;
        }
        auto normalized = normalize_root(std::move(root));
        const auto key = normalized_path_key(normalized);
        const auto duplicate = std::find_if(
            normalized_roots.begin(), normalized_roots.end(),
            [&](const auto &existing) { return normalized_path_key(existing) == key; });
        if (duplicate == normalized_roots.end()) {
            normalized_roots.push_back(std::move(normalized));
        }
    }
    auto normalized_exclusions = normalize_exclusions(std::move(excluded_directory_names));
    {
        std::unique_lock lock{data_mutex_};
        roots_ = normalized_roots;
        excluded_directory_names_ = normalized_exclusions;
        last_error_.clear();
    }
    (void)launch_scan(std::move(normalized_roots), std::move(normalized_exclusions));
}
void FileIndex::rebuild() {
    (void)rebuild_tracked();
}
std::uint64_t FileIndex::rebuild_tracked() {
    std::vector<std::filesystem::path> roots;
    std::vector<std::wstring> exclusions;
    {
        std::shared_lock lock{data_mutex_};
        roots = roots_;
        exclusions = excluded_directory_names_;
    }
    return launch_scan(std::move(roots), std::move(exclusions));
}
std::uint64_t FileIndex::current_scan_generation() const noexcept {
    return scan_gate_.current_generation();
}
IndexScanWaitResult FileIndex::wait_for_scan(std::uint64_t generation,
                                             std::stop_token stop_token) const noexcept {
    return scan_gate_.wait(generation, stop_token);
}
void FileIndex::stop() noexcept {
    std::scoped_lock lock{worker_mutex_};
    const auto generation = scan_gate_.current_generation();
    if (worker_.joinable()) {
        worker_.request_stop();
        scan_gate_.cancel(generation);
        worker_.join();
    }
}
bool FileIndex::add_root(std::filesystem::path root) {
    if (root.empty()) {
        return false;
    }
    auto normalized = normalize_root(std::move(root));
    const auto key = normalized_path_key(normalized);
    std::vector<std::filesystem::path> roots;
    std::vector<std::wstring> exclusions;
    {
        std::shared_lock lock{data_mutex_};
        roots = roots_;
        exclusions = excluded_directory_names_;
    }
    if (std::any_of(roots.begin(), roots.end(),
                    [&](const auto &existing) { return normalized_path_key(existing) == key; })) {
        return false;
    }
    roots.push_back(std::move(normalized));
    start(std::move(roots), std::move(exclusions));
    return true;
}
bool FileIndex::remove_root(const std::filesystem::path &root) {
    if (root.empty()) {
        return false;
    }
    const auto key = normalized_path_key(normalize_root(root));
    std::vector<std::filesystem::path> roots;
    std::vector<std::wstring> exclusions;
    {
        std::shared_lock lock{data_mutex_};
        roots = roots_;
        exclusions = excluded_directory_names_;
    }
    const auto old_size = roots.size();
    std::erase_if(roots,
                  [&](const auto &existing) { return normalized_path_key(existing) == key; });
    if (roots.size() == old_size) {
        return false;
    }
    start(std::move(roots), std::move(exclusions));
    return true;
}
bool FileIndex::add_exclusion(std::wstring name) {
    auto normalized = lower_copy(trim(name));
    if (normalized.empty() || normalized.find_first_of(L"/\\") != std::wstring::npos ||
        is_builtin_exclusion(normalized)) {
        return false;
    }
    std::vector<std::filesystem::path> roots;
    std::vector<std::wstring> exclusions;
    {
        std::shared_lock lock{data_mutex_};
        roots = roots_;
        exclusions = excluded_directory_names_;
    }
    if (std::find(exclusions.begin(), exclusions.end(), normalized) != exclusions.end()) {
        return false;
    }
    exclusions.push_back(std::move(normalized));
    start(std::move(roots), std::move(exclusions));
    return true;
}
bool FileIndex::remove_exclusion(std::wstring_view name) {
    const auto normalized = lower_copy(trim(name));
    if (normalized.empty()) {
        return false;
    }
    std::vector<std::filesystem::path> roots;
    std::vector<std::wstring> exclusions;
    {
        std::shared_lock lock{data_mutex_};
        roots = roots_;
        exclusions = excluded_directory_names_;
    }
    const auto old_size = exclusions.size();
    std::erase(exclusions, normalized);
    if (exclusions.size() == old_size) {
        return false;
    }
    start(std::move(roots), std::move(exclusions));
    return true;
}
void FileIndex::refresh_path(const std::filesystem::path &path) {
    if (path.empty()) {
        return;
    }
    if (state_.load(std::memory_order_acquire) == IndexState::indexing) {
        std::unique_lock lock{data_mutex_};
        if (state_.load(std::memory_order_relaxed) == IndexState::indexing) {
            pending_changes_.push_back(PendingChange{path, false});
        }
    }
    std::vector<std::filesystem::path> roots;
    std::vector<std::wstring> exclusions;
    {
        std::shared_lock lock{data_mutex_};
        roots = roots_;
        exclusions = excluded_directory_names_;
    }
    const auto normalized = normalize_root(path);
    if (!path_is_inside_roots(normalized, roots)) {
        return;
    }
    if (path_has_excluded_component(normalized, exclusions)) {
        remove_path(normalized);
        return;
    }
    std::error_code error;
    if (!std::filesystem::exists(normalized, error) || error) {
        remove_path(normalized);
        return;
    }
    std::vector<Record> replacements;
    append_subtree(normalized, exclusions, replacements);
    const auto key = normalized_path_key(normalized);
    {
        std::unique_lock lock{data_mutex_};
        std::erase_if(records_, [&](const Record &record) {
            return is_same_or_descendant(record.path_lower, key);
        });
        records_.insert(records_.end(), std::make_move_iterator(replacements.begin()),
                        std::make_move_iterator(replacements.end()));
        recalculate_counts(records_, file_count_, directory_count_);
        ++incremental_update_count_;
    }
}
void FileIndex::remove_path(const std::filesystem::path &path) {
    if (path.empty()) {
        return;
    }
    if (state_.load(std::memory_order_acquire) == IndexState::indexing) {
        std::unique_lock lock{data_mutex_};
        if (state_.load(std::memory_order_relaxed) == IndexState::indexing) {
            pending_changes_.push_back(PendingChange{path, true});
        }
    }
    const auto key = normalized_path_key(normalize_root(path));
    std::unique_lock lock{data_mutex_};
    const auto old_size = records_.size();
    std::erase_if(records_, [&](const Record &record) {
        return is_same_or_descendant(record.path_lower, key);
    });
    if (records_.size() != old_size) {
        recalculate_counts(records_, file_count_, directory_count_);
        ++incremental_update_count_;
    }
}
std::uint64_t FileIndex::launch_scan(std::vector<std::filesystem::path> roots,
                                     std::vector<std::wstring> excluded_directory_names) {
    std::scoped_lock lock{worker_mutex_};
    const auto generation = scan_gate_.begin();
    if (generation == 0) {
        state_.store(IndexState::failed, std::memory_order_release);
        std::unique_lock data_lock{data_mutex_};
        last_error_ = L"Failed to allocate file-index scan generation.";
        return 0;
    }
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    state_.store(IndexState::indexing, std::memory_order_release);
    worker_ = std::jthread(
        [this, generation, roots = std::move(roots),
         exclusions = std::move(excluded_directory_names)](std::stop_token stop_token) mutable {
            scan(stop_token, generation, std::move(roots), std::move(exclusions));
        });
    return generation;
}
void FileIndex::scan(std::stop_token stop_token, std::uint64_t generation,
                     std::vector<std::filesystem::path> roots,
                     std::vector<std::wstring> excluded_directory_names) {
    const auto started = std::chrono::steady_clock::now();
    std::vector<Record> next_records;
    try {
        for (const auto &root : roots) {
            if (stop_token.stop_requested()) {
                scan_gate_.cancel(generation);
                return;
            }
            std::error_code error;
            if (!std::filesystem::exists(root, error) || error) {
                continue;
            }
            const std::filesystem::directory_entry root_entry{root, error};
            if (!error && root_entry.is_directory(error) && !error &&
                should_skip_directory(root, excluded_directory_names)) {
                continue;
            }
            error.clear();
            if (const auto record = make_record(root_entry)) {
                next_records.push_back(*record);
            }
            if (!root_entry.is_directory(error) || error) {
                continue;
            }
            std::filesystem::recursive_directory_iterator iterator{
                root, std::filesystem::directory_options::skip_permission_denied, error};
            const std::filesystem::recursive_directory_iterator end;
            while (iterator != end) {
                if (stop_token.stop_requested()) {
                    scan_gate_.cancel(generation);
                    return;
                }
                if (error) {
                    error.clear();
                    iterator.increment(error);
                    continue;
                }
                if (iterator->is_directory(error) && !error &&
                    should_skip_directory(iterator->path(), excluded_directory_names)) {
                    iterator.disable_recursion_pending();
                    iterator.increment(error);
                    continue;
                }
                error.clear();
                if (const auto record = make_record(*iterator)) {
                    next_records.push_back(*record);
                }
                iterator.increment(error);
            }
        }
        if (stop_token.stop_requested()) {
            scan_gate_.cancel(generation);
            return;
        }
        next_records.shrink_to_fit();
        std::size_t next_files = 0;
        std::size_t next_directories = 0;
        recalculate_counts(next_records, next_files, next_directories);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        std::vector<PendingChange> pending;
        {
            std::unique_lock lock{data_mutex_};
            records_.swap(next_records);
            roots_ = std::move(roots);
            excluded_directory_names_ = std::move(excluded_directory_names);
            file_count_ = next_files;
            directory_count_ = next_directories;
            incremental_update_count_ = 0;
            last_scan_time_ = elapsed;
            last_error_.clear();
            pending.swap(pending_changes_);
            state_.store(IndexState::ready, std::memory_order_release);
        }
        // Filesystem notifications can arrive while a full scan is in flight.
        // Replay those mutations after publishing the scan so a late create,
        // delete, or rename cannot be overwritten by the snapshot swap.
        for (const auto &change : pending) {
            if (change.remove) {
                remove_path(change.path);
            } else {
                refresh_path(change.path);
            }
        }
        scan_gate_.complete(generation, true);
    } catch (const std::exception &exception) {
        std::wstring message;
        const std::string narrow = exception.what();
        message.assign(narrow.begin(), narrow.end());
        {
            std::unique_lock lock{data_mutex_};
            last_error_ = std::move(message);
        }
        state_.store(IndexState::failed, std::memory_order_release);
        scan_gate_.complete(generation, false);
    } catch (...) {
        {
            std::unique_lock lock{data_mutex_};
            last_error_ = L"Unknown file indexing failure.";
        }
        state_.store(IndexState::failed, std::memory_order_release);
        scan_gate_.complete(generation, false);
    }
}
std::vector<SearchHit> FileIndex::search(std::wstring_view query, std::size_t limit) const {
    const auto terms = split_terms(query);
    if (terms.empty() || limit == 0) {
        return {};
    }
    std::vector<SearchHit> matches;
    matches.reserve(std::min<std::size_t>(limit * 4, 256));
    std::shared_lock lock{data_mutex_};
    for (const auto &record : records_) {
        int total_score = record.is_directory ? 5 : 0;
        bool matched = true;
        for (const auto &term : terms) {
            const auto term_score = score_term(record.name_lower, record.path_lower, term);
            if (!term_score) {
                matched = false;
                break;
            }
            total_score += *term_score;
        }
        if (!matched) {
            continue;
        }
        matches.push_back({record.path, record.is_directory, record.size, total_score});
    }
    const auto better = [](const SearchHit &lhs, const SearchHit &rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        if (lhs.is_directory != rhs.is_directory) {
            return lhs.is_directory;
        }
        return lhs.path.native().size() < rhs.path.native().size();
    };
    if (matches.size() > limit) {
        std::partial_sort(matches.begin(), matches.begin() + static_cast<std::ptrdiff_t>(limit),
                          matches.end(), better);
        matches.resize(limit);
    } else {
        std::sort(matches.begin(), matches.end(), better);
    }
    return matches;
}
IndexSnapshot FileIndex::snapshot() const {
    IndexSnapshot snapshot;
    snapshot.state = state_.load(std::memory_order_acquire);
    std::shared_lock lock{data_mutex_};
    snapshot.entries = records_.size();
    snapshot.files = file_count_;
    snapshot.directories = directory_count_;
    snapshot.incremental_updates = incremental_update_count_;
    snapshot.last_scan_time = last_scan_time_;
    snapshot.roots = roots_;
    snapshot.excluded_directory_names = excluded_directory_names_;
    snapshot.last_error = last_error_;
    return snapshot;
}
std::wstring_view to_string(IndexState state) noexcept {
    switch (state) {
    case IndexState::idle:
        return L"idle";
    case IndexState::indexing:
        return L"indexing";
    case IndexState::ready:
        return L"ready";
    case IndexState::failed:
        return L"failed";
    default:
        return L"unknown";
    }
}
} // namespace axiom
