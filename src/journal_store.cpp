#include "journal_store.hpp"
#include "local_data_archive.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cwctype>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace axiom {
namespace {
constexpr std::array<std::byte, 4> magic{std::byte{'A'}, std::byte{'X'}, std::byte{'J'},
                                         std::byte{'R'}};
constexpr std::array<std::byte, 4> protected_magic{std::byte{'A'}, std::byte{'X'}, std::byte{'J'},
                                                   std::byte{'P'}};
constexpr std::uint32_t format_version = 1;
constexpr std::uint32_t protected_format_version = 1;
constexpr std::size_t header_bytes = 48;
constexpr std::size_t protected_header_bytes = 40;
constexpr std::size_t max_store_bytes = 128u * 1024u * 1024u;
constexpr std::size_t max_protected_store_bytes = 160u * 1024u * 1024u;
constexpr std::size_t max_records = 50000;
constexpr std::size_t max_text_bytes = 1024u * 1024u;
constexpr std::size_t max_source_bytes = 16u * 1024u;
constexpr std::size_t max_tag_bytes = 1024u;
constexpr std::size_t max_tags_per_entry = 32;
struct Candidate {
    JournalStoreSnapshot snapshot;
    std::filesystem::path path;
    bool protected_at_rest{};
};
struct CandidateScan {
    std::vector<Candidate> valid;
    bool any_present{};
};
[[nodiscard]] std::filesystem::path temp_path(const std::filesystem::path &path) {
    auto p = path;
    p += L".tmp";
    return p;
}
[[nodiscard]] std::filesystem::path backup_path(const std::filesystem::path &path) {
    auto p = path;
    p += L".bak";
    return p;
}
template <typename T, bool = std::is_enum_v<T>> struct raw_integer {
    using type = T;
};
template <typename T> struct raw_integer<T, true> {
    using type = std::underlying_type_t<T>;
};
template <typename T> using raw_integer_t = typename raw_integer<T>::type;
template <typename T> void append_le(std::vector<std::byte> &out, T value) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
    using Raw = raw_integer_t<T>;
    using Unsigned = std::make_unsigned_t<Raw>;
    const auto raw = static_cast<Unsigned>(value);
    for (std::size_t i = 0; i < sizeof(Unsigned); ++i) {
        out.push_back(static_cast<std::byte>((raw >> (i * 8u)) & 0xffu));
    }
}
template <typename T>
[[nodiscard]] T read_le(std::span<const std::byte> bytes, std::size_t &offset) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
    using Raw = raw_integer_t<T>;
    using Unsigned = std::make_unsigned_t<Raw>;
    if (offset > bytes.size() || bytes.size() - offset < sizeof(Unsigned)) {
        throw std::runtime_error{"Journal store is truncated."};
    }
    Unsigned value{};
    for (std::size_t i = 0; i < sizeof(Unsigned); ++i) {
        value |= static_cast<Unsigned>(std::to_integer<unsigned char>(bytes[offset + i]))
                 << (i * 8u);
    }
    offset += sizeof(Unsigned);
    return static_cast<T>(value);
}
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (const auto value : bytes) {
        crc ^= std::to_integer<std::uint8_t>(value);
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(-(static_cast<std::int32_t>(crc & 1u)));
            crc = (crc >> 1u) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}
