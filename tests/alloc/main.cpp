// main for baker_alloc_tests: it installs the default allocator before doctest runs.

#include <doctest.h>

#include <baker/allocator.hpp>

#include <atomic>
#include <cstdlib>

// Allocations with no owner, counted, and by kind (test_allocators.cpp).
std::atomic<size_t> g_default_allocations{0};
std::atomic<size_t> g_default_by_kind[baker::kMemoryKinds] = {};

int main(int argc, char** argv) {
    // Before Baker allocates anything: allocations with no owner go through malloc, never
    // operator new, so the tests can watch operator new alone.
    const baker::Allocator heap{
        [](void*, size_t size, size_t, baker::MemoryKind kind) -> void* {
            g_default_allocations.fetch_add(1, std::memory_order_relaxed);
            g_default_by_kind[static_cast<size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
            return std::malloc(size);
        },
        [](void*, void* p, size_t, baker::MemoryKind) { std::free(p); }, nullptr};
    if (!baker::set_default_allocator(heap)) return 2;
    doctest::Context context(argc, argv);
    return context.run();
}
