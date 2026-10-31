#pragma once

// Allocator: where Baker's memory comes from. A plain struct
// of function pointers and a context: no virtual calls, nothing from the STL.
//
// Each Context and Config takes one at construction, and everything it keeps comes from
// it. What has no owner (symbols, results handed back, errors, and until arenas land,
// trees) comes from the default allocator, which the host may set once, before Baker
// first allocates. By default both are operator new.

#include <cstddef>
#include <cstdint>
#include <new>

namespace baker {

// What an allocation is for, so a host can count Baker's bytes per kind against its
// budgets. Each owner tags the copy of the allocator it keeps, and every allocation
// through that copy carries the tag.
enum class MemoryKind : uint8_t {
    Other,    // results handed back, Callbacks and Lists, whatever has no owner below
    Symbols,  // the symbol table: names, never freed
    Trees,    // what loads build: arenas, arrays, strings
    Config,   // a Config's layers, tables, snapshots and checks; LoadSessions; shapes
    Script,   // a Context: functions, variables, objects, natives, script bodies
    Tasks,    // Schedulers and the frames of their tasks
    Tweaks,   // a tweak's state beyond the tweak itself
    Errors,   // error records
    Tools,    // baker::tools and baker::host
};
inline constexpr size_t kMemoryKinds = 9;
// "symbols", "trees", ...: for a report.
[[nodiscard]] const char* memory_kind_name(MemoryKind kind) noexcept;

struct Allocator {
    // Never returns null: if it does, Baker aborts. `align` is a power of two.
    void* (*alloc)(void* user, size_t size, size_t align, MemoryKind kind) = nullptr;
    // `size` and `kind` are what was given to alloc for `p`.
    void (*free)(void* user, void* p, size_t size, MemoryKind kind) = nullptr;
    void* user = nullptr;  // must outlive everything allocated through it
    // What the allocations through this copy are for. Baker sets it on the copies it keeps,
    // so what a host sets here is replaced: a Context's allocator allocates as Script.
    MemoryKind kind = MemoryKind::Other;
};

// `a`, tagged `kind`.
[[nodiscard]] constexpr Allocator with_kind(Allocator a, MemoryKind kind) noexcept {
    a.kind = kind;
    return a;
}

// The process-wide default allocator, tagged `kind`. The reference stays valid, and is the
// same for the same kind, for the life of the program.
[[nodiscard]] const Allocator& default_allocator(MemoryKind kind) noexcept;
// The default allocator, tagged Other.
[[nodiscard]] const Allocator& default_allocator() noexcept;
// Replaces the default allocator. Only before Baker first allocates through it: after
// that, returns false and changes nothing.
bool set_default_allocator(const Allocator& allocator) noexcept;

namespace detail {
// Through an allocator, aborting if it returns null.
[[nodiscard]] void* allocate(const Allocator& a, size_t size, size_t align);
inline void deallocate(const Allocator& a, void* p, size_t size) noexcept {
    if (p) a.free(a.user, p, size, a.kind);
}
// Through the default allocator: for the header types, which have no owner.
[[nodiscard]] void* default_alloc(size_t size, size_t align, MemoryKind kind = MemoryKind::Other);
void default_free(void* p, size_t size, MemoryKind kind = MemoryKind::Other) noexcept;

// One T from `a`, constructed in place; and back.
template <class T, class... A>
[[nodiscard]] T* create(const Allocator& a, A&&... args) {
    return ::new (allocate(a, sizeof(T), alignof(T))) T(static_cast<A&&>(args)...);
}
template <class T>
void destroy(const Allocator& a, T* p) noexcept {
    if (!p) return;
    p->~T();
    a.free(a.user, p, sizeof(T), a.kind);
}
}  // namespace detail

}  // namespace baker
