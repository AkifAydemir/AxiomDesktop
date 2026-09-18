#include "local_data_archive.hpp"
#include "restore_transaction.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dpapi.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace axiom {
namespace {
constexpr std::array<std::byte, 8> inner_magic{std::byte{'A'}, std::byte{'X'}, std::byte{'B'},
                                               std::byte{'K'}, std::byte{'1'}, std::byte{0},
                                               std::byte{0},   std::byte{0}};
constexpr std::array<std::byte, 8> protected_magic{std::byte{'A'}, std::byte{'X'}, std::byte{'B'},
                                                   std::byte{'P'}, std::byte{'1'}, std::byte{0},
                                                   std::byte{0},   std::byte{0}};
constexpr std::uint32_t format_version = 1;
constexpr std::uint64_t max_archive_bytes = 512ull * 1024ull * 1024ull;
constexpr std::uint64_t max_entry_bytes = 256ull * 1024ull * 1024ull;
constexpr std::size_t max_entries = 32;
constexpr std::size_t max_name_bytes = 96;
template <class T, bool = std::is_enum_v<T>> struct raw_type {
    using type = T;
};
template <class T> struct raw_type<T, true> {
    using type = std::underlying_type_t<T>;
};
template <class T> using raw_t = typename raw_type<T>::type;
template <class T> void append_le(std::vector<std::byte> &output, T value) {
    using U = std::make_unsigned_t<raw_t<T>>;
    const auto unsigned_value = static_cast<U>(value);
    for (std::size_t index = 0; index < sizeof(U); ++index) {
        output.push_back(static_cast<std::byte>((unsigned_value >> (index * 8)) & 0xFFu));
    }
}
template <class T> T read_le(std::span<const std::byte> data, std::size_t &offset) {
    using U = std::make_unsigned_t<raw_t<T>>;
    if (offset + sizeof(U) > data.size()) {
        throw std::runtime_error{"Backup archive is truncated."};
    }
    U value{};
    for (std::size_t index = 0; index < sizeof(U); ++index) {
        value |= static_cast<U>(std::to_integer<unsigned char>(data[offset++])) << (index * 8);
    }
    return static_cast<T>(value);
}
std::uint32_t crc32(std::span<const std::byte> data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto byte : data) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}
std::vector<std::byte> read_file(const std::filesystem::path &path,
                                 std::uint64_t max_size = max_entry_bytes) {
    std::ifstream stream{path, std::ios::binary | std::ios::ate};
    if (!stream) {
        throw std::runtime_error{"Unable to open local data file: " + path.string()};
    }
    const auto end = stream.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > max_size) {
        throw std::runtime_error{"Local data file exceeds size limit."};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char *>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) {
        throw std::runtime_error{"Unable to read local data file."};
    }
    return bytes;
}
void sync_file(const std::filesystem::path &path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "backup sync open failed");
    }
    const BOOL flushed = FlushFileBuffers(handle);
    const auto error = GetLastError();
    CloseHandle(handle);
    if (!flushed) {
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "backup sync failed");
    }
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "backup sync open failed");
    }
    const int result = ::fsync(fd);
    const int error = errno;
    ::close(fd);
    if (result != 0) {
        throw std::system_error(error, std::generic_category(), "backup sync failed");
    }
