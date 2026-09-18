#include "release_verification.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] axiom::ReleaseVerificationState parse_state(std::string_view value) {
    if (value == "pass")
        return axiom::ReleaseVerificationState::pass;
    if (value == "fail")
        return axiom::ReleaseVerificationState::fail;
    if (value == "unverified")
        return axiom::ReleaseVerificationState::unverified;
    throw std::invalid_argument{"State must be pass, fail, or unverified."};
}

void print_checklist() {
    std::cout << "Axiom Windows verification checklist\n";
    std::cout << "Product version is 0.16.0. No gate is implied PASS by this checklist.\n\n";
    for (const auto &gate : axiom::ReleaseVerification::checklist()) {
        std::cout << gate.id << " — " << gate.title << '\n';
        std::cout << "  procedure: " << gate.procedure << '\n';
        std::cout << "  pass: " << gate.pass_criteria << "\n\n";
    }
}

void print_status(const axiom::ReleaseVerificationReport &report) {
    std::cout << "Axiom Windows verification evidence\n";
    std::cout << "product-version=" << report.product_version << '\n';
    std::cout << "passed=" << axiom::ReleaseVerification::passed_count(report) << "/"
              << report.gates.size() << '\n';
    for (const auto &gate : report.gates) {
        std::cout << gate.id << '=' << axiom::to_string(gate.state) << '\n';
    }
    std::cout << "release-ready=" << (axiom::ReleaseVerification::all_passed(report) ? "yes" : "no")
              << '\n';
}

[[nodiscard]] std::string_view value_after(int &index, int argc, char **argv,
                                           std::string_view option) {
    if (index + 1 >= argc)
        throw std::invalid_argument{"Missing value after " + std::string{option}};
    return argv[++index];
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string_view{argv[1]} == "--checklist") {
            print_checklist();
            return 0;
        }
        if (argc == 3 && std::string_view{argv[1]} == "--template") {
            axiom::ReleaseVerification::save_atomic(argv[2],
                                                    axiom::ReleaseVerification::new_report());
            std::cout << "Created UNVERIFIED Windows verification template: " << argv[2] << '\n';
            return 0;
        }
        if (argc == 3 && std::string_view{argv[1]} == "--status") {
            print_status(axiom::ReleaseVerification::load(argv[2]));
            return 0;
        }
        if (argc >= 5 && std::string_view{argv[1]} == "--record") {
            if (!axiom::release_verification_recording_supported()) {
                throw std::runtime_error{
                    "PASS/FAIL evidence recording is disabled on non-Windows builds."};
            }
            const std::string report_path = argv[2];
            const std::string gate_id = argv[3];
            const auto state = parse_state(argv[4]);
            if (state == axiom::ReleaseVerificationState::unverified) {
                auto report = axiom::ReleaseVerification::load(report_path);
                axiom::ReleaseVerification::set_gate(report, gate_id, state, {}, {}, {});
                axiom::ReleaseVerification::save_atomic(report_path, report);
                print_status(report);
                return 0;
            }

            std::string observed_at;
            std::string toolchain;
            std::string evidence;
            bool attested = false;
            for (int index = 5; index < argc; ++index) {
                const std::string_view option{argv[index]};
                if (option == "--observed-at-utc") {
                    observed_at = value_after(index, argc, argv, option);
                } else if (option == "--toolchain") {
                    toolchain = value_after(index, argc, argv, option);
                } else if (option == "--evidence") {
                    evidence = value_after(index, argc, argv, option);
                } else if (option == "--attest-executed-on-windows") {
                    attested = true;
                } else {
                    throw std::invalid_argument{"Unknown record option: " + std::string{option}};
                }
            }
            if (!attested) {
                throw std::invalid_argument{
                    "PASS/FAIL recording requires --attest-executed-on-windows."};
            }
            auto report = axiom::ReleaseVerification::load(report_path);
            axiom::ReleaseVerification::set_gate(report, gate_id, state, std::move(observed_at),
                                                 std::move(toolchain), std::move(evidence));
            axiom::ReleaseVerification::save_atomic(report_path, report);
            print_status(report);
            return 0;
        }
        std::cerr << "Usage:\n"
                  << "  AxiomWindowsVerification --checklist\n"
                  << "  AxiomWindowsVerification --template <report>\n"
                  << "  AxiomWindowsVerification --status <report>\n"
                  << "  AxiomWindowsVerification --record <report> <gate> <pass|fail|unverified> "
                     "[--observed-at-utc <UTC>] [--toolchain <text>] [--evidence <text>] "
                     "[--attest-executed-on-windows]\n";
        return 2;
    } catch (const std::exception &exception) {
        std::cerr << "AxiomWindowsVerification: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
