#pragma once

// Ref<T>: intrusive reference-counted pointer.
//
// Refcounts are atomic by default, so Nodes and Refs may be copied and dropped on any
// thread; mutation is still single-threaded. Define BAKER_ATOMIC_REFCOUNT=0 (CMake:
// -DBAKER_ATOMIC_REFCOUNT=OFF) for plain counters in single-threaded programs.
// Every translation unit must agree on the setting.
//
// Arrays and strings are not counted one by one: each belongs to an owner, a load's arena or
// a solo block of one, and a reference counts the owner. A Ref<T>
// calls T's ref() / unref() members.

#include <cstdint>
#include <cstring>
#include <utility>

#if !defined(BAKER_ATOMIC_REFCOUNT) || BAKER_ATOMIC_REFCOUNT
#define BAKER_ATOMIC_REFCOUNT_ENABLED 1
#include <atomic>
#else
#define BAKER_ATOMIC_REFCOUNT_ENABLED 0
#endif

namespace baker {

namespace detail {

#if BAKER_ATOMIC_REFCOUNT_ENABLED
using RefCount = std::atomic<uint32_t>;
inline void retain(RefCount& c) noexcept {
    c.fetch_add(1, std::memory_order_relaxed);
}
// Returns true when the count reached zero and the object must be destroyed.
[[nodiscard]] inline bool release(RefCount& c) noexcept {
    return c.fetch_sub(1, std::memory_order_acq_rel) == 1;
}
[[nodiscard]] inline uint32_t count(const RefCount& c) noexcept {
    return c.load(std::memory_order_relaxed);
}
#else
using RefCount = uint32_t;
inline void retain(RefCount& c) noexcept {
    ++c;
}
[[nodiscard]] inline bool release(RefCount& c) noexcept {
    return --c == 0;
}
[[nodiscard]] inline uint32_t count(const RefCount& c) noexcept {
    return c;
}
#endif

// What frees arrays and strings: a load's arena, or a solo block holding one of them.
struct Owner {
    RefCount refs{1};
    uint32_t kind;  // OwnerKind
};
enum OwnerKind : uint32_t { kSoloArray, kSoloString, kArena };

// An array or string starts with the 32-bit offset back to its block, and a block starts
// with a pointer to its owner.
[[nodiscard]] inline Owner& owner_of(const void* payload) noexcept {
    uint32_t offset;
    std::memcpy(&offset, payload, sizeof offset);
    Owner* owner;
    // NOLINTNEXTLINE(bugprone-sizeof-expression): the owner pointer stored before the payload
    std::memcpy(&owner, static_cast<const char*>(payload) - offset, sizeof owner);
    return *owner;
}
class Arena;
struct CacheLayout;  // the binary cache's view of the layouts (cache.cpp)
// Frees an owner whose count reached zero.
void free_owner(Owner& owner) noexcept;
inline void retain_payload(const void* payload) noexcept {
    retain(owner_of(payload).refs);
}
inline void release_payload(const void* payload) noexcept {
    Owner& owner = owner_of(payload);
    if (release(owner.refs)) free_owner(owner);
}

}  // namespace detail

template <class T>
class Ref {
public:
    constexpr Ref() noexcept = default;
    constexpr Ref(std::nullptr_t) noexcept {}  // NOLINT(google-explicit-constructor)
    // Shares ownership of `p` (increments its count).
    explicit Ref(T* p) noexcept : p_(p) {
        if (p_) p_->ref();
    }
    // Takes over a reference the caller already owns (no increment).
    [[nodiscard]] static Ref adopt(T* p) noexcept {
        Ref r;
        r.p_ = p;
        return r;
    }

    Ref(const Ref& o) noexcept : p_(o.p_) {
        if (p_) p_->ref();
    }
    Ref(Ref&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    Ref& operator=(const Ref& o) noexcept {  // NOLINT(bugprone-unhandled-self-assignment)
        Ref(o).swap(*this);                  // copy and swap: safe for self-assignment
        return *this;
    }
    Ref& operator=(Ref&& o) noexcept {
        Ref(std::move(o)).swap(*this);
        return *this;
    }
    ~Ref() {
        if (p_) p_->unref();
    }

    [[nodiscard]] T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    T& operator*() const noexcept { return *p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }

    // Gives up ownership without decrementing.
    [[nodiscard]] T* release() noexcept { return std::exchange(p_, nullptr); }
    void reset() noexcept { Ref().swap(*this); }
    void swap(Ref& o) noexcept { std::swap(p_, o.p_); }

    friend bool operator==(const Ref& a, const Ref& b) noexcept { return a.p_ == b.p_; }
    friend bool operator==(const Ref& a, std::nullptr_t) noexcept { return a.p_ == nullptr; }

private:
    T* p_ = nullptr;
};

}  // namespace baker
