#include "release_verification.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error{message};
}

[[nodiscard]] std::filesystem::path temporary_path() {
    return std::filesystem::temp_directory_path() / "axiom-release-verification-tests.txt";
}

void run_tests() {
    const auto checklist = axiom::ReleaseVerification::checklist();
    require(checklist.size() == 8,
            "Windows verification checklist must contain exactly eight gates.");
    std::unordered_set<std::string_view> ids;
    for (const auto &gate : checklist) {
        require(!gate.id.empty(), "Gate id must not be empty.");
        require(!gate.procedure.empty(), "Gate procedure must not be empty.");
        require(!gate.pass_criteria.empty(), "Gate pass criteria must not be empty.");
        require(ids.emplace(gate.id).second, "Gate ids must be unique.");
    }

    auto report = axiom::ReleaseVerification::new_report();
    require(report.product_version == "0.16.0", "Verification product version must be 0.16.0.");
    require(axiom::ReleaseVerification::passed_count(report) == 0,
            "New verification report must start with zero PASS gates.");
    require(!axiom::ReleaseVerification::all_passed(report),
            "New verification report must not be release-ready.");

    axiom::ReleaseVerification::set_gate(
        report, "msvc-release-build", axiom::ReleaseVerificationState::pass, "2026-09-06T18:00:00Z",
        "MSVC 19.xx + Windows SDK",
        "Release configure/build and CTest passed.\nEvidence line two.");
    require(axiom::ReleaseVerification::passed_count(report) == 1,
            "Recorded PASS gate must be counted.");

    const auto serialized = axiom::ReleaseVerification::serialize(report);
    require(serialized.find("all-gates-pass=0") != std::string::npos,
            "Partial report must remain not ready.");
    require(serialized.find("Evidence line two.") != std::string::npos,
            "Evidence text must be retained.");
    require(serialized.find("Evidence line two.\n") != std::string::npos,
            "Evidence text must be normalized to one line before the report newline.");

    const auto parsed = axiom::ReleaseVerification::parse(serialized);
    require(parsed.gates[0].state == axiom::ReleaseVerificationState::pass,
            "Round-trip must preserve PASS state.");
    require(parsed.gates[0].evidence.find('\n') == std::string::npos,
            "Round-trip evidence must be single-line normalized.");

    for (const auto &gate : checklist) {
        axiom::ReleaseVerification::set_gate(report, gate.id, axiom::ReleaseVerificationState::pass,
                                             "2026-09-06T18:00:00Z", "MSVC/Windows",
                                             "Observed on Windows runtime.");
    }
    require(axiom::ReleaseVerification::all_passed(report),
            "All eight PASS gates must make the report release-ready.");
    require(axiom::ReleaseVerification::passed_count(report) == 8,
            "All eight PASS gates must be counted.");

    axiom::ReleaseVerification::set_gate(report, "wm-timechange-dst",
                                         axiom::ReleaseVerificationState::unverified, {}, {}, {});
    require(!axiom::ReleaseVerification::all_passed(report),
            "Resetting one gate must clear release readiness.");

    bool rejected = false;
    try {
        axiom::ReleaseVerification::set_gate(report, "unknown-gate",
                                             axiom::ReleaseVerificationState::fail,
                                             "2026-09-06T18:00:00Z", "Windows", "failure");
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "Unknown gate must be rejected.");

    rejected = false;
    try {
        axiom::ReleaseVerification::set_gate(report, "wm-timechange-dst",
                                             axiom::ReleaseVerificationState::fail, {}, "Windows",
                                             "failure");
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "PASS/FAIL evidence without timestamp must be rejected.");

    auto corrupted =
        axiom::ReleaseVerification::serialize(axiom::ReleaseVerification::new_report());
    const auto pos = corrupted.find("id=msvc-release-build");
    require(pos != std::string::npos, "Fixture gate must exist.");
    corrupted.replace(pos, std::string{"id=msvc-release-build"}.size(), "id=unknown-gate");
    rejected = false;
    try {
        (void)axiom::ReleaseVerification::parse(corrupted);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, "Unknown serialized gate must fail closed.");

    const auto path = temporary_path();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    axiom::ReleaseVerification::save_atomic(path, axiom::ReleaseVerification::new_report());
    const auto loaded = axiom::ReleaseVerification::load(path);
    require(loaded.gates.size() == 8, "Atomic save/load must preserve all gates.");
#ifndef _WIN32
    require(!axiom::release_verification_recording_supported(),
            "Non-Windows builds must disable PASS/FAIL evidence recording.");
#endif
    std::filesystem::remove(path, ignored);
}

} // namespace

int main() {
    try {
        run_tests();
        std::cout << "AxiomReleaseVerificationTests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "AxiomReleaseVerificationTests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