void append_utf8_codepoint(std::string &output, std::uint32_t cp) {
    if (cp <= 0x7fu)
        output.push_back(static_cast<char>(cp));
    else if (cp <= 0x7ffu) {
        output.push_back(static_cast<char>(0xc0u | (cp >> 6u)));
        output.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    } else if (cp <= 0xffffu) {
        output.push_back(static_cast<char>(0xe0u | (cp >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    } else {
        output.push_back(static_cast<char>(0xf0u | (cp >> 18u)));
        output.push_back(static_cast<char>(0x80u | ((cp >> 12u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    }
}
[[nodiscard]] std::string wide_to_utf8(std::wstring_view input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        std::uint32_t cp = static_cast<std::uint32_t>(input[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xd800u && cp <= 0xdbffu) {
                if (++i >= input.size())
                    throw std::runtime_error{"Journal text contains an unpaired UTF-16 surrogate."};
                const auto low = static_cast<std::uint32_t>(input[i]);
                if (low < 0xdc00u || low > 0xdfffu)
                    throw std::runtime_error{"Journal text contains an unpaired UTF-16 surrogate."};
                cp = 0x10000u + ((cp - 0xd800u) << 10u) + (low - 0xdc00u);
            } else if (cp >= 0xdc00u && cp <= 0xdfffu) {
                throw std::runtime_error{"Journal text contains an unpaired UTF-16 surrogate."};
            }
        } else if (cp >= 0xd800u && cp <= 0xdfffu) {
            throw std::runtime_error{"Journal text contains a surrogate codepoint."};
        }
        append_utf8_codepoint(output, cp);
    }
    return output;
}
[[nodiscard]] std::uint32_t decode_utf8(std::string_view input, std::size_t &offset) {
    if (offset >= input.size())
        throw std::runtime_error{"Journal store contains truncated UTF-8."};
    const auto first = static_cast<unsigned char>(input[offset++]);
    if (first <= 0x7f)
        return first;
    int count{};
    std::uint32_t cp{};
    std::uint32_t minimum{};
    if ((first & 0xe0u) == 0xc0u) {
        count = 1;
        cp = first & 0x1fu;
        minimum = 0x80u;
    } else if ((first & 0xf0u) == 0xe0u) {
        count = 2;
        cp = first & 0x0fu;
        minimum = 0x800u;
    } else if ((first & 0xf8u) == 0xf0u) {
        count = 3;
        cp = first & 0x07u;
        minimum = 0x10000u;
    } else
        throw std::runtime_error{"Journal store contains invalid UTF-8."};
    for (int i = 0; i < count; ++i) {
        if (offset >= input.size())
            throw std::runtime_error{"Journal store contains truncated UTF-8."};
        const auto next = static_cast<unsigned char>(input[offset++]);
        if ((next & 0xc0u) != 0x80u)
            throw std::runtime_error{"Journal store contains invalid UTF-8."};
        cp = (cp << 6u) | (next & 0x3fu);
    }
    if (cp < minimum || cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu))
        throw std::runtime_error{"Journal store contains non-canonical UTF-8."};
    return cp;
}
[[nodiscard]] std::wstring utf8_to_wide(std::string_view input) {
    std::wstring output;
    std::size_t offset{};
    while (offset < input.size()) {
        auto cp = decode_utf8(input, offset);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp <= 0xffffu)
                output.push_back(static_cast<wchar_t>(cp));
            else {
                cp -= 0x10000u;
                output.push_back(static_cast<wchar_t>(0xd800u + (cp >> 10u)));
                output.push_back(static_cast<wchar_t>(0xdc00u + (cp & 0x3ffu)));
            }
        } else
            output.push_back(static_cast<wchar_t>(cp));
    }
    return output;
}
void append_text(std::vector<std::byte> &out, std::wstring_view text, std::size_t limit,
                 const char *field) {
    const auto utf8 = wide_to_utf8(text);
    if (utf8.size() > limit)
        throw std::runtime_error{std::string{"Journal "} + field + " exceeds storage limit."};
    append_le(out, static_cast<std::uint32_t>(utf8.size()));
    out.insert(out.end(), reinterpret_cast<const std::byte *>(utf8.data()),
               reinterpret_cast<const std::byte *>(utf8.data() + utf8.size()));
}
[[nodiscard]] std::wstring read_text(std::span<const std::byte> bytes, std::size_t &offset,
                                     std::size_t limit, const char *field) {
    const auto size = read_le<std::uint32_t>(bytes, offset);
    if (size > limit || offset > bytes.size() || bytes.size() - offset < size)
        throw std::runtime_error{std::string{"Journal store contains invalid "} + field + "."};
    const auto *data = reinterpret_cast<const char *>(bytes.data() + offset);
    auto result = utf8_to_wide(std::string_view{data, size});
    offset += size;
    return result;
}
[[nodiscard]] std::wstring trim_copy(std::wstring_view input) {
    std::size_t begin{};
    while (begin < input.size() && std::iswspace(input[begin]))
        ++begin;
    std::size_t end = input.size();
    while (end > begin && std::iswspace(input[end - 1]))
        --end;
    return std::wstring{input.substr(begin, end - begin)};
}
[[nodiscard]] std::wstring lower_copy(std::wstring_view input) {
    std::wstring value{input};
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}
[[nodiscard]] std::vector<std::byte> serialize_payload(std::span<const JournalEntry> entries) {
    std::vector<std::byte> payload;
    for (const auto &entry : entries) {
        if (entry.id == 0 || entry.text.empty())
            throw std::runtime_error{"Journal entry ID/text cannot be empty."};
        if (entry.tags.size() > max_tags_per_entry)
            throw std::runtime_error{"Journal entry has too many tags."};
        append_le(payload, entry.id);
        append_le(payload, static_cast<std::uint8_t>(entry.kind));
        append_le(payload, std::uint8_t{0});
        append_le(payload, std::uint16_t{0});
        append_le(payload,
                  static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                entry.created_at.time_since_epoch())
                                                .count()));
        append_text(payload, entry.source, max_source_bytes, "source");
        append_text(payload, entry.text, max_text_bytes, "text");
        append_le(payload, static_cast<std::uint32_t>(entry.tags.size()));
        for (const auto &tag : entry.tags)
            append_text(payload, tag, max_tag_bytes, "tag");
    }
    return payload;
}
[[nodiscard]] std::vector<std::byte> serialize_file(std::uint64_t generation,
                                                    JournalEntryId next_id,
                                                    std::span<const JournalEntry> entries) {
    if (entries.size() > max_records)
        throw std::runtime_error{"Journal store exceeds record limit."};
    auto payload = serialize_payload(entries);
    if (header_bytes + payload.size() > max_store_bytes)
        throw std::runtime_error{"Journal store exceeds 128 MiB limit."};
    std::vector<std::byte> out;
    out.reserve(header_bytes + payload.size());
    out.insert(out.end(), magic.begin(), magic.end());
    append_le(out, format_version);
    append_le(out, generation);
    append_le(out, next_id);
    append_le(out, static_cast<std::uint64_t>(entries.size()));
    append_le(out, static_cast<std::uint64_t>(payload.size()));
    append_le(out, crc32(payload));
    append_le(out, std::uint32_t{0});
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
[[nodiscard]] JournalStoreSnapshot parse_plaintext_bytes(std::span<const std::byte> bytes) {
    if (bytes.size() < header_bytes || bytes.size() > max_store_bytes) {
        throw std::runtime_error{"Journal store has invalid size."};
    }
    if (!std::equal(magic.begin(), magic.end(), bytes.begin())) {
        throw std::runtime_error{"Journal store magic mismatch."};
    }
    std::size_t offset = magic.size();
    if (read_le<std::uint32_t>(bytes, offset) != format_version) {
        throw std::runtime_error{"Unsupported journal store version."};
    }
    JournalStoreSnapshot snapshot;
    snapshot.generation = read_le<std::uint64_t>(bytes, offset);
    snapshot.next_id = read_le<JournalEntryId>(bytes, offset);
    const auto count = read_le<std::uint64_t>(bytes, offset);
    const auto payload_size = read_le<std::uint64_t>(bytes, offset);
    const auto expected_crc = read_le<std::uint32_t>(bytes, offset);
    (void)read_le<std::uint32_t>(bytes, offset);
    if (count > max_records || payload_size > max_store_bytes ||
        offset + payload_size != bytes.size()) {
        throw std::runtime_error{"Journal store header bounds are invalid."};
    }
    const auto payload = bytes.subspan(offset, static_cast<std::size_t>(payload_size));
    if (crc32(payload) != expected_crc)
        throw std::runtime_error{"Journal store CRC mismatch."};
    std::size_t po{};
    JournalEntryId max_id{};
    snapshot.entries.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        JournalEntry entry;
        entry.id = read_le<JournalEntryId>(payload, po);
        const auto kind = read_le<std::uint8_t>(payload, po);
        (void)read_le<std::uint8_t>(payload, po);
        (void)read_le<std::uint16_t>(payload, po);
        if (kind != static_cast<std::uint8_t>(JournalEntryKind::note) &&
            kind != static_cast<std::uint8_t>(JournalEntryKind::activity)) {
            throw std::runtime_error{"Journal store contains invalid entry kind."};
        }
        entry.kind = static_cast<JournalEntryKind>(kind);
        const auto created_ms = read_le<std::int64_t>(payload, po);
        entry.created_at =
            std::chrono::system_clock::time_point{std::chrono::milliseconds{created_ms}};
        entry.source = read_text(payload, po, max_source_bytes, "source");
        entry.text = read_text(payload, po, max_text_bytes, "text");
        const auto tag_count = read_le<std::uint32_t>(payload, po);
        if (tag_count > max_tags_per_entry)
            throw std::runtime_error{"Journal store contains too many tags."};
        for (std::uint32_t t = 0; t < tag_count; ++t) {
            entry.tags.push_back(read_text(payload, po, max_tag_bytes, "tag"));
        }
        if (entry.id == 0 || entry.text.empty())
            throw std::runtime_error{"Journal store contains invalid entry."};
        max_id = std::max(max_id, entry.id);
        snapshot.entries.push_back(std::move(entry));
    }
    if (po != payload.size())
        throw std::runtime_error{"Journal store contains trailing payload bytes."};
    snapshot.next_id = std::max(snapshot.next_id, max_id + 1);
    if (snapshot.next_id == 0)
        throw std::runtime_error{"Journal ID space exhausted."};
    return snapshot;
}
[[nodiscard]] std::vector<std::byte> read_store_bytes(const std::filesystem::path &path) {
    std::ifstream stream{path, std::ios::binary | std::ios::ate};
    if (!stream)
        throw std::system_error(errno, std::generic_category(), "Unable to open journal store");
    const auto end = stream.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > max_protected_store_bytes) {
        throw std::runtime_error{"Journal store has invalid size."};
    }
    const auto size = static_cast<std::size_t>(end);
    if (size < magic.size())
        throw std::runtime_error{"Journal store is too small."};
    std::vector<std::byte> bytes(size);
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
    if (!stream)
        throw std::runtime_error{"Unable to read complete journal store."};
    return bytes;
}
[[nodiscard]] std::vector<std::byte> seal_protected(std::span<const std::byte> plaintext,
                                                    const DataProtector &protector) {
    if (!protector.available())
        throw std::runtime_error{"Journal protector is unavailable."};
    auto ciphertext = protector.protect(plaintext);
    if (ciphertext.size() > max_protected_store_bytes - protected_header_bytes) {
        throw std::runtime_error{"Protected journal store exceeds size limit."};
    }
    std::vector<std::byte> out;
    out.reserve(protected_header_bytes + ciphertext.size());
    out.insert(out.end(), protected_magic.begin(), protected_magic.end());
    append_le(out, protected_format_version);
    append_le(out, std::uint32_t{0});
    append_le(out, static_cast<std::uint64_t>(plaintext.size()));
    append_le(out, static_cast<std::uint64_t>(ciphertext.size()));
    append_le(out, crc32(ciphertext));
    append_le(out, crc32(plaintext));
    append_le(out, std::uint32_t{0});
    out.insert(out.end(), ciphertext.begin(), ciphertext.end());
    return out;
}
[[nodiscard]] std::vector<std::byte> open_protected(std::span<const std::byte> bytes,
                                                    const DataProtector &protector) {
    if (!protector.available())
        throw std::runtime_error{"Journal protector is unavailable."};
    if (bytes.size() < protected_header_bytes || bytes.size() > max_protected_store_bytes) {
        throw std::runtime_error{"Protected journal envelope has invalid size."};
    }
    std::size_t offset = protected_magic.size();
    if (read_le<std::uint32_t>(bytes, offset) != protected_format_version) {
        throw std::runtime_error{"Unsupported protected journal version."};
    }
    const auto flags = read_le<std::uint32_t>(bytes, offset);
    if (flags != 0)
        throw std::runtime_error{"Protected journal contains unsupported flags."};
    const auto plaintext_size = read_le<std::uint64_t>(bytes, offset);
    const auto ciphertext_size = read_le<std::uint64_t>(bytes, offset);
    const auto ciphertext_crc = read_le<std::uint32_t>(bytes, offset);
    const auto plaintext_crc = read_le<std::uint32_t>(bytes, offset);
    (void)read_le<std::uint32_t>(bytes, offset);
    if (plaintext_size > max_store_bytes || ciphertext_size > max_protected_store_bytes ||
        offset + ciphertext_size != bytes.size()) {
        throw std::runtime_error{"Protected journal envelope bounds are invalid."};
    }
    const auto ciphertext = bytes.subspan(offset, static_cast<std::size_t>(ciphertext_size));
    if (crc32(ciphertext) != ciphertext_crc) {
        throw std::runtime_error{"Protected journal ciphertext CRC mismatch."};
    }
    auto plaintext = protector.unprotect(ciphertext);
    if (plaintext.size() != plaintext_size || crc32(plaintext) != plaintext_crc) {
        throw std::runtime_error{"Protected journal plaintext integrity check failed."};
    }
    return plaintext;
}
[[nodiscard]] Candidate parse_candidate(const std::filesystem::path &path,
                                        const DataProtector *protector) {
    const auto bytes = read_store_bytes(path);
    if (std::equal(magic.begin(), magic.end(), bytes.begin())) {
        return Candidate{parse_plaintext_bytes(bytes), path, false};
    }
    if (std::equal(protected_magic.begin(), protected_magic.end(), bytes.begin())) {
        if (protector == nullptr || !protector->available()) {
            throw std::runtime_error{"Protected journal requires an available data protector."};
        }
        const auto plaintext = open_protected(bytes, *protector);
        return Candidate{parse_plaintext_bytes(plaintext), path, true};
    }
    throw std::runtime_error{"Journal store magic mismatch."};
}
void sync_file(const std::filesystem::path &path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "CreateFileW journal sync failed");
    const BOOL ok = FlushFileBuffers(handle);
    const auto error = GetLastError();
    CloseHandle(handle);
    if (!ok)
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "FlushFileBuffers journal sync failed");
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), "open journal sync failed");
    const int result = ::fsync(fd);
    const int error = errno;
    ::close(fd);
    if (result != 0)
        throw std::system_error(error, std::generic_category(), "fsync journal failed");
