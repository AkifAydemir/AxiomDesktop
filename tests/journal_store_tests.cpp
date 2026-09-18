#include "journal_store.hpp"
#include "local_data_archive.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
using namespace std::chrono_literals;
namespace {
void expect(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error{message};
}
class XorProtector final : public axiom::DataProtector {
  public:
    explicit XorProtector(bool available = true) : available_{available} {}
    bool available() const noexcept override {
        return available_;
    }
    std::wstring name() const override {
        return L"test-xor";
    }
    std::vector<std::byte> protect(std::span<const std::byte> plaintext) const override {
        if (!available_)
            throw std::runtime_error{"protector unavailable"};
        std::vector<std::byte> output{plaintext.begin(), plaintext.end()};
        for (auto &value : output)
            value ^= std::byte{0x5a};
        return output;
    }
    std::vector<std::byte> unprotect(std::span<const std::byte> ciphertext) const override {
        return protect(ciphertext);
    }

  private:
    bool available_{};
};
std::string magic4(const std::filesystem::path &path) {
    std::ifstream stream{path, std::ios::binary};
    char bytes[4]{};
    stream.read(bytes, 4);
    return {bytes, static_cast<std::size_t>(stream.gcount())};
}
std::filesystem::path temp_file(const char *name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(p, ec);
    std::filesystem::remove(p.wstring() + L".tmp", ec);
    std::filesystem::remove(p.wstring() + L".bak", ec);
    return p;
}
void cleanup(const std::filesystem::path &p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
    std::filesystem::remove(p.wstring() + L".tmp", ec);
    std::filesystem::remove(p.wstring() + L".bak", ec);
}
void persistence_and_ids() {
    const auto path = temp_file("axiom_journal_persistence.bin");
    const auto base = std::chrono::system_clock::time_point{1700000000000ms};
    axiom::JournalEntryId first{};
    {
        axiom::JournalStore store{path};
        (void)store.load();
        first = store.add_note(L"Design the native search palette",
                               {L"Project", L"#CPP", L"project"}, base);
        const auto second =
            store.add_activity(L"automation", L"Automation #4 completed", {L"system"}, base + 2min);
        expect(first == 1 && second == 2, "IDs must be stable and monotonic");
    }
    {
        axiom::JournalStore store{path};
        const auto snapshot = store.load();
        expect(snapshot.entries.size() == 2, "journal must survive restart");
        expect(snapshot.next_id == 3, "next ID must survive restart");
        const auto item = store.find(first);
        expect(item && item->tags.size() == 2, "tags must normalize/dedupe");
        expect(item->tags[0] == L"cpp" && item->tags[1] == L"project",
               "tags must be lowercase sorted");
    }
    cleanup(path);
}
void search_day_tag_and_context() {
    const auto path = temp_file("axiom_journal_search.bin");
    const auto base = std::chrono::system_clock::time_point{1700000000000ms};
    axiom::JournalStore store{path};
    (void)store.load();
    (void)store.add_note(L"Finish journal ranking", {L"cpp", L"axiom"}, base + 1h);
    (void)store.add_note(L"Buy milk after work", {L"personal"}, base + 2h);
    (void)store.add_activity(L"reminder", L"Reminder fired: stretch", {L"system"}, base + 26h);
    const auto search = store.search(L"journal", 10);
    expect(search.size() == 1 && search[0].text.find(L"ranking") != std::wstring::npos,
           "search must rank matching note");
    const auto tagged = store.tagged(L"#CPP", 10);
    expect(tagged.size() == 1, "tag lookup must normalize hashtag/case");
    const auto day = store.between(base, base + 24h, 20);
    expect(day.size() == 2, "day window must filter timestamps");
    const auto context = store.context(L"journal", 5, 12);
    expect(context.size() == 1 && context[0].text.size() <= 12,
           "context must enforce character budget");
    cleanup(path);
}
void retention_delete_and_recovery() {
    const auto path = temp_file("axiom_journal_retention.bin");
    axiom::JournalStore store{path, 3};
    (void)store.load();
    const auto a = store.add_note(L"a");
    const auto b = store.add_note(L"b");
    (void)store.add_activity(L"test", L"c");
    (void)store.add_note(L"d");
    expect(!store.find(a).has_value() && store.find(b).has_value(),
           "retention must evict oldest entry");
    expect(store.erase(b), "erase existing entry must succeed");
    expect(!store.erase(9999), "erase unknown entry must fail");
    (void)store.add_note(L"note e");
    (void)store.add_activity(L"test", L"activity f");
    const auto removed = store.clear_notes();
    expect(removed >= 1, "clear_notes must remove notes");
    const auto status = store.status();
    expect(status.note_count == 0 && status.activity_count >= 1,
           "clear_notes must preserve activities");
    // Force a newer valid backup generation, then corrupt primary. Loader must recover from
    // alternate.
    {
        axiom::JournalStore another{path, 3};
        (void)another.load();
        (void)another.add_activity(L"test", L"survives recovery");
    }
    {
        std::ofstream primary{path, std::ios::binary | std::ios::trunc};
        primary << "corrupt";
    }
    axiom::JournalStore recovered{path, 3};
    const auto snapshot = recovered.load();
    expect(!snapshot.entries.empty(),
           "loader must recover from valid .bak when primary is corrupt");
    expect(recovered.status().recovered_from_alternate, "status must report alternate recovery");
    cleanup(path);
}
void retention_policy() {
    const auto path = temp_file("axiom_journal_policy.bin");
    const auto now = std::chrono::system_clock::time_point{1700000000000ms};
    axiom::JournalStore store{path, 100};
    (void)store.load();
    (void)store.add_note(L"old note", {}, now - 20h);
    (void)store.add_activity(L"test", L"old activity", {}, now - 20h);
    (void)store.add_activity(L"test", L"new activity", {}, now);
    expect(store.prune_before(now - 10h, axiom::JournalEntryKind::activity) == 1,
           "activity retention must prune only matching old activities");
    expect(store.status().note_count == 1 && store.status().activity_count == 1,
           "retention must preserve notes when pruning activities");
    expect(store.set_max_entries(100) == 0, "setting unchanged safe capacity should not prune");
    cleanup(path);
}
void protection_migration_and_toggle() {
    const auto path = temp_file("axiom_journal_protection.bin");
    const auto portable = temp_file("axiom_journal_portable.bin");
    XorProtector protector;
    {
        axiom::JournalStore plaintext{path};
        (void)plaintext.load();
        (void)plaintext.add_note(L"migrate me", {L"privacy"});
    }
    expect(magic4(path) == "AXJR", "v12-compatible journal must start as plaintext AXJR");
    {
        axiom::JournalStore protected_store{path, 50000, &protector, true};
        const auto snapshot = protected_store.load();
        expect(snapshot.entries.size() == 1, "protected migration must preserve entries");
        const auto status = protected_store.status();
        expect(status.protected_at_rest && status.protection_migrated,
               "plaintext journal must migrate to protected AXJP");
        expect(magic4(path) == "AXJP", "protected journal must use AXJP envelope");
        expect(!std::filesystem::exists(path.wstring() + L".bak"),
               "successful migration must not leave plaintext backup");
        const auto portable_bytes = protected_store.plaintext_snapshot_bytes();
        expect(portable_bytes.size() >= 4 && std::to_integer<char>(portable_bytes[0]) == 'A' &&
                   std::to_integer<char>(portable_bytes[1]) == 'X' &&
                   std::to_integer<char>(portable_bytes[2]) == 'J' &&
                   std::to_integer<char>(portable_bytes[3]) == 'R',
               "portable snapshot must remain plaintext AXJR in memory");
        {
            std::ofstream output{portable, std::ios::binary | std::ios::trunc};
            output.write(reinterpret_cast<const char *>(portable_bytes.data()),
                         static_cast<std::streamsize>(portable_bytes.size()));
        }
        protected_store.set_protection_enabled(false);
        expect(magic4(path) == "AXJR", "explicit protection-off must rewrite plaintext AXJR");
        protected_store.set_protection_enabled(true);
        expect(magic4(path) == "AXJP", "protection re-enable must rewrite AXJP");
    }
    {
        axiom::JournalStore reopened{path, 50000, &protector, true};
        expect(reopened.load().entries.size() == 1, "protected journal must survive restart");
    }
    {
        axiom::JournalStore portable_store{portable};
        expect(portable_store.load().entries.size() == 1,
               "portable plaintext snapshot must be independently readable");
    }
    cleanup(path);
    cleanup(portable);
}
void protected_recovery_and_fail_closed() {
    const auto path = temp_file("axiom_journal_protected_recovery.bin");
    XorProtector protector;
    {
        axiom::JournalStore store{path, 50000, &protector, true};
        (void)store.load();
        (void)store.add_note(L"one");
        (void)store.add_note(L"two");
    }
    expect(magic4(path) == "AXJP", "protected store must use AXJP");
    expect(magic4(path.wstring() + L".bak") == "AXJP",
           "protected save backup must also be protected");
    {
        std::ofstream primary{path, std::ios::binary | std::ios::trunc};
        primary << "corrupt";
    }
    {
        axiom::JournalStore recovered{path, 50000, &protector, true};
        const auto snapshot = recovered.load();
        expect(!snapshot.entries.empty(), "protected loader must recover from protected backup");
        expect(recovered.status().recovered_from_alternate,
               "protected recovery must report alternate source");
        (void)recovered.add_note(L"after recovery");
        expect(magic4(path) == "AXJP", "post-recovery save must publish protected primary");
    }
    cleanup(path);
    {
        axiom::JournalStore store{path, 50000, &protector, true};
        (void)store.load();
        (void)store.add_note(L"only candidate");
    }
    std::error_code ec;
    std::filesystem::remove(path.wstring() + L".bak", ec);
    {
        std::ofstream primary{path, std::ios::binary | std::ios::trunc};
        primary << "corrupt";
    }
    bool rejected = false;
    try {
        axiom::JournalStore broken{path, 50000, &protector, true};
        (void)broken.load();
    } catch (...) {
        rejected = true;
    }
    expect(
        rejected,
        "existing but invalid protected candidates must fail closed, not become an empty journal");
    XorProtector unavailable{false};
    cleanup(path);
    bool unavailable_rejected = false;
    try {
        axiom::JournalStore unavailable_store{path, 50000, &unavailable, true};
        (void)unavailable_store.load();
    } catch (...) {
        unavailable_rejected = true;
    }
    expect(unavailable_rejected, "requested live protection must reject an unavailable protector");
    cleanup(path);
}
} // namespace
int main() {
    try {
        persistence_and_ids();
        search_day_tag_and_context();
        retention_delete_and_recovery();
        retention_policy();
        protection_migration_and_toggle();
        protected_recovery_and_fail_closed();
        std::cout << "journal_store_tests: PASS\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "journal_store_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
