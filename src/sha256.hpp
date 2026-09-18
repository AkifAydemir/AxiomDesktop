#pragma once
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
namespace axiom {
[[nodiscard]] std::string sha256_hex(std::span<const std::byte> bytes);
[[nodiscard]] std::string sha256_file_hex(const std::filesystem::path &path);
[[nodiscard]] bool is_sha256_hex(std::string_view value) noexcept;
} // namespace axiom
