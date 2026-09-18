#include "restore_transaction.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void expect(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error{message};
}

void write(const std::filesystem::path &path, std::string_view value) {
    const auto parent = path.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent);
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream << value;
}

std::string read(const std::filesystem::path &path) {
    std::ifstream stream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, {}};
}

void cleanup(const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

char hex_digit(unsigned value) {
    return static_cast<char>(value < 10 ? '0' + value : 'a' + value - 10);
}

std::string hex_encode(std::string_view input) {
    std::string output;
    for (unsigned char ch : input) {
        output.push_back(hex_digit((ch >> 4) & 0xF));
        output.push_back(hex_digit(ch & 0xF));
    }
    return output;
}

std::string path_utf8(const std::filesystem::path &path) {
    auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

void successful_commit() {
    const auto root = std::filesystem::temp_directory_path() / "axiom_restore_transaction_success";
    cleanup(root);
    const auto stage = root / "stage";
    const auto live = root / "live";
    const auto journal = root / "restore.journal";
    write(stage / "a", "new-a");
    write(stage / "b", "new-b");
    write(live / "a", "old-a");

    std::vector<axiom::RestoreFilePlan> plan{
        {"a", stage / "a", live / "a"},
        {"b", stage / "b", live / "b"},
    };
    const auto result = axiom::RestoreTransaction::commit(plan, journal);
    expect(result.files_committed == 2, "commit count mismatch");
    expect(result.existing_files_replaced == 1, "replace count mismatch");
    expect(result.new_files_created == 1, "create count mismatch");
    expect(!result.recovery_cleanup_pending, "successful commit should finish cleanup");
    expect(read(live / "a") == "new-a", "existing destination not replaced");
    expect(read(live / "b") == "new-b", "new destination not created");
    expect(!std::filesystem::exists(journal), "journal must be removed after success");
    cleanup(root);
}

void in_memory_blob_commit_preserves_binary_bytes() {
    const auto root = std::filesystem::temp_directory_path() / "axiom_restore_transaction_blob";
    cleanup(root);
    const auto live = root / "live";
    const auto journal = root / "restore.journal";
    write(live / "a", "old-a");

    const std::vector<std::byte> a{std::byte{0x00}, std::byte{0x41}, std::byte{0xFF},
                                   std::byte{0x0A}};
    const std::vector<std::byte> b{std::byte{0x42}, std::byte{0x00}, std::byte{0x43}};
    const std::vector<axiom::RestoreBlobPlan> plan{
        {"a", std::span<const std::byte>{a}, live / "a"},
        {"b", std::span<const std::byte>{b}, live / "b"},
    };

    const auto result = axiom::RestoreTransaction::commit(plan, journal);
    expect(result.files_committed == 2, "blob commit count mismatch");

    const auto a_bytes = read(live / "a");
    const auto b_bytes = read(live / "b");
    expect(a_bytes.size() == a.size(), "blob a size mismatch");
    expect(b_bytes.size() == b.size(), "blob b size mismatch");
    expect(static_cast<unsigned char>(a_bytes[0]) == 0x00 &&
               static_cast<unsigned char>(a_bytes[1]) == 0x41 &&
               static_cast<unsigned char>(a_bytes[2]) == 0xFF &&
               static_cast<unsigned char>(a_bytes[3]) == 0x0A,
           "blob a binary contents mismatch");
    expect(static_cast<unsigned char>(b_bytes[0]) == 0x42 &&
               static_cast<unsigned char>(b_bytes[1]) == 0x00 &&
               static_cast<unsigned char>(b_bytes[2]) == 0x43,
           "blob b binary contents mismatch");
    expect(!std::filesystem::exists(journal), "blob commit journal must be removed");
    cleanup(root);
}

void mid_commit_failure_rolls_back() {
    const auto root = std::filesystem::temp_directory_path() / "axiom_restore_transaction_rollback";
    cleanup(root);
    const auto stage = root / "stage";
    const auto live = root / "live";
    const auto journal = root / "restore.journal";
    write(stage / "a", "new-a");
    write(stage / "b", "new-b");
    write(live / "a", "old-a");
    const auto blocked_parent = root / "blocked-parent";
    write(blocked_parent, "not-a-directory");

    std::vector<axiom::RestoreFilePlan> plan{
        {"a", stage / "a", live / "a"},
        {"b", stage / "b", blocked_parent / "b"},
    };

    bool rejected = false;
    try {
        (void)axiom::RestoreTransaction::commit(plan, journal);
    } catch (const std::exception &) {
        rejected = true;
    }
    expect(rejected, "commit must fail when a later destination cannot be created");
    expect(read(live / "a") == "old-a", "earlier committed file must be rolled back");
    expect(!std::filesystem::exists(journal), "successful rollback must remove recovery journal");
    cleanup(root);
}

void interrupted_transaction_recovery() {
    const auto root = std::filesystem::temp_directory_path() / "axiom_restore_transaction_recovery";
    cleanup(root);
    const auto live = std::filesystem::absolute(root / "live" / "a").lexically_normal();
    const auto journal = root / "restore.journal";
    const auto backup = std::filesystem::path{live.wstring() + L".axiom-restore.a1-1.bak"};
    const auto temporary = std::filesystem::path{live.wstring() + L".axiom-restore.a1-1.new"};
    write(live, "new-a");
    write(backup, "old-a");
    write(temporary, "stale-temp");

    std::filesystem::create_directories(journal.parent_path());
    std::ofstream stream{journal, std::ios::binary | std::ios::trunc};
    stream << "AXRT1\n";
    stream << "phase=committing\n";
    stream << "token=a1-1\n";
    stream << "count=1\n";
    stream << "item=" << hex_encode("a") << '|' << hex_encode(path_utf8(live)) << '|'
           << hex_encode(path_utf8(temporary)) << '|' << hex_encode(path_utf8(backup)) << '|'
           << "1|1|1|1\n";
    stream.close();

    const std::vector<std::filesystem::path> allowed{live};
    const auto result = axiom::RestoreTransaction::recover_interrupted(journal, allowed);
    expect(result.files_rolled_back == 1, "recovery must restore original file");
    expect(result.temporary_files_removed == 1, "recovery must remove stale temporary file");
    expect(result.journal_removed, "recovery must remove journal after success");
    expect(read(live) == "old-a", "recovery did not restore original bytes");
    expect(!std::filesystem::exists(backup), "recovery backup artifact must be consumed");
    expect(!std::filesystem::exists(temporary), "recovery temporary artifact must be removed");
    cleanup(root);
}

void committed_transaction_recovery_keeps_published_files() {
    const auto root =
        std::filesystem::temp_directory_path() / "axiom_restore_transaction_committed_recovery";
    cleanup(root);
    const auto live = std::filesystem::absolute(root / "live" / "a").lexically_normal();
    const auto journal = root / "restore.journal";
    const auto backup = std::filesystem::path{live.wstring() + L".axiom-restore.a1-1.bak"};
    const auto temporary = std::filesystem::path{live.wstring() + L".axiom-restore.a1-1.new"};
    write(live, "new-a");
    write(backup, "old-a");
    write(temporary, "stale-temp");
    auto journal_temporary = journal;
    journal_temporary += L".tmp";
    write(journal_temporary, "stale-journal-temp");

    std::filesystem::create_directories(journal.parent_path());
    std::ofstream stream{journal, std::ios::binary | std::ios::trunc};
    stream << "AXRT1\n";
    stream << "phase=committed\n";
    stream << "token=a1-1\n";
    stream << "count=1\n";
    stream << "item=" << hex_encode("a") << '|' << hex_encode(path_utf8(live)) << '|'
           << hex_encode(path_utf8(temporary)) << '|' << hex_encode(path_utf8(backup)) << '|'
           << "1|1|1|1\n";
    stream.close();

    const std::vector<std::filesystem::path> allowed{live};
    const auto result = axiom::RestoreTransaction::recover_interrupted(journal, allowed);
    expect(result.files_rolled_back == 0, "committed recovery must not roll back published data");
    expect(result.temporary_files_removed == 2,
           "committed recovery must remove data and journal temporaries");
    expect(result.journal_removed, "committed recovery must remove journal after cleanup");
    expect(read(live) == "new-a", "committed recovery must keep published bytes");
    expect(!std::filesystem::exists(backup), "committed recovery must remove stale backup");
    expect(!std::filesystem::exists(temporary),
           "committed recovery must remove stale temporary artifact");
    expect(!std::filesystem::exists(journal_temporary),
           "committed recovery must remove stale journal temporary");
    cleanup(root);
}

void forged_recovery_paths_fail_closed() {
    const auto root = std::filesystem::temp_directory_path() / "axiom_restore_transaction_forged";
    cleanup(root);
    const auto allowed_live =
        std::filesystem::absolute(root / "live" / "settings.conf").lexically_normal();
    const auto victim = std::filesystem::absolute(root / "victim.txt").lexically_normal();
    const auto journal = root / "restore.journal";
    const auto forged_backup = std::filesystem::path{victim.wstring() + L".axiom-restore.a1-1.bak"};
    const auto forged_temporary =
        std::filesystem::path{victim.wstring() + L".axiom-restore.a1-1.new"};
    write(victim, "must-survive");
    write(forged_backup, "fake-backup");
    write(forged_temporary, "fake-temp");

    std::filesystem::create_directories(journal.parent_path());
    std::ofstream stream{journal, std::ios::binary | std::ios::trunc};
    stream << "AXRT1\n";
    stream << "phase=committing\n";
    stream << "token=a1-1\n";
    stream << "count=1\n";
    stream << "item=" << hex_encode("settings.conf") << '|' << hex_encode(path_utf8(victim)) << '|'
           << hex_encode(path_utf8(forged_temporary)) << '|' << hex_encode(path_utf8(forged_backup))
           << '|' << "1|1|1|1\n";
    stream.close();

    bool rejected = false;
    try {
        const std::vector<std::filesystem::path> allowed{allowed_live};
        (void)axiom::RestoreTransaction::recover_interrupted(journal, allowed);
    } catch (const std::exception &) {
        rejected = true;
    }
    expect(rejected, "forged recovery destination must fail closed");
    expect(read(victim) == "must-survive",
           "forged recovery journal must not mutate non-allowlisted path");
    expect(std::filesystem::exists(journal),
           "rejected recovery journal must be preserved for inspection");
    cleanup(root);
}

void existing_recovery_journal_blocks_new_restore() {
    const auto root =
        std::filesystem::temp_directory_path() / "axiom_restore_transaction_existing_journal";
    cleanup(root);
    const auto live = root / "live" / "a";
    const auto journal = root / "restore.journal";
    write(journal, "unrecovered-state");
    const std::vector<std::byte> data{std::byte{0x41}};
    const std::vector<axiom::RestoreBlobPlan> plan{{"a", std::span<const std::byte>{data}, live}};

    bool rejected = false;
    try {
        (void)axiom::RestoreTransaction::commit(plan, journal);
    } catch (const std::exception &) {
        rejected = true;
    }
    expect(rejected, "new restore must not overwrite an existing recovery journal");
    expect(read(journal) == "unrecovered-state",
           "existing recovery metadata must remain untouched");
    expect(!std::filesystem::exists(live), "blocked restore must not mutate destination");
    cleanup(root);
}

} // namespace

int main() {
    try {
        successful_commit();
        in_memory_blob_commit_preserves_binary_bytes();
        mid_commit_failure_rolls_back();
        interrupted_transaction_recovery();
        committed_transaction_recovery_keeps_published_files();
        forged_recovery_paths_fail_closed();
        existing_recovery_journal_blocks_new_restore();
        std::cout << "restore_transaction_tests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "restore_transaction_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