#endif
}
void write_atomic(const std::filesystem::path &path, std::span<const std::byte> bytes) {
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            throw std::system_error(error, "Unable to create backup directory");
    }
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
        if (!stream)
            throw std::runtime_error{"Unable to create temporary backup."};
        if (!bytes.empty()) {
            stream.write(reinterpret_cast<const char *>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        }
        stream.flush();
        if (!stream)
            throw std::runtime_error{"Unable to write backup."};
    }
    sync_file(temporary);
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            throw std::system_error(error, "Unable to publish backup");
        }
    }
}
bool valid_name(std::string_view name) {
    return !name.empty() && name.size() <= max_name_bytes &&
           name.find('/') == std::string_view::npos && name.find('\\') == std::string_view::npos &&
           name != "." && name != "..";
}
std::vector<std::byte> make_inner(std::span<const LocalDataFile> files, BackupInfo &info,
                                  std::span<const LocalDataBlob> overrides) {
    if (files.size() > max_entries) {
        throw std::runtime_error{"Too many local data files in backup."};
    }
    std::unordered_map<std::string, const LocalDataBlob *> override_by_name;
    for (const auto &override_entry : overrides) {
        if (!valid_name(override_entry.name)) {
            throw std::runtime_error{"Unsafe backup override entry name."};
        }
        if (override_entry.data.size() > max_entry_bytes) {
            throw std::runtime_error{"Backup override entry exceeds size limit."};
        }
        if (!override_by_name.emplace(override_entry.name, &override_entry).second) {
            throw std::runtime_error{"Duplicate backup override entry name."};
        }
    }
    std::unordered_set<std::string> seen_names;
    std::vector<std::byte> output;
    output.insert(output.end(), inner_magic.begin(), inner_magic.end());
    append_le(output, format_version);
    append_le(output, static_cast<std::uint32_t>(files.size()));
    for (const auto &file : files) {
        if (!valid_name(file.name))
            throw std::runtime_error{"Unsafe backup entry name."};
        if (!seen_names.insert(file.name).second) {
            throw std::runtime_error{"Duplicate backup entry name."};
        }
        std::vector<std::byte> bytes;
        if (const auto override_it = override_by_name.find(file.name);
            override_it != override_by_name.end()) {
            bytes = override_it->second->data;
            override_by_name.erase(override_it);
        } else {
            std::error_code error;
            if (!std::filesystem::exists(file.path, error) || error) {
                continue;
            }
            bytes = read_file(file.path);
        }
        const auto checksum = crc32(bytes);
        append_le(output, static_cast<std::uint16_t>(file.name.size()));
        append_le(output, std::uint16_t{0});
        append_le(output, static_cast<std::uint64_t>(bytes.size()));
        append_le(output, checksum);
        append_le(output, std::uint32_t{0});
        for (const unsigned char ch : file.name) {
            output.push_back(static_cast<std::byte>(ch));
        }
        output.insert(output.end(), bytes.begin(), bytes.end());
        info.entries.push_back({file.name, bytes.size(), checksum});
        if (output.size() > max_archive_bytes) {
            throw std::runtime_error{"Backup exceeds size limit."};
        }
    }
    if (!override_by_name.empty()) {
        throw std::runtime_error{"Backup override name is not present in the requested file list."};
    }
    // The requested list may contain optional stores that do not exist yet.
    // Patch the serialized count to the actual number of included entries.
    const auto count = static_cast<std::uint32_t>(info.entries.size());
    for (std::size_t index = 0; index < 4; ++index) {
        output[12 + index] = static_cast<std::byte>((count >> (index * 8)) & 0xFFu);
    }
    return output;
}
struct ParsedArchive {
    BackupInfo info;
    std::vector<std::pair<std::string, std::vector<std::byte>>> data;
};
ParsedArchive parse_inner(std::span<const std::byte> bytes) {
    if (bytes.size() < 16 || !std::equal(inner_magic.begin(), inner_magic.end(), bytes.begin())) {
        throw std::runtime_error{"Backup archive magic mismatch."};
    }
    std::size_t offset = inner_magic.size();
    if (read_le<std::uint32_t>(bytes, offset) != format_version) {
        throw std::runtime_error{"Unsupported backup version."};
    }
    const auto count = read_le<std::uint32_t>(bytes, offset);
    if (count > max_entries) {
        throw std::runtime_error{"Backup entry count exceeds limit."};
    }
    ParsedArchive parsed;
    std::unordered_set<std::string> seen_names;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto name_length = read_le<std::uint16_t>(bytes, offset);
        (void)read_le<std::uint16_t>(bytes, offset);
        const auto size = read_le<std::uint64_t>(bytes, offset);
        const auto expected_crc = read_le<std::uint32_t>(bytes, offset);
        (void)read_le<std::uint32_t>(bytes, offset);
        if (name_length == 0 || name_length > max_name_bytes || size > max_entry_bytes ||
            offset + name_length + size > bytes.size()) {
            throw std::runtime_error{"Backup entry bounds are invalid."};
        }
        std::string name;
        name.reserve(name_length);
        for (std::uint16_t name_index = 0; name_index < name_length; ++name_index) {
            name.push_back(static_cast<char>(std::to_integer<unsigned char>(bytes[offset++])));
        }
        if (!valid_name(name)) {
            throw std::runtime_error{"Backup contains unsafe entry name."};
        }
        if (!seen_names.insert(name).second) {
            throw std::runtime_error{"Backup contains duplicate entry names."};
        }
        std::vector<std::byte> data(static_cast<std::size_t>(size));
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    static_cast<std::ptrdiff_t>(size), data.begin());
        offset += static_cast<std::size_t>(size);
        const auto actual_crc = crc32(data);
        if (actual_crc != expected_crc) {
            throw std::runtime_error{"Backup entry CRC mismatch."};
        }
        parsed.info.entries.push_back({name, size, actual_crc});
        parsed.data.emplace_back(std::move(name), std::move(data));
    }
    if (offset != bytes.size()) {
        throw std::runtime_error{"Backup contains trailing bytes."};
    }
    return parsed;
}
std::vector<std::byte> load_archive_bytes(const std::filesystem::path &path,
                                          const DataProtector *protector, bool &protected_flag) {
    auto raw = read_file(path, max_archive_bytes);
    if (raw.size() >= 24 &&
        std::equal(protected_magic.begin(), protected_magic.end(), raw.begin())) {
        protected_flag = true;
        if (protector == nullptr || !protector->available()) {
            throw std::runtime_error{
                "Backup is OS-protected but no compatible protector is available."};
        }
        std::size_t offset = protected_magic.size();
        if (read_le<std::uint32_t>(raw, offset) != format_version) {
            throw std::runtime_error{"Unsupported protected backup version."};
        }
        (void)read_le<std::uint32_t>(raw, offset);
        const auto size = read_le<std::uint64_t>(raw, offset);
        if (size > max_archive_bytes || offset + size != raw.size()) {
            throw std::runtime_error{"Protected backup bounds are invalid."};
        }
        return protector->unprotect(
            std::span<const std::byte>{raw}.subspan(offset, static_cast<std::size_t>(size)));
    }
    protected_flag = false;
    return raw;
}
} // namespace
DpapiDataProtector::DpapiDataProtector(std::wstring purpose) : purpose_{std::move(purpose)} {
    if (purpose_.empty()) {
        purpose_ = L"LocalData";
    }
}
bool DpapiDataProtector::available() const noexcept {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}
std::wstring DpapiDataProtector::name() const {
    return available() ? L"Windows DPAPI (CurrentUser)" : L"unavailable";
}
std::vector<std::byte> DpapiDataProtector::protect(std::span<const std::byte> plaintext) const {
#ifdef _WIN32
    if (plaintext.size() > std::numeric_limits<DWORD>::max()) {
        throw std::runtime_error{"DPAPI input exceeds DWORD size limit."};
    }
    DATA_BLOB input{static_cast<DWORD>(plaintext.size()),
                    reinterpret_cast<BYTE *>(const_cast<std::byte *>(plaintext.data()))};
    DATA_BLOB output{};
    const std::wstring entropy_text = L"AxiomDesktop." + purpose_ + L".v1";
    const auto entropy_bytes = (entropy_text.size() + 1u) * sizeof(wchar_t);
    if (entropy_bytes > std::numeric_limits<DWORD>::max()) {
        throw std::runtime_error{"DPAPI entropy exceeds DWORD size limit."};
    }
    DATA_BLOB entropy{static_cast<DWORD>(entropy_bytes),
                      reinterpret_cast<BYTE *>(const_cast<wchar_t *>(entropy_text.c_str()))};
    const std::wstring description = L"Axiom Desktop " + purpose_;
    if (!CryptProtectData(&input, description.c_str(), &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "CryptProtectData failed");
    }
    std::vector<std::byte> result(output.cbData);
    std::copy_n(reinterpret_cast<std::byte *>(output.pbData), output.cbData, result.begin());
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
#else
    (void)plaintext;
    throw std::runtime_error{"Windows DPAPI is unavailable on this platform."};
#endif
}
std::vector<std::byte> DpapiDataProtector::unprotect(std::span<const std::byte> ciphertext) const {
#ifdef _WIN32
    if (ciphertext.size() > std::numeric_limits<DWORD>::max()) {
        throw std::runtime_error{"DPAPI input exceeds DWORD size limit."};
    }
    DATA_BLOB input{static_cast<DWORD>(ciphertext.size()),
                    reinterpret_cast<BYTE *>(const_cast<std::byte *>(ciphertext.data()))};
    DATA_BLOB output{};
    const std::wstring entropy_text = L"AxiomDesktop." + purpose_ + L".v1";
    const auto entropy_bytes = (entropy_text.size() + 1u) * sizeof(wchar_t);
    if (entropy_bytes > std::numeric_limits<DWORD>::max()) {
        throw std::runtime_error{"DPAPI entropy exceeds DWORD size limit."};
    }
    DATA_BLOB entropy{static_cast<DWORD>(entropy_bytes),
                      reinterpret_cast<BYTE *>(const_cast<wchar_t *>(entropy_text.c_str()))};
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &output)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "CryptUnprotectData failed");
    }
    std::vector<std::byte> result(output.cbData);
    std::copy_n(reinterpret_cast<std::byte *>(output.pbData), output.cbData, result.begin());
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
#else
    (void)ciphertext;
    throw std::runtime_error{"Windows DPAPI is unavailable on this platform."};
