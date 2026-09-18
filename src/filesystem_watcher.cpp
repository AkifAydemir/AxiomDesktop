#include "filesystem_watcher.hpp"
#include "win32_handle.hpp"

#ifdef _WIN32

#include <array>
#include <cstddef>
#include <system_error>
#include <utility>

namespace axiom {
namespace {

constexpr DWORD notify_filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                FILE_NOTIFY_CHANGE_CREATION;

[[nodiscard]] FileChangeKind map_action(DWORD action) noexcept {
    switch (action) {
    case FILE_ACTION_ADDED:
        return FileChangeKind::added;
    case FILE_ACTION_REMOVED:
        return FileChangeKind::removed;
    case FILE_ACTION_RENAMED_OLD_NAME:
        return FileChangeKind::renamed_old;
    case FILE_ACTION_RENAMED_NEW_NAME:
        return FileChangeKind::renamed_new;
    case FILE_ACTION_MODIFIED:
    default:
        return FileChangeKind::modified;
    }
}

} // namespace

struct FileSystemWatcher::Watch final {
    std::filesystem::path root;
    UniqueWin32Handle directory;
    UniqueWin32Handle event;
    Callback callback;
    IssueCallback issue_callback;
    std::jthread thread;

    ~Watch() {
        if (thread.joinable()) {
            thread.request_stop();
            thread.join();
        }
    }

    void report_issue(WatcherIssueKind kind, DWORD error_code) const {
        if (issue_callback) {
            issue_callback(WatcherIssue{
                kind,
                root,
                static_cast<std::uint32_t>(error_code),
            });
        }
    }

    void run(std::stop_token stop_token) {
        std::array<std::byte, 64 * 1024> buffer{};

        while (!stop_token.stop_requested()) {
            ResetEvent(event.get());
            OVERLAPPED overlapped{};
            overlapped.hEvent = event.get();

            const BOOL started = ReadDirectoryChangesW(
                directory.get(), buffer.data(), static_cast<DWORD>(buffer.size()), TRUE,
                notify_filter, nullptr, &overlapped, nullptr);

            if (!started) {
                const DWORD error = GetLastError();
                report_issue(error == ERROR_NOTIFY_ENUM_DIR ? WatcherIssueKind::overflow
                                                            : WatcherIssueKind::error,
                             error);
                return;
            }

            while (!stop_token.stop_requested()) {
                const DWORD wait = WaitForSingleObject(event.get(), 200);
                if (wait == WAIT_OBJECT_0) {
                    break;
                }
                if (wait == WAIT_FAILED) {
                    const DWORD error = GetLastError();
                    CancelIoEx(directory.get(), &overlapped);
                    report_issue(WatcherIssueKind::error, error);
                    return;
                }
            }

            if (stop_token.stop_requested()) {
                CancelIoEx(directory.get(), &overlapped);
                WaitForSingleObject(event.get(), 1000);
                return;
            }

            DWORD bytes = 0;
            if (!GetOverlappedResult(directory.get(), &overlapped, &bytes, FALSE)) {
                const DWORD error = GetLastError();
                if (error == ERROR_OPERATION_ABORTED) {
                    return;
                }
                report_issue(error == ERROR_NOTIFY_ENUM_DIR ? WatcherIssueKind::overflow
                                                            : WatcherIssueKind::error,
                             error);
                continue;
            }
            if (bytes == 0) {
                // Microsoft documents zero transferred bytes as a loss-of-detail
                // condition that requires enumerating the directory/subtree again.
                report_issue(WatcherIssueKind::overflow, ERROR_NOTIFY_ENUM_DIR);
                continue;
            }

            DWORD offset = 0;
            while (offset < bytes) {
                const auto *info =
                    reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(buffer.data() + offset);
                const std::wstring_view relative{info->FileName,
                                                 info->FileNameLength / sizeof(wchar_t)};

                if (!relative.empty() && callback) {
                    callback(FileChange{
                        map_action(info->Action),
                        (root / std::filesystem::path{relative}).lexically_normal(),
                    });
                }

                if (info->NextEntryOffset == 0) {
                    break;
                }
                offset += info->NextEntryOffset;
            }
        }
    }
};

FileSystemWatcher::FileSystemWatcher() = default;

FileSystemWatcher::~FileSystemWatcher() {
    stop();
}

void FileSystemWatcher::start(std::vector<std::filesystem::path> roots, Callback callback,
                              IssueCallback issue_callback) {
    stop();

    std::vector<std::unique_ptr<Watch>> next;
    next.reserve(roots.size());

    for (auto &root : roots) {
        std::error_code error;
        root = std::filesystem::absolute(root, error).lexically_normal();
        if (error || !std::filesystem::is_directory(root, error) || error) {
            continue;
        }

        UniqueWin32Handle directory{
            CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr)};
        if (!directory) {
            if (issue_callback) {
                issue_callback(WatcherIssue{
                    WatcherIssueKind::error,
                    root,
                    static_cast<std::uint32_t>(GetLastError()),
                });
            }
            continue;
        }

        UniqueWin32Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!event) {
            if (issue_callback) {
                issue_callback(WatcherIssue{
                    WatcherIssueKind::error,
                    root,
                    static_cast<std::uint32_t>(GetLastError()),
                });
            }
            continue;
        }

        auto watch = std::make_unique<Watch>();
        watch->root = std::move(root);
        watch->directory = std::move(directory);
        watch->event = std::move(event);
        watch->callback = callback;
        watch->issue_callback = issue_callback;
        Watch *raw = watch.get();
        watch->thread = std::jthread([raw](std::stop_token stop_token) { raw->run(stop_token); });
        next.push_back(std::move(watch));
    }

    std::scoped_lock lock{mutex_};
    watches_ = std::move(next);
}

void FileSystemWatcher::stop() noexcept {
    std::vector<std::unique_ptr<Watch>> old;
    {
        std::scoped_lock lock{mutex_};
        old.swap(watches_);
    }

    for (auto &watch : old) {
        if (watch->thread.joinable()) {
            watch->thread.request_stop();
        }
    }
    for (auto &watch : old) {
        if (watch->thread.joinable()) {
            watch->thread.join();
        }
    }
}

std::size_t FileSystemWatcher::watch_count() const noexcept {
    std::scoped_lock lock{mutex_};
    return watches_.size();
}

} // namespace axiom

#endif
