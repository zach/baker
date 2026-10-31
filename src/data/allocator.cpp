#include <baker/allocator.hpp>
#include <baker/error.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#define BAKER_COLD __declspec(noinline)
#else
#define BAKER_COLD [[gnu::noinline, gnu::cold]]
#endif

namespace baker {
namespace {

// Baker never asks for more than the default alignment of new, so free() needs no
// alignment to pick the matching delete.
void* new_alloc(void*, size_t size, size_t align, MemoryKind) {
    if (align > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
        detail::fatal("an allocation aligned past what the default allocator gives");
    return ::operator new(size, std::nothrow);
}

void new_free(void*, void* p, size_t, MemoryKind) {
    ::operator delete(p);
}

// The default allocator, one copy a kind: a reference to one stays valid.
constexpr Allocator tagged(Allocator a, size_t kind) noexcept {
    return with_kind(a, static_cast<MemoryKind>(kind));
}
constexpr Allocator kNew{new_alloc, new_free, nullptr};
Allocator g_default[kMemoryKinds] = {tagged(kNew, 0), tagged(kNew, 1), tagged(kNew, 2),
                                     tagged(kNew, 3), tagged(kNew, 4), tagged(kNew, 5),
                                     tagged(kNew, 6), tagged(kNew, 7), tagged(kNew, 8)};
static_assert(kMemoryKinds == static_cast<size_t>(MemoryKind::Tools) + 1);
std::atomic<bool> g_used{false};

const Allocator& used_default(MemoryKind kind) noexcept {
    if (!g_used.load(std::memory_order_relaxed)) g_used.store(true, std::memory_order_relaxed);
    return g_default[static_cast<size_t>(kind)];
}

}  // namespace

const char* memory_kind_name(MemoryKind kind) noexcept {
    static constexpr const char* kNames[kMemoryKinds] = {
        "other", "symbols", "trees", "config", "script", "tasks", "tweaks", "errors", "tools"};
    const auto i = static_cast<size_t>(kind);
    return i < kMemoryKinds ? kNames[i] : "other";
}

const Allocator& default_allocator(MemoryKind kind) noexcept {
    return used_default(kind);
}

const Allocator& default_allocator() noexcept {
    return used_default(MemoryKind::Other);
}

bool set_default_allocator(const Allocator& allocator) noexcept {
    if (g_used.load(std::memory_order_relaxed) || !allocator.alloc || !allocator.free) return false;
    for (size_t k = 0; k < kMemoryKinds; ++k) g_default[k] = tagged(allocator, k);
    return true;
}

namespace detail {

// Out of line, so the allocation it follows compiles as it would alone.
[[noreturn]] BAKER_COLD void allocation_failed(size_t size) noexcept {
    char bytes[32];
    const int n = std::snprintf(bytes, sizeof bytes, "%zu bytes", size);
    fatal("an allocator returned null, asked for ", {bytes, n > 0 ? static_cast<size_t>(n) : 0});
}

void* allocate(const Allocator& a, size_t size, size_t align) {
    void* p = a.alloc(a.user, size ? size : 1, align, a.kind);
    if (!p) allocation_failed(size);
    return p;
}

void* default_alloc(size_t size, size_t align, MemoryKind kind) {
    return allocate(used_default(kind), size, align);
}

void default_free(void* p, size_t size, MemoryKind kind) noexcept {
    deallocate(g_default[static_cast<size_t>(kind)], p, size);
}

}  // namespace detail
}  // namespace baker
