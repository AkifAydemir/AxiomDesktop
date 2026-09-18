#include "execution_diagnostics.hpp"
#include <algorithm>
#include <stdexcept>
namespace axiom {
namespace {
[[nodiscard]] bool same_subject(std::wstring_view lhs, std::wstring_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        wchar_t a = lhs[i];
        wchar_t b = rhs[i];
        if (a >= L'A' && a <= L'Z')
            a = static_cast<wchar_t>(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z')
            b = static_cast<wchar_t>(b - L'A' + L'a');
        if (a != b)
            return false;
    }
    return true;
}
} // namespace
ExecutionDiagnostics::ExecutionDiagnostics(std::size_t capacity) : capacity_{capacity} {
    if (capacity_ == 0) {
        throw std::invalid_argument{"ExecutionDiagnostics capacity must be positive."};
    }
}
std::uint64_t ExecutionDiagnostics::record(DiagnosticSeverity severity, DiagnosticDomain domain,
                                           std::wstring subject, std::wstring message,
                                           std::chrono::milliseconds duration) {
    std::scoped_lock lock{mutex_};
    const auto id = next_id_++;
    events_.push_back(DiagnosticEvent{
        id,
        std::chrono::system_clock::now(),
        severity,
        domain,
        std::move(subject),
        std::move(message),
        duration < std::chrono::milliseconds::zero() ? std::chrono::milliseconds::zero() : duration,
    });
    ++total_recorded_;
    if (severity == DiagnosticSeverity::error) {
        ++total_errors_;
    }
    while (events_.size() > capacity_) {
        events_.pop_front();
        ++dropped_oldest_;
    }
    return id;
}
std::vector<DiagnosticEvent> ExecutionDiagnostics::recent(std::size_t limit) const {
    std::scoped_lock lock{mutex_};
    limit = std::min(limit, events_.size());
    std::vector<DiagnosticEvent> output;
    output.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
        output.push_back(events_[events_.size() - 1 - i]);
    }
    return output;
}
std::vector<DiagnosticEvent> ExecutionDiagnostics::for_subject(DiagnosticDomain domain,
                                                               std::wstring_view subject,
                                                               std::size_t limit) const {
    std::scoped_lock lock{mutex_};
    std::vector<DiagnosticEvent> output;
    output.reserve(std::min(limit, events_.size()));
    for (auto it = events_.rbegin(); it != events_.rend() && output.size() < limit; ++it) {
        if (it->domain == domain && same_subject(it->subject, subject)) {
            output.push_back(*it);
        }
    }
    return output;
}
std::optional<DiagnosticEvent> ExecutionDiagnostics::last_error(DiagnosticDomain domain,
                                                                std::wstring_view subject) const {
    std::scoped_lock lock{mutex_};
    for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
        if (it->severity != DiagnosticSeverity::error || it->domain != domain) {
            continue;
        }
        if (subject.empty() || same_subject(it->subject, subject)) {
            return *it;
        }
    }
    return std::nullopt;
}
DiagnosticsStats ExecutionDiagnostics::stats() const noexcept {
    std::scoped_lock lock{mutex_};
    return DiagnosticsStats{
        events_.size(), capacity_, total_recorded_, total_errors_, dropped_oldest_,
    };
}
void ExecutionDiagnostics::clear() noexcept {
    std::scoped_lock lock{mutex_};
    events_.clear();
}
std::wstring_view diagnostic_severity_name(DiagnosticSeverity severity) noexcept {
    switch (severity) {
    case DiagnosticSeverity::info:
        return L"info";
    case DiagnosticSeverity::warning:
        return L"warning";
    case DiagnosticSeverity::error:
        return L"error";
    }
    return L"unknown";
}
std::wstring_view diagnostic_domain_name(DiagnosticDomain domain) noexcept {
    switch (domain) {
    case DiagnosticDomain::runtime:
        return L"runtime";
    case DiagnosticDomain::action:
        return L"action";
    case DiagnosticDomain::automation:
        return L"automation";
    case DiagnosticDomain::plugin:
        return L"plugin";
    }
    return L"unknown";
}
} // namespace axiom
