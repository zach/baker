#pragma once

// TimeSource: where Baker reads time, for reload telemetry and call statistics. A test
// can pass a fake clock and check exact sums.

#include <cstdint>

namespace baker {

// Monotonic nanoseconds from an arbitrary origin.
using TimeSource = uint64_t (*)(void* user);

// The default: std::chrono::steady_clock, in nanoseconds.
[[nodiscard]] uint64_t steady_now(void* user = nullptr) noexcept;

}  // namespace baker
