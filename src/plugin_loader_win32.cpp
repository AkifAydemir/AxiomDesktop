#ifdef _WIN32
#include "plugin_loader_win32.hpp"
#include <algorithm>
#include <system_error>
namespace axiom {
namespace {
std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
        throw std::system_error{static_cast<int>(GetLastError()), std::system_category(),
                                "Invalid UTF-8 text"};
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), output.data(), size) <= 0) {
        throw std::system_error{static_cast<int>(GetLastError()), std::system_category(),
                                "Invalid UTF-8 text"};
    }
    return output;
}
std::wstring last_error_text(DWORD code) {
    return L"Windows error " + std::to_wstring(code);
}
bool descriptor_matches(const PluginManifest &manifest, const AxiomPluginDescriptorV1 &descriptor) {
    if (descriptor.struct_size < sizeof(AxiomPluginDescriptorV1))
        return false;
    if (descriptor.abi_version != AXIOM_PLUGIN_ABI_V1)
        return false;
    if (descriptor.plugin_id == nullptr || descriptor.plugin_version == nullptr)
        return false;
    if (manifest.id != descriptor.plugin_id || manifest.version != descriptor.plugin_version)
        return false;
    if ((descriptor.requested_capabilities & ~manifest.capabilities) != 0)
        return false;
    return true;
}
struct LoadDiagnosticRecorder {
    ExecutionDiagnostics *diagnostics{};
    PluginLoadResult *result{};
    ~LoadDiagnosticRecorder() {
        try {
            if (diagnostics == nullptr || result == nullptr) {
                return;
            }
            std::wstring subject;
            if (!result->plugin_id.empty()) {
                subject = utf8_to_wide(result->plugin_id);
            } else {
                subject = result->manifest_path.filename().wstring();
            }
            [[maybe_unused]] const auto id = diagnostics->record(
                result->success ? DiagnosticSeverity::info : DiagnosticSeverity::error,
                DiagnosticDomain::plugin, std::move(subject),
                L"load: " +
                    (result->message.empty() ? std::wstring{L"no diagnostic"} : result->message));
        } catch (...) {
        }
    }
};
} // namespace
PluginLoaderWin32::PluginLoaderWin32(PluginHost &host, ExecutionDiagnostics *diagnostics)
    : host_{host}, diagnostics_{diagnostics} {}
