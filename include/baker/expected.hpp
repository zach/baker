#pragma once

// baker::expected / baker::unexpected: std::expected when the standard library has it,
// otherwise a minimal shim with the subset of the interface Baker uses: has_value, value,
// operator*, value_or, error, and_then, and comparison with a value.
//
// Compiled Baker functions return Expected<T>, so the library and every translation unit
// using it must make the same choice. CMake decides once, when configuring the library,
// and exports BAKER_STD_EXPECTED=0/1 to consumers. Without it,
// the choice follows the current translation unit.

#include <version>

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#define BAKER_LIB_HAS_STD_EXPECTED 1
#else
#define BAKER_LIB_HAS_STD_EXPECTED 0
#endif

#if defined(BAKER_STD_EXPECTED)
#if BAKER_STD_EXPECTED && !BAKER_LIB_HAS_STD_EXPECTED
#error "baker was built with std::expected (C++23); compile this code as C++23 too"
#endif
#define BAKER_HAS_STD_EXPECTED BAKER_STD_EXPECTED
#else
#define BAKER_HAS_STD_EXPECTED BAKER_LIB_HAS_STD_EXPECTED
#endif

#if BAKER_HAS_STD_EXPECTED
#include <expected>
#else
#include <new>
#include <type_traits>
#include <utility>
#endif

#include <cstdlib>

namespace baker {

#if BAKER_HAS_STD_EXPECTED

template <class T, class E>
using expected = std::expected<T, E>;
template <class E>
using unexpected = std::unexpected<E>;

#else

template <class E>
class unexpected {
public:
    explicit unexpected(E e) : error_(std::move(e)) {}
    [[nodiscard]] const E& error() const& noexcept { return error_; }
    [[nodiscard]] E& error() & noexcept { return error_; }
    [[nodiscard]] E&& error() && noexcept { return std::move(error_); }

private:
    E error_;
};

namespace detail {
template <class E>
[[noreturn]] void throw_bad_expected_access(const E& e);
}  // namespace detail

template <class T, class E>
class expected {
public:
    using value_type = T;
    using error_type = E;

    expected() : ok_(true) { new (&v_) T(); }
    expected(const T& v) : ok_(true) { new (&v_) T(v); }  // NOLINT(google-explicit-constructor)
    expected(T&& v) : ok_(true) { new (&v_) T(std::move(v)); }  // NOLINT
    template <class G>
    expected(unexpected<G> u) : ok_(false) {  // NOLINT(google-explicit-constructor)
        new (&e_) E(std::move(u).error());
    }
    expected(const expected& o) : ok_(o.ok_) {
        if (ok_)
            new (&v_) T(o.v_);
        else
            new (&e_) E(o.e_);
    }
    expected(expected&& o) noexcept : ok_(o.ok_) {
        if (ok_)
            new (&v_) T(std::move(o.v_));
        else
            new (&e_) E(std::move(o.e_));
    }
    expected& operator=(const expected& o) {
        if (this != &o) {
            destroy();
            ok_ = o.ok_;
            if (ok_)
                new (&v_) T(o.v_);
            else
                new (&e_) E(o.e_);
        }
        return *this;
    }
    expected& operator=(expected&& o) noexcept {
        if (this != &o) {
            destroy();
            ok_ = o.ok_;
            if (ok_)
                new (&v_) T(std::move(o.v_));
            else
                new (&e_) E(std::move(o.e_));
        }
        return *this;
    }
    ~expected() { destroy(); }

    [[nodiscard]] bool has_value() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    [[nodiscard]] const T& operator*() const& noexcept { return v_; }
    [[nodiscard]] T& operator*() & noexcept { return v_; }
    [[nodiscard]] T&& operator*() && noexcept { return std::move(v_); }
    [[nodiscard]] const T* operator->() const noexcept { return &v_; }
    [[nodiscard]] T* operator->() noexcept { return &v_; }

    [[nodiscard]] const T& value() const& {
        check();
        return v_;
    }
    [[nodiscard]] T& value() & {
        check();
        return v_;
    }
    [[nodiscard]] T&& value() && {
        check();
        return std::move(v_);
    }
    template <class U>
    [[nodiscard]] T value_or(U&& fallback) const& {
        return ok_ ? v_ : static_cast<T>(std::forward<U>(fallback));
    }

    [[nodiscard]] const E& error() const& noexcept { return e_; }
    [[nodiscard]] E& error() & noexcept { return e_; }
    [[nodiscard]] E&& error() && noexcept { return std::move(e_); }

    // As std::expected: f(value) when there is one, an expected with the same error type;
    // otherwise the error, passed on.
    template <class F>
    auto and_then(F&& f) & {
        using R = std::remove_cvref_t<decltype(f(v_))>;
        static_assert(std::is_same_v<typename R::error_type, E>, "and_then keeps the error type");
        return ok_ ? std::forward<F>(f)(v_) : R(unexpected<E>(e_));
    }
    template <class F>
    auto and_then(F&& f) const& {
        using R = std::remove_cvref_t<decltype(f(v_))>;
        static_assert(std::is_same_v<typename R::error_type, E>, "and_then keeps the error type");
        return ok_ ? std::forward<F>(f)(v_) : R(unexpected<E>(e_));
    }
    template <class F>
    auto and_then(F&& f) && {
        using R = std::remove_cvref_t<decltype(f(std::move(v_)))>;
        static_assert(std::is_same_v<typename R::error_type, E>, "and_then keeps the error type");
        return ok_ ? std::forward<F>(f)(std::move(v_)) : R(unexpected<E>(std::move(e_)));
    }

    // As std::expected: equal to a value when it holds one that compares equal.
    template <class U>
        requires(!std::is_same_v<std::remove_cvref_t<U>, expected>)
    friend bool operator==(const expected& x, const U& v) {
        return x.ok_ && x.v_ == v;
    }

private:
    void check() const {
        if (!ok_) detail::throw_bad_expected_access(e_);
    }
    void destroy() noexcept {
        if (ok_)
            v_.~T();
        else
            e_.~E();
    }
    union {
        T v_;
        E e_;
    };
    bool ok_;
};

template <class E>
class expected<void, E> {
public:
    using value_type = void;
    using error_type = E;

    expected() = default;
    template <class G>
    expected(unexpected<G> u) : e_(std::move(u).error()), failed_(true) {}  // NOLINT

    [[nodiscard]] bool has_value() const noexcept { return !failed_; }
    explicit operator bool() const noexcept { return !failed_; }
    void value() const {
        if (failed_) detail::throw_bad_expected_access(e_);
    }
    [[nodiscard]] const E& error() const& noexcept { return e_; }
    [[nodiscard]] E& error() & noexcept { return e_; }

    // As std::expected: f() when there is no error, an expected with the same error type;
    // otherwise the error, passed on.
    template <class F>
    auto and_then(F&& f) const& {
        using R = std::remove_cvref_t<decltype(f())>;
        static_assert(std::is_same_v<typename R::error_type, E>, "and_then keeps the error type");
        return failed_ ? R(unexpected<E>(e_)) : std::forward<F>(f)();
    }
    template <class F>
    auto and_then(F&& f) && {
        using R = std::remove_cvref_t<decltype(f())>;
        static_assert(std::is_same_v<typename R::error_type, E>, "and_then keeps the error type");
        return failed_ ? R(unexpected<E>(std::move(e_))) : std::forward<F>(f)();
    }

private:
    E e_{};  // default-constructed (empty) when there is no error
    bool failed_ = false;
};

#endif

}  // namespace baker
