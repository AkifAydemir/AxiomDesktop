#include "release_verification.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace axiom {
namespace {

constexpr std::size_t max_product_version_bytes = 32;
constexpr std::size_t max_timestamp_bytes = 64;
constexpr std::size_t max_toolchain_bytes = 256;
constexpr std::size_t max_evidence_bytes = 2048;

constexpr std::array<ReleaseVerificationGateDescriptor, 8> gate_descriptors{{
    {"msvc-release-build", "MSVC / Windows SDK Release build",
     "Configure with Visual Studio 17 2022 x64, build Release, then run CTest -C Release with "
     "--output-on-failure.",
     "Release configure/build succeeds with MSVC and Windows SDK; all registered Release tests "
     "pass."},
    {"axiom-lifecycle", "Axiom.exe lifecycle",
     "Launch Axiom.exe, exercise single-instance activation, Alt+Space palette, tray hide/show, "
     "and clean exit/restart.",
     "No startup error, duplicate live instance, stuck tray state, or shutdown/restart failure is "
     "observed."},
    {"sample-plugin-lifecycle", "Sample plugin DLL load/unload",
     "Build AxiomHelloPlugin.dll, install/enable the sample package, invoke hello/hello-action, "
     "then disable/reload/remove while callbacks are idle.",
     "Manifest/hash/capability checks pass, callbacks execute, and unload/reload/remove complete "
     "without stale registry visibility or crash."},
    {"current-user-dpapi-roundtrip", "CurrentUser DPAPI round-trip",
     "Under the same Windows user, exercise protected live journal plus protected "
     "backup/inspect/restore across an application restart.",
     "Protected data decrypts after restart for the same CurrentUser, payload round-trips "
     "losslessly, and incompatible/corrupt candidates fail closed."},
    {"watcher-overflow-resync", "ReadDirectoryChangesW loss/error recovery",
     "Exercise real ReadDirectoryChangesW notification loss/overflow and a generic watcher error, "
     "then observe restart/resync state and exact-generation full scan completion.",
     "Overflow requires full resync; generic error requires successful watcher restart plus full "
     "resync; stale generations cannot clear newer health state."},
    {"wm-timechange-dst", "WM_TIMECHANGE / local DST lifecycle",
     "With local-calendar automations armed, exercise Windows local time/time-zone change and DST "
     "gap/ambiguity scenarios while the app is running and across restart.",
     "WM_TIMECHANGE triggers rebase; spring-forward gaps are skipped; fall-back ambiguity resolves "
     "once; active retry timer semantics remain intact."},
    {"axrt1-restart-recovery", "AXRT1 interrupted restore / restart recovery",
     "Interrupt a multi-file restore at controlled pre-commit and committed-cleanup phases, "
     "restart Axiom, and inspect startup recovery before settings/stores load.",
     "Uncommitted work rolls back, committed work is preserved, forged/suspicious artifacts fail "
     "closed, and recovery finishes before store reads."},
    {"support-bundle-win32-publish", "SupportBundle Win32 publication",
     "Create support bundles on Windows with default redaction and explicit opt-ins while "
     "observing CreateFileW/FlushFileBuffers/MoveFileExW publication behavior.",
     "Unique exclusive temporary creation, durable flush, atomic replacement, cleanup-on-failure "
     "and privacy/redaction contracts hold on the real Windows filesystem."},
}};

[[nodiscard]] std::string clean_single_line(std::string value, std::size_t max_bytes) {
    if (value.size() > max_bytes) {
        throw std::invalid_argument{"Release verification evidence field exceeds its byte limit."};
    }
    for (char &ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        } else if (byte < 0x20u || byte == 0x7fu) {
            ch = '?';
        }
    }
    return value;
}

[[nodiscard]] ReleaseVerificationState parse_state(std::string_view value) {
    if (value == "unverified")
        return ReleaseVerificationState::unverified;
    if (value == "pass")
        return ReleaseVerificationState::pass;
    if (value == "fail")
        return ReleaseVerificationState::fail;
    throw std::runtime_error{"Release verification state is invalid."};
}

[[nodiscard]] bool known_gate(std::string_view id) noexcept {
    return std::ranges::any_of(gate_descriptors,
                               [id](const auto &descriptor) { return descriptor.id == id; });
}

[[nodiscard]] ReleaseVerificationGateEvidence &gate_by_id(ReleaseVerificationReport &report,
                                                          std::string_view gate_id) {
    const auto it = std::ranges::find_if(
        report.gates, [gate_id](const auto &gate) { return gate.id == gate_id; });
    if (it == report.gates.end()) {
        throw std::invalid_argument{"Unknown release verification gate."};
    }
    return *it;
}

