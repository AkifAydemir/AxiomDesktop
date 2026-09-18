#pragma once
#include "journal_store.hpp"
#include <cstddef>
#include <filesystem>
namespace axiom {
struct JournalTransferResult {
    std::size_t entries{};
    std::size_t notes{};
    std::size_t activities{};
};
JournalTransferResult export_journal(const JournalStore &store, const std::filesystem::path &path,
                                     bool include_activities = false);
JournalTransferResult import_journal(JournalStore &store, const std::filesystem::path &path);
} // namespace axiom
