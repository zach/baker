#pragma once

// Objects: C++ types that scripts can call ("Objects").
//
//   struct Person : baker::Object<Person> {
//       float height = 6.0f;
//       float on_grow(float amount) { height += amount; return height; }
//       static void bind(baker::Binder<Person>& b) {
//           b.method("grow", &Person::on_grow).doc("Grows by `amount`.");
//           b.property("height", &Person::height);
//           b.method("shrink", [](Person& self, float a) { self.height -= a; });
//       }
//   };
//
//   Person bob;
//   ctx.add(bob, "bob");                 // {bob grow 10} now works from script
//   ctx.call(bob, "grow", 10);           // and the same dispatch from C++
//
// A Context refers to an object by a salted handle, so a
// script holding a reference to a destroyed object gets a clean error, never a dangling
// pointer. An object removes itself from its Context when destroyed; a copy of an object
// is not in any Context.
//
// To derive from another bound type, name it as the second argument, so there is one
// ObjectBase: `struct Boss : baker::Object<Boss, Person>`. Boss gets Person's methods and
// properties (copied when Boss's table is built, so dispatch never walks a chain), and a
// method Boss binds replaces Person's of the same name.
//
// A command sent to an object tries, in order: script handlers attached to its type by
// {class}, its native methods, then the methods every object has: `get`, `set`,
// `type`, and `name`. Every object can also carry loose parameters: `set` and `get` reach
// a bound property first, then the parameters.
//
// Typed messages are plain aggregates with a script name:
//
//   struct HitMsg { static constexpr auto name = "hit"; int points; };
//   b.on<HitMsg>(&Person::on_hit);       // void on_hit(const HitMsg&)
//   bob.handle(HitMsg{.points = 20});    // C++ to C++: no Node, no allocation
//
// on<M> also binds a method named M::name whose parameters are M's fields, in order, so
// {bob hit 20} reaches the same handler, and handle() reaches a script handler (hit ($points)
// ...) first, as a command would.

#include <baker/script.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace baker {

class ObjectBase;

