#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef AXIOM_SOURCE_DIR
#error AXIOM_SOURCE_DIR must be defined.
#endif

namespace {
std::string read_all(const char *relative) {
    std::ifstream stream{std::string{AXIOM_SOURCE_DIR} + "/" + relative, std::ios::binary};
    if (!stream)
        throw std::runtime_error{"unable to read integration source"};
    return {std::istreambuf_iterator<char>{stream}, {}};
}
void expect_contains(std::string_view text, std::string_view needle, const char *message) {
    if (text.find(needle) == std::string_view::npos)
        throw std::runtime_error{message};
}
} // namespace
int main() {
    try {
        const auto header = read_all("src/filesystem_watcher.hpp");
        const auto source = read_all("src/filesystem_watcher.cpp");
        expect_contains(header, "enum class WatcherIssueKind", "watcher issue kind missing");
        expect_contains(header, "using IssueCallback", "watcher issue callback missing");
        expect_contains(source, "bytes == 0", "zero-byte overflow path missing");
        expect_contains(source, "ERROR_NOTIFY_ENUM_DIR", "enumeration-loss path missing");
        expect_contains(source, "WatcherIssueKind::overflow", "overflow callback missing");
        expect_contains(source, "WatcherIssueKind::error", "error callback missing");
        expect_contains(source, "ERROR_OPERATION_ABORTED", "cooperative shutdown path missing");
        std::cout << "watcher_win32_contract_tests: PASS\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "watcher_win32_contract_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
