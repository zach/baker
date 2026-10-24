#pragma once

// baker::script: evaluating commands, binding native functions, and variables.
//
// Evaluation is lazy. Running {f a b} looks up f's handler and hands it the raw
// command; nothing evaluates `a` or `b` until the handler reads one by type. So `if`,
// `set`, `do`, `and` and `or` are ordinary registered functions, and the evaluator has no
// special forms. A parameter declared Raw<Node> is never evaluated unless the function
// asks; every other parameter is evaluated, then converted through Convert<T>.
//
//   baker::Context ctx;
//   ctx.func("hypot", [](double a, double b) { return std::sqrt(a * a + b * b); });
//   double h = ctx.eval("{hypot 3 4}").as_float();                   // 5
//
// Registering a name again with a different signature adds an overload. Overloads are
// tried in registration order, first on exact node types, then with Int read as Float.
//
// Variables are dynamically scoped, by shallow binding: each $name has one value cell per
// Context, and {do ($a) ...} saves $a, runs the body, and restores it, even on error. A
// function sees, and can change, the variables of whoever called it. That is deliberate:
// it is the original design, and it is why reading $a is a single load. Do not "fix" it
// into lexical scoping.
//
// Typed lookups in baker::data (find_int, get<T>, ...) evaluate a Command or Var value in
// the calling thread's current Context: the innermost Context::Scope, else the Context
// being evaluated, else the first Context created on the thread.
//
// A Context is single-threaded. Separate Contexts may evaluate the same trees at once,
// because evaluation never modifies a tree.

