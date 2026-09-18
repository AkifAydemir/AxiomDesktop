#pragma once

#ifdef _WIN32

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace axiom {

enum class FileChangeKind {
    added,
    modified,
    removed,
    renamed_old,
    renamed_new,
};

struct FileChange {
    FileChangeKind kind{FileChangeKind::modified};
    std::filesystem::path path;
};

enum class WatcherIssueKind : std::uint8_t {
    overflow,
    error,
};

struct WatcherIssue {
    WatcherIssueKind kind{WatcherIssueKind::error};
    std::filesystem::path root;
    std::uint32_t error_code{};
};

class FileSystemWatcher final {
  public:
    using Callback = std::function<void(const FileChange &)>;
    using IssueCallback = std::function<void(const WatcherIssue &)>;

    FileSystemWatcher();
    ~FileSystemWatcher();

    FileSystemWatcher(const FileSystemWatcher &) = delete;
    FileSystemWatcher &operator=(const FileSystemWatcher &) = delete;

    void start(std::vector<std::filesystem::path> roots, Callback callback,
               IssueCallback issue_callback = {});
    void stop() noexcept;

    [[nodiscard]] std::size_t watch_count() const noexcept;

  private:
    struct Watch;

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Watch>> watches_;
};

} // namespace axiom

#endif
