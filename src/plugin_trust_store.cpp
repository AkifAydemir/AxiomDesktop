#include "plugin_trust_store.hpp"
#include "plugin_manifest.hpp"
#include "sha256.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
namespace axiom {
namespace {
constexpr std::string_view trust_header = "AXIOM_PLUGIN_TRUST_V1\n";
constexpr std::string_view checksum_prefix = "checksum=";
constexpr std::size_t maximum_records = 2048;
constexpr std::size_t maximum_reason_bytes = 4096;
[[nodiscard]] std::filesystem::path temp_path(const std::filesystem::path &path) {
    auto output = path;
    output += L".tmp";
    return output;
}
[[nodiscard]] std::filesystem::path backup_path(const std::filesystem::path &path) {
    auto output = path;
    output += L".bak";
    return output;
}
[[nodiscard]] std::string escape_field(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (const char ch : input) {
        switch (ch) {
        case '\\':
            output += "\\\\";
            break;
        case '\t':
            output += "\\t";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        default:
            output.push_back(ch);
            break;
        }
    }
    return output;
}
[[nodiscard]] std::string unescape_field(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '\\') {
            output.push_back(input[i]);
            continue;
        }
        if (++i >= input.size()) {
            throw std::runtime_error{"Plugin trust record contains a trailing escape."};
        }
        switch (input[i]) {
        case '\\':
            output.push_back('\\');
            break;
        case 't':
            output.push_back('\t');
            break;
        case 'n':
            output.push_back('\n');
            break;
        case 'r':
            output.push_back('\r');
            break;
        default:
            throw std::runtime_error{"Plugin trust record contains an invalid escape."};
        }
    }
    return output;
}
[[nodiscard]] std::vector<std::string> split_fields(std::string_view line) {
    std::vector<std::string> fields;
    std::size_t offset = 0;
    while (true) {
        const auto separator = line.find('\t', offset);
        const auto length =
            separator == std::string_view::npos ? line.size() - offset : separator - offset;
        fields.push_back(unescape_field(line.substr(offset, length)));
        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1;
    }
    return fields;
}
void validate_record(const PluginTrustRecord &record) {
    if (!is_valid_plugin_id(record.id)) {
        throw std::runtime_error{"Invalid plugin trust ID."};
    }
    if (record.version.empty() || record.version.size() > 64) {
        throw std::runtime_error{"Invalid plugin trust version."};
    }
    if (record.library.empty() || record.library.size() > 180 ||
        record.library.find('/') != std::string::npos ||
        record.library.find('\\') != std::string::npos) {
        throw std::runtime_error{"Invalid plugin trust library filename."};
    }
    if (!is_sha256_hex(record.manifest_sha256) || !is_sha256_hex(record.library_sha256)) {
        throw std::runtime_error{"Invalid plugin SHA-256 pin."};
    }
    if (record.quarantine_reason.size() > maximum_reason_bytes) {
        throw std::runtime_error{"Plugin quarantine reason exceeds the limit."};
    }
    if (record.quarantined && record.enabled) {
        throw std::runtime_error{"A quarantined plugin cannot be enabled."};
    }
}
void normalize_records(std::vector<PluginTrustRecord> &records) {
    if (records.size() > maximum_records) {
        throw std::runtime_error{"Plugin trust store record limit exceeded."};
    }
    for (const auto &record : records) {
        validate_record(record);
    }
    std::sort(
        records.begin(), records.end(),
        [](const PluginTrustRecord &lhs, const PluginTrustRecord &rhs) { return lhs.id < rhs.id; });
    for (std::size_t i = 1; i < records.size(); ++i) {
        if (records[i - 1].id == records[i].id) {
            throw std::runtime_error{"Duplicate plugin trust ID."};
        }
    }
}
[[nodiscard]] std::string serialize_records(std::vector<PluginTrustRecord> records) {
    normalize_records(records);
    std::string payload{trust_header};
    for (const auto &record : records) {
        payload += escape_field(record.id) + '\t' + escape_field(record.version) + '\t' +
                   escape_field(record.library) + '\t' + (record.enabled ? "1" : "0") + '\t' +
                   (record.quarantined ? "1" : "0") + '\t' + record.manifest_sha256 + '\t' +
                   record.library_sha256 + '\t' + escape_field(record.quarantine_reason) +
                   "\trecord\n";
    }
    const auto *bytes = reinterpret_cast<const std::byte *>(payload.data());
    const auto digest = sha256_hex(std::span<const std::byte>{bytes, payload.size()});
    return payload + std::string{checksum_prefix} + digest + '\n';
}
[[nodiscard]] std::vector<PluginTrustRecord> parse_records(std::string_view content) {
    const auto checksum_position = content.rfind(checksum_prefix);
    if (checksum_position == std::string_view::npos || checksum_position == 0) {
        throw std::runtime_error{"Plugin trust store checksum is missing."};
    }
    if (checksum_position < content.size() && content[checksum_position - 1] != '\n') {
        throw std::runtime_error{"Plugin trust store checksum boundary is malformed."};
    }
    auto checksum_line = content.substr(checksum_position + checksum_prefix.size());
    if (!checksum_line.empty() && checksum_line.back() == '\n') {
        checksum_line.remove_suffix(1);
    }
    if (!checksum_line.empty() && checksum_line.back() == '\r') {
        checksum_line.remove_suffix(1);
    }
    if (!is_sha256_hex(checksum_line)) {
        throw std::runtime_error{"Plugin trust store checksum is invalid."};
    }
    const auto payload = content.substr(0, checksum_position);
    const auto *bytes = reinterpret_cast<const std::byte *>(payload.data());
    if (sha256_hex(std::span<const std::byte>{bytes, payload.size()}) != checksum_line) {
        throw std::runtime_error{"Plugin trust store checksum mismatch."};
    }
    if (!payload.starts_with(trust_header)) {
        throw std::runtime_error{"Plugin trust store header/version mismatch."};
    }
    std::vector<PluginTrustRecord> records;
    std::size_t offset = trust_header.size();
    while (offset < payload.size()) {
        const auto end = payload.find('\n', offset);
        if (end == std::string_view::npos) {
            throw std::runtime_error{"Plugin trust store record is not line terminated."};
        }
        const auto line = payload.substr(offset, end - offset);
        offset = end + 1;
        if (line.empty()) {
            continue;
        }
        const auto fields = split_fields(line);
        if (fields.size() != 9 || fields[8] != "record") {
            throw std::runtime_error{"Plugin trust record field contract mismatch."};
        }
        if ((fields[3] != "0" && fields[3] != "1") || (fields[4] != "0" && fields[4] != "1")) {
            throw std::runtime_error{"Plugin trust record boolean field is invalid."};
        }
        PluginTrustRecord record;
        record.id = fields[0];
        record.version = fields[1];
        record.library = fields[2];
        record.enabled = fields[3] == "1";
        record.quarantined = fields[4] == "1";
        record.manifest_sha256 = fields[5];
        record.library_sha256 = fields[6];
        record.quarantine_reason = fields[7];
        records.push_back(std::move(record));
    }
    normalize_records(records);
    return records;
}
[[nodiscard]] std::vector<PluginTrustRecord> read_records(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error{"Unable to open plugin trust store."};
    }
    const std::string content{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
    if (!input.eof() && input.fail()) {
        throw std::runtime_error{"Failed while reading plugin trust store."};
    }
    return parse_records(content);
}
void write_records(const std::filesystem::path &path,
                   const std::vector<PluginTrustRecord> &records) {
    const auto content = serialize_records(records);
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        throw std::runtime_error{"Unable to write plugin trust store."};
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error{"Failed while flushing plugin trust store."};
    }
}
} // namespace
PluginTrustStore::PluginTrustStore(std::filesystem::path path) : path_{std::move(path)} {}
std::vector<PluginTrustRecord> PluginTrustStore::load() {
    std::scoped_lock lock{mutex_};
    records_.clear();
    recovered_from_alternate_ = false;
    const std::array candidates{path_, temp_path(path_), backup_path(path_)};
    std::exception_ptr last_error;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidates[i], error) || error) {
            continue;
        }
        try {
            records_ = read_records(candidates[i]);
            recovered_from_alternate_ = i != 0;
            return records_;
        } catch (...) {
            last_error = std::current_exception();
        }
    }
    std::error_code error;
    const bool any_candidate_exists =
        std::any_of(candidates.begin(), candidates.end(), [&](const auto &candidate) {
            return std::filesystem::exists(candidate, error) && !error;
        });
    if (any_candidate_exists && last_error) {
        std::rethrow_exception(last_error);
    }
    return records_;
}
void PluginTrustStore::commit_locked(std::vector<PluginTrustRecord> records) {
    normalize_records(records);
    std::error_code error;
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path(), error);
        if (error) {
            throw std::runtime_error{"Unable to create plugin trust directory."};
        }
    }
    const auto temporary = temp_path(path_);
    const auto backup = backup_path(path_);
    write_records(temporary, records);
    if (std::filesystem::exists(path_, error) && !error) {
        std::filesystem::copy_file(path_, backup, std::filesystem::copy_options::overwrite_existing,
                                   error);
        if (error) {
            std::filesystem::remove(temporary, error);
            throw std::runtime_error{"Unable to create plugin trust backup."};
        }
    }
    error.clear();
    std::filesystem::remove(path_, error);
    error.clear();
    std::filesystem::rename(temporary, path_, error);
    if (error) {
        std::error_code restore_error;
        if (std::filesystem::exists(backup, restore_error) && !restore_error) {
            std::filesystem::copy_file(
                backup, path_, std::filesystem::copy_options::overwrite_existing, restore_error);
        }
        throw std::runtime_error{"Unable to commit plugin trust store."};
    }
    records_ = std::move(records);
    recovered_from_alternate_ = false;
}
void PluginTrustStore::save(std::vector<PluginTrustRecord> records) {
    std::scoped_lock lock{mutex_};
    commit_locked(std::move(records));
}
std::vector<PluginTrustRecord> PluginTrustStore::list() const {
    std::scoped_lock lock{mutex_};
    return records_;
}
std::optional<PluginTrustRecord> PluginTrustStore::find(std::string_view id) const {
    std::scoped_lock lock{mutex_};
    const auto it = std::find_if(records_.begin(), records_.end(),
                                 [&](const PluginTrustRecord &record) { return record.id == id; });
    if (it == records_.end()) {
        return std::nullopt;
    }
    return *it;
}
void PluginTrustStore::upsert(PluginTrustRecord record) {
    validate_record(record);
    std::scoped_lock lock{mutex_};
    auto next = records_;
    const auto it = std::find_if(next.begin(), next.end(), [&](const PluginTrustRecord &current) {
        return current.id == record.id;
    });
    if (it == next.end()) {
        next.push_back(std::move(record));
    } else {
        *it = std::move(record);
    }
    commit_locked(std::move(next));
}
bool PluginTrustStore::set_enabled(std::string_view id, bool enabled) {
    std::scoped_lock lock{mutex_};
    auto next = records_;
    const auto it = std::find_if(next.begin(), next.end(),
                                 [&](const PluginTrustRecord &record) { return record.id == id; });
    if (it == next.end()) {
        return false;
    }
    if (enabled && it->quarantined) {
        throw std::runtime_error{"A quarantined plugin cannot be enabled; remove and reinstall it "
                                 "to establish new pins."};
    }
    it->enabled = enabled;
    commit_locked(std::move(next));
    return true;
}
bool PluginTrustStore::quarantine(std::string_view id, std::string reason) {
    if (reason.empty()) {
        reason = "Plugin package failed trust verification.";
    }
    std::scoped_lock lock{mutex_};
    auto next = records_;
    const auto it = std::find_if(next.begin(), next.end(),
                                 [&](const PluginTrustRecord &record) { return record.id == id; });
    if (it == next.end()) {
        return false;
    }
    it->quarantined = true;
    it->enabled = false;
    it->quarantine_reason = std::move(reason);
    commit_locked(std::move(next));
    return true;
}
bool PluginTrustStore::erase(std::string_view id) {
    std::scoped_lock lock{mutex_};
    auto next = records_;
    const auto previous_size = next.size();
    std::erase_if(next, [&](const PluginTrustRecord &record) { return record.id == id; });
    if (next.size() == previous_size) {
        return false;
    }
    commit_locked(std::move(next));
    return true;
}
PluginTrustStatus PluginTrustStore::status() const {
    std::scoped_lock lock{mutex_};
    PluginTrustStatus output;
    output.path = path_;
    output.records = records_.size();
    output.recovered_from_alternate = recovered_from_alternate_;
    for (const auto &record : records_) {
        if (record.enabled) {
            ++output.enabled;
        }
        if (record.quarantined) {
            ++output.quarantined;
        }
    }
    return output;
}
const std::filesystem::path &PluginTrustStore::path() const noexcept {
    return path_;
}
} // namespace axiom
