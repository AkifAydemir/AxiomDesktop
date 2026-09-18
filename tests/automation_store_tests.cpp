#include "automation_store.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <vector>
namespace {
using namespace std::chrono_literals;
[[noreturn]] void fail(const char *message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}
void require(bool condition, const char *message) {
    if (!condition)
        fail(message);
}
std::filesystem::path unique_test_path() {
    auto path = std::filesystem::temp_directory_path();
    path /= "axiom-automation-store-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin";
    return path;
}
void cleanup(const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    auto temp = path;
    temp += ".tmp";
    std::filesystem::remove(temp, error);
    auto backup = path;
    backup += ".bak";
    std::filesystem::remove(backup, error);
}
template <typename T>
    requires std::is_integral_v<T>
void append_le(std::vector<std::byte> &output, T value) {
    using Unsigned = std::make_unsigned_t<T>;
    auto raw = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>((raw >> (index * 8u)) & 0xFFu));
    }
}
std::uint32_t crc32(std::span<const std::byte> bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto byte : bytes) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<unsigned char>(byte));
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}
void append_ascii_text(std::vector<std::byte> &output, std::string_view value) {
    append_le(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), reinterpret_cast<const std::byte *>(value.data()),
                  reinterpret_cast<const std::byte *>(value.data() + value.size()));
}
void write_legacy_v1_fixture(const std::filesystem::path &path) {
    std::vector<std::byte> payload;
    append_le(payload, std::uint64_t{42});
    append_le(payload, std::int64_t{1'800'000'000'000});
    append_le(payload, std::int64_t{60'000});
    append_le(payload, std::uint64_t{3});
    append_le(payload, std::uint64_t{1});
    append_le(payload, std::numeric_limits<std::int64_t>::min());
    append_le(payload, std::uint32_t{0x3u});
    append_ascii_text(payload, "legacy recurring");
    append_ascii_text(payload, "notify");
    append_ascii_text(payload, "hello");
    append_ascii_text(payload, "");
    std::vector<std::byte> bytes;
    bytes.push_back(std::byte{'A'});
    bytes.push_back(std::byte{'X'});
    bytes.push_back(std::byte{'A'});
    bytes.push_back(std::byte{'U'});
    append_le(bytes, std::uint32_t{1});
    append_le(bytes, std::uint64_t{7});
    append_le(bytes, std::uint64_t{43});
    append_le(bytes, std::uint64_t{1});
    append_le(bytes, static_cast<std::uint64_t>(payload.size()));
    append_le(bytes, crc32(payload));
    append_le(bytes, std::uint32_t{0});
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(stream), "legacy fixture write should succeed");
}
} // namespace
int main() {
    const auto path = unique_test_path();
    cleanup(path);
    try {
        axiom::AutomationStore store{path};
        const auto empty = store.load();
        require(empty.automations.empty(), "new automation store should be empty");
        require(empty.next_id == 1, "new automation store should start at id 1");
        const auto now = std::chrono::system_clock::now();
        std::vector<axiom::Automation> automations;
        axiom::Automation first;
        first.id = 4;
        first.due_at = now + 1h;
        first.name = L"Sabah özeti 你好";
        first.action_name = L"notify";
        first.action_payload = L"Günaydın";
        automations.push_back(first);
        axiom::Automation second;
        second.id = 9;
        second.enabled = false;
        second.due_at = now + 5min;
        second.schedule_kind = axiom::AutomationScheduleKind::fixed_interval;
        second.repeat_interval = 5min;
        second.failure_policy.kind = axiom::AutomationRetryKind::exponential;
        second.failure_policy.max_retries = 4;
        second.failure_policy.initial_delay = 5s;
        second.failure_policy.max_delay = 30s;
        second.failure_policy.disable_on_exhaustion = true;
        second.retry_attempt = 2;
        second.retry_resume_at = now + 10min;
        second.consecutive_failures = 3;
        second.run_count = 7;
        second.failure_count = 2;
        second.last_run_at = now - 1min;
        second.last_success = false;
        second.name = L"Rebuild index";
        second.action_name = L"index-rebuild";
        second.last_error = L"test failure";
        automations.push_back(second);
        axiom::Automation third;
        third.id = 10;
        third.due_at = now + 24h;
        third.schedule_kind = axiom::AutomationScheduleKind::local_calendar;
        third.calendar_schedule.weekday_mask = 0x2Au;
        third.calendar_schedule.hour = 9;
        third.calendar_schedule.minute = 15;
        third.calendar_schedule.time_zone = L"local";
        third.name = L"Calendar automation";
        third.action_name = L"notify";
        automations.push_back(third);
        store.save(11, automations);
        const auto loaded = store.load();
        require(loaded.generation == 1, "first automation generation should be 1");
        require(loaded.next_id == 11, "automation next id should round-trip");
        require(loaded.automations.size() == 3, "three automations should round-trip");
        require(loaded.automations[0].name == first.name,
                "Unicode automation name should round-trip");
        require(loaded.automations[1].schedule_kind ==
                    axiom::AutomationScheduleKind::fixed_interval,
                "fixed schedule kind should round-trip");
        require(loaded.automations[1].repeat_interval == 5min, "recurrence should round-trip");
        require(loaded.automations[1].failure_policy.kind ==
                    axiom::AutomationRetryKind::exponential,
                "retry policy kind should round-trip");
        require(loaded.automations[1].retry_attempt == 2 &&
                    loaded.automations[1].retry_resume_at.has_value(),
                "retry state should round-trip");
        require(loaded.automations[1].consecutive_failures == 3,
                "consecutive failure state should round-trip");
        require(!loaded.automations[1].enabled, "disabled state should round-trip");
        require(loaded.automations[1].failure_count == 2, "failure count should round-trip");
        require(loaded.automations[1].last_run_at.has_value(),
                "last run timestamp should round-trip");
        require(loaded.automations[1].last_error == L"test failure",
                "last error should round-trip");
        require(loaded.automations[2].schedule_kind ==
                    axiom::AutomationScheduleKind::local_calendar,
                "calendar schedule kind should round-trip");
        require(loaded.automations[2].calendar_schedule.weekday_mask == 0x2Au &&
                    loaded.automations[2].calendar_schedule.hour == 9 &&
                    loaded.automations[2].calendar_schedule.minute == 15,
                "calendar wall-clock metadata should round-trip");
        automations.erase(automations.begin());
        store.save(11, automations);
        const auto second_snapshot = store.load();
        require(second_snapshot.generation == 2,
                "generation should advance per committed automation snapshot");
        require(second_snapshot.automations.size() == 2,
                "replacement snapshot should remove old automation");
        auto temp = path;
        temp += ".tmp";
        auto backup = path;
        backup += ".bak";
        std::filesystem::copy_file(path, temp, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::rename(path, backup);
        axiom::AutomationStore recovered{path};
        const auto recovered_snapshot = recovered.load();
        require(recovered_snapshot.generation == 2,
                "valid temp should recover after interrupted rotate");
        require(recovered.status().recovered_from_alternate,
                "recovery status should report alternate candidate");
        {
            std::fstream corrupt{temp, std::ios::binary | std::ios::in | std::ios::out};
            require(static_cast<bool>(corrupt), "temp automation snapshot should be writable");
            corrupt.seekg(-1, std::ios::end);
            char byte = 0;
            corrupt.read(&byte, 1);
            byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x5Au);
            corrupt.seekp(-1, std::ios::end);
            corrupt.write(&byte, 1);
        }
        axiom::AutomationStore fallback{path};
        const auto fallback_snapshot = fallback.load();
        require(fallback_snapshot.generation == 2, "CRC failure should fall back to valid backup");
        require(fallback_snapshot.automations.size() == 2,
                "backup automation snapshot should preserve records");
        axiom::Automation invalid_resume = second;
        invalid_resume.retry_attempt = 0;
        invalid_resume.retry_resume_at = now + 30min;
        bool rejected_orphan_resume = false;
        try {
            std::vector<axiom::Automation> invalid_records{invalid_resume};
            fallback.save(11, invalid_records);
        } catch (const std::exception &) {
            rejected_orphan_resume = true;
        }
        require(rejected_orphan_resume, "retry resume time without active retry must fail closed");
        cleanup(path);
        const auto legacy_path = unique_test_path();
        cleanup(legacy_path);
        write_legacy_v1_fixture(legacy_path);
        axiom::AutomationStore legacy_store{legacy_path};
        const auto legacy = legacy_store.load();
        require(legacy.generation == 7 && legacy.next_id == 43, "legacy v1 header should load");
        require(legacy.automations.size() == 1, "legacy v1 record should load");
        require(legacy.automations[0].schedule_kind ==
                    axiom::AutomationScheduleKind::fixed_interval,
                "legacy repeat interval should migrate to fixed schedule kind");
        require(legacy.automations[0].repeat_interval == 60s,
                "legacy interval should be preserved");
        require(!legacy.automations[0].failure_policy.retries_enabled(),
                "legacy record should default to no retry policy");
        legacy_store.save(legacy.next_id, legacy.automations);
        const auto upgraded = legacy_store.load();
        require(upgraded.generation == 8,
                "saving legacy snapshot should upgrade into next generation v2 file");
        require(upgraded.automations[0].schedule_kind ==
                    axiom::AutomationScheduleKind::fixed_interval,
                "upgraded v2 record should preserve migrated schedule kind");
        cleanup(legacy_path);
    } catch (...) {
        cleanup(path);
        throw;
    }
    std::cout << "AxiomAutomationStoreTests: PASS\n";
    return 0;
}