namespace detail {

// What a Context knows about a bound type: built once per type from T::bind. Its layout
// is private to baker::script (internal.hpp); a binding reaches it through the functions
// below, called once per type.
struct TypeRecord;
struct MethodEntry;
struct ObjectParams;

[[nodiscard]] TypeRecord* new_type_record(std::string_view name);
void set_type_name(TypeRecord& type, std::string_view name);
// Adds an overload to method `name`, replacing an inherited method of that name.
[[nodiscard]] MethodEntry* add_method(TypeRecord& type, Symbol name, Overload overload);
void set_method_doc(MethodEntry& method, std::string_view doc);
void set_method_status(MethodEntry& method, Status status, Symbol replacement);
using PropertyGet = Node (*)(const void* accessor, ObjectBase& self);
using PropertySet = bool (*)(const void* accessor, ObjectBase& self, const Node& value);
void add_property(TypeRecord& type, Symbol name, std::string_view value_type, Shared accessor,
                  PropertyGet get, PropertySet set, std::string_view doc);
void inherit(TypeRecord& type, const TypeRecord& base);
// {new T} and its delete, through the creating Context's allocator.
using CreateObject = ObjectBase* (*)(const Allocator& allocator);
using DestroyObject = void (*)(const Allocator& allocator, ObjectBase* object);
void set_create(TypeRecord& type, CreateObject create, DestroyObject destroy);
void finish_type_record(TypeRecord& type);
// Whether `type` is `other` or derives from it.
[[nodiscard]] bool type_is(const TypeRecord& type, const TypeRecord* other) noexcept;
[[nodiscard]] std::string_view type_record_name(const TypeRecord& type) noexcept;

// --- Messages ---

// A message's typed handler: the C++ path. `key` identifies the message type.
using MessageThunk = Expected<Node> (*)(const void* callable, ObjectBase& self,
                                        const void* message);
struct MessageTarget {
    MessageThunk thunk = nullptr;  // null: the type has no typed handler for the message
    const void* callable = nullptr;
};
void add_message(TypeRecord& type, const void* key, MessageThunk thunk, Shared callable);
[[nodiscard]] MessageTarget find_message(const TypeRecord& type, const void* key) noexcept;
// Marks a method as the script face of message type `message_type`, for the reference.
void set_method_message(MethodEntry& method, std::string_view message_type);
[[nodiscard]] bool has_method(const TypeRecord& type, Symbol name) noexcept;
// Whether a script handler named `name` applies to `object`, in its Context.
[[nodiscard]] bool scripted(const ObjectBase& object, Symbol name) noexcept;
[[nodiscard]] Error unhandled(const ObjectBase& object, Symbol name);

// A message type's identity: the address of this variable. No RTTI, no string compare.
template <class M>
inline constexpr char message_key = 0;

template <class M>
[[nodiscard]] Symbol message_name() {
    static const Symbol name{std::string_view(M::name)};
    return name;
}

// The number of fields of aggregate M: the most initializers M{...} accepts. One more than
// kMaxMessageFields is probed, so a larger message is an error rather than a wrong count.
inline constexpr size_t kMaxMessageFields = 8;
struct AnyField {
    template <class T>
    operator T() const noexcept;  // NOLINT(google-explicit-constructor); never defined
};
template <class M, size_t... I>
constexpr bool takes_fields(std::index_sequence<I...>) {
    return requires { M{(static_cast<void>(I), AnyField{})...}; };
}
template <class M, size_t N = kMaxMessageFields + 1>
constexpr size_t field_count() {
    if constexpr (N == 0 || takes_fields<M>(std::make_index_sequence<N>{}))
        return N;
    else
        return field_count<M, N - 1>();
}

// Calls f with references to the fields of `m`, in order.
template <class M, class F>
decltype(auto) with_fields(M& m, F&& f) {
    constexpr size_t n = field_count<std::remove_const_t<M>>();
    static_assert(n <= kMaxMessageFields, "a message has at most 8 fields");
    if constexpr (n == 0) {
        return f();
    } else if constexpr (n == 1) {
        auto& [a] = m;
        return f(a);
    } else if constexpr (n == 2) {
        auto& [a, b] = m;
        return f(a, b);
    } else if constexpr (n == 3) {
        auto& [a, b, c] = m;
        return f(a, b, c);
    } else if constexpr (n == 4) {
        auto& [a, b, c, d] = m;
        return f(a, b, c, d);
    } else if constexpr (n == 5) {
        auto& [a, b, c, d, e] = m;
        return f(a, b, c, d, e);
    } else if constexpr (n == 6) {
        auto& [a, b, c, d, e, g] = m;
        return f(a, b, c, d, e, g);
    } else if constexpr (n == 7) {
        auto& [a, b, c, d, e, g, h] = m;
        return f(a, b, c, d, e, g, h);
    } else {
        auto& [a, b, c, d, e, g, h, i] = m;
        return f(a, b, c, d, e, g, h, i);
    }
}
template <class M>
using FieldTypes = decltype(with_fields(std::declval<M&>(), [](auto&... f) {
    return TypeList<std::remove_cvref_t<decltype(f)>...>{};
}));

template <class M>
concept Message = std::is_aggregate_v<M> && requires { std::string_view(M::name); };

// The general path: the message sent as {object name fields...}.
template <class M>
Expected<Node> send_message(Context& ctx, const ObjectBase& object, const M& m) {
    return with_fields(m,
                       [&](const auto&... f) { return ctx.call(object, message_name<M>(), f...); });
}

// The unqualified name of T, from the compiler's signature of this function: no RTTI. The
// view points into that signature, which is static.
template <class T>
std::string_view type_name_of() {
#if defined(_MSC_VER)
    std::string_view s = __FUNCSIG__;  // ...type_name_of<struct ns::Person>(void)
    s = s.substr(0, s.rfind(">("));
    s = s.substr(s.find("type_name_of<") + 13);
#else
    std::string_view s = __PRETTY_FUNCTION__;  // ...[with T = ns::Person; ...] or [T = ns::Person]
    s = s.substr(s.find("T = ") + 4);
    s = s.substr(0, s.find_first_of(";]"));
#endif
    for (std::string_view prefix : {"struct ", "class "})
        if (s.substr(0, prefix.size()) == prefix) s.remove_prefix(prefix.size());
    const size_t scope = s.substr(0, s.find('<')).rfind("::");
    if (scope != std::string_view::npos) s.remove_prefix(scope + 2);
    return s;
}

// The object a method is called on: a parameter that takes no argument.
template <class T>
struct Self {
    T* object;
};
template <class T>
struct Param<Self<T>> {
    using Holder = Self<T>;
    static constexpr ParamKind kind = ParamKind::Implicit;
    static constexpr std::string_view type() { return ""; }
    static constexpr bool (*match)(const Node&, bool) noexcept = nullptr;
    static bool read(const Args& a, size_t, std::optional<Holder>& out, Expected<Node>&) {
        out.emplace(Self<T>{static_cast<T*>(a.self())});
        return true;
    }
    static Self<T> pass(Self<T> h) noexcept { return h; }
};

template <class L>
struct DropFirst;
template <class First, class... Rest>
struct DropFirst<TypeList<First, Rest...>> {
    using type = TypeList<Rest...>;
};

// A method as a function taking Self<T> first: from a member function pointer, or from a
// callable whose first parameter is T& (or a base of T).
template <class T, class R, class F, class... A>
auto method_wrapper(F f, TypeList<A...>*) {
    return [f](Self<T> self, A... a) -> R {
        if constexpr (std::is_member_function_pointer_v<F>)
            return (self.object->*f)(std::forward<A>(a)...);
        else
            return f(*self.object, std::forward<A>(a)...);
    };
}

}  // namespace detail

