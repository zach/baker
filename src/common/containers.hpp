#pragma once

// The container alias layer. Code in src/ uses Vec, Str, and
// HashMap, never std::vector, std::string, or std::unordered_map. They are the STL's by
// default, or EASTL's with BAKER_USE_EASTL=ON, and always allocate through a Baker
// Allocator: the default allocator unless the owner passes its own.

#include <baker/allocator.hpp>
#include <baker/symbol.hpp>
#include <baker/text.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <type_traits>
#include <utility>

#if BAKER_USE_EASTL
#include <EASTL/hash_map.h>
#include <EASTL/string.h>
#include <EASTL/vector.h>
#else
#include <string>
#include <unordered_map>
#include <vector>
#endif

namespace baker::detail {

// Hashes for HashMap keys, the same under either backend.
template <class K>
struct Hash;
template <>
struct Hash<Symbol> {
    size_t operator()(Symbol s) const noexcept { return static_cast<size_t>(s.hash()); }
};
template <>
struct Hash<uint64_t> {  // a key that is already a hash, or close to one
    size_t operator()(uint64_t x) const noexcept { return static_cast<size_t>(x ^ (x >> 29)); }
};
template <class T>
struct Hash<T*> {
    size_t operator()(const T* p) const noexcept {
        const auto x = reinterpret_cast<uintptr_t>(p);
        return static_cast<size_t>(x ^ (x >> 7));
    }
};

#if BAKER_USE_EASTL

class EastlAllocator {
public:
    explicit EastlAllocator(const char* = nullptr) noexcept : a_(&default_allocator()) {}
    EastlAllocator(const Allocator& a) noexcept : a_(&a) {}  // NOLINT(google-explicit-constructor)

    void* allocate(size_t n, int = 0) {
        return detail::allocate(*a_, n, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    }
    void* allocate(size_t n, size_t align, size_t offset, int = 0) {
        if (offset != 0) std::abort();  // only EASTL's fixed containers ask for an offset
        return detail::allocate(*a_, n, align);
    }
    void deallocate(void* p, size_t n) noexcept { detail::deallocate(*a_, p, n); }
    [[nodiscard]] const char* get_name() const noexcept { return "baker"; }
    void set_name(const char*) noexcept {}

    friend bool operator==(const EastlAllocator& x, const EastlAllocator& y) noexcept {
        return x.a_ == y.a_;
    }
    friend bool operator!=(const EastlAllocator& x, const EastlAllocator& y) noexcept {
        return x.a_ != y.a_;
    }

private:
    const Allocator* a_;
};

template <class T>
using Vec = eastl::vector<T, EastlAllocator>;
template <class K, class V>
using HashMap = eastl::hash_map<K, V, Hash<K>, eastl::equal_to<K>, EastlAllocator>;
using BasicStr = eastl::basic_string<char, EastlAllocator>;

#else

// A std allocator over a Baker Allocator. Copies keep their source's allocator, and
// assignment never moves one: a table keeps its owner's allocator whatever is assigned in.
template <class T>
class StdAllocator {
public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::false_type;
    using propagate_on_container_swap = std::false_type;
    using is_always_equal = std::false_type;

    StdAllocator() noexcept : a_(&default_allocator()) {}
    StdAllocator(const Allocator& a) noexcept : a_(&a) {}  // NOLINT(google-explicit-constructor)
    template <class U>
    StdAllocator(const StdAllocator<U>& o) noexcept : a_(o.allocator()) {}  // NOLINT

    // NOLINTBEGIN(bugprone-sizeof-expression): T is a pointer in a table of pointers
    [[nodiscard]] T* allocate(size_t n) {
        return static_cast<T*>(detail::allocate(*a_, n * sizeof(T), alignof(T)));
    }
    void deallocate(T* p, size_t n) noexcept { detail::deallocate(*a_, p, n * sizeof(T)); }
    // NOLINTEND(bugprone-sizeof-expression)
    [[nodiscard]] const Allocator* allocator() const noexcept { return a_; }

