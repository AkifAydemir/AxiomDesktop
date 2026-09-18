#include "plugin_package.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error{message};
}
void write_manifest(const std::filesystem::path &path) {
    std::ofstream out{path};
    out << "id=hello\nname=Hello "
           "Plugin\nversion=1.0.0\nabi=65536\nlibrary=hello.dll\ncapabilities=actions,commands\n";
}
} // namespace
int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() / "axiom-plugin-package-tests";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        const auto source = root / "source";
        const auto managed = root / "managed";
        std::filesystem::create_directories(source);
        write_manifest(source / "hello.axp");
        std::ofstream{source / "hello.dll", std::ios::binary} << "fake-dll-v1";
        axiom::PluginTrustStore trust{root / "trust.conf"};
        (void)trust.load();
        axiom::PluginPackageManager packages{managed, trust};
        const auto install = packages.install(source / "hello.axp");
        require(install.success && install.plugin_id == "hello", "install failed");
        const auto rec = trust.find("hello");
        require(rec && !rec->enabled && !rec->quarantined, "install must default disabled");
        require(packages.verify(*rec).success, "fresh package verification failed");
        std::ofstream{packages.plugin_directory("hello") / "hello.dll",
                      std::ios::binary | std::ios::app}
            << "tamper";
        require(!packages.verify(*rec).success, "tampered library verified");
        require(trust.set_enabled("hello", true), "enable setup failed");
        require(!packages.remove("hello").success, "enabled plugin removal should be rejected");
        require(trust.set_enabled("hello", false), "disable failed");
        require(packages.remove("hello").success, "disabled plugin removal failed");
        require(!trust.find("hello"), "trust record survived removal");
        require(!std::filesystem::exists(packages.plugin_directory("hello")),
                "managed directory survived removal");
        std::filesystem::remove_all(root, ec);
        std::cout << "plugin package tests passed\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
