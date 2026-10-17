#pragma once

// Callback<R(A...)>: an owning, copyable callable, for the few places Baker stores host
// code to call later (a file reader, reload and change callbacks). It is what
// std::function would be here, but <functional> stays out of public headers.
//
// A callable is copied into the default allocator when it is stored. A plain
// function, or a lambda with no captures, can instead be stored by pointer with no
// allocation.

#include <baker/allocator.hpp>

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace baker {

template <class Sig>
class Callback;

template <class R, class... A>
class Callback<R(A...)> {
public:
    Callback() noexcept = default;
    Callback(std::nullptr_t) noexcept {}  // NOLINT(google-explicit-constructor)

    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, Callback> &&
                 std::is_invocable_r_v<R, F&, A...>)
    Callback(F&& f) {  // NOLINT(google-explicit-constructor)
        using T = std::remove_cvref_t<F>;
        if constexpr (std::is_convertible_v<T, R (*)(A...)>) {
            fn_ = static_cast<R (*)(A...)>(f);
        } else {
            static const Ops ops{
                [](void* p, A... a) -> R { return (*static_cast<T*>(p))(std::forward<A>(a)...); },
                [](const void* p) -> void* { return make<T>(*static_cast<const T*>(p)); },
                [](void* p) {
                    static_cast<T*>(p)->~T();
                    detail::default_free(p, sizeof(T));
                }};
            ops_ = &ops;
            obj_ = make<T>(std::forward<F>(f));
        }
    }

    Callback(const Callback& o)
        : fn_(o.fn_), ops_(o.ops_), obj_(o.ops_ ? o.ops_->clone(o.obj_) : nullptr) {}
    Callback(Callback&& o) noexcept : fn_(o.fn_), ops_(o.ops_), obj_(o.obj_) {
        o.ops_ = nullptr;
        o.obj_ = nullptr;
        o.fn_ = nullptr;
    }
    Callback& operator=(Callback o) noexcept {
        std::swap(fn_, o.fn_);
        std::swap(ops_, o.ops_);
        std::swap(obj_, o.obj_);
        return *this;
    }
    ~Callback() {
        if (ops_) ops_->destroy(obj_);
    }

    explicit operator bool() const noexcept { return fn_ || ops_; }
    R operator()(A... a) const {
        if (ops_) return ops_->call(obj_, std::forward<A>(a)...);
        return fn_(std::forward<A>(a)...);
    }

private:
    template <class T, class... X>
    static T* make(X&&... x) {
        return ::new (detail::default_alloc(sizeof(T), alignof(T))) T(std::forward<X>(x)...);
    }
    struct Ops {
        R (*call)(void*, A...);
        void* (*clone)(const void*);
        void (*destroy)(void*);
    };
    R (*fn_)(A...) = nullptr;
    const Ops* ops_ = nullptr;
    void* obj_ = nullptr;
};

}  // namespace baker
