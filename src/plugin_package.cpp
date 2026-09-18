#include "plugin_package.hpp"
#include "plugin_manifest.hpp"
#include "sha256.hpp"
#include <filesystem>
#include <stdexcept>
namespace axiom {
namespace {
[[nodiscard]] std::filesystem::path staging_path(const std::filesystem::path &root,
                                                 std::string_view id) {
    return root / std::filesystem::path{".install-" + std::string{id}};
}
[[nodiscard]] std::filesystem::path removal_path(const std::filesystem::path &root,
                                                 std::string_view id) {
    return root / std::filesystem::path{".remove-" + std::string{id}};
}
} // namespace
PluginPackageManager::PluginPackageManager(std::filesystem::path root,
                                           PluginTrustStore &trust_store)
    : root_{std::move(root)}, trust_store_{trust_store} {}
std::filesystem::path PluginPackageManager::plugin_directory(std::string_view plugin_id) const {
    if (!is_valid_plugin_id(plugin_id)) {
        throw std::invalid_argument{"Invalid plugin ID."};
    }
    return root_ / std::filesystem::path{std::string{plugin_id}};
}
const std::filesystem::path &PluginPackageManager::root() const noexcept {
    return root_;
}
PluginPackageResult PluginPackageManager::install(const std::filesystem::path &manifest_path) {
    const auto parsed = parse_plugin_manifest(manifest_path);
    if (!parsed.success) {
        return {false, {}, parsed.error};
    }
    const auto &manifest = parsed.manifest;
    if (trust_store_.find(manifest.id)) {
        return {
            false,
            manifest.id,
            "Plugin is already installed. Remove it before installing a different pinned package.",
        };
    }
    if (manifest.library == "plugin.axp") {
        return {false, manifest.id,
                "Plugin library filename conflicts with the managed manifest filename."};
    }
    const auto source_library =
        manifest_path.parent_path() / std::filesystem::path{manifest.library};
    std::error_code error;
    if (!std::filesystem::is_regular_file(source_library, error) || error) {
        return {false, manifest.id, "Plugin library referenced by the manifest does not exist."};
    }
    std::filesystem::create_directories(root_, error);
    if (error) {
        return {false, manifest.id, "Unable to create the managed plugin root."};
    }
    const auto destination = plugin_directory(manifest.id);
    if (std::filesystem::exists(destination, error) && !error) {
        return {false, manifest.id, "Managed plugin directory already exists."};
    }
    const auto staging = staging_path(root_, manifest.id);
    std::filesystem::remove_all(staging, error);
    error.clear();
    std::filesystem::create_directories(staging, error);
    if (error) {
        return {false, manifest.id, "Unable to create the plugin staging directory."};
    }
    try {
        const auto managed_manifest = staging / "plugin.axp";
        const auto managed_library = staging / std::filesystem::path{manifest.library};
        std::filesystem::copy_file(manifest_path, managed_manifest,
                                   std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(source_library, managed_library,
                                   std::filesystem::copy_options::overwrite_existing);
        const auto manifest_hash = sha256_file_hex(managed_manifest);
        const auto library_hash = sha256_file_hex(managed_library);
        const auto staged_parse = parse_plugin_manifest(managed_manifest);
        if (!staged_parse.success || staged_parse.manifest.id != manifest.id ||
            staged_parse.manifest.version != manifest.version ||
            staged_parse.manifest.library != manifest.library) {
            throw std::runtime_error{"Managed plugin manifest changed during installation."};
        }
        std::filesystem::rename(staging, destination);
        try {
            trust_store_.upsert({
                manifest.id,
                manifest.version,
                manifest.library,
                false,
                false,
                manifest_hash,
                library_hash,
                {},
            });
        } catch (...) {
            std::filesystem::remove_all(destination, error);
            throw;
        }
    } catch (const std::exception &exception) {
        std::filesystem::remove_all(staging, error);
        return {false, manifest.id, exception.what()};
    }
    return {
        true,
        manifest.id,
        "Plugin installed disabled with pinned manifest/library SHA-256. Enable it explicitly "
        "after review.",
    };
}
PluginPackageResult PluginPackageManager::remove(std::string_view plugin_id) {
    const auto record = trust_store_.find(plugin_id);
    if (!record) {
        return {false, std::string{plugin_id}, "Plugin is not installed."};
    }
    if (record->enabled) {
        return {false, record->id, "Disable the plugin before removal."};
    }
    const auto directory = plugin_directory(plugin_id);
    const auto removal = removal_path(root_, plugin_id);
    std::error_code error;
    std::filesystem::remove_all(removal, error);
    error.clear();
    const bool directory_exists = std::filesystem::exists(directory, error) && !error;
    if (directory_exists) {
        std::filesystem::rename(directory, removal, error);
        if (error) {
            return {false, record->id, "Unable to stage the managed plugin directory for removal."};
        }
    }
    try {
        if (!trust_store_.erase(plugin_id)) {
            throw std::runtime_error{"Plugin trust record disappeared during removal."};
        }
    } catch (const std::exception &exception) {
        if (directory_exists) {
            std::error_code restore_error;
            std::filesystem::rename(removal, directory, restore_error);
        }
        return {false, record->id, exception.what()};
    }
    if (directory_exists) {
        std::filesystem::remove_all(removal, error);
    }
    return {true, record->id, "Plugin removed."};
}
PluginVerificationResult PluginPackageManager::verify(const PluginTrustRecord &record) const {
    PluginVerificationResult result;
    if (!is_valid_plugin_id(record.id)) {
        result.message = "Invalid installed plugin ID.";
        return result;
    }
    const auto directory = plugin_directory(record.id);
    result.manifest_path = directory / "plugin.axp";
    const auto parsed = parse_plugin_manifest(result.manifest_path);
    if (!parsed.success) {
        result.message = parsed.error;
        return result;
    }
    if (parsed.manifest.id != record.id || parsed.manifest.version != record.version ||
        parsed.manifest.library != record.library) {
        result.message =
            "Installed manifest identity/version/library no longer matches pinned trust metadata.";
        return result;
    }
    result.library_path = directory / std::filesystem::path{record.library};
    std::error_code error;
    if (!std::filesystem::is_regular_file(result.library_path, error) || error) {
        result.message = "Pinned plugin library is missing.";
        return result;
    }
    try {
        result.manifest_sha256 = sha256_file_hex(result.manifest_path);
        result.library_sha256 = sha256_file_hex(result.library_path);
    } catch (const std::exception &exception) {
        result.message = exception.what();
        return result;
    }
    if (result.manifest_sha256 != record.manifest_sha256) {
        result.message = "Plugin manifest SHA-256 pin mismatch.";
        return result;
    }
    if (result.library_sha256 != record.library_sha256) {
        result.message = "Plugin library SHA-256 pin mismatch.";
        return result;
    }
    result.success = true;
    result.message = "Pinned plugin package verified.";
    return result;
}
} // namespace axiom