// What a Context stores. Derive from Object<T>, not from this.
class ObjectBase {
public:
    ObjectBase() noexcept = default;
    ObjectBase(const ObjectBase&) noexcept {}  // a copy is not in any Context
    // Keeps this object's place in its Context: there is nothing to copy.
    // NOLINTNEXTLINE(bugprone-unhandled-self-assignment)
    ObjectBase& operator=(const ObjectBase&) noexcept { return *this; }
    virtual ~ObjectBase();

    [[nodiscard]] virtual const detail::TypeRecord& type() const noexcept = 0;
    // The name scripts use for the object's type: "Person".
    [[nodiscard]] std::string_view type_name() const noexcept {
        return detail::type_record_name(type());
    }
    // An Object node referring to this object, or Int 0 if it is in no Context.
    [[nodiscard]] Node node() const noexcept { return ctx_ ? Node::object(handle_) : Node(); }
    [[nodiscard]] Context* context() const noexcept { return ctx_; }

    // Sends a typed message (detail::Message): to a script handler for M::name if the
    // object's class has one, else to the typed handler on<M> bound (allocation-free), else
    // to a method named M::name. The handler's result, Int 0 for void; NotFound if nothing
    // handles it.
    template <class M>
    [[nodiscard]] Expected<Node> handle(const M& message);
    // Whether handle(M) would find a handler. Allocates nothing.
    template <class M>
    [[nodiscard]] bool handles() const noexcept;

private:
    friend class Context;
    friend struct Context::Impl;
    friend bool detail::scripted(const ObjectBase&, Symbol) noexcept;
    Context* ctx_ = nullptr;
    ObjectHandle handle_;
    // Loose parameters: made on the first set, from the Context's allocator, and freed when
    // the object leaves the Context.
    detail::ObjectParams* params_ = nullptr;
};

template <class T>
class Binder;

template <class T, class Base = ObjectBase>
class Object : public Base {
    static_assert(std::is_base_of_v<ObjectBase, Base>, "Base must be a bound type");

public:
    using Base::Base;
    // T's bindings, built on first use.
    [[nodiscard]] static const detail::TypeRecord& record();
    [[nodiscard]] const detail::TypeRecord& type() const noexcept override { return record(); }
};

// What method() returns: adds documentation and status to the method just bound.
class MethodRef {
public:
    explicit MethodRef(detail::MethodEntry& m) noexcept : m_(&m) {}
    MethodRef& doc(std::string_view text) {
        detail::set_method_doc(*m_, text);
        return *this;
    }
    MethodRef& status(Status status, Symbol replacement = {}) {
        detail::set_method_status(*m_, status, replacement);
        return *this;
    }
    MethodRef& message(std::string_view message_type) {
        detail::set_method_message(*m_, message_type);
        return *this;
    }

private:
    detail::MethodEntry* m_;
};

