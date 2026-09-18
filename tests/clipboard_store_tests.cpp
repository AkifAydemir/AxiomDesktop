#include "clipboard_store.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>
namespace {
void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}
void test_classification_and_recent_order() {
    axiom::ClipboardStore store;
    const auto url = store.record_text(L"https://openai.com/docs");
    const auto path = store.record_text(L"C:\\Projects\\Axiom\\src\\main.cpp");
    const auto json = store.record_text(L"{\"status\":\"ok\",\"count\":2}");
    const auto code = store.record_text(L"#include <vector>\nint main() { return 0; }");
    require(url && path && json && code, "all clipboard items should be accepted");
    const auto recent = store.recent(10);
    require(recent.size() == 4, "recent should return all entries");
    require(recent[0].kind == axiom::ClipboardKind::code, "code classification failed");
    require(recent[1].kind == axiom::ClipboardKind::json, "json classification failed");
    require(recent[2].kind == axiom::ClipboardKind::path, "path classification failed");
    require(recent[3].kind == axiom::ClipboardKind::url, "url classification failed");
}
void test_deduplication_preserves_id_and_moves_front() {
    axiom::ClipboardStore store;
    const auto first = store.record_text(L"alpha");
    const auto second = store.record_text(L"beta");
    const auto duplicate = store.record_text(L"alpha");
    require(first && second && duplicate, "test setup should record entries");
    require(*first == *duplicate, "duplicate text should preserve stable id");
    const auto recent = store.recent(10);
    require(recent.size() == 2, "duplicate should not create a third entry");
    require(recent[0].id == *first, "duplicate should move refreshed entry to front");
    require(recent[0].capture_count == 2, "duplicate should increment capture count");
}
void test_pin_survives_eviction() {
    axiom::ClipboardStore store{3, 1024 * 1024, 1024 * 1024};
    const auto pinned = store.record_text(L"keep-me");
    require(pinned.has_value(), "pinned fixture missing");
    require(store.set_pinned(*pinned, true), "pin should succeed");
    require(store.record_text(L"two").has_value(), "two should be recorded");
    require(store.record_text(L"three").has_value(), "three should be recorded");
    require(store.record_text(L"four").has_value(), "four should be recorded");
    require(store.record_text(L"five").has_value(), "five should be recorded");
    const auto snapshot = store.snapshot();
    require(snapshot.entries == 3, "entry limit should be enforced");
    require(snapshot.pinned == 1, "pinned count should be preserved");
    require(store.find(*pinned).has_value(), "pinned entry should survive eviction");
}
void test_search_and_clear() {
    axiom::ClipboardStore store;
    const auto important = store.record_text(L"clipboard architecture notes");
    require(store.record_text(L"unrelated value").has_value(), "unrelated value missing");
    require(store.record_text(L"clipboard benchmark result").has_value(),
            "benchmark value missing");
    require(important.has_value(), "important item missing");
    require(store.set_pinned(*important, true), "pin should succeed");
    const auto hits = store.search(L"clipboard", 10);
    require(hits.size() == 2, "search should match two entries");
    require(hits.front().id == *important, "pinned match should receive ranking bonus");
    const auto removed = store.clear_unpinned();
    require(removed == 2, "clear_unpinned should remove two entries");
    require(store.snapshot().entries == 1, "only pinned entry should remain");
}
void test_item_size_limit() {
    axiom::ClipboardStore store{20, 1024, 16};
    require(store.record_text(L"tiny").has_value(), "small item should be accepted");
    require(!store.record_text(L"this text is intentionally too large").has_value(),
            "oversized item should be rejected");
}
} // namespace
int main() {
    try {
        test_classification_and_recent_order();
        test_deduplication_preserves_id_and_moves_front();
        test_pin_survives_eviction();
        test_search_and_clear();
        test_item_size_limit();
        std::cout << "clipboard_store_tests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "clipboard_store_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