#endif
}
void write_atomic(const std::filesystem::path &path, std::span<const std::byte> bytes,
                  bool rotate_main) {
    std::error_code ec;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
        throw std::system_error(ec, "Unable to create journal directory");
    const auto temp = temp_path(path);
    const auto backup = backup_path(path);
    {
        std::ofstream stream{temp, std::ios::binary | std::ios::trunc};
        if (!stream)
            throw std::runtime_error{"Unable to create journal temp file."};
        stream.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream)
            throw std::runtime_error{"Unable to write journal temp file."};
    }
    sync_file(temp);
    ec.clear();
    if (std::filesystem::exists(path, ec) && !ec) {
        if (rotate_main) {
            std::filesystem::remove(backup, ec);
            ec.clear();
            std::filesystem::rename(path, backup, ec);
            if (ec)
                throw std::system_error(ec, "Unable to rotate journal backup");
        } else {
            std::filesystem::remove(path, ec);
            if (ec)
                throw std::system_error(ec, "Unable to discard invalid journal primary");
        }
    }
    ec.clear();
    std::filesystem::rename(temp, path, ec);
    if (ec)
        throw std::system_error(ec, "Unable to publish journal store");
}
[[nodiscard]] bool has_magic(const std::filesystem::path &path,
                             const std::array<std::byte, 4> &expected) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream)
        return false;
    std::array<std::byte, 4> prefix{};
    stream.read(reinterpret_cast<char *>(prefix.data()),
                static_cast<std::streamsize>(prefix.size()));
    return stream.gcount() == static_cast<std::streamsize>(prefix.size()) && prefix == expected;
}
void remove_plaintext_alternates(const std::filesystem::path &path) {
    for (const auto &alternate : {temp_path(path), backup_path(path)}) {
        std::error_code ec;
        if (std::filesystem::exists(alternate, ec) && !ec && has_magic(alternate, magic)) {
            std::filesystem::remove(alternate, ec);
        }
    }
}
[[nodiscard]] CandidateScan scan_candidates(const std::filesystem::path &path,
                                            const DataProtector *protector) {
    CandidateScan scan;
    for (const auto &candidate : {path, temp_path(path), backup_path(path)}) {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) || ec)
            continue;
        scan.any_present = true;
        try {
            scan.valid.push_back(parse_candidate(candidate, protector));
        } catch (...) {
        }
    }
    return scan;
}
[[nodiscard]] int path_priority(const std::filesystem::path &candidate,
                                const std::filesystem::path &primary) {
    if (candidate == primary)
        return 3;
    if (candidate == temp_path(primary))
        return 2;
    return 1;
}
[[nodiscard]] int relevance(const JournalEntry &entry, std::wstring_view raw_query) {
    const auto query = lower_copy(trim_copy(raw_query));
    if (query.empty())
        return 1;
    const auto text = lower_copy(entry.text);
    const auto source = lower_copy(entry.source);
    int score = 0;
    if (text == query)
        score += 1200;
    else if (text.starts_with(query))
        score += 800;
    else if (text.find(query) != std::wstring::npos)
        score += 500;
    if (source == query)
        score += 500;
    else if (source.find(query) != std::wstring::npos)
        score += 200;
    for (const auto &tag : entry.tags) {
        const auto lower = lower_copy(tag);
        if (lower == query || (query.starts_with(L"#") && lower == query.substr(1)))
            score += 900;
        else if (lower.find(query) != std::wstring::npos)
            score += 300;
    }
    std::size_t pos{};
    while (pos < query.size()) {
        while (pos < query.size() && std::iswspace(query[pos]))
            ++pos;
        const auto start = pos;
        while (pos < query.size() && !std::iswspace(query[pos]))
            ++pos;
        if (start < pos) {
            const auto token = query.substr(start, pos - start);
            if (text.find(token) != std::wstring::npos)
                score += 100;
            else if (source.find(token) != std::wstring::npos)
                score += 50;
        }
    }
    return score;
}
} // namespace
JournalStore::JournalStore(std::filesystem::path path, std::size_t max_entries,
                           const DataProtector *protector, bool protection_enabled)
    : path_{std::move(path)}, max_entries_{std::clamp<std::size_t>(max_entries, 1, max_records)},
      protector_{protector}, protection_enabled_{protection_enabled} {}