template <class T>
class Binder {
public:
    explicit Binder(detail::TypeRecord& record) noexcept : r_(record) {}

    // The name scripts and messages use for T; by default T's unqualified C++ name.
    void name(std::string_view name) { detail::set_type_name(r_, name); }

    // Binds a member function, or a callable whose first parameter is T& (or a base's).
    // Binding a name again adds an overload, as Context::func does.
    template <class F>
    MethodRef method(Symbol name, F f) {
        using Sig = detail::signature_of<F>;
        using R = typename Sig::result;
        auto w = [&] {
            if constexpr (std::is_member_function_pointer_v<F>)
                return detail::method_wrapper<T, R>(f, static_cast<typename Sig::params*>(nullptr));
            else
                return detail::method_wrapper<T, R>(
                    f,
                    static_cast<typename detail::DropFirst<typename Sig::params>::type*>(nullptr));
        }();
        using W = decltype(w);
        return MethodRef(*detail::add_method(
            r_, name,
            detail::make_overload<W, R>(
                std::move(w), static_cast<typename detail::signature_of<W>::params*>(nullptr))));
    }

    // The handler for message type M: a member function taking const M&, or a callable
    // taking T& and const M&. Also binds the method M::name, taking M's fields.
    template <class M, class H>
        requires detail::Message<M>
    MethodRef on(H handler) {
        auto call = [handler](T& self, const M& m) -> decltype(auto) {
            if constexpr (std::is_member_function_pointer_v<H>)
                return (self.*handler)(m);
            else
                return handler(self, m);
        };
        using C = decltype(call);
        using R = decltype(call(std::declval<T&>(), std::declval<const M&>()));
        detail::add_message(
            r_, &detail::message_key<M>,
            [](const void* c, ObjectBase& self, const void* m) -> Expected<Node> {
                const C& f = *static_cast<const C*>(c);
                if constexpr (std::is_void_v<R>) {
                    f(static_cast<T&>(self), *static_cast<const M*>(m));
                    return Node();
                } else {
                    return detail::to_result(f(static_cast<T&>(self), *static_cast<const M*>(m)));
                }
            },
            detail::Shared::make<C>(call));
        MethodRef ref = method(detail::message_name<M>(),
                               from_fields<M>(call, static_cast<detail::FieldTypes<M>*>(nullptr)));
        ref.message(detail::type_name_of<M>());
        return ref;
    }

    // A data member, read and written through Convert<M>.
    template <class M, class C>
    void property(Symbol name, M C::*member, std::string_view doc = {}) {
        static_assert(std::is_base_of_v<C, T>, "the member must belong to T or a base");
        detail::add_property(
            r_, name, detail::Param<M>::type(), detail::Shared::make<M C::*>(member),
            [](const void* acc, ObjectBase& self) {
                auto r =
                    detail::to_result(static_cast<T&>(self).*(*static_cast<M C::*const*>(acc)));
                return r ? std::move(*r) : Node();
            },
            [](const void* acc, ObjectBase& self, const Node& v) {
                return Values(std::span<const Node>(&v, 1), nullptr)
                    .read(0, static_cast<T&>(self).*(*static_cast<M C::*const*>(acc)));
            },
            doc);
    }

    // Declares that T derives from bound type B. Object<T, B> does this already; naming it
    // again is harmless.
    template <class B>
    void inherits() {
        static_assert(std::is_base_of_v<B, T> && !std::is_same_v<B, T>, "B must be a base of T");
        inherit(B::record());
    }

    void inherit(const detail::TypeRecord& b) { detail::inherit(r_, b); }

private:
    // The script face of a message handler: a method taking M's fields.
    template <class M, class C, class... F>
    static auto from_fields(C call, detail::TypeList<F...>*) {
        return [call](T& self, F... f) -> decltype(auto) { return call(self, M{std::move(f)...}); };
    }

    detail::TypeRecord& r_;
};