[[nodiscard]] std::string value_after(std::string_view line, std::string_view prefix) {
    if (!line.starts_with(prefix)) {
        throw std::runtime_error{"Release verification report structure is invalid."};
    }
    return std::string{line.substr(prefix.size())};
}

[[nodiscard]] std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto end = text.find('\n', offset);
        const auto count = end == std::string_view::npos ? text.size() - offset : end - offset;
        auto line = text.substr(offset, count);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        lines.push_back(line);
        if (end == std::string_view::npos)
            break;
        offset = end + 1;
    }
    return lines;
}

std::atomic<std::uint64_t> temporary_sequence{1};

[[nodiscard]] std::filesystem::path temporary_path_for(const std::filesystem::path &path) {
    const auto ticks =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    auto temporary = path;
    temporary += L".tmp." + std::to_wstring(ticks) + L"." +
                 std::to_wstring(temporary_sequence.fetch_add(1, std::memory_order_relaxed));
    return temporary;
}

void write_file_exclusive(const std::filesystem::path &path, std::string_view bytes) {
#ifdef _WIN32
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream)
        throw std::runtime_error{"Unable to create release verification temporary file."};
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream)
        throw std::runtime_error{"Unable to write release verification temporary file."};
#else
    const int flags = O_WRONLY | O_CREAT | O_EXCL
#ifdef O_CLOEXEC
                      | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                      | O_NOFOLLOW
#endif
        ;
    const int fd = ::open(path.c_str(), flags, 0600);
    if (fd < 0) {
        throw std::system_error{errno, std::generic_category(),
                                "Unable to create release verification temporary file"};
    }
    std::size_t offset = 0;
    int failure = 0;
    while (offset < bytes.size()) {
        const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            failure = errno;
            break;
        }
        if (written == 0) {
            failure = EIO;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (failure == 0 && ::fsync(fd) != 0)
        failure = errno;
    if (::close(fd) != 0 && failure == 0)
        failure = errno;
    if (failure != 0) {
        ::unlink(path.c_str());
        throw std::system_error{failure, std::generic_category(),
                                "Unable to write release verification temporary file"};
    }
#endif
}

} // namespace

std::span<const ReleaseVerificationGateDescriptor> ReleaseVerification::checklist() noexcept {
    return gate_descriptors;
}

ReleaseVerificationReport ReleaseVerification::new_report() {
    ReleaseVerificationReport report;
    for (std::size_t index = 0; index < gate_descriptors.size(); ++index) {
        report.gates[index].id = std::string{gate_descriptors[index].id};
    }
    return report;
}

bool ReleaseVerification::all_passed(const ReleaseVerificationReport &report) noexcept {
    return std::ranges::all_of(report.gates, [](const auto &gate) {
        return gate.state == ReleaseVerificationState::pass;
    });
}

std::size_t ReleaseVerification::passed_count(const ReleaseVerificationReport &report) noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(report.gates, [](const auto &gate) {
        return gate.state == ReleaseVerificationState::pass;
    }));
}

std::string ReleaseVerification::serialize(const ReleaseVerificationReport &report) {
    if (report.product_version.empty() ||
        report.product_version.size() > max_product_version_bytes) {
        throw std::invalid_argument{"Release verification product version is invalid."};
    }
    std::unordered_set<std::string> seen;
    for (const auto &gate : report.gates) {
        if (!known_gate(gate.id) || !seen.emplace(gate.id).second) {
            throw std::invalid_argument{"Release verification report gate set is invalid."};
        }
    }
    if (seen.size() != gate_descriptors.size()) {
        throw std::invalid_argument{"Release verification report gate set is incomplete."};
    }

    std::string output;
    output.reserve(8192);
    output += "AXIOM-WINDOWS-VERIFICATION v1\n";
    output +=
        "product-version=" + clean_single_line(report.product_version, max_product_version_bytes) +
        "\n";
    output += "all-gates-pass=";
    output += all_passed(report) ? "1\n" : "0\n";
    output += "gate-count=8\n";

    for (const auto &descriptor : gate_descriptors) {
        const auto it = std::ranges::find_if(
            report.gates, [&descriptor](const auto &gate) { return gate.id == descriptor.id; });
        output += "\n[gate]\n";
        output += "id=" + it->id + "\n";
        output += "state=" + std::string{to_string(it->state)} + "\n";
        output +=
            "observed-at-utc=" + clean_single_line(it->observed_at_utc, max_timestamp_bytes) + "\n";
        output += "toolchain=" + clean_single_line(it->toolchain, max_toolchain_bytes) + "\n";
        output += "evidence=" + clean_single_line(it->evidence, max_evidence_bytes) + "\n";
        output += "procedure=" +
                  clean_single_line(std::string{descriptor.procedure}, max_evidence_bytes) + "\n";
        output += "pass-criteria=" +
                  clean_single_line(std::string{descriptor.pass_criteria}, max_evidence_bytes) +
                  "\n";
    }
    return output;
}

