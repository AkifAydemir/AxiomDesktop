#pragma once
#ifdef _WIN32
#include "runtime.hpp"
#include <chrono>
#include <cstdint>
#include <mutex>
namespace axiom {
struct SystemMetricsSnapshot {
    bool ready{};
    double cpu_percent{};
    std::uint64_t physical_total_bytes{};
    std::uint64_t physical_available_bytes{};
    std::uint64_t process_working_set_bytes{};
    std::uint64_t uptime_ms{};
    std::chrono::system_clock::time_point sampled_at{};
};
class SystemMetricsSampler final {
  public:
    explicit SystemMetricsSampler(RuntimeScheduler &scheduler);
    ~SystemMetricsSampler();
    SystemMetricsSampler(const SystemMetricsSampler &) = delete;
    SystemMetricsSampler &operator=(const SystemMetricsSampler &) = delete;
    void start();
    void stop() noexcept;
    [[nodiscard]] SystemMetricsSnapshot snapshot() const noexcept;

  private:
    RuntimeScheduler &scheduler_;
    mutable std::mutex mutex_;
    SystemMetricsSnapshot snapshot_{};
    RuntimeTaskId task_id_{};
    std::uint64_t previous_idle_{};
    std::uint64_t previous_kernel_{};
    std::uint64_t previous_user_{};
    bool have_cpu_baseline_{};
    void sample() noexcept;
};
} // namespace axiom
#endif
