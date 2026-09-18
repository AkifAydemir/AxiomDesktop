#include "startup_restore_recovery.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
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
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
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

void recovery_happens_before_settings_are_read() {
    const auto root =
        std::filesystem::temp_directory_path() / "axiom_startup_restore_recovery_order";
    cleanup(root);
    const auto settings = std::filesystem::absolute(root / "settings.conf").lexically_normal();
    const auto journal = root / "restore.journal";
    const auto backup = std::filesystem::path{settings.wstring() + L".axiom-restore.a1-1.bak"};
    const auto temporary = std::filesystem::path{settings.wstring() + L".axiom-restore.a1-1.new"};

    write(settings, "partially-restored-settings");
    write(backup, "original-settings");
    write(temporary, "stale-temp");

    std::ofstream stream{journal, std::ios::binary | std::ios::trunc};
    stream << "AXRT1\n";
    stream << "phase=committing\n";
    stream << "token=a1-1\n";
    stream << "count=1\n";
    stream << "item=" << hex_encode("settings.conf") << '|' << hex_encode(path_utf8(settings))
           << '|' << hex_encode(path_utf8(temporary)) << '|' << hex_encode(path_utf8(backup)) << '|'
           << "1|1|1|1\n";
    stream.close();

    const std::vector<std::filesystem::path> allowed{settings};
    const auto report = axiom::StartupRestoreRecovery::recover_before_store_load(journal, allowed);
    expect(report.journal_found, "startup bootstrap must observe interrupted journal");
    expect(report.recovery.files_rolled_back == 1,
           "startup bootstrap must roll back interrupted settings restore");

    // This read intentionally models AppSettingsStore::load. It must happen only
    // after bootstrap recovery, otherwise the process can initialize from a
    // partially-published restore generation.
    const auto loaded_settings = read(settings);
    expect(loaded_settings == "original-settings", "settings loader must observe recovered bytes");
    expect(!std::filesystem::exists(journal), "successful startup recovery must remove journal");
    cleanup(root);
}

void orphan_journal_temporary_is_safe_to_clean_before_store_load() {
    const auto root = std::filesystem::temp_directory_path() / "axiom_startup_restore_orphan_tmp";
    cleanup(root);
    const auto settings = std::filesystem::absolute(root / "settings.conf").lexically_normal();
    const auto journal = root / "restore.journal";
    auto temporary_journal = journal;
    temporary_journal += L".tmp";
    write(settings, "stable-settings");
    write(temporary_journal, "incomplete-journal-publish");

    const std::vector<std::filesystem::path> allowed{settings};
    const auto report = axiom::StartupRestoreRecovery::recover_before_store_load(journal, allowed);
    expect(!report.journal_found,
           "orphan temporary must not be treated as committed recovery metadata");
    expect(report.orphan_journal_temporary_removed, "orphan journal temporary must be cleaned");
    expect(read(settings) == "stable-settings",
           "orphan journal temporary cleanup must not mutate live data");
    expect(!std::filesystem::exists(temporary_journal), "orphan journal temporary must be removed");
    cleanup(root);
}

void suspicious_orphan_artifact_fails_closed() {
    const auto root =
        std::filesystem::temp_directory_path() / "axiom_startup_restore_orphan_directory";
    cleanup(root);
    const auto settings = std::filesystem::absolute(root / "settings.conf").lexically_normal();
    const auto journal = root / "restore.journal";
    auto temporary_journal = journal;
    temporary_journal += L".tmp";
    write(settings, "stable-settings");
    std::filesystem::create_directories(temporary_journal);

    bool rejected = false;
    try {
        const std::vector<std::filesystem::path> allowed{settings};
        (void)axiom::StartupRestoreRecovery::recover_before_store_load(journal, allowed);
    } catch (const std::exception &) {
        rejected = true;
    }
    expect(rejected, "non-file recovery artifact must fail closed");
    expect(read(settings) == "stable-settings",
           "fail-closed startup recovery must not mutate live data");
    cleanup(root);
}

} // namespace

int main() {
    try {
        recovery_happens_before_settings_are_read();
        orphan_journal_temporary_is_safe_to_clean_before_store_load();
        suspicious_orphan_artifact_fails_closed();
        std::cout << "startup_restore_recovery_tests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "startup_restore_recovery_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
