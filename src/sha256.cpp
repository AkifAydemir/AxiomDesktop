#include "sha256.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
namespace axiom {
namespace {
constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};
constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned bits) noexcept {
    return (value >> bits) | (value << (32u - bits));
}
class Sha256Context final {
  public:
    void update(std::span<const std::byte> bytes) {
        if (bytes.empty()) {
            return;
        }
        if (total_bytes_ > (std::numeric_limits<std::uint64_t>::max() / 8u) - bytes.size()) {
            throw std::overflow_error{"SHA-256 input is too large."};
        }
        total_bytes_ += static_cast<std::uint64_t>(bytes.size());
        std::size_t offset = 0;
        if (buffer_size_ != 0) {
            const auto needed = block_size - buffer_size_;
            const auto copied = std::min(needed, bytes.size());
            std::copy_n(bytes.data(), copied, buffer_.data() + buffer_size_);
            buffer_size_ += copied;
            offset += copied;
            if (buffer_size_ == block_size) {
                process_block(buffer_.data());
                buffer_size_ = 0;
            }
        }
        while (offset + block_size <= bytes.size()) {
            process_block(bytes.data() + offset);
            offset += block_size;
        }
        if (offset < bytes.size()) {
            buffer_size_ = bytes.size() - offset;
            std::copy_n(bytes.data() + offset, buffer_size_, buffer_.data());
        }
    }
    [[nodiscard]] std::string finalize_hex() {
        const auto bit_length = total_bytes_ * 8u;
        std::array<std::byte, block_size * 2> final_bytes{};
        std::copy_n(buffer_.data(), buffer_size_, final_bytes.data());
        std::size_t final_size = buffer_size_;
        final_bytes[final_size++] = std::byte{0x80};
        const auto target_without_length = final_size <= 56 ? 56u : 120u;
        while (final_size < target_without_length) {
            final_bytes[final_size++] = std::byte{0};
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            final_bytes[final_size++] = static_cast<std::byte>((bit_length >> shift) & 0xffu);
        }
        for (std::size_t offset = 0; offset < final_size; offset += block_size) {
            process_block(final_bytes.data() + offset);
        }
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const auto value : state_) {
            output << std::setw(8) << value;
        }
        return output.str();
    }

  private:
    static constexpr std::size_t block_size = 64;
    std::array<std::uint32_t, 8> state_{
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    std::array<std::byte, block_size> buffer_{};
    std::size_t buffer_size_{};
    std::uint64_t total_bytes_{};
    void process_block(const std::byte *block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto *cursor = block + i * 4;
            words[i] =
                (static_cast<std::uint32_t>(std::to_integer<unsigned char>(cursor[0])) << 24u) |
                (static_cast<std::uint32_t>(std::to_integer<unsigned char>(cursor[1])) << 16u) |
                (static_cast<std::uint32_t>(std::to_integer<unsigned char>(cursor[2])) << 8u) |
                static_cast<std::uint32_t>(std::to_integer<unsigned char>(cursor[3]));
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const auto s0 = rotate_right(words[i - 15], 7) ^ rotate_right(words[i - 15], 18) ^
                            (words[i - 15] >> 3u);
            const auto s1 = rotate_right(words[i - 2], 17) ^ rotate_right(words[i - 2], 19) ^
                            (words[i - 2] >> 10u);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];
        auto e = state_[4];
        auto f = state_[5];
        auto g = state_[6];
        auto h = state_[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const auto sigma1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const auto choose = (e & f) ^ ((~e) & g);
            const auto temporary1 = h + sigma1 + choose + round_constants[i] + words[i];
            const auto sigma0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = sigma0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }
};
} // namespace
std::string sha256_hex(std::span<const std::byte> bytes) {
    Sha256Context context;
    context.update(bytes);
    return context.finalize_hex();
}
std::string sha256_file_hex(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error{"Unable to open file for SHA-256: " + path.string()};
    }
    Sha256Context context;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            const auto *bytes = reinterpret_cast<const std::byte *>(buffer.data());
            context.update(std::span<const std::byte>{bytes, static_cast<std::size_t>(count)});
        }
    }
    if (!input.eof()) {
        throw std::runtime_error{"Failed while reading file for SHA-256: " + path.string()};
    }
    return context.finalize_hex();
}
bool is_sha256_hex(std::string_view value) noexcept {
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}
} // namespace axiom
