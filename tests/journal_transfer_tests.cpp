#include "journal_transfer.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
namespace {
void expect(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error{message};
}
void clean_store(const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.wstring() + L".tmp", error);
    std::filesystem::remove(path.wstring() + L".bak", error);
}
void roundtrip() {
    const auto root = std::filesystem::temp_directory_path();
    const auto source_path = root / "axiom_transfer_source.bin";
    const auto destination_path = root / "axiom_transfer_destination.bin";
    const auto export_path = root / "axiom_export.axj";
    clean_store(source_path);
    clean_store(destination_path);
    std::filesystem::remove(export_path);
    const auto base = std::chrono::system_clock::time_point{1700000000000ms};
    axiom::JournalStore source{source_path};
    (void)source.load();
    (void)source.add_note(L"Türkçe note\nline", {L"cpp", L"iş"}, base);
    (void)source.add_activity(L"automation", L"ran, ok", {L"system"}, base + 1s);
    const auto exported = axiom::export_journal(source, export_path, true);
    expect(exported.entries == 2 && exported.notes == 1 && exported.activities == 1,
           "journal export counts are incorrect");
    axiom::JournalStore destination{destination_path};
    (void)destination.load();
    const auto imported = axiom::import_journal(destination, export_path);
    expect(imported.entries == 2 && imported.notes == 1 && imported.activities == 1,
           "journal import counts are incorrect");
    expect(destination.recent(10).size() == 2, "round-trip entry count mismatch");
    expect(destination.search(L"Türkçe", 10).size() == 1, "Unicode note did not round-trip");
    expect(destination.search(L"ran, ok", 10).size() == 1, "escaped comma did not round-trip");
    clean_store(source_path);
    clean_store(destination_path);
    std::filesystem::remove(export_path);
}
void malformed_import_is_non_mutating() {
    const auto root = std::filesystem::temp_directory_path();
    const auto store_path = root / "axiom_transfer_atomic.bin";
    const auto import_path = root / "axiom_transfer_invalid.axj";
    clean_store(store_path);
    {
        std::ofstream stream{import_path, std::ios::binary | std::ios::trunc};
        stream << "AXIOM-JOURNAL-EXPORT\t1\n";
        stream << "N\t1700000000000\tmanual\tcpp\tvalid first record\n";
        stream << "BROKEN\n";
    }
    axiom::JournalStore store{store_path};
    (void)store.load();
    (void)store.add_note(L"existing note");
    const auto before = store.status().entry_count;
    bool rejected = false;
    try {
        (void)axiom::import_journal(store, import_path);
    } catch (...) {
        rejected = true;
    }
    expect(rejected, "malformed import must be rejected");
    expect(store.status().entry_count == before,
           "malformed import must not partially mutate the journal");
    clean_store(store_path);
    std::filesystem::remove(import_path);
}
} // namespace
int main() {
    try {
        roundtrip();
        malformed_import_is_non_mutating();
        std::cout << "journal_transfer_tests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "journal_transfer_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
