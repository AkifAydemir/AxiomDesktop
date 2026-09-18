#include "file_index.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
namespace {
void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}
std::uint64_t wait_until_ready(axiom::FileIndex &index) {
    const auto generation = index.current_scan_generation();
    expect(generation != 0, "index scan generation should be non-zero");
    const auto result = index.wait_for_scan(generation);
    expect(result == axiom::IndexScanWaitResult::succeeded,
           "file index scan should complete successfully");
    return generation;
}
class Fixture final {
  public:
    Fixture() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ =
            std::filesystem::temp_directory_path() / ("axiom-index-test-" + std::to_string(stamp));
        second_root_ = std::filesystem::temp_directory_path() /
                       ("axiom-index-test-second-" + std::to_string(stamp));
        std::filesystem::create_directories(root_ / "Projects" / "Axiom");
        std::filesystem::create_directories(root_ / "Docs");
        std::filesystem::create_directories(root_ / "node_modules" / "ignored");
        std::filesystem::create_directories(root_ / "Cache" / "ignored");
        std::filesystem::create_directories(second_root_);
        write(root_ / "Projects" / "Axiom" / "scheduler.cpp", "int main(){}\n");
        write(root_ / "Projects" / "Axiom" / "file_index.cpp", "// index\n");
        write(root_ / "Docs" / "Axiom Architecture.pdf", "fake-pdf\n");
        write(root_ / "node_modules" / "ignored" / "should-not-index.txt", "ignored\n");
        write(root_ / "Cache" / "ignored" / "custom-excluded.txt", "ignored\n");
        write(second_root_ / "second-root.txt", "second\n");
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::remove_all(second_root_, error);
    }
    [[nodiscard]] const std::filesystem::path &root() const noexcept {
        return root_;
    }
    [[nodiscard]] const std::filesystem::path &second_root() const noexcept {
        return second_root_;
    }
    static void write(const std::filesystem::path &path, std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream{path, std::ios::binary};
        stream << text;
    }

  private:
    std::filesystem::path root_;
    std::filesystem::path second_root_;
};
void run_tests() {
    Fixture fixture;
    axiom::FileIndex index;
    index.start({fixture.root()}, {L"cache"});
    const auto initial_generation = wait_until_ready(index);
    auto snapshot = index.snapshot();
    expect(snapshot.state == axiom::IndexState::ready, "index should be ready");
    expect(snapshot.files == 3, "built-in and configured exclusions must not be indexed");
    expect(snapshot.directories >= 3, "fixture directories should be indexed");
    expect(snapshot.excluded_directory_names.size() == 1, "configured exclusion should persist");
    const auto exact = index.search(L"scheduler.cpp", 10);
    expect(!exact.empty(), "exact search should return a hit");
    expect(exact.front().path.filename() == "scheduler.cpp", "exact hit should rank first");
    const auto fuzzy = index.search(L"schdlr", 10);
    expect(!fuzzy.empty(), "fuzzy subsequence search should return a hit");
    expect(fuzzy.front().path.filename() == "scheduler.cpp", "fuzzy hit should rank correctly");
    const auto multi = index.search(L"axiom architecture", 10);
    expect(!multi.empty(), "multi-term search should return a hit");
    expect(multi.front().path.filename() == "Axiom Architecture.pdf",
           "multi-term hit should rank correctly");
    expect(index.search(L"custom-excluded", 10).empty(), "configured exclusion should be absent");
    expect(index.search(L"should-not-index", 10).empty(), "built-in exclusion should be absent");
    const auto dynamic_file = fixture.root() / "Docs" / "live-created.json";
    Fixture::write(dynamic_file, "{}\n");
    index.refresh_path(dynamic_file);
    expect(!index.search(L"live-created", 10).empty(), "incremental refresh should add a new file");
    snapshot = index.snapshot();
    expect(snapshot.incremental_updates == 1, "incremental update counter should advance");
    std::filesystem::remove(dynamic_file);
    index.remove_path(dynamic_file);
    expect(index.search(L"live-created", 10).empty(), "incremental remove should delete a file");
    expect(index.snapshot().incremental_updates == 2, "remove should advance incremental counter");
    const auto excluded_dynamic = fixture.root() / "Cache" / "ignored" / "late.txt";
    Fixture::write(excluded_dynamic, "ignored\n");
    index.refresh_path(excluded_dynamic);
    expect(index.search(L"late.txt", 10).empty(), "refresh inside exclusion must stay excluded");
    expect(index.add_root(fixture.second_root()), "adding a second root should report change");
    wait_until_ready(index);
    expect(!index.search(L"second-root", 10).empty(), "new root should become searchable");
    expect(!index.add_root(fixture.second_root()), "duplicate root should be rejected");
    expect(index.remove_root(fixture.second_root()), "existing root should be removable");
    wait_until_ready(index);
    expect(index.search(L"second-root", 10).empty(), "removed root must disappear from results");
    expect(index.add_exclusion(L"docs"), "new exclusion should trigger rebuild");
    wait_until_ready(index);
    expect(index.search(L"architecture", 10).empty(),
           "new exclusion should remove matching subtree");
    expect(index.remove_exclusion(L"docs"), "existing exclusion should be removable");
    wait_until_ready(index);
    expect(!index.search(L"architecture", 10).empty(), "removed exclusion should restore subtree");
    const auto rebuild_generation = index.rebuild_tracked();
    expect(rebuild_generation > initial_generation, "tracked rebuild generation should advance");
    expect(index.wait_for_scan(rebuild_generation) == axiom::IndexScanWaitResult::succeeded,
           "tracked rebuild should complete through the event-driven scan gate");
    expect(index.snapshot().files == 3, "rebuild should preserve deterministic file count");
    expect(index.snapshot().incremental_updates == 0,
           "full rebuild should reset incremental counter");
}
} // namespace
int main() {
    try {
        run_tests();
        std::cout << "AxiomIndexTests: PASS\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "AxiomIndexTests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
