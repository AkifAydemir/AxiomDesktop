#include "system_metrics.hpp"
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <algorithm>
namespace axiom {
namespace {
[[nodiscard]] std::uint64_t filetime_value(const FILETIME &value) noexcept {
    ULARGE_INTEGER combined{};
    combined.LowPart = value.dwLowDateTime;
    combined.HighPart = value.dwHighDateTime;
    return combined.QuadPart;
}
} // namespace
SystemMetricsSampler::SystemMetricsSampler(RuntimeScheduler &scheduler) : scheduler_{scheduler} {}
SystemMetricsSampler::~SystemMetricsSampler() {
    stop();
}
void SystemMetricsSampler::start() {
    if (task_id_ != 0) {
        return;
    }
    sample();
    task_id_ = scheduler_.schedule_every(std::chrono::seconds{1}, std::chrono::seconds{1},
                                         [this] { sample(); });
}
void SystemMetricsSampler::stop() noexcept {
    if (task_id_ == 0) {
        return;
    }
    (void)scheduler_.cancel(task_id_);
    task_id_ = 0;
}
SystemMetricsSnapshot SystemMetricsSampler::snapshot() const noexcept {
    std::scoped_lock lock{mutex_};
    return snapshot_;
}
void SystemMetricsSampler::sample() noexcept {
    SystemMetricsSnapshot next{};
    next.sampled_at = std::chrono::system_clock::now();
    next.uptime_ms = GetTickCount64();
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        next.physical_total_bytes = memory.ullTotalPhys;
        next.physical_available_bytes = memory.ullAvailPhys;
    }
    PROCESS_MEMORY_COUNTERS_EX process_memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&process_memory),
                             sizeof(process_memory))) {
        next.process_working_set_bytes = process_memory.WorkingSetSize;
    }
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const auto idle_value = filetime_value(idle);
        const auto kernel_value = filetime_value(kernel);
        const auto user_value = filetime_value(user);
        if (have_cpu_baseline_) {
            const auto idle_delta = idle_value - previous_idle_;
            const auto kernel_delta = kernel_value - previous_kernel_;
            const auto user_delta = user_value - previous_user_;
            const auto total_delta = kernel_delta + user_delta;
            if (total_delta > 0) {
                const double busy =
                    static_cast<double>(total_delta - std::min(idle_delta, total_delta));
                next.cpu_percent =
                    std::clamp((busy * 100.0) / static_cast<double>(total_delta), 0.0, 100.0);
                next.ready = true;
            }
        }
        previous_idle_ = idle_value;
        previous_kernel_ = kernel_value;
        previous_user_ = user_value;
        have_cpu_baseline_ = true;
    }
    std::scoped_lock lock{mutex_};
    snapshot_ = next;
}
} // namespace axiom
#endif
