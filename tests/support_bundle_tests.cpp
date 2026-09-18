#include "support_bundle.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

void expect(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error{message};
}

std::string read(const std::filesystem::path &path) {
    std::ifstream stream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, {}};
}

void privacy_defaults() {
    const auto path = std::filesystem::temp_directory_path() / "axiom_support_bundle_default.txt";
    std::error_code error;
    std::filesystem::remove(path, error);

    std::vector<axiom::SupportField> fields{
        {"product-version", "0.16.0", false},
        {"watcher-health", "resync-required", false},
        {"local-data-root", "C:/Users/example/AppData/Local/Axiom", true},
    };
    std::vector<axiom::SupportDiagnostic> diagnostics{
        {7, 123456, "error", "automation", "42", "secret payload-like detail\nnext line"},
    };

    const auto report = axiom::SupportBundle::create(path, fields, diagnostics);
    const auto text = read(path);
    expect(report.fields_written == 2, "non-sensitive field count mismatch");
    expect(report.sensitive_fields_omitted == 1, "sensitive omission count mismatch");
    expect(report.diagnostics_written == 1, "diagnostic count mismatch");
    expect(report.diagnostic_messages_omitted == 1, "diagnostic message omission mismatch");
    expect(text.find("local-data-root") == std::string::npos,
           "sensitive field must be omitted by default");
    expect(text.find("secret payload-like detail") == std::string::npos,
           "diagnostic message must be omitted by default");
    expect(text.find("subject=42") != std::string::npos,
           "diagnostic subject should remain available for correlation");
    expect(text.find("Raw action payloads, clipboard contents and journal entries are not part") !=
               std::string::npos,
           "privacy contract note missing");
    std::filesystem::remove(path, error);
}

void explicit_sensitive_opt_in() {
    const auto path = std::filesystem::temp_directory_path() / "axiom_support_bundle_optin.txt";
    std::error_code error;
    std::filesystem::remove(path, error);

    std::vector<axiom::SupportField> fields{
        {"local-data-root", "C:/Users/example/AppData/Local/Axiom\nInjected=bad", true},
    };
    std::vector<axiom::SupportDiagnostic> diagnostics{
        {9, 654321, "warning", "plugin", "sample", "load failed\tpath hidden"},
    };
    axiom::SupportBundleOptions options;
    options.include_sensitive_fields = true;
    options.include_diagnostic_messages = true;
    options.max_diagnostics = 10;

    const auto report = axiom::SupportBundle::create(path, fields, diagnostics, options);
    const auto text = read(path);
    expect(report.sensitive_fields_omitted == 0, "explicit opt-in should include sensitive field");
    expect(report.diagnostic_messages_omitted == 0, "explicit opt-in should include messages");
    expect(text.find("local-data-root=C:/Users/example/AppData/Local/Axiom Injected=bad") !=
               std::string::npos,
           "embedded newline must be neutralized");
    expect(text.find("message=load failed path hidden") != std::string::npos,
           "embedded tab must be neutralized");
    std::filesystem::remove(path, error);
}

void invalid_keys_fail_closed() {
    const auto path = std::filesystem::temp_directory_path() / "axiom_support_bundle_invalid.txt";
    std::vector<axiom::SupportField> fields{{"bad key", "x", false}};
    bool rejected = false;
    try {
        (void)axiom::SupportBundle::create(path, fields, {});
    } catch (const std::exception &) {
        rejected = true;
    }
    expect(rejected, "invalid metadata keys must fail closed");
}

void duplicate_keys_fail_closed() {
    const auto path = std::filesystem::temp_directory_path() / "axiom_support_bundle_duplicate.txt";
    std::vector<axiom::SupportField> fields{
        {"watcher-health", "healthy", false},
        {"watcher-health", "degraded", false},
    };
    bool rejected = false;
    try {
        (void)axiom::SupportBundle::create(path, fields, {});
    } catch (const std::exception &) {
        rejected = true;
    }
    expect(rejected, "duplicate metadata keys must fail closed");
}

void successful_publish_leaves_no_temporary_artifact() {
    const auto root = std::filesystem::temp_directory_path() / "axiom_support_bundle_atomic";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    const auto path = root / "bundle.txt";
    const std::vector<axiom::SupportField> fields{{"product-version", "0.16.0", false}};
    (void)axiom::SupportBundle::create(path, fields, {});

    std::size_t temporaries = 0;
    for (const auto &entry : std::filesystem::directory_iterator{root}) {
        if (entry.path().filename().wstring().starts_with(L"bundle.txt.tmp."))
            ++temporaries;
    }
    expect(temporaries == 0,
           "successful support bundle publish must not leave temporary artifacts");
    std::filesystem::remove_all(root, error);
}

void failed_publish_cleans_temporary_artifact() {
    const auto root =
        std::filesystem::temp_directory_path() / "axiom_support_bundle_failed_publish";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "bundle.txt");
    const auto path = root / "bundle.txt";
    const std::vector<axiom::SupportField> fields{{"product-version", "0.16.0", false}};

    bool rejected = false;
    try {
        (void)axiom::SupportBundle::create(path, fields, {});
    } catch (const std::exception &) {
        rejected = true;
    }
    expect(rejected, "publication over an existing directory must fail");
    std::size_t temporaries = 0;
    for (const auto &entry : std::filesystem::directory_iterator{root}) {
        if (entry.path().filename().wstring().starts_with(L"bundle.txt.tmp."))
            ++temporaries;
    }
    expect(temporaries == 0, "failed support bundle publish must clean its temporary artifact");
    std::filesystem::remove_all(root, error);
}

void bounded_diagnostics_and_owner_only_publish() {
    const auto path = std::filesystem::temp_directory_path() / "axiom_support_bundle_bounds.txt";
    std::error_code error;
    std::filesystem::remove(path, error);

    std::vector<axiom::SupportDiagnostic> diagnostics;
    for (std::uint64_t i = 0; i < 300; ++i) {
        diagnostics.push_back(
            {i + 1, static_cast<std::int64_t>(i), "info", "runtime", "executor", "message"});
    }
    axiom::SupportBundleOptions options;
    options.max_diagnostics = 9999;
    const auto report = axiom::SupportBundle::create(path, {}, diagnostics, options);
    expect(report.diagnostics_written == 256,
           "support bundle must clamp diagnostic count to the hard limit");
#ifndef _WIN32
    struct stat status{};
    expect(::stat(path.c_str(), &status) == 0, "support bundle output stat failed");
    expect((status.st_mode & 0777) == 0600,
           "support bundle output must remain owner-only on POSIX");
#endif
    std::filesystem::remove(path, error);
}

} // namespace

int main() {
    try {
        privacy_defaults();
        explicit_sensitive_opt_in();
        invalid_keys_fail_closed();
        duplicate_keys_fail_closed();
        successful_publish_leaves_no_temporary_artifact();
        failed_publish_cleans_temporary_artifact();
        bounded_diagnostics_and_owner_only_publish();
        std::cout << "support_bundle_tests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "support_bundle_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
