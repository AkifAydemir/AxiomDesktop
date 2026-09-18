#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace axiom {

enum class ReleaseVerificationState {
    unverified,
    pass,
    fail,
};

struct ReleaseVerificationGateDescriptor {
    std::string_view id;
    std::string_view title;
    std::string_view procedure;
    std::string_view pass_criteria;
};

struct ReleaseVerificationGateEvidence {
    std::string id;
    ReleaseVerificationState state{ReleaseVerificationState::unverified};
    std::string observed_at_utc;
    std::string toolchain;
    std::string evidence;
};

struct ReleaseVerificationReport {
    std::string product_version{"0.16.0"};
    std::array<ReleaseVerificationGateEvidence, 8> gates{};
};

class ReleaseVerification final {
  public:
    [[nodiscard]] static std::span<const ReleaseVerificationGateDescriptor> checklist() noexcept;
    [[nodiscard]] static ReleaseVerificationReport new_report();
    [[nodiscard]] static bool all_passed(const ReleaseVerificationReport &report) noexcept;
    [[nodiscard]] static std::size_t passed_count(const ReleaseVerificationReport &report) noexcept;
    [[nodiscard]] static std::string serialize(const ReleaseVerificationReport &report);
    [[nodiscard]] static ReleaseVerificationReport parse(std::string_view text);
    static void set_gate(ReleaseVerificationReport &report, std::string_view gate_id,
                         ReleaseVerificationState state, std::string observed_at_utc,
                         std::string toolchain, std::string evidence);
    static void save_atomic(const std::filesystem::path &path,
                            const ReleaseVerificationReport &report);
    [[nodiscard]] static ReleaseVerificationReport load(const std::filesystem::path &path);
};

[[nodiscard]] std::string_view to_string(ReleaseVerificationState state) noexcept;
[[nodiscard]] bool release_verification_recording_supported() noexcept;

} // namespace axiom
