#include "sha256.hpp"
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error{message};
}
std::span<const std::byte> bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}
} // namespace
int main() {
    try {
        require(axiom::sha256_hex(bytes("")) ==
                    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                "empty vector mismatch");
        require(axiom::sha256_hex(bytes("abc")) ==
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "abc vector mismatch");
        require(axiom::is_sha256_hex(std::string(64, 'a')), "valid digest rejected");
        require(!axiom::is_sha256_hex(std::string(63, 'a')), "short digest accepted");
        require(!axiom::is_sha256_hex(std::string(64, 'G')), "non-hex digest accepted");
        const auto root = std::filesystem::temp_directory_path() / "axiom-sha256-tests";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root);
        const auto file = root / "abc.bin";
        std::ofstream{file, std::ios::binary} << "abc";
        require(axiom::sha256_file_hex(file) ==
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "file digest mismatch");
        std::filesystem::remove_all(root, ec);
        std::cout << "sha256 tests passed\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