#include <baker/array.hpp>
#include <baker/clock.hpp>
#include <baker/list.hpp>
#include <baker/node.hpp>
#include <baker/parse.hpp>
#include <baker/repeats.hpp>
#include <baker/text.hpp>

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace baker {

class Config;
struct ValidatorStack;
class Context;
class Scheduler;
class ObjectBase;
namespace detail {
struct TypeRecord;
struct ToolsAccess;  // baker::tools' way to the internals (src/tools/access.hpp)
}  // namespace detail

// Where a function comes from ("One namespace, three tiers"). Macros are expanded
// by the reader before evaluation, so no function record is ever a macro.
enum class Tier : uint8_t { Native, Script };

// A function's maturity, shown in the reference and the demand report.
enum class Status : uint8_t { Normal, Stub, Provisional, Deprecated };
inline constexpr Status Stub = Status::Stub;
inline constexpr Status Provisional = Status::Provisional;
inline constexpr Status Deprecated = Status::Deprecated;

// The arguments of one call, after the function name.
class Args {
public:
    [[nodiscard]] size_t size() const noexcept { return size_; }
    // Argument `i` (from 0, after the name), raw: never evaluated.
    [[nodiscard]] const Node& raw(size_t i) const noexcept { return base_[i]; }
    // Argument `i`, evaluated.
    [[nodiscard]] Expected<Node> eval(size_t i) const;
    // Argument `i`, evaluated, then converted through Convert<T>. A string_view stays valid
    // until the function returns.
    template <class T>
    [[nodiscard]] Expected<T> get(size_t i) const;
    // try_get for the binding layer: reads into `out`, or puts the error in `error`.
    template <class T>
    bool read_into(size_t i, T& out, Expected<Node>& error) const;
    // Argument `i`, evaluated, as an array (valid until the function returns).
    [[nodiscard]] Expected<const Array*> array(size_t i) const;

    // The arguments from `i` on.
    [[nodiscard]] Args rest(size_t i) const noexcept;
    [[nodiscard]] const Array& command() const noexcept { return *command_; }
    [[nodiscard]] Context& context() const noexcept { return *ctx_; }
    // The function's name, or a method's.
    [[nodiscard]] Symbol name() const noexcept;
    // The object a method was called on; null for a function.
    [[nodiscard]] ObjectBase* self() const noexcept { return self_; }
    // An error naming this function and located at this call.
    [[nodiscard]] Error error(std::string_view message, ErrorCode code = ErrorCode::Type) const;

    // `first` is the index in `command` of the first argument: 1 for {f ...}, 2 for
    // {object method ...}.
    Args(Context& ctx, const Array& command, const Node* base, size_t size, size_t offset,
         bool evaluated, size_t first = 1, ObjectBase* self = nullptr) noexcept
        : ctx_(&ctx),
          command_(&command),
          base_(base),
          self_(self),
          size_(static_cast<uint32_t>(size)),
          offset_(static_cast<uint32_t>(offset)),
          first_(static_cast<uint8_t>(first)),
          evaluated_(evaluated) {}

private:
    template <class T>
    [[nodiscard]] Expected<T> get_slow(size_t i) const;
    template <class T>
    bool read_slow(size_t i, T& out, Expected<Node>& error) const;
    [[nodiscard]] Expected<Node> eval_node(const Node& n) const;
    void keep(Node n) const;
    [[nodiscard]] Error type_error(size_t i, const Node& n, std::string_view want) const;

    Context* ctx_;
    const Array* command_;
    const Node* base_;
    ObjectBase* self_;
    uint32_t size_;
    uint32_t offset_;  // index of base_[0] among all the arguments, for messages
    uint8_t first_;
    bool evaluated_;  // base_ holds values already evaluated (an overload set's arguments)
};

// A parameter that is not evaluated before the call. Raw<Node> is the argument as
// written; Raw<Var> names a variable, for functions like `set`.
template <class T>
class Raw;

template <>
class Raw<Node> {
public:
    Raw(Context& ctx, const Node& node) noexcept : ctx_(&ctx), node_(&node) {}
    [[nodiscard]] const Node& node() const noexcept { return *node_; }
    [[nodiscard]] const Node& operator*() const noexcept { return *node_; }
    [[nodiscard]] const Node* operator->() const noexcept { return node_; }
    [[nodiscard]] Expected<Node> eval() const;

private:
    Context* ctx_;
    const Node* node_;
};

// A variable's value cell. Obtained from Context::var() or a Raw<Var> parameter; stays
// valid for the Context's lifetime.
class VarRef {
public:
    VarRef(Node* cell, Symbol name) noexcept : cell_(cell), name_(name) {}
    [[nodiscard]] Symbol name() const noexcept { return name_; }
    [[nodiscard]] bool is_set() const noexcept;
    // The value; Int 0 if the variable is not set.
    [[nodiscard]] const Node& value() const noexcept;
    [[nodiscard]] const Node* operator->() const noexcept { return &value(); }
    VarRef& operator=(Node v) noexcept {
        *cell_ = std::move(v);
        return *this;
    }
    void unset() noexcept;

private:
    Node* cell_;
    Symbol name_;
};

struct Var {};  // tag for Raw<Var>

// A place to store a value: a $variable, or a [property] of $this (in a method).
template <>
class Raw<Var> {
public:
    Raw(Node* cell, Symbol name) noexcept : cell_(cell), name_(name) {}
    Raw(Context& ctx, const Array& property) noexcept : ctx_(&ctx), property_(&property) {}
    [[nodiscard]] bool is_property() const noexcept { return property_ != nullptr; }
    // The variable's name; empty for a property.
    [[nodiscard]] Symbol name() const noexcept { return name_; }
    // Stores `v`. Setting a property can fail: no $this, or a value of the wrong type.
    [[nodiscard]] Expected<void> store(Node v) const;
    // The variable's value (Int 0 if unset); for a property, its value or Int 0.
    [[nodiscard]] Node load() const;

private:
    Node* cell_ = nullptr;
    Symbol name_;
    Context* ctx_ = nullptr;
    const Array* property_ = nullptr;
};

namespace detail {

// Implicit: a parameter that takes no argument (Context&, the object of a method).
enum class ParamKind : uint8_t { Value, Raw, Optional, Rest, Implicit };

// What messages, the reference and check read of a parameter, and a call does not: kept
// apart, so the table a call reads stays small.
struct ParamText {
    std::string_view type;
    // Baker's type of each value the parameter reads (Convert<T>::values for a compound
    // type), for the reference and for check without the game; empty when it does not say.
    std::span<const std::string_view> values;
    ValueCount count = ValueCount::Exact;  // how many it reads (Convert<T>::count)
    ListElement element{};                 // a list's element and length (Convert<T>::element)
};
struct ParamInfo {
    const ParamText* text;
    ParamKind kind;
    // Whether an (evaluated, or literal) argument has the right node type; null: any.
    bool (*match)(const Node& n, bool promote) noexcept;
    bool raw;  // not evaluated before the call (Raw<T>, or an optional one)
    // A list whose elements are described (text->element): held to them. Beside raw, in
    // what was padding, so a declared call learns it without reaching the text.
    bool listed;

    [[nodiscard]] std::string_view type() const noexcept { return text->type; }
    [[nodiscard]] std::span<const std::string_view> values() const noexcept { return text->values; }
    [[nodiscard]] ValueCount count() const noexcept { return text->count; }
    [[nodiscard]] const ListElement& element() const noexcept { return text->element; }
};

template <class P>
inline constexpr bool is_raw = false;
template <class T>
inline constexpr bool is_raw<Raw<T>> = true;
template <class T>
inline constexpr bool is_raw<std::optional<Raw<T>>> = true;

// A refcounted, type-erased, immutable object: the callable behind a function. get() is
// one load, as std::shared_ptr's is. From an owner's allocator, which must outlive it (a
// Context's natives and script bodies), else the default allocator.
class Shared {
public:
    Shared() noexcept = default;
    template <class T, class... A>
    [[nodiscard]] static Shared make_in(const Allocator& alloc, A&&... a) {
        auto* box = ::new (allocate(alloc, sizeof(Box<T>), alignof(Box<T>)))
            Box<T>(alloc, std::forward<A>(a)...);
        Shared s;
        s.base_ = box;
        s.object_ = &box->value;
        return s;
    }
    template <class T, class... A>
    [[nodiscard]] static Shared make(A&&... a) {
        return make_in<T>(default_allocator(), std::forward<A>(a)...);
    }
    Shared(const Shared& o) noexcept : base_(o.base_), object_(o.object_) {
        if (base_) base_->refs.fetch_add(1, std::memory_order_relaxed);
    }
    Shared(Shared&& o) noexcept : base_(o.base_), object_(o.object_) {
        o.base_ = nullptr;
        o.object_ = nullptr;
    }
    Shared& operator=(Shared o) noexcept {
        Base* b = base_;
        base_ = o.base_;
        o.base_ = b;
        const void* p = object_;
        object_ = o.object_;
        o.object_ = p;
        return *this;
    }
    ~Shared() {
        if (base_ && base_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            base_->destroy(base_);
    }
    [[nodiscard]] const void* get() const noexcept { return object_; }

private:
    struct Base {
        std::atomic<uint32_t> refs{1};
        void (*destroy)(Base*) = nullptr;
        const Allocator* alloc = nullptr;
    };
    template <class T>
    struct Box : Base {
        template <class... A>
        explicit Box(const Allocator& from, A&&... a) : value(std::forward<A>(a)...) {
            this->alloc = &from;
            this->destroy = [](Base* b) {
                const Allocator& owner = *b->alloc;
                static_cast<Box*>(b)->~Box();
                deallocate(owner, b, sizeof(Box));
            };
        }
        T value;
    };
    Base* base_ = nullptr;
    const void* object_ = nullptr;
};

// One signature of a function.
struct Overload {
    using Thunk = Expected<Node> (*)(const void* callable, Args& args);
    Thunk thunk = nullptr;
    Shared callable;
    std::span<const ParamInfo> params;  // excluding implicit ones; static, or owned by callable
    std::string_view result;
    uint32_t min_args = 0;
    uint32_t max_args = 0;  // UINT32_MAX: takes the rest
};

// A list of types, for a signature's parameters.
template <class... T>
struct TypeList {};

// Storage for one value per index, as std::tuple would hold them.
template <size_t I, class T>
struct Slot {
    T value;
};
template <class Seq, class... T>
struct Slots;
template <size_t... I, class... T>
struct Slots<std::index_sequence<I...>, T...> : Slot<I, T>... {};
template <size_t I, class T>
T& slot(Slot<I, T>& s) noexcept {
    return s.value;
}

// --- Signature deduction. ---

template <class F>
struct signature_of : signature_of<decltype(&std::remove_cvref_t<F>::operator())> {};
template <class R, class... A>
struct signature_of<R (*)(A...)> {
    using result = R;
    using params = TypeList<A...>;
};
template <class R, class... A>
struct signature_of<R (*)(A...) noexcept> : signature_of<R (*)(A...)> {};
template <class C, class R, class... A>
struct signature_of<R (C::*)(A...) const> : signature_of<R (*)(A...)> {};
template <class C, class R, class... A>
struct signature_of<R (C::*)(A...) const noexcept> : signature_of<R (*)(A...)> {};
template <class C, class R, class... A>
struct signature_of<R (C::*)(A...)> : signature_of<R (*)(A...)> {};
template <class C, class R, class... A>
struct signature_of<R (C::*)(A...) noexcept> : signature_of<R (*)(A...)> {};

template <class T>
bool match_value(const Node& n, bool promote) noexcept {
    if constexpr (std::is_same_v<T, bool> || std::is_integral_v<T>) {
        return n.type() == Type::Int;
    } else if constexpr (std::is_floating_point_v<T>) {
        return n.type() == Type::Float || (promote && n.type() == Type::Int);
    } else if constexpr (std::is_same_v<T, Symbol>) {
        return n.type() == Type::Symbol;
    } else if constexpr (std::is_same_v<T, std::string_view> || requires(const T& t) {
                             { t.c_str() } -> std::convertible_to<const char*>;
                         }) {
        return n.type() == Type::String;
    } else if constexpr (std::is_same_v<T, Node>) {
        return true;
    } else {
        T out{};
        return Values(std::span<const Node>(&n, 1), nullptr).read(0, out);
    }
}

// Moves a read's value into `out`, or its error into `result`.
template <class E, class H>
bool take(E&& r, std::optional<H>& out, Expected<Node>& result) {
    if (!r) {
        result = Unexpected(std::forward<E>(r).error());
        return false;
    }
    out.emplace(std::move(*r));
    return true;
}

// How a parameter of type P reads its argument. `Holder` is what is read; pass() turns it
// into the argument.
template <class P>
struct Param {
    using T = std::remove_cvref_t<P>;
    using Holder = T;
    static constexpr ParamKind kind = ParamKind::Value;
    static constexpr std::string_view type() {
        if constexpr (std::is_same_v<T, Node>)
            return "value";
        else
            return convert_info<T>().name;
    }
    static constexpr auto match = &match_value<T>;
    static bool read(const Args& a, size_t i, std::optional<T>& out, Expected<Node>& result) {
        return a.read_into(i, out.emplace(), result);
    }
    static T&& pass(T& h) noexcept { return std::move(h); }
};
template <>
struct Param<const Array&> {
    using Holder = const Array*;
    static constexpr ParamKind kind = ParamKind::Value;
    static constexpr std::string_view type() { return "array"; }
    static bool match_array(const Node& n, bool) noexcept { return n.type() == Type::Array; }
    static constexpr auto match = &match_array;
    static bool read(const Args& a, size_t i, std::optional<Holder>& out, Expected<Node>& result) {
        return take(a.array(i), out, result);
    }
    static const Array& pass(const Array* h) noexcept { return *h; }
};
template <>
struct Param<Raw<Node>> {
    using Holder = Raw<Node>;
    static constexpr ParamKind kind = ParamKind::Raw;
    static constexpr std::string_view type() { return "unevaluated"; }
    static constexpr bool (*match)(const Node&, bool) noexcept = nullptr;
    static bool read(const Args& a, size_t i, std::optional<Holder>& out, Expected<Node>&) {
        out.emplace(a.context(), a.raw(i));
        return true;
    }
    static Raw<Node> pass(Raw<Node>& h) noexcept { return h; }
};
Expected<Raw<Var>> read_var(const Args& a, size_t i);
template <>
struct Param<Raw<Var>> {
    using Holder = Raw<Var>;
    static constexpr ParamKind kind = ParamKind::Raw;
    static constexpr std::string_view type() { return "$variable"; }
    static bool match_var(const Node& n, bool) noexcept {
        return n.type() == Type::Var || n.type() == Type::Property;
    }
    static constexpr auto match = &match_var;
    static bool read(const Args& a, size_t i, std::optional<Holder>& out, Expected<Node>& result) {
        return take(read_var(a, i), out, result);
    }
    static Raw<Var> pass(Raw<Var>& h) noexcept { return h; }
};
template <class T>
struct Param<std::optional<T>> {
    using Holder = std::optional<T>;
    static constexpr ParamKind kind = ParamKind::Optional;
    static constexpr std::string_view type() { return Param<T>::type(); }
    static constexpr auto match = Param<T>::match;
    static bool read(const Args& a, size_t i, std::optional<Holder>& out, Expected<Node>& result) {
        if (i >= a.size()) {
            out.emplace();
            return true;
        }
        std::optional<typename Param<T>::Holder> inner;
        if (!Param<T>::read(a, i, inner, result)) return false;
        out.emplace(std::move(*inner));
        return true;
    }
    static std::optional<T>&& pass(std::optional<T>& h) noexcept { return std::move(h); }
};
template <>
struct Param<Args> {
    using Holder = Args;
    static constexpr ParamKind kind = ParamKind::Rest;
    static constexpr std::string_view type() { return "values..."; }
    static constexpr bool (*match)(const Node&, bool) noexcept = nullptr;
    static bool read(const Args& a, size_t i, std::optional<Holder>& out, Expected<Node>&) {
        out.emplace(a.rest(i));
        return true;
    }
    static Args pass(Args& h) noexcept { return h; }
};
template <>
struct Param<Context&> {
    using Holder = Context*;
    static constexpr ParamKind kind = ParamKind::Implicit;
    static constexpr std::string_view type() { return ""; }
    static constexpr bool (*match)(const Node&, bool) noexcept = nullptr;
    static bool read(const Args& a, size_t, std::optional<Holder>& out, Expected<Node>&) {
        out.emplace(&a.context());
        return true;
    }
    static Context& pass(Context* h) noexcept { return *h; }
};

template <class T>
struct is_expected : std::false_type {};
template <class U>
struct is_expected<expected<U, Error>> : std::true_type {};

// The return value as a Node.
template <class R>
Expected<Node> to_result(R&& r) {
    using T = std::remove_cvref_t<R>;
    if constexpr (std::is_same_v<T, Node>) {
        return std::forward<R>(r);
    } else if constexpr (is_expected<T>::value) {
        if (!r) return Unexpected(std::forward<R>(r).error());
        if constexpr (std::is_void_v<typename T::value_type>)
            return Node();
        else
            return to_result(*std::forward<R>(r));
    } else if constexpr (std::is_same_v<T, bool>) {
        return Node(r ? 1 : 0);
    } else if constexpr (std::is_same_v<T, float>) {
        return Node(float_to_decimal_double(r));
    } else if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, Symbol>) {
        return Node(r);
    } else if constexpr (std::is_same_v<T, Ref<Array>>) {
        return Node(std::forward<R>(r));
    } else {
        ValueList out;
        Convert<T>::to(r, out);
        if (out.nodes().size() == 1) return std::move(out.nodes()[0]);
        return Node(Array::make_from(out.nodes()));
    }
}

template <class R>
constexpr std::string_view result_name() {
    using T = std::remove_cvref_t<R>;
    if constexpr (std::is_void_v<T>) {
        return "nothing";
    } else if constexpr (std::is_same_v<T, Node>) {
        return "value";
    } else if constexpr (std::is_same_v<T, Ref<Array>>) {
        return "array";
    } else if constexpr (is_expected<T>::value) {
        return result_name<typename T::value_type>();
    } else {
        return convert_info<T>().name;
    }
}

// The argument index of parameter `k`: a Context& parameter takes no argument.
template <class... P>
struct ArgIndex {
    static constexpr size_t of(size_t k) {
        constexpr bool takes[] = {(Param<P>::kind != ParamKind::Implicit)..., false};
        size_t n = 0;
        for (size_t j = 0; j < k; ++j) n += takes[j] ? 1 : 0;
        return n;
    }
};

template <class F, class R, class... P, size_t... I>
Expected<Node> invoke(const void* callable, [[maybe_unused]] Args& args,
                      std::index_sequence<I...>) {
    // One Expected for the whole call, holding a read error or the result: frames here
    // nest once per script call, so they stay small.
    Expected<Node> result;
    [[maybe_unused]] Slots<std::index_sequence<I...>, std::optional<typename Param<P>::Holder>...>
        held;
    bool ok = true;
    if constexpr (sizeof...(P) > 0) {
        using Index = ArgIndex<P...>;
        // Each index a constant, so a debug build runs no loop for it.
        ok = (Param<P>::read(args, std::integral_constant<size_t, Index::of(I)>::value,
                             slot<I>(held), result) &&
              ...);
    }
    if (ok) {
        const F& f = *static_cast<const F*>(callable);
        if constexpr (std::is_void_v<R>)
            f(Param<P>::pass(*slot<I>(held))...);
        else
            result = to_result(f(Param<P>::pass(*slot<I>(held))...));
    }
    return result;
}

// Baker's type of each value a parameter reads: an optional one's as its value's.
template <class T>
struct OptionalOf {
    using type = T;
};
template <class T>
struct OptionalOf<std::optional<T>> {
    using type = T;
};
template <class P>
[[nodiscard]] constexpr std::span<const std::string_view> param_values() noexcept {
    return value_types<typename OptionalOf<std::remove_cvref_t<P>>::type>();
}

template <class P>
inline constexpr ParamText kParamText{
    Param<P>::type(), param_values<P>(),
    value_count<typename OptionalOf<std::remove_cvref_t<P>>::type>(),
    list_element<typename OptionalOf<std::remove_cvref_t<P>>::type>()};

// The parameter descriptions of a signature, built at compile time: no allocation.
template <class... P>
struct ParamTable {
    static constexpr size_t kCount = (size_t{Param<P>::kind != ParamKind::Implicit} + ... + 0);
    struct Storage {
        ParamInfo items[kCount ? kCount : 1];
    };
    static constexpr Storage build() {
        Storage s{};
        [[maybe_unused]] size_t i = 0;
        (
            [&] {
                if constexpr (Param<P>::kind != ParamKind::Implicit)
                    s.items[i++] = {&kParamText<P>, Param<P>::kind, Param<P>::match, is_raw<P>,
                                    !kParamText<P>.element.values.empty()};
            }(),
            ...);
        return s;
    }
    static constexpr Storage kStorage = build();
};

template <class F, class R, class... P>
Overload make_overload(F&& f, TypeList<P...>*, const Allocator& alloc = default_allocator()) {
    using Fn = std::remove_cvref_t<F>;
    using Table = ParamTable<P...>;
    Overload o;
    o.thunk = [](const void* c, Args& a) {
        return invoke<Fn, R, P...>(c, a, std::index_sequence_for<P...>{});
    };
    o.callable = Shared::make_in<Fn>(alloc, std::forward<F>(f));
    o.result = result_name<R>();
    o.params = std::span<const ParamInfo>(Table::kStorage.items, Table::kCount);
    for (const ParamInfo& p : o.params) {
        if (p.kind == ParamKind::Rest) {
            o.max_args = UINT32_MAX;
            break;
        }
        ++o.max_args;
        if (p.kind != ParamKind::Optional) o.min_args = o.max_args;
    }
    return o;
}

}  // namespace detail

// What func() returns: adds documentation and status to the function just registered.
class FuncRef {
public:
    explicit FuncRef(void* function) noexcept : f_(function) {}
    FuncRef& doc(std::string_view text);
    // `replacement` names what to use instead of a Deprecated function.
    FuncRef& status(Status status, Symbol replacement = {});

private:
    void* f_;
};

// The result of baker::check (below).
struct CheckReport {
    struct Site {
        Symbol file;
        int32_t line = 0;
    };
    // The push-up backlog: what the data asks for that nothing provides yet.
    struct Demand {
        Symbol name;
        bool stub = false;  // a call to a function whose status is Stub; else unresolved
        List<Site> sites;
        Symbol on;  // for a method: the class of the object it was sent to
    };
    List<Error> errors;
    List<Demand> demand;   // in order of first appearance
    List<Error> warnings;  // the one-way rule: never an error
    List<Repeat> repeats;  // subtrees repeated across the trees of one call (repeats.hpp)
    // Informational, never an error: each pack that declares fewer than two owners.
    List<Error> findings;
    // Filled by the host, not by check: what loading the data reported (a file that did not
    // parse, an #autorun command that failed), and its shape check (baker::shape, which this
    // library does not depend on). A shape warning is a field no shape declares.
    List<Error> load;
    List<Error> shape_errors;
    List<Error> shape_warnings;
    // The data against the registered tweaks (check_tweaks): filled by check() of a Config,
    // from the registry it binds. A value its tweak cannot take is an error; a tweak the data
    // lacks, or a key beside a group's tweaks that none reads, a warning.
    List<Error> tweak_errors;
    List<Error> tweak_warnings;
    // From CheckOptions: whether a call to a function nothing defines (not a stub) fails.
    bool unresolved_fails = false;

    // Passes: no errors, no load problems, no shape or tweak errors, and under
    // unresolved_fails, no unresolved demand. Warnings, findings, repeats, and stubs never
    // fail it.
    [[nodiscard]] bool ok() const noexcept {
        if (!errors.empty() || !load.empty() || !shape_errors.empty() || !tweak_errors.empty())
            return false;
        if (unresolved_fails)
            for (const Demand& d : demand)
                if (!d.stub) return false;
        return true;
    }
};

// What check reads besides the trees and the Context.
struct CheckOptions {
    // The session that loaded the trees: with it, check applies the one-way rule to
    // calls and reports packs with fewer than two owners.
    const LoadSession* packs = nullptr;
    RepeatOptions repeats{};  // repeats.min_repeats = 0: no repetition report
    // A call to a function nothing defines fails the check (CheckReport::ok), unless the
    // function is declared a stub: an agent's or CI's gate, where it is nearly always a typo.
    bool unresolved_fails = false;
};

// Writes the report: errors, warnings, the demand report, repeats, then findings, one each.
// <baker/std_interop.hpp> has to_string(report).
void write(const CheckReport& report, TextSink out);

// What one function, or one script handler of a {class}, has cost: its call statistics.
// Times are in TimeSource nanoseconds: `total_ns` includes the calls it made,
// `self_ns` does not. A recursive function's total counts each level.
struct CallStats {
    Symbol name;
    Symbol on;  // the {class} of a script handler; empty for a function
    Tier tier{};
    uint64_t calls = 0;
    uint64_t total_ns = 0;
    uint64_t self_ns = 0;
};

// One call, as the trace hook sees it: on entry `result` is null; on exit it is the call's
// result or error.
struct TraceEvent {
    const Array* command;
    uint32_t depth;  // 0 for a call made from C++
    const Expected<Node>* result;
};

// A function's signature described as data, in the reference's terms (baker.reference,
// docs/reference-json.md), for Context::declare. A parameter's `type` is a type name as the
// reference writes it ("int", "float", "symbol", "string", "array", "value", ...), its
// `kind` "value", "raw", "optional" or "rest", and `raw` whether its argument is passed
// unevaluated. The views need last only for the call.
struct ParamDescription {
    std::string_view type;
    std::string_view kind = "value";
    bool raw = false;
    // Baker's type of each value, as the reference writes them: what lets a type of the
    // game's own ("vec2") be checked without the game; and how many it takes.
    std::span<const std::string_view> values{};
    ValueCount count = ValueCount::Exact;
    // A list's element and length, as the reference's "element" and "length" give them:
    // what lets a list of the game's own type ("vec2") be checked without the game.
    ListElement element{};
};
struct OverloadDescription {
    std::span<const ParamDescription> params;
    std::string_view result = "value";
};

// A call in progress, on the C++ stack: the frames of a Context form a list from the
// innermost call out (Context::top_frame). Read-only, and valid only while the call runs.
struct CallFrame {
    const Array* command;  // the call as written: its head, file, and line
    const CallFrame* parent;
};

class Context {
public:
    // Everything the Context keeps comes from `allocator`, which must outlive it.
    explicit Context(const Allocator& allocator = default_allocator());
    // The allocator it was given: everything it keeps comes from it.
    [[nodiscard]] const Allocator& allocator() const noexcept;
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // --- Evaluation. ---

    // A Command runs its function, a Var reads its variable, anything else is itself.
    [[nodiscard]] Expected<Node> eval(const Node& node);
    // Parses `text` and evaluates each top-level node in order, returning the last value
    // (Int 0 if there is none). The entry point for consoles and remote commands. The
    // text's tree is built in an arena from the Context's allocator: a value kept from the
    // result, or a {func} the text defined, keeps that arena, so the allocator must outlive
    // it as well as the Context.
    [[nodiscard]] Expected<Node> eval(std::string_view text, std::string_view name = "<eval>");
    // Runs a Config's #autorun commands (Config::autorun()) after a stack change: first it
    // restores every function and class handler the previous run_autorun's commands
    // defined or replaced, then it runs `commands` in order. So script a removed layer
    // defined is gone, and what it replaced (a native, a lower layer's, or nothing) is back.
    // A command that fails does not stop the rest; the failures are returned.
    [[nodiscard]] List<Error> run_autorun(std::span<const Node> commands);

    // --- Tasks (task.hpp). ---

    // The default Scheduler, from the Context's allocator; scripts call it `scheduler`.
    [[nodiscard]] Scheduler& scheduler();
    // The sync point: applies the removals ({delete}, remove()) made while a
    // Scheduler was ticking. tick() calls it at its end; a host that ticks several
    // Schedulers can call it once after them all.
    void sync();
    // Evaluates each node of `nodes` in order and returns the last value: runs a script.
    [[nodiscard]] Expected<Node> run(std::span<const Node> nodes);

    // --- Functions. ---

    // Registers `f` under `name`. Parameter and return types are deduced from its
    // signature ("Native function binding"); a registration that would make an
    // overload set ambiguous is an error.
    template <class F>
    [[nodiscard]] Expected<FuncRef> func(Symbol name, F&& f) {
        using Sig = detail::signature_of<F>;
        return add_overload(name, detail::make_overload<F, typename Sig::result>(
                                      std::forward<F>(f),
                                      static_cast<typename Sig::params*>(nullptr), allocator()));
    }
    [[nodiscard]] bool has_func(Symbol name) const noexcept;
    // Registers `name` from a description of its overloads rather than from a C++ callable:
    // what a tool that checks data without the game registers from the game's reference.
    // check() then treats a call to it as one to the function described, counting its
    // arguments and checking its literal arguments' types, and it is in the reference and in
    // check's demand like a native function; set its status and documentation on the
    // FuncRef. A call to it fails: it has no body. A type this Context cannot check, neither
    // one of Baker's nor one that a native function or method registered here takes, is an
    // error naming the function, and nothing is registered; so is an unknown kind.
    [[nodiscard]] Expected<FuncRef> declare(Symbol name,
                                            std::span<const OverloadDescription> overloads);
    // As declare, with a body the host builds at runtime (a function of an embedded
    // scripting language): check and the reference see the described parameters, and a call
    // runs `body`. When no parameter is raw, each argument is evaluated once and held to its
    // parameter's type before the body runs, as a native's would be, and the body reads them
    // from `Args` already evaluated; with a raw one, the body gets them as written and
    // evaluates what it needs.
    [[nodiscard]] Expected<FuncRef> declare(Symbol name,
                                            std::span<const OverloadDescription> overloads,
                                            const Callback<Expected<Node>(Args&)>& body);

    // --- Variables. ---

    // The variable `name`, created unset if new.
    [[nodiscard]] VarRef var(Symbol name);
    // A documented global: sets `name` to `initial` and lists it in the reference.
    VarRef global(Symbol name, Node initial, std::string_view doc = {});

    // --- Objects (object.hpp: Object<T>, Binder<T>). ---

    // Adds `object`, named `name` if given, and returns an Object node referring to it. The
    // object removes itself when destroyed. A name must not be a function's or another
    // object's; an object can be in one Context at a time.
    [[nodiscard]] Expected<Node> add(ObjectBase& object, Symbol name = {});
    // During a Scheduler's tick, waits for the sync point.
    void remove(ObjectBase& object) noexcept;
    // The object `object` (an Object node) refers to; null if it was removed (weak).
    [[nodiscard]] ObjectBase* try_resolve(const Node& object) const noexcept;
    // As try_resolve, and of type T or derived from it.
    template <class T>
    [[nodiscard]] T* try_get(const Node& object) const noexcept;
    // Strong resolution: as try_get, but an error saying why ("the Person object was
    // removed", "the object is a Hero, not a Person").
    template <class T>
    [[nodiscard]] Expected<T*> get(const Node& object) const;
    // The object named `name`, as an Object node; Int 0 if there is none.
    [[nodiscard]] Node object(Symbol name) const noexcept;
    // Makes T known before any T is added: for {class T ...}, {new T}, and the reference.
    template <class T>
    void add_type();
    // Sends {object method args...}: script handlers, then native methods, then the
    // methods every object has (object.hpp).
    [[nodiscard]] Expected<Node> call(const Node& object, Symbol method,
                                      std::span<const Node> args = {});
    template <class... A>
    [[nodiscard]] Expected<Node> call(const ObjectBase& object, Symbol method, A&&... args);

    // Nesting deeper than 1,000 calls, or than `bytes` of stack below the outermost call, is
    // a Limit error rather than a stack overflow. The default, 256 KB, is safe on any common
    // thread; release builds nest about 400 calls in it, debug builds fewer. Raise it on a
    // thread with a larger stack.
    void set_stack_budget(size_t bytes) noexcept;

    // --- Debugging. ---

    // The innermost call in progress; null when nothing is running. Follow `parent` out.
    [[nodiscard]] const CallFrame* top_frame() const noexcept;
    // The calls in progress, innermost first, as an error's stack records them.
    [[nodiscard]] List<StackEntry> backtrace() const;
    [[nodiscard]] StackEntry stack_entry(const CallFrame& frame) const;

    // --- Call statistics and tracing. Development aids: under BAKER_SHIPPING they are
    // compiled out, stats() is empty, and the switches do nothing. ---

    // Calls are always counted (one increment each). Timing them takes two clock reads per
    // call, so it waits for set_profiling(true).
    void set_profiling(bool on) noexcept;
    void set_time_source(TimeSource fn, void* user = nullptr) noexcept;
    // Every function and script handler called since the last reset, by self time, then
    // by calls. Reading allocates; counting does not.
    [[nodiscard]] List<CallStats> stats() const;
    void reset_stats() noexcept;
    // The script functions and handlers whose self time is at least `threshold_ns`: the
    // measured list of what to move to C++.
    [[nodiscard]] List<CallStats> hardening_candidates(uint64_t threshold_ns) const;

    // While on, every call is reported to the trace sink, on entry and on exit. By default
    // the sink prints each call to stderr, indented by depth.
    using TraceSink = void (*)(void* user, const TraceEvent& event);
    void trace(bool on) noexcept;
    void set_trace_sink(TraceSink sink, void* user) noexcept;

    // --- Notices: things worth saying that are not errors, such as a native function
    // replacing a script one. By default they go to stderr. ---
    using NoticeSink = void (*)(void* user, const Error& notice);
    void set_notice_sink(NoticeSink sink, void* user) noexcept;
    void notice(const Error& notice) const;

    // Makes a Context current on this thread for typed lookups in baker::data.
    class Scope {
    public:
        explicit Scope(Context& ctx) noexcept;
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Context* previous_;
    };
    [[nodiscard]] static Context* current() noexcept;

    struct Impl;
    [[nodiscard]] Impl& impl() const noexcept { return *impl_; }

private:
    friend struct detail::ToolsAccess;
    [[nodiscard]] Expected<FuncRef> add_overload(Symbol name, detail::Overload overload);
    void add_type(const detail::TypeRecord& type);
    [[nodiscard]] Error resolve_error(const Node& object, std::string_view type) const;

    Impl* impl_;
};

// Writes the script calls in progress on this thread's current Context
// (Context::current), one line each, innermost first; nothing if there are none. For an
// assert handler or crash reporter to print beside the C++ stack. Allocates nothing.
void script_backtrace(TextSink out);

// Walks `trees` without evaluating anything and reports, located, every call to a
// registered function with the wrong number of arguments for every overload, or a literal
// argument of the wrong type where the signature fixes one; plus the demand report, and the
// subtrees repeated across `trees` (find_repeats). Calls to functions defined by {func} in
// the checked trees count as registered. Unregistered heads, arguments that are commands or
// variables, and unevaluated or rest parameters are not checked: their types are unknown
// until run. Accumulates into `report`, so one report can cover many calls; repetition is
// found within one call, so give it every file to compare.
//
// Given `options.packs`, the session that loaded the trees, it also applies the one-way
// rule to calls: a call, in a pack's file, to a script function defined in a
// softer pack's file is a warning, once per pack and function; and each pack that declares
// (owners ...) with fewer than two is a finding.
void check(std::span<const Array* const> trees, const Context& ctx, CheckReport& report,
           const CheckOptions& options = {});
void check(const Array& tree, const Context& ctx, CheckReport& report,
           const CheckOptions& options = {});
[[nodiscard]] CheckReport check(const Array& tree, const Context& ctx,
                                const CheckOptions& options = {});
// Checks a Config's data as it loads: its merged tree, and the #autorun commands of its
// enabled layers, which are not in the tree (so a {func} an #autorun defines is seen
// too), with the Config's read session as `packs` unless options give one. `extra` adds
// trees checked alongside, such as scenarios. The merged tree is also held to the tweaks
// the Config binds (check_tweaks), into tweak_errors and tweak_warnings.
void check(const Config& cfg, const Context& ctx, CheckReport& report,
           const CheckOptions& options = {}, std::span<const Array* const> extra = {});
// For Config::add_validator (the same type as Config::Validator): checks the merged root
// after every merge, and each candidate layer, and reports check's errors and one-way
// warnings through the Config's warning handler; the demand report, repeats, and
// findings are for tools. Without `options.packs`, the stack's session is the packs. `ctx`
// and `options.packs` must outlive the Config's use of it. Silent without a warning handler.
// It reads the Context, which is single-threaded: add it without ValidatorOptions::any_thread.
[[nodiscard]] Callback<void(const ValidatorStack& stack, const Callback<void(const Error&)>& warn)>
check_validator(const Context& ctx, const CheckOptions& options = {});

// Evaluates one line from a tool and writes one line of JSON (schema baker.reply/1.1,
// docs/reply-json.md). A request is (ID {command}), its ID an int, symbol or string that
// the reply echoes; any other line is evaluated whole, with a null id, as eval(text) would.
// The reply holds the result's printed form ("text", the authority) and a JSON projection
// of it ("value"), or the error with its script stack. Never fails: a line that does not
// parse gets an error reply.
void eval_request(Context& ctx, std::string_view line, TextSink reply);

// --- Template definitions. ---

template <class T>
Expected<T> Args::get(size_t i) const {
    // The common case: a literal that converts.
    if (i < size_) {
        const Node& n = base_[i];
        if (evaluated_ || (n.type() != Type::Command && n.type() != Type::Var)) {
            T out{};
            if (Values(std::span<const Node>(&n, 1), command_).read(0, out)) return out;
        }
    }
    return get_slow<T>(i);
}

template <class T>
bool Args::read_into(size_t i, T& out, Expected<Node>& error) const {
    if (i < size_) {
        const Node& n = base_[i];
        if (evaluated_ || (n.type() != Type::Command && n.type() != Type::Var))
            if (Values(std::span<const Node>(&n, 1), command_).read(0, out)) return true;
    }
    return read_slow(i, out, error);
}

template <class T>
bool Args::read_slow(size_t i, T& out, Expected<Node>& error) const {
    auto r = get_slow<T>(i);
    if (!r) {
        error = Unexpected(std::move(r).error());
        return false;
    }
    out = std::move(*r);
    return true;
}

template <class T>
Expected<T> Args::get_slow(size_t i) const {
    static constexpr detail::ConvertInfo kInfo = detail::convert_info<T>();
    if (i >= size_) return Unexpected(type_error(i, Node(), kInfo.name));
    const Node* n = base_ + i;
    Node value;
    if (!evaluated_ && (n->type() == Type::Command || n->type() == Type::Var)) {
        auto r = eval_node(*n);
        if (!r) return Unexpected(std::move(r).error());
        value = std::move(*r);
        n = &value;
    }
    T out{};
    if (!Values(std::span<const Node>(n, 1), command_).read(0, out))
        return Unexpected(type_error(i, *n, std::is_same_v<T, Node> ? "value" : kInfo.name));
    if constexpr (std::is_same_v<T, std::string_view>)
        if (n == &value) keep(std::move(value));
    return out;
}

}  // namespace baker
