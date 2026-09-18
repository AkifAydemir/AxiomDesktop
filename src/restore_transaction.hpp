#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace axiom {

struct RestoreFilePlan {
    std::string name;
    std::filesystem::path staged_path;
    std::filesystem::path destination_path;
};

struct RestoreBlobPlan {
    std::string name;
    std::span<const std::byte> data;
    std::filesystem::path destination_path;
};

struct RestoreTransactionResult {
    std::size_t files_committed{};
    std::size_t existing_files_replaced{};
    std::size_t new_files_created{};
    bool recovery_cleanup_pending{};
};

struct RestoreRecoveryResult {
    std::size_t files_rolled_back{};
    std::size_t temporary_files_removed{};
    bool journal_removed{};
};

class RestoreTransaction final {
  public:
    [[nodiscard]] static RestoreTransactionResult
    commit(std::span<const RestoreFilePlan> files,
           const std::filesystem::path &recovery_journal_path);

    [[nodiscard]] static RestoreTransactionResult
    commit(std::span<const RestoreBlobPlan> files,
           const std::filesystem::path &recovery_journal_path);

    [[nodiscard]] static RestoreRecoveryResult
    recover_interrupted(const std::filesystem::path &recovery_journal_path,
                        std::span<const std::filesystem::path> allowed_destinations);
};

} // namespace axiom
