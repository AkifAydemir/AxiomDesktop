#include "reminder_store.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
namespace {
using namespace std::chrono_literals;
[[noreturn]] void fail(const char *message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}
void require(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}
std::filesystem::path unique_test_path() {
    auto path = std::filesystem::temp_directory_path();
    path /= "axiom-reminder-store-test-" +
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
} // namespace
int main() {
    const auto path = unique_test_path();
    cleanup(path);
    try {
        axiom::ReminderStore store{path};
        const auto empty = store.load();
        require(empty.reminders.empty(), "new store should load empty");
        require(empty.next_id == 1, "new store should begin with id 1");
        const auto now = std::chrono::system_clock::now();
        std::vector<axiom::Reminder> reminders{
            {3, now + 10min, 0ms, 0, L"absolute test: Türkçe 你好"},
            {8, now + 30s, 30s, 4, L"recurring build check"},
        };
        store.save(9, reminders);
        const auto first = store.load();
        require(first.generation == 1, "first persisted generation should be 1");
        require(first.next_id == 9, "next id should round-trip");
        require(first.reminders.size() == 2, "two reminders should round-trip");
        require(first.reminders[0].message == reminders[0].message,
                "Unicode reminder should round-trip");
        require(first.reminders[1].repeat_interval == 30s, "repeat interval should round-trip");
        require(first.reminders[1].fire_count == 4, "fire count should round-trip");
        reminders.erase(reminders.begin());
        store.save(10, reminders);
        const auto second = store.load();
        require(second.generation == 2, "generation should advance per committed snapshot");
        require(second.reminders.size() == 1, "updated snapshot should replace old records");
        // Simulate a crash after the old main file was rotated but before the new temp
        // snapshot was published: the loader must choose the valid higher-generation temp.
        auto temp = path;
        temp += ".tmp";
        auto backup = path;
        backup += ".bak";
        std::filesystem::copy_file(path, temp, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::rename(path, backup);
        axiom::ReminderStore recovered{path};
        const auto recovered_snapshot = recovered.load();
        require(recovered_snapshot.generation == 2,
                "loader should recover from valid temp snapshot");
        require(recovered.status().recovered_from_alternate,
                "status should report alternate recovery");
        // Corrupt the preferred temp candidate; the valid backup must still be selected.
        {
            std::fstream corrupt{temp, std::ios::binary | std::ios::in | std::ios::out};
            require(static_cast<bool>(corrupt),
                    "temp snapshot should be writable for corruption test");
            corrupt.seekg(-1, std::ios::end);
            char byte = 0;
            corrupt.read(&byte, 1);
            byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x5Au);
            corrupt.seekp(-1, std::ios::end);
            corrupt.write(&byte, 1);
        }
        axiom::ReminderStore fallback{path};
        const auto fallback_snapshot = fallback.load();
        require(fallback_snapshot.generation == 2,
                "loader should fall back to valid backup after CRC failure");
        require(fallback_snapshot.reminders.size() == 1, "backup recovery should preserve records");
        cleanup(path);
    } catch (...) {
        cleanup(path);
        throw;
    }
    std::cout << "Axiom reminder store tests passed\n";
    return 0;
}