JournalStoreSnapshot JournalStore::load() {
    std::scoped_lock lock{mutex_};
    protection_migrated_ = false;
    if (protection_enabled_ && (protector_ == nullptr || !protector_->available())) {
        throw std::runtime_error{
            "Journal live-store protection is enabled but its data protector is unavailable."};
    }
    auto scan = scan_candidates(path_, protector_);
    if (scan.valid.empty()) {
        if (scan.any_present) {
            throw std::runtime_error{
                "Journal store candidates exist but none are valid or decryptable."};
        }
        generation_ = 0;
        next_id_ = 1;
        entries_.clear();
        recovered_from_alternate_ = false;
        skip_primary_rotation_once_ = false;
        protected_at_rest_ = protection_enabled_;
        return {};
    }
    const auto best = std::max_element(
        scan.valid.begin(), scan.valid.end(), [this](const Candidate &a, const Candidate &b) {
            if (a.snapshot.generation != b.snapshot.generation) {
                return a.snapshot.generation < b.snapshot.generation;
            }
            const bool a_desired = a.protected_at_rest == protection_enabled_;
            const bool b_desired = b.protected_at_rest == protection_enabled_;
            if (a_desired != b_desired)
                return !a_desired && b_desired;
            return path_priority(a.path, path_) < path_priority(b.path, path_);
        });
    generation_ = best->snapshot.generation;
    next_id_ = best->snapshot.next_id;
    entries_ = best->snapshot.entries;
    recovered_from_alternate_ = best->path != path_;
    skip_primary_rotation_once_ = recovered_from_alternate_;
    protected_at_rest_ = best->protected_at_rest;
    enforce_limit_locked();
    if (protected_at_rest_ != protection_enabled_) {
        const bool recovered = recovered_from_alternate_;
        save_locked();
        recovered_from_alternate_ = recovered;
        protection_migrated_ = true;
    }
    if (protection_enabled_) {
        remove_plaintext_alternates(path_);
    }
    return JournalStoreSnapshot{generation_, next_id_, entries_};
}
std::vector<std::wstring> JournalStore::normalize_tags(std::span<const std::wstring> tags) {
    std::vector<std::wstring> result;
    for (const auto &raw : tags) {
        auto tag = lower_copy(trim_copy(raw));
        while (!tag.empty() && tag.front() == L'#')
            tag.erase(tag.begin());
        tag.erase(
            std::remove_if(tag.begin(), tag.end(), [](wchar_t c) { return std::iswspace(c) != 0; }),
            tag.end());
        if (tag.empty())
            continue;
        if (wide_to_utf8(tag).size() > max_tag_bytes)
            throw std::runtime_error{"Journal tag exceeds storage limit."};
        if (std::find(result.begin(), result.end(), tag) == result.end())
            result.push_back(std::move(tag));
        if (result.size() >= max_tags_per_entry)
            break;
    }
    std::sort(result.begin(), result.end());
    return result;
}
JournalEntryId JournalStore::add_entry(JournalEntry entry) {
    std::scoped_lock lock{mutex_};
    entry.text = trim_copy(entry.text);
    entry.source = trim_copy(entry.source);
    entry.tags = normalize_tags(entry.tags);
    if (entry.text.empty())
        throw std::invalid_argument{"Journal text cannot be empty."};
    if (next_id_ == 0)
        throw std::overflow_error{"Journal ID space exhausted."};
    entry.id = next_id_++;
    entries_.push_back(std::move(entry));
    enforce_limit_locked();
    save_locked();
    return entries_.back().id;
}
JournalEntryId JournalStore::add_note(std::wstring text, std::vector<std::wstring> tags,
                                      std::chrono::system_clock::time_point created_at) {
    return add_entry(JournalEntry{0, JournalEntryKind::note, created_at, L"manual", std::move(text),
                                  std::move(tags)});
}
JournalEntryId JournalStore::add_activity(std::wstring source, std::wstring text,
                                          std::vector<std::wstring> tags,
                                          std::chrono::system_clock::time_point created_at) {
    return add_entry(JournalEntry{0, JournalEntryKind::activity, created_at, std::move(source),
                                  std::move(text), std::move(tags)});
}
std::optional<JournalEntry> JournalStore::find(JournalEntryId id) const {
    std::scoped_lock lock{mutex_};
    const auto it =
        std::find_if(entries_.begin(), entries_.end(), [&](const auto &e) { return e.id == id; });
    return it == entries_.end() ? std::nullopt : std::optional<JournalEntry>{*it};
}
std::vector<JournalEntry> JournalStore::recent(std::size_t limit) const {
    std::scoped_lock lock{mutex_};
    std::vector<JournalEntry> out;
    limit = std::min(limit, entries_.size());
    out.reserve(limit);
    for (auto it = entries_.rbegin(); it != entries_.rend() && out.size() < limit; ++it) {
        out.push_back(*it);
    }
    return out;
}
std::vector<JournalEntry> JournalStore::between(std::chrono::system_clock::time_point begin,
                                                std::chrono::system_clock::time_point end,
                                                std::size_t limit) const {
    std::scoped_lock lock{mutex_};
    std::vector<JournalEntry> out;
    for (auto it = entries_.rbegin(); it != entries_.rend() && out.size() < limit; ++it) {
        if (it->created_at >= begin && it->created_at < end) {
            out.push_back(*it);
        }
    }
    return out;
}
std::vector<JournalEntry> JournalStore::search(std::wstring_view query, std::size_t limit) const {
    std::scoped_lock lock{mutex_};
    struct Hit {
        int score;
        const JournalEntry *entry;
    };
    std::vector<Hit> hits;
    for (const auto &entry : entries_) {
        const int score = relevance(entry, query);
        if (score > 0)
            hits.push_back({score, &entry});
    }
    std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
        if (a.score != b.score)
            return a.score > b.score;
        if (a.entry->created_at != b.entry->created_at)
            return a.entry->created_at > b.entry->created_at;
        return a.entry->id > b.entry->id;
    });
    std::vector<JournalEntry> out;
    for (const auto &hit : hits) {
        if (out.size() >= limit)
            break;
        out.push_back(*hit.entry);
    }
    return out;
}
std::vector<JournalEntry> JournalStore::tagged(std::wstring_view raw_tag, std::size_t limit) const {
    const auto normalized = normalize_tags(std::array<std::wstring, 1>{std::wstring{raw_tag}});
    if (normalized.empty())
        return {};
    std::scoped_lock lock{mutex_};
    std::vector<JournalEntry> out;
    for (auto it = entries_.rbegin(); it != entries_.rend() && out.size() < limit; ++it) {
        if (std::find(it->tags.begin(), it->tags.end(), normalized.front()) != it->tags.end()) {
            out.push_back(*it);
        }
    }
    return out;
}
std::vector<JournalContextItem> JournalStore::context(std::wstring_view query, std::size_t limit,
                                                      std::size_t max_characters) const {
    auto matches = trim_copy(query).empty() ? recent(limit) : search(query, limit);
    std::vector<JournalContextItem> out;
    std::size_t used{};
    for (const auto &entry : matches) {
        const std::size_t cost = entry.text.size();
        if (!out.empty() && used + cost > max_characters)
            break;
        std::wstring text = entry.text;
        if (text.size() > max_characters - std::min(used, max_characters))
            text.resize(max_characters - std::min(used, max_characters));
        if (text.empty())
            break;
        used += text.size();
        out.push_back({entry.id, entry.created_at, entry.kind, std::move(text), entry.tags});
        if (used >= max_characters)
            break;
    }
    return out;
}
bool JournalStore::erase(JournalEntryId id) {
    std::scoped_lock lock{mutex_};
    const auto old = entries_.size();
    std::erase_if(entries_, [&](const auto &e) { return e.id == id; });
    if (entries_.size() == old)
        return false;
    save_locked();
    return true;
}
std::size_t JournalStore::clear_notes() {
    std::scoped_lock lock{mutex_};
    const auto old = entries_.size();
    std::erase_if(entries_, [](const auto &e) { return e.kind == JournalEntryKind::note; });
    const auto removed = old - entries_.size();
    if (removed)
        save_locked();
    return removed;
}
std::size_t JournalStore::prune_before(std::chrono::system_clock::time_point cutoff,
                                       std::optional<JournalEntryKind> kind) {
    std::lock_guard lock{mutex_};
    const auto before = entries_.size();
    std::erase_if(entries_, [&](const JournalEntry &entry) {
        return entry.created_at < cutoff && (!kind || entry.kind == *kind);
    });
    const auto removed = before - entries_.size();
    if (removed != 0)
        save_locked();
    return removed;
}
std::size_t JournalStore::set_max_entries(std::size_t max_entries) {
    if (max_entries < 100 || max_entries > 200000) {
        throw std::invalid_argument{"Journal max_entries must be between 100 and 200000."};
    }
    std::lock_guard lock{mutex_};
    max_entries_ = max_entries;
    const auto before = entries_.size();
    enforce_limit_locked();
    const auto removed = before - entries_.size();
    if (removed != 0)
        save_locked();
    return removed;
}
std::size_t JournalStore::import_entries(std::vector<JournalEntry> entries) {
    if (entries.empty())
        return 0;
    std::lock_guard lock{mutex_};
    auto candidate = entries_;
    auto candidate_next_id = next_id_;
    candidate.reserve(std::min<std::size_t>(max_records, candidate.size() + entries.size()));
    for (auto &entry : entries) {
        entry.text = trim_copy(entry.text);
        entry.source = trim_copy(entry.source);
        entry.tags = normalize_tags(entry.tags);
        if (entry.text.empty())
            throw std::invalid_argument{"Imported journal text cannot be empty."};
        if (candidate_next_id == 0)
            throw std::overflow_error{"Journal ID space exhausted."};
        entry.id = candidate_next_id++;
        candidate.push_back(std::move(entry));
    }
    if (candidate.size() > max_entries_) {
        const auto remove_count = candidate.size() - max_entries_;
        candidate.erase(candidate.begin(),
                        candidate.begin() + static_cast<std::ptrdiff_t>(remove_count));
    }
    if (generation_ == 0) {
        auto scan = scan_candidates(path_, protector_);
        if (scan.valid.empty() && scan.any_present) {
            throw std::runtime_error{
                "Journal store candidates exist but none are valid or decryptable."};
        }
        for (const auto &existing : scan.valid) {
            generation_ = std::max(generation_, existing.snapshot.generation);
        }
    }
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error{"Journal generation exhausted."};
    }
    const auto next_generation = generation_ + 1;
    const auto plaintext = serialize_file(next_generation, candidate_next_id, candidate);
    std::vector<std::byte> storage = plaintext;
    if (protection_enabled_) {
        if (protector_ == nullptr || !protector_->available()) {
            throw std::runtime_error{"Journal live-store protector is unavailable."};
        }
        storage = seal_protected(plaintext, *protector_);
    }
    write_atomic(path_, storage, !skip_primary_rotation_once_);
    skip_primary_rotation_once_ = false;
    entries_ = std::move(candidate);
    next_id_ = candidate_next_id;
    generation_ = next_generation;
    protected_at_rest_ = protection_enabled_;
    if (protection_enabled_)
        remove_plaintext_alternates(path_);
    return entries.size();
}
void JournalStore::set_protection_enabled(bool enabled) {
    std::lock_guard lock{mutex_};
    if (enabled && (protector_ == nullptr || !protector_->available())) {
        throw std::runtime_error{
            "Journal live-store protection requires an available data protector."};
    }
    if (enabled == protection_enabled_ && protected_at_rest_ == enabled)
        return;
    const bool previous = protection_enabled_;
    protection_enabled_ = enabled;
    try {
        save_locked();
        protection_migrated_ = true;
    } catch (...) {
        protection_enabled_ = previous;
        throw;
    }
}
std::vector<std::byte> JournalStore::plaintext_snapshot_bytes() const {
    std::scoped_lock lock{mutex_};
    return serialize_file(generation_, next_id_, entries_);
}
JournalStoreStatus JournalStore::status() const {
    std::scoped_lock lock{mutex_};
    JournalStoreStatus result;
    result.path = path_;
    result.generation = generation_;
    result.entry_count = entries_.size();
    result.recovered_from_alternate = recovered_from_alternate_;
    result.protection_requested = protection_enabled_;
    result.protected_at_rest = protected_at_rest_;
    result.protection_migrated = protection_migrated_;
    result.protector_available = protector_ != nullptr && protector_->available();
    result.protector_name = protector_ == nullptr ? L"none" : protector_->name();
    for (const auto &entry : entries_) {
        (entry.kind == JournalEntryKind::note ? result.note_count : result.activity_count)++;
    }
    return result;
}
void JournalStore::enforce_limit_locked() {
    if (entries_.size() <= max_entries_)
        return;
    const auto remove_count = entries_.size() - max_entries_;
    entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(remove_count));
}
void JournalStore::save_locked() {
    if (generation_ == 0) {
        auto scan = scan_candidates(path_, protector_);
        if (scan.valid.empty() && scan.any_present) {
            throw std::runtime_error{
                "Journal store candidates exist but none are valid or decryptable."};
        }
        for (const auto &candidate : scan.valid) {
            generation_ = std::max(generation_, candidate.snapshot.generation);
        }
    }
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error{"Journal generation exhausted."};
    }
    const auto next_generation = generation_ + 1;
    const auto plaintext = serialize_file(next_generation, next_id_, entries_);
    std::vector<std::byte> storage = plaintext;
    if (protection_enabled_) {
        if (protector_ == nullptr || !protector_->available()) {
            throw std::runtime_error{"Journal live-store protector is unavailable."};
        }
        storage = seal_protected(plaintext, *protector_);
    }
    write_atomic(path_, storage, !skip_primary_rotation_once_);
    skip_primary_rotation_once_ = false;
    generation_ = next_generation;
    protected_at_rest_ = protection_enabled_;
    if (protection_enabled_)
        remove_plaintext_alternates(path_);
}
} // namespace axiom
