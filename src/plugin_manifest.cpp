#include "plugin_manifest.hpp"
#include "plugin_api.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <sstream>
namespace axiom {
namespace {
std::string trim_ascii(std::string value) {
    auto is_space = [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}
bool valid_library_name(std::string_view value) {
    if (value.empty() || value.size() > 180)
        return false;
    if (value.find('/') != std::string_view::npos || value.find('\\') != std::string_view::npos)
        return false;
    if (value == "." || value == "..")
        return false;
    return true;
}
std::uint64_t parse_capabilities(std::string_view value, bool &ok) {
    ok = true;
    std::uint64_t caps = 0;
    std::stringstream stream{std::string{value}};
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = trim_ascii(std::move(item));
        if (item.empty())
            continue;
        if (item == "actions")
            caps |= AXIOM_PLUGIN_CAP_ACTIONS;
        else if (item == "commands")
            caps |= AXIOM_PLUGIN_CAP_COMMANDS;
        else {
            ok = false;
            return 0;
        }
    }
    return caps;
}
} // namespace
bool is_valid_plugin_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64)
        return false;
    for (char ch : value) {
        const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        const bool digit = ch >= '0' && ch <= '9';
        if (!alpha && !digit && ch != '-' && ch != '_' && ch != '.')
            return false;
    }
    return true;
}
PluginManifestResult parse_plugin_manifest(const std::filesystem::path &path) {
    std::ifstream input{path};
    if (!input)
        return {false, {}, "Unable to open plugin manifest."};
    std::map<std::string, std::string> values;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim_ascii(std::move(line));
        if (line.empty() || line.front() == '#')
            continue;
        const auto equal = line.find('=');
        if (equal == std::string::npos) {
            return {
                false, {}, "Malformed plugin manifest line " + std::to_string(line_number) + "."};
        }
        auto key = trim_ascii(line.substr(0, equal));
        auto value = trim_ascii(line.substr(equal + 1));
        if (key.empty() || value.empty()) {
            return {false,
                    {},
                    "Empty plugin manifest key/value at line " + std::to_string(line_number) + "."};
        }
        if (!values.emplace(std::move(key), std::move(value)).second) {
            return {false,
                    {},
                    "Duplicate plugin manifest key at line " + std::to_string(line_number) + "."};
        }
    }
    const char *required[] = {"id", "name", "version", "library", "abi", "capabilities"};
    for (const char *key : required) {
        if (!values.contains(key))
            return {false, {}, std::string{"Missing plugin manifest key: "} + key};
    }
    PluginManifest manifest;
    manifest.id = values["id"];
    manifest.name = values["name"];
    manifest.version = values["version"];
    manifest.library = values["library"];
    manifest.source_path = path;
    if (!is_valid_plugin_id(manifest.id))
        return {false, {}, "Invalid plugin id."};
    if (manifest.name.empty() || manifest.name.size() > 120)
        return {false, {}, "Invalid plugin display name."};
    if (manifest.version.empty() || manifest.version.size() > 64)
        return {false, {}, "Invalid plugin version."};
    if (!valid_library_name(manifest.library))
        return {false, {}, "Plugin library must be a filename without path separators."};
    std::uint32_t abi = 0;
    const auto abi_text = values["abi"];
    const auto [ptr, ec] =
        std::from_chars(abi_text.data(), abi_text.data() + abi_text.size(), abi, 10);
    if (ec != std::errc{} || ptr != abi_text.data() + abi_text.size()) {
        return {false, {}, "Invalid plugin ABI value."};
    }
    manifest.abi_version = abi;
    if (manifest.abi_version != AXIOM_PLUGIN_ABI_V1) {
        return {false, {}, "Unsupported plugin ABI version."};
    }
    bool capabilities_ok = false;
    manifest.capabilities = parse_capabilities(values["capabilities"], capabilities_ok);
    if (!capabilities_ok)
        return {false, {}, "Unknown plugin capability."};
    if (manifest.capabilities == 0)
        return {false, {}, "Plugin must request at least one capability."};
    return {true, std::move(manifest), {}};
}
std::vector<std::filesystem::path>
discover_plugin_manifests(const std::filesystem::path &directory) {
    std::vector<std::filesystem::path> output;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
        return output;
    for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
        if (error)
            break;
        if (!entry.is_regular_file(error))
            continue;
        if (entry.path().extension() == ".axp")
            output.push_back(entry.path());
    }
    std::sort(output.begin(), output.end());
    return output;
}
} // namespace axiom
