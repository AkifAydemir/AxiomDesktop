#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>
namespace axiom {
class DataProtector {
  public:
    virtual ~DataProtector() = default;
    [[nodiscard]] virtual bool available() const noexcept = 0;
    [[nodiscard]] virtual std::wstring name() const = 0;
    [[nodiscard]] virtual std::vector<std::byte>
    protect(std::span<const std::byte> plaintext) const = 0;
    [[nodiscard]] virtual std::vector<std::byte>
    unprotect(std::span<const std::byte> ciphertext) const = 0;
};
class DpapiDataProtector final : public DataProtector {
  public:
    explicit DpapiDataProtector(std::wstring purpose = L"LocalData");
    [[nodiscard]] bool available() const noexcept override;
    [[nodiscard]] std::wstring name() const override;
    [[nodiscard]] std::vector<std::byte>
    protect(std::span<const std::byte> plaintext) const override;
    [[nodiscard]] std::vector<std::byte>
    unprotect(std::span<const std::byte> ciphertext) const override;

  private:
    std::wstring purpose_;
};
struct LocalDataFile {
    std::string name;
    std::filesystem::path path;
};
struct LocalDataBlob {
    std::string name;
    std::vector<std::byte> data;
};
struct BackupEntryInfo {
    std::string name;
    std::uint64_t size{};
    std::uint32_t crc32{};
};
struct BackupInfo {
    bool protected_by_os{};
    bool recovery_cleanup_pending{};
    std::vector<BackupEntryInfo> entries;
};
class LocalDataArchive final {
  public:
    static BackupInfo create(const std::filesystem::path &archive_path,
                             std::span<const LocalDataFile> files,
                             const DataProtector *protector = nullptr,
                             std::span<const LocalDataBlob> overrides = {});
    static BackupInfo inspect(const std::filesystem::path &archive_path,
                              const DataProtector *protector = nullptr);
    static BackupInfo restore(const std::filesystem::path &archive_path,
                              std::span<const LocalDataFile> destinations,
                              const std::filesystem::path &recovery_journal_path,
                              const DataProtector *protector = nullptr);
};
} // namespace axiom
