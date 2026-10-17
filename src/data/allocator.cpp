#include <baker/allocator.hpp>

#include <atomic>
#include <cstdlib>
#include <new>

namespace baker {
namespace {

// Baker never asks for more than the default alignment of new, so free() needs no
// alignment to pick the matching delete.
void* new_alloc(void*, size_t size, size_t align) {
    if (align > __STDCPP_DEFAULT_NEW_ALIGNMENT__) std::abort();
    return ::operator new(size, std::nothrow);
}

void new_free(void*, void* p, size_t) {
    ::operator delete(p);
}

Allocator g_default{new_alloc, new_free, nullptr};
std::atomic<bool> g_used{false};

const Allocator& used_default() noexcept {
    if (!g_used.load(std::memory_order_relaxed)) g_used.store(true, std::memory_order_relaxed);
    return g_default;
}

}  // namespace

const Allocator& default_allocator() noexcept {
    return used_default();
}

bool set_default_allocator(const Allocator& allocator) noexcept {
    if (g_used.load(std::memory_order_relaxed) || !allocator.alloc || !allocator.free) return false;
    g_default = allocator;
    return true;
}

namespace detail {

void* allocate(const Allocator& a, size_t size, size_t align) {
    void* p = a.alloc(a.user, size ? size : 1, align);
    if (!p) std::abort();
    return p;
}

void* default_alloc(size_t size, size_t align) {
    return allocate(used_default(), size, align);
}

void default_free(void* p, size_t size) noexcept {
    deallocate(g_default, p, size);
}

}  // namespace detail
}  // namespace baker
