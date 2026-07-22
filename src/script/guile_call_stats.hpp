#pragma once

#include <atomic>
#include <cstdint>

namespace cind::guile_stats {

// Instrumentation for the state-ownership inversion measurements
// (design/09-guile-first.md §7). `run_guile_call` is the single choke point for
// every C++ -> Guile entry, so counting there yields the calls and the time a
// keystroke actually spends inside Scheme. Host primitives invoked from Scheme
// are already inside the VM and are not counted; a primitive that calls policy
// back nests, so only the outermost entry contributes time.
inline std::atomic<std::uint64_t> call_count{0};
inline std::atomic<std::uint64_t> call_nanoseconds{0};
// Counting is free; timing costs two clock reads per outermost call, so the
// measurement tool opts in and ordinary editing sessions do not pay for it.
inline std::atomic<bool> timing_enabled{false};

struct Sample {
    std::uint64_t calls = 0;
    std::uint64_t nanoseconds = 0;
};

inline Sample sample() {
    return {.calls = call_count.load(std::memory_order_relaxed),
            .nanoseconds = call_nanoseconds.load(std::memory_order_relaxed)};
}

inline Sample since(const Sample& previous) {
    const Sample current = sample();
    return {.calls = current.calls - previous.calls,
            .nanoseconds = current.nanoseconds - previous.nanoseconds};
}

} // namespace cind::guile_stats
