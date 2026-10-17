#pragma once

// Allocator: where Baker's memory comes from. A plain struct
// of function pointers and a context: no virtual calls, nothing from the STL.
//
// Each Context and Config takes one at construction, and everything it keeps comes from
// it. What has no owner (symbols, results handed back, errors, and until arenas land,
// trees) comes from the default allocator, which the host may set once, before Baker
// first allocates. By default both are operator new.

#include <cstddef>
#include <new>

namespace baker {

struct Allocator {
    // Never returns null: if it does, Baker aborts. `align` is a power of two.
    void* (*alloc)(void* user, size_t size, size_t align) = nullptr;
    // `size` is what was asked of alloc for `p`.
    void (*free)(void* user, void* p, size_t size) = nullptr;
    void* user = nullptr;  // must outlive everything allocated through it
};

// The process-wide default allocator.
[[nodiscard]] const Allocator& default_allocator() noexcept;
// Replaces the default allocator. Only before Baker first allocates through it: after
// that, returns false and changes nothing.
bool set_default_allocator(const Allocator& allocator) noexcept;

namespace detail {
// Through an allocator, aborting if it returns null.
[[nodiscard]] void* allocate(const Allocator& a, size_t size, size_t align);
inline void deallocate(const Allocator& a, void* p, size_t size) noexcept {
    if (p) a.free(a.user, p, size);
}
// Through the default allocator: for the header types, which have no owner.
[[nodiscard]] void* default_alloc(size_t size, size_t align);
void default_free(void* p, size_t size) noexcept;

// One T from `a`, constructed in place; and back.
template <class T, class... A>
[[nodiscard]] T* create(const Allocator& a, A&&... args) {
    return ::new (allocate(a, sizeof(T), alignof(T))) T(static_cast<A&&>(args)...);
}
template <class T>
void destroy(const Allocator& a, T* p) noexcept {
    if (!p) return;
    p->~T();
    a.free(a.user, p, sizeof(T));
}
}  // namespace detail

}  // namespace baker
