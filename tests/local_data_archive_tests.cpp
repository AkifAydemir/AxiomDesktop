#include "local_data_archive.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
class XorProtector final : public axiom::DataProtector {
  public:
    bool available() const noexcept override {
        return true;
    }
    std::wstring name() const override {
        return L"test-xor";
    }
    std::vector<std::byte> protect(std::span<const std::byte> p) const override {
        std::vector<std::byte> o(p.begin(), p.end());
        for (auto &b : o)
            b ^= std::byte{0x5a};
        return o;
    }
    std::vector<std::byte> unprotect(std::span<const std::byte> p) const override {
        return protect(p);
    }
};
void expect(bool c, const char *m) {
    if (!c)
        throw std::runtime_error{m};
}
std::string read(const std::filesystem::path &p) {
    std::ifstream s{p, std::ios::binary};
    return {std::istreambuf_iterator<char>{s}, {}};
}
void write(const std::filesystem::path &p, std::string_view v) {
    std::ofstream s{p, std::ios::binary | std::ios::trunc};
    s << v;
}
void cleanup(const std::filesystem::path &p) {
    std::error_code e;
    std::filesystem::remove_all(p, e);
}
void roundtrip() {
    auto root = std::filesystem::temp_directory_path() / "axiom_backup_test";
    cleanup(root);
    std::filesystem::create_directories(root / "src");
    std::filesystem::create_directories(root / "dst");
    write(root / "src/settings.conf", "settings-v10");
    write(root / "src/journal.bin", "journal-data");
    std::vector<axiom::LocalDataFile> src{{"settings.conf", root / "src/settings.conf"},
                                          {"journal.bin", root / "src/journal.bin"},
                                          {"missing.bin", root / "src/missing.bin"}};
    auto info = axiom::LocalDataArchive::create(root / "state.axbak", src);
    expect(info.entries.size() == 2, "missing files must be skipped");
    auto inspected = axiom::LocalDataArchive::inspect(root / "state.axbak");
    expect(!inspected.protected_by_os && inspected.entries.size() == 2,
           "inspect must expose archive metadata");
    std::vector<axiom::LocalDataFile> dst{{"settings.conf", root / "dst/settings.conf"},
                                          {"journal.bin", root / "dst/journal.bin"}};
    axiom::LocalDataArchive::restore(root / "state.axbak", dst, root / "restore.journal");
    expect(read(root / "dst/settings.conf") == "settings-v10", "settings restore mismatch");
    expect(read(root / "dst/journal.bin") == "journal-data", "journal restore mismatch");
    cleanup(root);
}
void protected_roundtrip() {
    auto root = std::filesystem::temp_directory_path() / "axiom_backup_protected";
    cleanup(root);
    std::filesystem::create_directories(root / "src");
    std::filesystem::create_directories(root / "dst");
    write(root / "src/journal.bin", "secret-state");
    std::vector<axiom::LocalDataFile> src{{"journal.bin", root / "src/journal.bin"}};
    XorProtector protector;
    auto info = axiom::LocalDataArchive::create(root / "state.axbak", src, &protector);
    expect(info.protected_by_os, "protected envelope flag");
    auto inspected = axiom::LocalDataArchive::inspect(root / "state.axbak", &protector);
    expect(inspected.protected_by_os && inspected.entries.size() == 1, "protected inspect");
    std::vector<axiom::LocalDataFile> dst{{"journal.bin", root / "dst/journal.bin"}};
    axiom::LocalDataArchive::restore(root / "state.axbak", dst, root / "restore.journal",
                                     &protector);
    expect(read(root / "dst/journal.bin") == "secret-state", "protected restore");
    cleanup(root);
}
void in_memory_override() {
    auto root = std::filesystem::temp_directory_path() / "axiom_backup_override";
    cleanup(root);
    std::filesystem::create_directories(root / "src");
    std::filesystem::create_directories(root / "dst");
    write(root / "src/journal.bin", "AXJP-live-protected-placeholder");
    std::vector<axiom::LocalDataFile> src{{"journal.bin", root / "src/journal.bin"}};
    const std::string portable = "AXJR-portable-inner";
    axiom::LocalDataBlob blob;
    blob.name = "journal.bin";
    blob.data.resize(portable.size());
    for (std::size_t i = 0; i < portable.size(); ++i)
        blob.data[i] = std::byte{static_cast<unsigned char>(portable[i])};
    std::vector<axiom::LocalDataBlob> overrides;
    overrides.push_back(blob);
    auto info = axiom::LocalDataArchive::create(root / "portable.axbak", src, nullptr, overrides);
    expect(info.entries.size() == 1, "override archive entry missing");
    std::vector<axiom::LocalDataFile> dst{{"journal.bin", root / "dst/journal.bin"}};
    axiom::LocalDataArchive::restore(root / "portable.axbak", dst, root / "restore.journal");
    expect(read(root / "dst/journal.bin") == portable,
           "in-memory override must replace source bytes");
    bool threw = false;
    try {
        axiom::LocalDataBlob unknown;
        unknown.name = "unknown.bin";
        unknown.data = {std::byte{0x01}};
        std::vector<axiom::LocalDataBlob> bad{unknown};
        (void)axiom::LocalDataArchive::create(root / "bad.axbak", src, nullptr, bad);
    } catch (...) {
        threw = true;
    }
    expect(threw, "unknown override name must be rejected");
    cleanup(root);
}
void transactional_restore_rolls_back_on_destination_failure() {
    auto root = std::filesystem::temp_directory_path() / "axiom_backup_transactional_rollback";
    cleanup(root);
    std::filesystem::create_directories(root / "src");
    std::filesystem::create_directories(root / "dst");
    write(root / "src/a", "new-a");
    write(root / "src/b", "new-b");
    write(root / "dst/a", "old-a");
    std::filesystem::create_directories(root / "dst/b");
    std::vector<axiom::LocalDataFile> src{{"a", root / "src/a"}, {"b", root / "src/b"}};
    axiom::LocalDataArchive::create(root / "state.axbak", src);
    std::vector<axiom::LocalDataFile> dst{{"a", root / "dst/a"}, {"b", root / "dst/b"}};
    bool threw = false;
    try {
        (void)axiom::LocalDataArchive::restore(root / "state.axbak", dst, root / "restore.journal");
    } catch (...) {
        threw = true;
    }
    expect(threw, "transactional restore must reject non-regular destination");
    expect(read(root / "dst/a") == "old-a",
           "earlier destination must roll back after later failure");
    expect(std::filesystem::is_directory(root / "dst/b"),
           "failing destination directory must survive");
    expect(std::filesystem::exists(root / "restore.journal"),
           "suspicious non-regular destination must preserve recovery journal for fail-closed "
           "inspection");
    cleanup(root);
}
void corruption() {
    auto root = std::filesystem::temp_directory_path() / "axiom_backup_corrupt";
    cleanup(root);
    std::filesystem::create_directories(root);
    write(root / "a", "abcdef");
    std::vector<axiom::LocalDataFile> f{{"a", root / "a"}};
    axiom::LocalDataArchive::create(root / "x.axbak", f);
    {
        std::fstream s{root / "x.axbak", std::ios::binary | std::ios::in | std::ios::out};
        s.seekp(-1, std::ios::end);
        char x = 'X';
        s.write(&x, 1);
    }
    bool threw = false;
    try {
        (void)axiom::LocalDataArchive::inspect(root / "x.axbak");
    } catch (...) {
        threw = true;
    }
    expect(threw, "CRC corruption must be rejected");
    cleanup(root);
}
} // namespace
int main() {
    try {
        roundtrip();
        protected_roundtrip();
        in_memory_override();
        transactional_restore_rolls_back_on_destination_failure();
        corruption();
        std::cout << "local_data_archive_tests: PASS\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "local_data_archive_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