    template <class U>
    friend bool operator==(const StdAllocator& x, const StdAllocator<U>& y) noexcept {
        return x.a_ == y.allocator();
    }

private:
    const Allocator* a_;
};

template <class T>
using Vec = std::vector<T, StdAllocator<T> >;
template <class K, class V>
using HashMap =
    std::unordered_map<K, V, Hash<K>, std::equal_to<K>, StdAllocator<std::pair<const K, V> > >;
using BasicStr = std::basic_string<char, std::char_traits<char>, StdAllocator<char> >;

#endif

// A string: the backend's, behind the few operations Baker uses, so that both backends
// read and convert to std::string_view the same way.
class Str {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);

    Str() = default;
    explicit Str(const Allocator& a) : s_(a) {}
    Str(std::string_view v) : s_(v.data(), v.size()) {}  // NOLINT(google-explicit-constructor)
    Str(const char* c) : Str(std::string_view(c)) {}     // NOLINT(google-explicit-constructor)
    Str(std::string_view v, const Allocator& a) : s_(v.data(), v.size(), a) {}
    Str(const char* first, const char* last)
        : Str(std::string_view(first, static_cast<size_t>(last - first))) {}
    Str(size_t n, char c) : s_(n, c) {}

    operator std::string_view() const noexcept { return {s_.data(), s_.size()}; }  // NOLINT
    [[nodiscard]] std::string_view view() const noexcept { return *this; }
    [[nodiscard]] const char* c_str() const noexcept { return s_.c_str(); }
    [[nodiscard]] const char* data() const noexcept { return s_.data(); }
    [[nodiscard]] char* data() noexcept { return s_.data(); }
    [[nodiscard]] size_t size() const noexcept { return s_.size(); }
    [[nodiscard]] bool empty() const noexcept { return s_.empty(); }
    [[nodiscard]] char operator[](size_t i) const noexcept { return s_[i]; }
    [[nodiscard]] char& operator[](size_t i) noexcept { return s_[i]; }
    [[nodiscard]] char back() const noexcept { return s_.back(); }
    [[nodiscard]] const char* begin() const noexcept { return s_.data(); }
    [[nodiscard]] const char* end() const noexcept { return s_.data() + s_.size(); }
    [[nodiscard]] char* begin() noexcept { return s_.data(); }
    [[nodiscard]] char* end() noexcept { return s_.data() + s_.size(); }

    void clear() noexcept { s_.clear(); }
    void reserve(size_t n) { s_.reserve(n); }
    void resize(size_t n, char c = '\0') { s_.resize(n, c); }
    void push_back(char c) { s_.push_back(c); }
    void pop_back() { s_.pop_back(); }
    Str& append(std::string_view v) {
        s_.append(v.data(), v.size());
        return *this;
    }
    Str& append(size_t n, char c) {
        s_.append(n, c);
        return *this;
    }
    Str& assign(std::string_view v) {
        s_.assign(v.data(), v.size());
        return *this;
    }
    Str& operator=(std::string_view v) { return assign(v); }
    Str& operator=(const char* c) { return assign(c); }
    Str& operator+=(std::string_view v) { return append(v); }
    Str& operator+=(const Str& v) { return append(v.view()); }
    Str& operator+=(const char* c) { return append(c); }
    Str& operator+=(char c) {
        s_.push_back(c);
        return *this;
    }
    Str& append(const char* first, const char* last) {
        return append(std::string_view(first, static_cast<size_t>(last - first)));
    }
    void insert(size_t at, std::string_view v) { s_.insert(at, v.data(), v.size()); }
    void replace(size_t at, size_t n, std::string_view v) {
        erase(at, n);
        insert(at, v);
    }
    void erase(size_t at, size_t n = npos) { s_.erase(at, n == npos ? s_.size() - at : n); }

    [[nodiscard]] size_t find(char c, size_t from = 0) const noexcept {
        return view().find(c, from);
    }
    [[nodiscard]] size_t find(std::string_view v, size_t from = 0) const noexcept {
        return view().find(v, from);
    }
    [[nodiscard]] size_t rfind(char c, size_t from = npos) const noexcept {
        return view().rfind(c, from);
    }
    [[nodiscard]] size_t find_first_of(std::string_view v, size_t from = 0) const noexcept {
        return view().find_first_of(v, from);
    }
    [[nodiscard]] Str substr(size_t at, size_t n = npos) const { return view().substr(at, n); }
    [[nodiscard]] bool starts_with(std::string_view v) const noexcept {
        return view().starts_with(v);
    }
    [[nodiscard]] bool ends_with(std::string_view v) const noexcept { return view().ends_with(v); }