template <class T, class Base>
const detail::TypeRecord& Object<T, Base>::record() {
    static const detail::TypeRecord* const r = [] {
        detail::TypeRecord& rec = *detail::new_type_record(detail::type_name_of<T>());
        Binder<T> b(rec);
        if constexpr (!std::is_same_v<Base, ObjectBase>) b.inherit(Base::record());
        if constexpr (requires { T::bind(b); }) T::bind(b);
        if constexpr (std::is_default_constructible_v<T>)
            detail::set_create(
                rec, [](const Allocator& a) -> ObjectBase* { return detail::create<T>(a); },
                [](const Allocator& a, ObjectBase* o) { detail::destroy(a, static_cast<T*>(o)); });
        detail::finish_type_record(rec);
        return &rec;
    }();
    return *r;
}

// --- Context templates. ---

template <class T>
T* Context::try_get(const Node& object) const noexcept {
    ObjectBase* o = try_resolve(object);
    if (!o) return nullptr;
    const detail::TypeRecord& t = o->type();
    return &t == &T::record() || detail::type_is(t, &T::record()) ? static_cast<T*>(o) : nullptr;
}

template <class T>
Expected<T*> Context::get(const Node& object) const {
    if (T* p = try_get<T>(object)) return p;
    return Unexpected(resolve_error(object, detail::type_record_name(T::record())));
}

template <class T>
void Context::add_type() {
    add_type(T::record());
}

template <class... A>
Expected<Node> Context::call(const ObjectBase& object, Symbol method, A&&... args) {
    const Node nodes[] = {detail::to_node(std::forward<A>(args))..., Node()};
    return call(object.node(), method, std::span<const Node>(nodes, sizeof...(A)));
}

// --- Messages. ---

template <class M>
Expected<Node> ObjectBase::handle(const M& message) {
    static_assert(detail::Message<M>,
                  "a message is an aggregate with `static constexpr auto name = \"...\"`");
    const Symbol name = detail::message_name<M>();
    if (ctx_ && detail::scripted(*this, name)) return detail::send_message(*ctx_, *this, message);
    if (const detail::MessageTarget t = detail::find_message(type(), &detail::message_key<M>);
        t.thunk)
        return t.thunk(t.callable, *this, &message);
    if (ctx_ && detail::has_method(type(), name))
        return detail::send_message(*ctx_, *this, message);
    return Unexpected(detail::unhandled(*this, name));
}

template <class M>
bool ObjectBase::handles() const noexcept {
    const Symbol name = detail::message_name<M>();
    if (detail::find_message(type(), &detail::message_key<M>).thunk) return true;
    return ctx_ && (detail::scripted(*this, name) || detail::has_method(type(), name));
}

// Sends `message` to each object, since where there is one there are many: the
// type's handler is looked up once for each run of objects of one type, so a span sorted
// by type pays one search per type. Objects that do not handle M are skipped. Returns how
// many handled it, or the first error a handler returned.
template <class M>
Expected<size_t> handle_all(std::span<ObjectBase* const> objects, const M& message) {
    static_assert(detail::Message<M>,
                  "a message is an aggregate with `static constexpr auto name = \"...\"`");
    const Symbol name = detail::message_name<M>();
    const detail::TypeRecord* type = nullptr;
    detail::MessageTarget typed;
    bool method = false;
    size_t handled = 0;
    for (ObjectBase* o : objects) {
        const detail::TypeRecord& t = o->type();
        if (&t != type) {
            type = &t;
            typed = detail::find_message(t, &detail::message_key<M>);
            method = detail::has_method(t, name);
        }
        Context* ctx = o->context();
        Expected<Node> r;
        // A script handler wins over the typed one, which wins over a method.
        // NOLINTBEGIN(bugprone-branch-clone)
        if (ctx && detail::scripted(*o, name))
            r = detail::send_message(*ctx, *o, message);
        else if (typed.thunk)
            r = typed.thunk(typed.callable, *o, &message);
        else if (ctx && method)
            r = detail::send_message(*ctx, *o, message);
        else
            continue;
        // NOLINTEND(bugprone-branch-clone)
        if (!r) return Unexpected(std::move(r).error());
        ++handled;
    }
    return handled;
}

}  // namespace baker
