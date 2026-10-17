#include <baker/clock.hpp>

#include <chrono>

namespace baker {

uint64_t steady_now(void*) noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

}  // namespace baker
