#pragma once

// Test-only global allocation counter: allocation counts are enforced by a test, not by
// eyeball. Linking alloc_counter.cpp replaces the global operator new.

#include <cstddef>

namespace baker::test {

// False when operator new cannot be replaced: ThreadSanitizer's runtime defines it.
[[nodiscard]] bool counting_allocations() noexcept;

// Allocations through operator new since the program started (0 if not counting).
[[nodiscard]] size_t allocations() noexcept;

// Counts the allocations made during its lifetime.
class AllocationScope {
public:
    AllocationScope() noexcept : start_(allocations()) {}
    [[nodiscard]] size_t count() const noexcept { return allocations() - start_; }

private:
    size_t start_;
};

}  // namespace baker::test
