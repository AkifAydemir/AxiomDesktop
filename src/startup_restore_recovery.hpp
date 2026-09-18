#pragma once

#include "restore_transaction.hpp"

#include <cstddef>
#include <filesystem>
#include <span>

namespace axiom {

struct StartupRestoreRecoveryReport {
    bool journal_found{};
    bool orphan_journal_temporary_removed{};
    RestoreRecoveryResult recovery{};
};

// Bootstrap-only recovery entry point. Product wiring must call this before any
// settings/store constructor reads a destination that can participate in restore.
// A lone <journal>.tmp is safe to remove because RestoreTransaction publishes the
// durable main journal before making the first destination mutation.
class StartupRestoreRecovery final {
  public:
    [[nodiscard]] static StartupRestoreRecoveryReport
    recover_before_store_load(const std::filesystem::path &recovery_journal_path,
                              std::span<const std::filesystem::path> allowed_destinations);
};

} // namespace axiom