    friend bool operator==(const Str& a, const Str& b) noexcept { return a.view() == b.view(); }
    friend bool operator==(const Str& a, std::string_view b) noexcept { return a.view() == b; }
    friend bool operator==(const Str& a, const char* b) noexcept { return a.view() == b; }
    friend auto operator<=>(const Str& a, const Str& b) noexcept { return a.view() <=> b.view(); }
    friend Str operator+(Str a, std::string_view b) { return std::move(a.append(b)); }
    friend Str operator+(Str a, const Str& b) { return std::move(a.append(b.view())); }
    friend Str operator+(Str a, const char* b) { return std::move(a.append(b)); }
    friend Str operator+(Str a, char b) { return std::move(a += b); }
    friend Str operator+(std::string_view a, const Str& b) { return Str(a).append(b.view()); }
    friend Str operator+(const char* a, const Str& b) { return Str(a).append(b.view()); }

private:
    BasicStr s_;
};

template <>
struct Hash<Str> {
    size_t operator()(const Str& s) const noexcept {
        return static_cast<size_t>(hash_symbol_text(s.view()));
    }
};

// Decimal text of an integer, as std::to_string would give.
[[nodiscard]] Str decimal(int64_t value);

// A TextSink that appends to `s`.
[[nodiscard]] inline TextSink str_out(Str& s) {
    return {[](void* u, std::string_view t) { static_cast<Str*>(u)->append(t); }, &s};
}

// One owned T, from an allocator: what std::unique_ptr would be.
template <class T>
class Own {
public:
    Own() noexcept = default;
    Own(Own&& o) noexcept : p_(o.p_), a_(o.a_) { o.p_ = nullptr; }
    Own& operator=(Own&& o) noexcept {
        std::swap(p_, o.p_);
        std::swap(a_, o.a_);
        return *this;
    }
    ~Own() {
        if (p_) destroy(*a_, p_);
    }
    template <class... A>
    [[nodiscard]] static Own make(const Allocator& a, A&&... args) {
        Own o;
        o.p_ = create<T>(a, std::forward<A>(args)...);
        o.a_ = &a;
        return o;
    }
    [[nodiscard]] T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    T& operator*() const noexcept { return *p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }

private:
    T* p_ = nullptr;
    const Allocator* a_ = nullptr;
};

// A shared T, from an allocator: what std::shared_ptr<T> would be. Baker shares only what
// is finished being built, and reads it.
template <class T>
class Rc {
public:
    Rc() noexcept = default;
    Rc(std::nullptr_t) noexcept {}  // NOLINT(google-explicit-constructor)
    Rc(const Rc& o) noexcept : b_(o.b_) {
        if (b_) b_->refs.fetch_add(1, std::memory_order_relaxed);
    }
    Rc& operator=(Rc o) noexcept {
        std::swap(b_, o.b_);
        return *this;
    }
    ~Rc() {
        if (b_ && b_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) destroy(*b_->a, b_);
    }
    template <class... A>
    [[nodiscard]] static Rc make(const Allocator& a, A&&... args) {
        Rc r;
        r.b_ = create<Block>(a, a, std::forward<A>(args)...);
        return r;
    }
    [[nodiscard]] T* get() const noexcept { return b_ ? &b_->value : nullptr; }
    T* operator->() const noexcept { return &b_->value; }
    T& operator*() const noexcept { return b_->value; }
    explicit operator bool() const noexcept { return b_ != nullptr; }

private:
    struct Block {
        template <class... A>
        explicit Block(const Allocator& alloc, A&&... args)
            : value(std::forward<A>(args)...), a(&alloc) {}
        T value;
        const Allocator* a;
        std::atomic<uint32_t> refs{1};
    };
    Block* b_ = nullptr;
};

}  // namespace baker::detail

namespace baker {
using detail::HashMap;
using detail::Own;
using detail::Rc;
using detail::Str;
using detail::Vec;
}  // namespace baker
