#pragma once
#include "index_scan_gate.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
namespace axiom {
enum class IndexState : std::uint8_t {
    idle,
    indexing,
    ready,
    failed,
};
struct SearchHit {
    std::filesystem::path path;
    bool is_directory{false};
    std::uintmax_t size{0};
    int score{0};
};
struct IndexSnapshot {
    IndexState state{IndexState::idle};
    std::size_t entries{0};
    std::size_t files{0};
    std::size_t directories{0};
    std::size_t incremental_updates{0};
    std::chrono::milliseconds last_scan_time{0};
    std::vector<std::filesystem::path> roots;
    std::vector<std::wstring> excluded_directory_names;
    std::wstring last_error;
};
class FileIndex final {
  public:
    FileIndex() = default;
    ~FileIndex();
    FileIndex(const FileIndex &) = delete;
    FileIndex &operator=(const FileIndex &) = delete;
    void start(std::vector<std::filesystem::path> roots,
               std::vector<std::wstring> excluded_directory_names = {});
    void rebuild();
    [[nodiscard]] std::uint64_t rebuild_tracked();
    [[nodiscard]] std::uint64_t current_scan_generation() const noexcept;
    [[nodiscard]] IndexScanWaitResult wait_for_scan(std::uint64_t generation,
                                                    std::stop_token stop_token = {}) const noexcept;
    void stop() noexcept;
    [[nodiscard]] bool add_root(std::filesystem::path root);
    [[nodiscard]] bool remove_root(const std::filesystem::path &root);
    [[nodiscard]] bool add_exclusion(std::wstring name);
    [[nodiscard]] bool remove_exclusion(std::wstring_view name);
    // Incremental mutation hooks used by the Windows filesystem watcher.
    // refresh_path() replaces the indexed record/subtree with the current
    // filesystem state; remove_path() removes a record/subtree without I/O.
    void refresh_path(const std::filesystem::path &path);
    void remove_path(const std::filesystem::path &path);
    [[nodiscard]] std::vector<SearchHit> search(std::wstring_view query,
                                                std::size_t limit = 20) const;
    [[nodiscard]] IndexSnapshot snapshot() const;

  private:
    struct Record {
        std::filesystem::path path;
        std::wstring name_lower;
        std::wstring path_lower;
        bool is_directory{false};
        std::uintmax_t size{0};
    };
    struct PendingChange {
        std::filesystem::path path;
        bool remove{false};
    };
    mutable std::shared_mutex data_mutex_;
    mutable std::mutex worker_mutex_;
    std::vector<Record> records_;
    std::vector<std::filesystem::path> roots_;
    std::vector<std::wstring> excluded_directory_names_;
    std::vector<PendingChange> pending_changes_;
    std::wstring last_error_;
    std::chrono::milliseconds last_scan_time_{0};
    std::size_t file_count_{0};
    std::size_t directory_count_{0};
    std::size_t incremental_update_count_{0};
    std::atomic<IndexState> state_{IndexState::idle};
    std::jthread worker_;
    IndexScanGate scan_gate_{};
    [[nodiscard]] static std::optional<Record>
    make_record(const std::filesystem::directory_entry &entry);
    static void append_subtree(const std::filesystem::path &root,
                               const std::vector<std::wstring> &excluded_directory_names,
                               std::vector<Record> &output);
    static void recalculate_counts(const std::vector<Record> &records, std::size_t &files,
                                   std::size_t &directories);
    [[nodiscard]] std::uint64_t launch_scan(std::vector<std::filesystem::path> roots,
                                            std::vector<std::wstring> excluded_directory_names);
    void scan(std::stop_token stop_token, std::uint64_t generation,
              std::vector<std::filesystem::path> roots,
              std::vector<std::wstring> excluded_directory_names);
};
[[nodiscard]] std::wstring_view to_string(IndexState state) noexcept;
} // namespace axiom