ReleaseVerificationReport ReleaseVerification::parse(std::string_view text) {
    const auto lines = split_lines(text);
    if (lines.size() < 4 || lines[0] != "AXIOM-WINDOWS-VERIFICATION v1") {
        throw std::runtime_error{"Unsupported release verification report."};
    }
    ReleaseVerificationReport report = new_report();
    report.product_version = value_after(lines[1], "product-version=");
    (void)value_after(lines[2], "all-gates-pass=");
    if (value_after(lines[3], "gate-count=") != "8") {
        throw std::runtime_error{"Release verification gate count is invalid."};
    }

    std::unordered_set<std::string> seen;
    std::size_t cursor = 4;
    while (cursor < lines.size()) {
        while (cursor < lines.size() && lines[cursor].empty())
            ++cursor;
        if (cursor == lines.size())
            break;
        if (lines[cursor++] != "[gate]" || cursor + 7 > lines.size()) {
            throw std::runtime_error{"Release verification gate block is truncated."};
        }
        const auto id = value_after(lines[cursor++], "id=");
        const auto state = parse_state(value_after(lines[cursor++], "state="));
        auto observed = value_after(lines[cursor++], "observed-at-utc=");
        auto toolchain = value_after(lines[cursor++], "toolchain=");
        auto evidence = value_after(lines[cursor++], "evidence=");
        (void)value_after(lines[cursor++], "procedure=");
        (void)value_after(lines[cursor++], "pass-criteria=");
        if (!known_gate(id) || !seen.emplace(id).second) {
            throw std::runtime_error{
                "Release verification report contains an unknown or duplicate gate."};
        }
        set_gate(report, id, state, std::move(observed), std::move(toolchain), std::move(evidence));
    }
    if (seen.size() != gate_descriptors.size()) {
        throw std::runtime_error{"Release verification report is missing one or more gates."};
    }
    return report;
}

void ReleaseVerification::set_gate(ReleaseVerificationReport &report, std::string_view gate_id,
                                   ReleaseVerificationState state, std::string observed_at_utc,
                                   std::string toolchain, std::string evidence) {
    auto &gate = gate_by_id(report, gate_id);
    if (state == ReleaseVerificationState::unverified) {
        gate.state = state;
        gate.observed_at_utc.clear();
        gate.toolchain.clear();
        gate.evidence.clear();
        return;
    }
    observed_at_utc = clean_single_line(std::move(observed_at_utc), max_timestamp_bytes);
    toolchain = clean_single_line(std::move(toolchain), max_toolchain_bytes);
    evidence = clean_single_line(std::move(evidence), max_evidence_bytes);
    if (observed_at_utc.empty() || toolchain.empty() || evidence.empty()) {
        throw std::invalid_argument{"Pass/fail release verification evidence requires timestamp, "
                                    "toolchain and evidence text."};
    }
    gate.state = state;
    gate.observed_at_utc = std::move(observed_at_utc);
    gate.toolchain = std::move(toolchain);
    gate.evidence = std::move(evidence);
}

void ReleaseVerification::save_atomic(const std::filesystem::path &path,
                                      const ReleaseVerificationReport &report) {
    if (path.empty())
        throw std::invalid_argument{"Release verification report path must not be empty."};
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error)
            throw std::system_error{error, "Unable to create release verification directory"};
    }
    const auto temporary = temporary_path_for(path);
    try {
        write_file_exclusive(temporary, serialize(report));
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporary, path, error);
            if (error)
                throw std::system_error{error, "Unable to publish release verification report"};
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

ReleaseVerificationReport ReleaseVerification::load(const std::filesystem::path &path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream)
        throw std::runtime_error{"Unable to open release verification report."};
    const std::string text{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
    return parse(text);
}

std::string_view to_string(ReleaseVerificationState state) noexcept {
    switch (state) {
    case ReleaseVerificationState::unverified:
        return "unverified";
    case ReleaseVerificationState::pass:
        return "pass";
    case ReleaseVerificationState::fail:
        return "fail";
    }
    return "unverified";
}

bool release_verification_recording_supported() noexcept {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

} // namespace axiom
