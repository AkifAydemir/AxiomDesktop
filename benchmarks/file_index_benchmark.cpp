#include "file_index.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
using Clock = std::chrono::steady_clock;
void wait_until_ready(axiom::FileIndex &index) {
    const auto generation = index.current_scan_generation();
    if (generation == 0) {
        throw std::runtime_error{"File index scan generation was not created."};
    }
    const auto result = index.wait_for_scan(generation);
    if (result != axiom::IndexScanWaitResult::succeeded) {
        throw std::runtime_error{"File index failed or was superseded during benchmark."};
    }
}
[[nodiscard]] double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::clamp(fraction, 0.0, 1.0) *
                                                static_cast<double>(values.size() - 1));
    return values[index];
}
class Fixture final {
  public:
    Fixture(std::size_t directories, std::size_t files_per_directory) {
        const auto stamp = Clock::now().time_since_epoch().count();
        root_ =
            std::filesystem::temp_directory_path() / ("axiom-benchmark-" + std::to_string(stamp));
        std::filesystem::create_directories(root_);
        for (std::size_t dir = 0; dir < directories; ++dir) {
            const auto directory = root_ / ("Project_" + std::to_string(dir));
            std::filesystem::create_directories(directory);
            for (std::size_t file = 0; file < files_per_directory; ++file) {
                const auto path = directory / ("module_" + std::to_string(dir) + "_file_" +
                                               std::to_string(file) + ".cpp");
                std::ofstream stream{path, std::ios::binary};
                stream << "// synthetic benchmark fixture\n";
            }
        }
        std::filesystem::create_directories(root_ / "Documents");
        std::ofstream{root_ / "Documents" / "Axiom Architecture Final.pdf", std::ios::binary}
            << "benchmark\n";
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }
    [[nodiscard]] const std::filesystem::path &root() const noexcept {
        return root_;
    }

  private:
    std::filesystem::path root_;
};
} // namespace
int main() {
    try {
        constexpr std::size_t directories = 100;
        constexpr std::size_t files_per_directory = 50;
        constexpr std::size_t query_iterations = 2500;
        Fixture fixture{directories, files_per_directory};
        axiom::FileIndex index;
        const auto scan_started = Clock::now();
        index.start({fixture.root()});
        wait_until_ready(index);
        const auto scan_elapsed =
            std::chrono::duration<double, std::milli>(Clock::now() - scan_started).count();
        const std::vector<std::wstring> queries{
            L"module_42_file_17.cpp", L"module 87 file 3", L"axiom architecture",
            L"modl_51_fl_12",         L"file_49",
        };
        std::vector<double> latencies_us;
        latencies_us.reserve(query_iterations);
        std::size_t hit_count = 0;
        for (std::size_t i = 0; i < query_iterations; ++i) {
            const auto &query = queries[i % queries.size()];
            const auto started = Clock::now();
            const auto hits = index.search(query, 20);
            const auto elapsed =
                std::chrono::duration<double, std::micro>(Clock::now() - started).count();
            latencies_us.push_back(elapsed);
            hit_count += hits.size();
        }
        const auto dynamic_file = fixture.root() / "Documents" / "live-benchmark.json";
        std::ofstream{dynamic_file, std::ios::binary} << "{}\n";
        const auto incremental_started = Clock::now();
        index.refresh_path(dynamic_file);
        const auto incremental_us =
            std::chrono::duration<double, std::micro>(Clock::now() - incremental_started).count();
        const auto snapshot = index.snapshot();
        const double average = std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0) /
                               static_cast<double>(latencies_us.size());
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Axiom FileIndex benchmark\n";
        std::cout << "entries=" << snapshot.entries << " files=" << snapshot.files
                  << " directories=" << snapshot.directories << '\n';
        std::cout << "initial_scan_ms=" << scan_elapsed << '\n';
        std::cout << "query_iterations=" << query_iterations << " aggregate_hits=" << hit_count
                  << '\n';
        std::cout << "query_avg_us=" << average << '\n';
        std::cout << "query_p50_us=" << percentile(latencies_us, 0.50) << '\n';
        std::cout << "query_p95_us=" << percentile(latencies_us, 0.95) << '\n';
        std::cout << "query_p99_us=" << percentile(latencies_us, 0.99) << '\n';
        std::cout << "incremental_refresh_us=" << incremental_us << '\n';
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "AxiomIndexBenchmark: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
