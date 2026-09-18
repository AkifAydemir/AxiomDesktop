#include "plugin_api.hpp"
#include "plugin_manifest.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
void expect(bool value, const char *message) {
    if (!value)
        throw std::runtime_error{message};
}
void run_tests() {
    const auto root = std::filesystem::temp_directory_path() / "axiom-plugin-manifest-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto good = root / "hello.axp";
    {
        std::ofstream out{good};
        out << "id=hello.sample\nname=Hello Sample\nversion=1.0.0\nlibrary=hello.dll\nabi="
            << AXIOM_PLUGIN_ABI_V1 << "\ncapabilities=actions,commands\n";
    }
    const auto parsed = axiom::parse_plugin_manifest(good);
    expect(parsed.success, parsed.error.c_str());
    expect(parsed.manifest.id == "hello.sample", "id mismatch");
    expect(parsed.manifest.capabilities == (AXIOM_PLUGIN_CAP_ACTIONS | AXIOM_PLUGIN_CAP_COMMANDS),
           "capability mismatch");
    expect(axiom::discover_plugin_manifests(root).size() == 1, "discovery mismatch");
    const auto traversal = root / "bad.axp";
    {
        std::ofstream out{traversal};
        out << "id=bad\nname=Bad\nversion=1\nlibrary=../bad.dll\nabi=" << AXIOM_PLUGIN_ABI_V1
            << "\ncapabilities=commands\n";
    }
    expect(!axiom::parse_plugin_manifest(traversal).success,
           "path traversal library must be rejected");
    std::filesystem::remove_all(root);
}
} // namespace
int main() {
    try {
        run_tests();
        std::cout << "AxiomPluginManifestTests: PASS\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "AxiomPluginManifestTests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
