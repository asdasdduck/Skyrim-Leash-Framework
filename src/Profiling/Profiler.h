#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <source_location>
#include <string_view>

namespace LeashFramework::Profiling {
    class Metric;

    namespace Detail {
        [[nodiscard]] inline std::atomic<Metric*>& MetricHead() {
            static std::atomic<Metric*> head;
            return head;
        }
    }  // namespace Detail

    struct Snapshot {
        std::uint64_t calls{};
        std::int64_t totalNanoseconds{};
        std::int64_t maximumNanoseconds{};
    };

    class Metric {
    public:
        explicit Metric(std::string_view a_name) noexcept : _name(a_name) {
            auto* head = Detail::MetricHead().load(std::memory_order_relaxed);
            do {
                _next = head;
            } while (!Detail::MetricHead().compare_exchange_weak(head, this, std::memory_order_release, std::memory_order_relaxed));
        }

        void Record(std::int64_t a_nanoseconds) noexcept {
            _calls.fetch_add(1, std::memory_order_relaxed);
            _totalNanoseconds.fetch_add(a_nanoseconds, std::memory_order_relaxed);
            auto maximum = _maximumNanoseconds.load(std::memory_order_relaxed);
            while (a_nanoseconds > maximum && !_maximumNanoseconds.compare_exchange_weak(maximum, a_nanoseconds, std::memory_order_relaxed)) {
            }
        }

        [[nodiscard]] Snapshot TakeSnapshot() noexcept {
            return {.calls = _calls.exchange(0, std::memory_order_relaxed),
                .totalNanoseconds = _totalNanoseconds.exchange(0, std::memory_order_relaxed),
                .maximumNanoseconds = _maximumNanoseconds.exchange(0, std::memory_order_relaxed)};
        }

        [[nodiscard]] std::string_view Name() const noexcept { return _name; }
        [[nodiscard]] Metric* Next() const noexcept { return _next; }

    private:
        std::string_view _name;
        std::atomic<std::uint64_t> _calls{};
        std::atomic<std::int64_t> _totalNanoseconds{};
        std::atomic<std::int64_t> _maximumNanoseconds{};
        Metric* _next{};
    };

    class Scope {
    public:
        explicit Scope(Metric& a_metric) noexcept : _metric(a_metric), _start(Clock::now()) {}
        Scope(const Scope&) = delete;
        Scope(Scope&&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope& operator=(Scope&&) = delete;

        ~Scope() noexcept {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - _start).count();
            _metric.Record(elapsed);
        }

    private:
        using Clock = std::chrono::steady_clock;

        Metric& _metric;
        Clock::time_point _start;
    };

    inline void Report() {
        using Clock = std::chrono::steady_clock;
        constexpr auto reportInterval = std::chrono::seconds{5};

        const auto now = Clock::now();
        static auto previousReport = now;
        const auto elapsed = now - previousReport;
        if (elapsed < reportInterval) {
            return;
        }
        previousReport = now;

        const auto elapsedSeconds = std::chrono::duration<double>(elapsed).count();
        bool wroteHeader{};
        for (auto* metric = Detail::MetricHead().load(std::memory_order_acquire); metric; metric = metric->Next()) {
            const auto snapshot = metric->TakeSnapshot();
            if (snapshot.calls == 0) {
                continue;
            }
            if (!wroteHeader) {
                SKSE::log::info("[Profile] {:.2f}s sample", elapsedSeconds);
                wroteHeader = true;
            }
            const auto totalMilliseconds = static_cast<double>(snapshot.totalNanoseconds) / 1'000'000.0;
            const auto averageMilliseconds = totalMilliseconds / static_cast<double>(snapshot.calls);
            const auto maximumMilliseconds = static_cast<double>(snapshot.maximumNanoseconds) / 1'000'000.0;
            SKSE::log::info("[Profile] {} calls={} rate={:.1f}/s total={:.3f}ms avg={:.3f}ms max={:.3f}ms", metric->Name(), snapshot.calls, static_cast<double>(snapshot.calls) / elapsedSeconds, totalMilliseconds,
                averageMilliseconds, maximumMilliseconds);
        }
    }
}  // namespace LeashFramework::Profiling

#define LF_DETAIL_JOIN_IMPL(a_left, a_right) a_left##a_right
#define LF_DETAIL_JOIN(a_left, a_right) LF_DETAIL_JOIN_IMPL(a_left, a_right)
#define LF_DETAIL_PROFILE_SCOPE(a_name, a_id)                                                 \
    static ::LeashFramework::Profiling::Metric LF_DETAIL_JOIN(lfProfileMetric, a_id){a_name}; \
    [[maybe_unused]] const ::LeashFramework::Profiling::Scope LF_DETAIL_JOIN(lfProfileScope, a_id) { LF_DETAIL_JOIN(lfProfileMetric, a_id) }
#define LF_PROFILE_SCOPE(a_name) LF_DETAIL_PROFILE_SCOPE(a_name, __COUNTER__)
#define LF_PROFILE_FUNCTION() LF_PROFILE_SCOPE(std::source_location::current().function_name())
#define LF_PROFILE_REPORT() ::LeashFramework::Profiling::Report()
