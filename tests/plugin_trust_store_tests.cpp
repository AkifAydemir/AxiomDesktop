#include "plugin_trust_store.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error{message};
}
axiom::PluginTrustRecord record(std::string id, char fill) {
    return {std::move(id),         "1.0.0", "plugin.dll", false, false, std::string(64, fill),
            std::string(64, fill), {}};
}
} // namespace
int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() / "axiom-plugin-trust-tests";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root);
        const auto path = root / "trust.conf";
        axiom::PluginTrustStore store{path};
        require(store.load().empty(), "fresh trust store should be empty");
        store.upsert(record("hello", 'a'));
        require(store.set_enabled("hello", true), "enable failed");
        require(store.find("hello")->enabled, "enable not persisted in memory");
        require(store.quarantine("hello", "hash mismatch"), "quarantine failed");
        const auto quarantined = store.find("hello");
        require(quarantined && quarantined->quarantined && !quarantined->enabled,
                "quarantine must force disabled");
        bool rejected = false;
        try {
            (void)store.set_enabled("hello", true);
        } catch (...) {
            rejected = true;
        }
        require(rejected, "quarantined plugin was enabled");
        // Create a healthy backup, then corrupt the primary. load() must recover fail-closed from
        // backup.
        store.upsert(record("second", 'b'));
        std::ofstream{path, std::ios::binary | std::ios::trunc} << "corrupt\n";
        axiom::PluginTrustStore recovered{path};
        const auto loaded = recovered.load();
        require(!loaded.empty(), "backup recovery returned no records");
        require(recovered.status().recovered_from_alternate, "alternate recovery flag missing");
        std::filesystem::remove(path.string() + ".bak", ec);
        std::filesystem::remove(path.string() + ".tmp", ec);
        std::ofstream{path, std::ios::binary | std::ios::trunc} << "corrupt\n";
        axiom::PluginTrustStore broken{path};
        bool failed_closed = false;
        try {
            (void)broken.load();
        } catch (...) {
            failed_closed = true;
        }
        require(failed_closed, "corrupt trust store did not fail closed");
        std::filesystem::remove_all(root, ec);
        std::cout << "plugin trust store tests passed\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
