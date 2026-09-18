#include "journal_transfer.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
namespace axiom {
namespace {
constexpr std::size_t max_import_records = 200000;
void append_utf8(std::string &output, std::uint32_t codepoint) {
    if (codepoint <= 0x7Fu) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFu) {
        output.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint <= 0xFFFFu) {
        output.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint <= 0x10FFFFu) {
        output.push_back(static_cast<char>(0xF0u | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else {
        throw std::runtime_error{"Invalid Unicode codepoint in journal export."};
    }
}
std::string wide_to_utf8(std::wstring_view value) {
    std::string output;
    output.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        std::uint32_t codepoint = static_cast<std::uint32_t>(value[index]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
                if (++index >= value.size())
                    throw std::runtime_error{"Unpaired UTF-16 high surrogate."};
                const auto low = static_cast<std::uint32_t>(value[index]);
                if (low < 0xDC00u || low > 0xDFFFu)
                    throw std::runtime_error{"Invalid UTF-16 surrogate pair."};
                codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) + (low - 0xDC00u);
            } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
                throw std::runtime_error{"Unpaired UTF-16 low surrogate."};
            }
        }
        append_utf8(output, codepoint);
    }
    return output;
}
std::wstring utf8_to_wide(std::string_view value) {
    std::wstring output;
    output.reserve(value.size());
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index++]);
        std::uint32_t codepoint{};
        int continuation_count{};
        if ((first & 0x80u) == 0) {
            codepoint = first;
        } else if ((first & 0xE0u) == 0xC0u) {
            codepoint = first & 0x1Fu;
            continuation_count = 1;
        } else if ((first & 0xF0u) == 0xE0u) {
            codepoint = first & 0x0Fu;
            continuation_count = 2;
        } else if ((first & 0xF8u) == 0xF0u) {
            codepoint = first & 0x07u;
            continuation_count = 3;
        } else {
            throw std::runtime_error{"Invalid UTF-8 lead byte in journal export."};
        }
        for (int part = 0; part < continuation_count; ++part) {
            if (index >= value.size())
                throw std::runtime_error{"Truncated UTF-8 in journal export."};
            const auto byte = static_cast<unsigned char>(value[index++]);
            if ((byte & 0xC0u) != 0x80u)
                throw std::runtime_error{"Invalid UTF-8 continuation byte."};
            codepoint = (codepoint << 6) | (byte & 0x3Fu);
        }
        if ((continuation_count == 1 && codepoint < 0x80u) ||
            (continuation_count == 2 && codepoint < 0x800u) ||
            (continuation_count == 3 && codepoint < 0x10000u) || codepoint > 0x10FFFFu ||
            (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
            throw std::runtime_error{"Non-canonical UTF-8 in journal export."};
        }
        if constexpr (sizeof(wchar_t) == 2) {
            if (codepoint <= 0xFFFFu) {
                output.push_back(static_cast<wchar_t>(codepoint));
            } else {
                codepoint -= 0x10000u;
                output.push_back(static_cast<wchar_t>(0xD800u + (codepoint >> 10)));
                output.push_back(static_cast<wchar_t>(0xDC00u + (codepoint & 0x3FFu)));
            }
        } else {
            output.push_back(static_cast<wchar_t>(codepoint));
        }
    }
    return output;
}
std::string escape_field(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (const char ch : value) {
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
        case ',':
            output += "\\,";
            break;
        default:
            output.push_back(ch);
            break;
        }
    }
    return output;
}
std::string unescape_field(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '\\') {
            output.push_back(value[index]);
            continue;
        }
        if (++index >= value.size())
            throw std::runtime_error{"Dangling escape in journal export."};
        switch (value[index]) {
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
        case ',':
            output.push_back(',');
            break;
        default:
            throw std::runtime_error{"Unknown escape in journal export."};
        }
    }
    return output;
}
std::vector<std::string> split_escaped(std::string_view value, char delimiter) {
    std::vector<std::string> fields;
    std::string current;
    bool escaped = false;
    for (const char ch : value) {
        if (escaped) {
            current.push_back('\\');
            current.push_back(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == delimiter) {
            fields.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (escaped)
        throw std::runtime_error{"Dangling escape in journal export."};
    fields.push_back(std::move(current));
    return fields;
}
std::vector<std::wstring> parse_tags(std::string_view value) {
    std::vector<std::wstring> tags;
    if (value.empty())
        return tags;
    for (const auto &raw : split_escaped(value, ',')) {
        if (!raw.empty())
            tags.push_back(utf8_to_wide(unescape_field(raw)));
    }
    return JournalStore::normalize_tags(tags);
}
void publish_text_file(const std::filesystem::path &path, const std::string &text) {
    std::error_code error;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error)
            throw std::system_error(error, "Unable to create journal export directory");
    }
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
        if (!stream)
            throw std::runtime_error{"Unable to create temporary journal export."};
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream)
            throw std::runtime_error{"Unable to write journal export."};
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            throw std::system_error(error, "Unable to publish journal export");
        }
    }
}
} // namespace
JournalTransferResult export_journal(const JournalStore &store, const std::filesystem::path &path,
                                     bool include_activities) {
    auto entries = store.recent(max_import_records);
    std::reverse(entries.begin(), entries.end());
    std::string output{"AXIOM-JOURNAL-EXPORT\t1\n"};
    JournalTransferResult result;
    for (const auto &entry : entries) {
        if (entry.kind == JournalEntryKind::activity && !include_activities)
            continue;
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      entry.created_at.time_since_epoch())
                                      .count();
        std::string tags;
        for (std::size_t index = 0; index < entry.tags.size(); ++index) {
            if (index != 0)
                tags.push_back(',');
            tags += escape_field(wide_to_utf8(entry.tags[index]));
        }
        output += entry.kind == JournalEntryKind::note ? "N\t" : "A\t";
        output += std::to_string(milliseconds);
        output.push_back('\t');
        output += escape_field(wide_to_utf8(entry.source));
        output.push_back('\t');
        output += tags;
        output.push_back('\t');
        output += escape_field(wide_to_utf8(entry.text));
        output.push_back('\n');
        ++result.entries;
        if (entry.kind == JournalEntryKind::note)
            ++result.notes;
        else
            ++result.activities;
    }
    publish_text_file(path, output);
    return result;
}
JournalTransferResult import_journal(JournalStore &store, const std::filesystem::path &path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream)
        throw std::runtime_error{"Unable to open journal import."};
    std::string line;
    if (!std::getline(stream, line) || line != "AXIOM-JOURNAL-EXPORT\t1") {
        throw std::runtime_error{"Unsupported journal export format."};
    }
    JournalTransferResult result;
    std::vector<JournalEntry> parsed;
    std::size_t line_count{};
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (++line_count > max_import_records)
            throw std::runtime_error{"Journal import exceeds record limit."};
        auto fields = split_escaped(line, '\t');
        if (fields.size() != 5 || (fields[0] != "N" && fields[0] != "A")) {
            throw std::runtime_error{"Malformed journal export record."};
        }
        std::int64_t milliseconds{};
        try {
            std::size_t consumed{};
            milliseconds = std::stoll(fields[1], &consumed, 10);
            if (consumed != fields[1].size())
                throw std::runtime_error{"bad timestamp"};
        } catch (...) {
            throw std::runtime_error{"Invalid journal export timestamp."};
        }
        JournalEntry entry;
        entry.kind = fields[0] == "N" ? JournalEntryKind::note : JournalEntryKind::activity;
        entry.created_at =
            std::chrono::system_clock::time_point{std::chrono::milliseconds{milliseconds}};
        entry.source = utf8_to_wide(unescape_field(fields[2]));
        entry.tags = parse_tags(fields[3]);
        entry.text = utf8_to_wide(unescape_field(fields[4]));
        if (entry.text.empty())
            throw std::runtime_error{"Journal import contains empty text."};
        if (entry.kind == JournalEntryKind::note)
            ++result.notes;
        else
            ++result.activities;
        parsed.push_back(std::move(entry));
    }
    result.entries = store.import_entries(std::move(parsed));
    return result;
}
} // namespace axiom
