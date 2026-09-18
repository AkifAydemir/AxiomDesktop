#include "startup_restore_recovery.hpp"

#include <stdexcept>
#include <system_error>

namespace axiom {
namespace {

[[nodiscard]] bool exists_checked(const std::filesystem::path &path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        throw std::system_error{error, "Unable to inspect startup restore recovery artifact"};
    }
    return exists;
}

void remove_orphan_journal_temporary(const std::filesystem::path &path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error) {
        throw std::system_error{error, "Unable to inspect orphan restore journal temporary"};
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw std::runtime_error{
            "Orphan restore journal temporary is not a regular file; refusing startup cleanup."};
    }
    if (!std::filesystem::remove(path, error) || error) {
        if (error) {
            throw std::system_error{error, "Unable to remove orphan restore journal temporary"};
        }
        throw std::runtime_error{"Unable to remove orphan restore journal temporary."};
    }
}

} // namespace

StartupRestoreRecoveryReport StartupRestoreRecovery::recover_before_store_load(
    const std::filesystem::path &recovery_journal_path,
    std::span<const std::filesystem::path> allowed_destinations) {
    if (recovery_journal_path.empty()) {
        throw std::invalid_argument{"Startup restore recovery requires a journal path."};
    }
    if (allowed_destinations.empty()) {
        throw std::invalid_argument{
            "Startup restore recovery requires an explicit destination allowlist."};
    }

    StartupRestoreRecoveryReport report;
    if (exists_checked(recovery_journal_path)) {
        report.journal_found = true;
        report.recovery =
            RestoreTransaction::recover_interrupted(recovery_journal_path, allowed_destinations);
        return report;
    }

    auto temporary = recovery_journal_path;
    temporary += L".tmp";
    if (exists_checked(temporary)) {
        remove_orphan_journal_temporary(temporary);
        report.orphan_journal_temporary_removed = true;
    }
    return report;
}

} // namespace axiom