PluginLoaderWin32::~PluginLoaderWin32() {
    unload_all();
}
PluginLoadResult PluginLoaderWin32::load_manifest(const std::filesystem::path &manifest_path) {
    PluginLoadResult result;
    result.manifest_path = manifest_path;
    LoadDiagnosticRecorder diagnostic_recorder{diagnostics_, &result};
    const auto parsed = parse_plugin_manifest(manifest_path);
    if (!parsed.success) {
        result.message = utf8_to_wide(parsed.error);
        return result;
    }
    const auto &manifest = parsed.manifest;
    result.plugin_id = manifest.id;
    if (is_loaded(manifest.id)) {
        result.message = L"Plugin is already loaded.";
        return result;
    }
    const auto library_path =
        manifest_path.parent_path() / std::filesystem::path{utf8_to_wide(manifest.library)};
    const HMODULE module =
        LoadLibraryExW(library_path.c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr) {
        result.message = L"LoadLibraryExW failed: " + last_error_text(GetLastError());
        return result;
    }
    const auto query =
        reinterpret_cast<AxiomPluginQueryFnV1>(GetProcAddress(module, AXIOM_PLUGIN_QUERY_SYMBOL));
    const auto load =
        reinterpret_cast<AxiomPluginLoadFnV1>(GetProcAddress(module, AXIOM_PLUGIN_LOAD_SYMBOL));
    const auto unload =
        reinterpret_cast<AxiomPluginUnloadFnV1>(GetProcAddress(module, AXIOM_PLUGIN_UNLOAD_SYMBOL));
    if (query == nullptr || load == nullptr) {
        result.message = L"Plugin is missing required v1 entry points.";
        FreeLibrary(module);
        return result;
    }
    const AxiomPluginDescriptorV1 *descriptor = nullptr;
    try {
        descriptor = query();
    } catch (...) {
        descriptor = nullptr;
    }
    if (descriptor == nullptr || !descriptor_matches(manifest, *descriptor)) {
        result.message = L"Plugin descriptor does not match its manifest/ABI contract.";
        FreeLibrary(module);
        return result;
    }
    PluginHost::RegistrationScope scope{host_, manifest.id, descriptor->requested_capabilities};
    int loaded = 0;
    try {
        loaded = load(scope.c_host());
    } catch (...) {
        loaded = 0;
    }
    if (loaded == 0) {
        host_.unregister_plugin(manifest.id);
        result.message = L"Plugin load callback rejected initialization.";
        FreeLibrary(module);
        return result;
    }
    host_.mark_plugin_loaded({
        manifest.id,
        utf8_to_wide(manifest.name),
        manifest.version,
        descriptor->requested_capabilities,
        scope.registered_actions(),
        scope.registered_commands(),
    });
    modules_.push_back({manifest.id, module, unload});
    result.success = true;
    result.message = L"Loaded.";
    return result;
}
bool PluginLoaderWin32::unload_plugin(std::string_view plugin_id) noexcept {
    const auto it = std::find_if(modules_.begin(), modules_.end(), [&](const LoadedModule &module) {
        return module.plugin_id == plugin_id;
    });
    if (it == modules_.end()) {
        if (diagnostics_ != nullptr) {
            try {
                [[maybe_unused]] const auto id = diagnostics_->record(
                    DiagnosticSeverity::warning, DiagnosticDomain::plugin, utf8_to_wide(plugin_id),
                    L"unload requested but plugin is not loaded.");
            } catch (...) {
            }
        }
        return false;
    }
    try {
        // unregister_plugin closes the callback gate and waits for active callbacks
        // before returning, so no registry-held callback can execute after FreeLibrary.
        const std::string id = it->plugin_id;
        host_.unregister_plugin(id);
        if (it->unload != nullptr) {
            try {
                it->unload();
            } catch (...) {
            }
        }
        if (it->module != nullptr) {
            FreeLibrary(it->module);
        }
        modules_.erase(it);
        if (diagnostics_ != nullptr) {
            try {
                [[maybe_unused]] const auto diagnostic_id =
                    diagnostics_->record(DiagnosticSeverity::info, DiagnosticDomain::plugin,
                                         utf8_to_wide(id), L"unload: completed.");
            } catch (...) {
            }
        }
        return true;
    } catch (...) {
        if (diagnostics_ != nullptr) {
            try {
                [[maybe_unused]] const auto diagnostic_id = diagnostics_->record(
                    DiagnosticSeverity::error, DiagnosticDomain::plugin, utf8_to_wide(plugin_id),
                    L"unload: failed with an unknown exception.");
            } catch (...) {
            }
        }
        return false;
    }
}
bool PluginLoaderWin32::is_loaded(std::string_view plugin_id) const noexcept {
    return std::any_of(modules_.begin(), modules_.end(),
                       [&](const LoadedModule &module) { return module.plugin_id == plugin_id; });
}
std::vector<std::string> PluginLoaderWin32::loaded_plugin_ids() const {
    std::vector<std::string> output;
    output.reserve(modules_.size());
    for (const auto &module : modules_)
        output.push_back(module.plugin_id);
    std::sort(output.begin(), output.end());
    return output;
}
void PluginLoaderWin32::unload_all() noexcept {
    while (!modules_.empty()) {
        const auto id = modules_.back().plugin_id;
        if (!unload_plugin(id)) {
            // Fail safe: leaking a DLL until process exit is preferable to freeing code
            // that may still have a callback in flight.
            break;
        }
    }
}
} // namespace axiom
#endif