#endif
}
BackupInfo LocalDataArchive::create(const std::filesystem::path &archive_path,
                                    std::span<const LocalDataFile> files,
                                    const DataProtector *protector,
                                    std::span<const LocalDataBlob> overrides) {
    BackupInfo info;
    auto inner = make_inner(files, info, overrides);
    std::vector<std::byte> final_bytes = inner;
    if (protector != nullptr) {
        if (!protector->available()) {
            throw std::runtime_error{"Requested OS-protected backup but protector is unavailable."};
        }
        auto encrypted = protector->protect(inner);
        final_bytes.clear();
        final_bytes.insert(final_bytes.end(), protected_magic.begin(), protected_magic.end());
        append_le(final_bytes, format_version);
        append_le(final_bytes, std::uint32_t{0});
        append_le(final_bytes, static_cast<std::uint64_t>(encrypted.size()));
        final_bytes.insert(final_bytes.end(), encrypted.begin(), encrypted.end());
        info.protected_by_os = true;
    }
    write_atomic(archive_path, final_bytes);
    return info;
}
BackupInfo LocalDataArchive::inspect(const std::filesystem::path &archive_path,
                                     const DataProtector *protector) {
    bool protected_flag = false;
    auto bytes = load_archive_bytes(archive_path, protector, protected_flag);
    auto parsed = parse_inner(bytes);
    parsed.info.protected_by_os = protected_flag;
    return parsed.info;
}
BackupInfo LocalDataArchive::restore(const std::filesystem::path &archive_path,
                                     std::span<const LocalDataFile> destinations,
                                     const std::filesystem::path &recovery_journal_path,
                                     const DataProtector *protector) {
    if (recovery_journal_path.empty()) {
        throw std::invalid_argument{
            "Local-data restore requires an explicit recovery journal path."};
    }
    bool protected_flag = false;
    auto bytes = load_archive_bytes(archive_path, protector, protected_flag);
    auto parsed = parse_inner(bytes);
    std::unordered_map<std::string, std::filesystem::path> allowed;
    for (const auto &destination : destinations) {
        if (!valid_name(destination.name)) {
            throw std::runtime_error{"Unsafe restore destination name."};
        }
        if (!allowed.emplace(destination.name, destination.path).second) {
            throw std::runtime_error{"Duplicate restore destination name."};
        }
    }
    std::vector<RestoreBlobPlan> transaction;
    transaction.reserve(parsed.data.size());
    for (const auto &[name, data] : parsed.data) {
        const auto target = allowed.find(name);
        if (target == allowed.end()) {
            throw std::runtime_error{"Backup contains an entry not allowed by this Axiom build: " +
                                     name};
        }
        transaction.push_back({name, std::span<const std::byte>{data}, target->second});
    }
    if (!transaction.empty()) {
        const auto result = RestoreTransaction::commit(transaction, recovery_journal_path);
        parsed.info.recovery_cleanup_pending = result.recovery_cleanup_pending;
    }
    parsed.info.protected_by_os = protected_flag;
    return parsed.info;
}
} // namespace axiom
