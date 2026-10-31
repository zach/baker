// The functions every Context starts with: control (if, set, do, func, override) and a
// small calculator core. Each is registered through the same func() as user code; the
// evaluator knows none of them by name.

#include "common/containers.hpp"
#include "internal.hpp"

#include <cstdint>
#include <limits>

namespace baker {
namespace {
// A script function's parameter: any value.
constexpr detail::ParamText kScriptParam{"value", {}};
}  // namespace

namespace {

// Ints wrap on overflow, as two's complement, rather than being undefined.
int64_t wrap(uint64_t v) noexcept {
    return static_cast<int64_t>(v);
}

Expected<Node> run_script(const void* callable, Args& args) {
    return args.context().impl().run_script(*static_cast<const ScriptBody*>(callable), args,
                                            nullptr);
}

// {func name ($a ...) (status stub) (doc "...") body...}, or {override name ...}.
Expected<Node> define(Args a, bool override) {
    const char* form = override ? "override" : "func";
    if (a.size() < 2 || a.raw(0).type() != Type::Symbol || a.raw(1).type() != Type::Array)
        return Unexpected(
            a.error(Str("write {") + form + " name ($params...) body...}", ErrorCode::Syntax));
    const Symbol name = *a.raw(0).try_sym();
    Context::Impl& impl = a.context().impl();
    detail::Shared holder = detail::Shared::make_in<ScriptBody>(impl.alloc, impl.alloc);
    // Filled here, before any other reference to it exists.
    auto* body = const_cast<ScriptBody*>(static_cast<const ScriptBody*>(holder.get()));
    if (!parse_params(*a.raw(1).try_array(), *body))
        return Unexpected(
            a.error("parameters of " + quoted(name) + " must be $variables", ErrorCode::Syntax));
    Status status = Status::Normal;
    Symbol replacement;
    Str doc;
    size_t i = 2;
    for (; i < a.size(); ++i) {
        // The leading (@status ...) and (@doc ...) forms; (status 1) is the body's first form.
        static const Symbol status_key("@status"), doc_key("@doc");
        const Array* meta = a.raw(i).try_array();
        const auto tag = meta && !meta->empty() ? (*meta)[0].try_sym() : std::nullopt;
        if (!tag || !tag->is_meta()) break;
        if (*tag != status_key && *tag != doc_key)
            return Unexpected(a.error(quoted(name) + " has unknown metadata " + quoted(*tag) +
                                          ": (@status ...) or (@doc ...)",
                                      ErrorCode::Syntax));
        if (*tag == status_key && meta->size() >= 2) {
            const Symbol s = (*meta)[1].try_sym().value_or(Symbol());
            status = s == "stub"          ? Status::Stub
                     : s == "provisional" ? Status::Provisional
                     : s == "deprecated"  ? Status::Deprecated
                                          : Status::Normal;
            if (meta->size() >= 3) replacement = (*meta)[2].try_sym().value_or(Symbol());
        } else if (*tag == doc_key && meta->size() >= 2) {
            doc = (*meta)[1].try_str().value_or("");
        }
    }
    impl.note_function(name);  // run_autorun restores what this replaces
    Function& f = impl.function(name);
    if (f.tier == Tier::Native && !f.overloads.empty()) {
        if (!override)
            return Unexpected(a.error(quoted(name) + " is a native function; write {override " +
                                          Str(name.str()) + " ...} to replace it",
                                      ErrorCode::Refused));
        a.context().notice(
            a.error("script {override} replaces native " + quoted(name), ErrorCode::Refused));
    }
    // The definition array lives in the parsed tree; holding a Ref keeps it alive.
    body->definition = Ref<Array>(const_cast<Array*>(&a.command()));
    body->body = i + 1;
    detail::Overload o;
    o.thunk = &run_script;
    o.min_args = o.max_args = static_cast<uint32_t>(body->params.size());
    body->param_info.assign(
        body->params.size(),
        detail::ParamInfo{&kScriptParam, detail::ParamKind::Value, nullptr, false, false});
    o.params = body->param_info;
    o.result = "value";
    o.callable = std::move(holder);
    f.tier = Tier::Script;
    f.status = status;
    f.replacement = replacement;
    f.doc = std::move(doc);
    f.file = a.command().file();
    f.line = a.command().line();
    a.context().impl().retire(f.overloads);  // this very function may be running
    f.overloads.push_back(std::move(o));
    f.refresh();
    return Node(name);
}

}  // namespace

bool parse_params(const Array& params, ScriptBody& body) {
    for (const Node& p : params.nodes()) {
        if (p.type() != Type::Var) return false;
        body.params.push_back(p.var_name());
    }
    return true;
}

// Script functions and handlers bind their parameters by dynamic scope, like do: arguments
// are evaluated first, in the caller's bindings, then bound with $this, then the body runs.
Expected<Node> Context::Impl::run_script(const ScriptBody& body, Args& args,
                                         const Node* this_node) {
    const size_t first = kept.size();
    for (size_t i = 0; i < args.size(); ++i) {
        auto r = args.eval(i);
        if (!r) return r;
        kept.push_back(std::move(*r));
    }
    Rebind rebind(*this);
    if (this_node) bind(sym_this(), *this_node);
    for (size_t i = 0; i < body.params.size(); ++i) bind(body.params[i], kept[first + i]);
    return run(body.definition->nodes().subspan(body.body));
}

namespace {

// A number, int or float, as the operators take it: a value type of its own, so a type
// error and `check` say "not number" and the reference shows `{+ number number...}`.
struct Number {
    Node value;
};
// The rest of a variadic operator's arguments, each a number: `{+ 1 2 3}`. Its own kind of
// rest parameter, so `check` holds every literal to the type (reference.cpp).
struct Numbers {
    Args rest;
};

}  // namespace

template <>
struct Convert<Number> {
    static constexpr std::string_view name = "number";
    static constexpr bool scalar = true;
    static bool from(const Values& v, Number& out) noexcept {
        if (v.empty() || !v[0].is_number()) return false;
        out.value = v[0];
        return true;
    }
};

namespace detail {
template <>
struct Param<Numbers> {
    using Holder = Numbers;
    static constexpr ParamKind kind = ParamKind::Rest;
    static constexpr std::string_view type() { return "number..."; }
    static bool match_number(const Node& n, bool) noexcept { return n.is_number(); }
    static constexpr auto match = &match_number;
    static bool read(const Args& a, size_t i, std::optional<Holder>& out, Expected<Node>&) {
        out.emplace(Numbers{a.rest(i)});
        return true;
    }
    static Numbers pass(Numbers& h) noexcept { return h; }
};
}  // namespace detail

namespace {

double as_double(const Node& n) noexcept {
    return n.type() == Type::Int ? static_cast<double>(*n.try_int()) : *n.try_float();
}

// Argument `i` of `rest`, evaluated once, which must be a number; `first` is the argument
// number of rest's first, for the message.
Expected<Node> number_at(const Args& rest, size_t i, size_t first) {
    auto v = rest.eval(i);
    if (!v || v->is_number()) return v;
    return Unexpected(rest.error("argument " + detail::decimal(static_cast<int64_t>(first + i)) +
                                 " of " + quoted(rest.name()) + " is " + Str(type_name(v->type())) +
                                 ", not number"));
}

enum class Op : uint8_t { Add, Sub, Mul, Div };

// `a op b` for two numbers: ints stay ints (wrapping) until a float joins; a quotient is
// always a float.
Node arith(Op op, const Node& a, const Node& b) noexcept {
    if (a.type() == Type::Int && b.type() == Type::Int && op != Op::Div) {
        const auto x = static_cast<uint64_t>(*a.try_int());
        const auto y = static_cast<uint64_t>(*b.try_int());
        switch (op) {
            case Op::Add: return wrap(x + y);
            case Op::Sub: return wrap(x - y);
            default: return wrap(x * y);
        }
    }
    const double x = as_double(a);
    const double y = as_double(b);
    switch (op) {
        case Op::Add: return x + y;
        case Op::Sub: return x - y;
        case Op::Mul: return x * y;
        default: return x / y;
    }
}

// `acc op rest[0] op rest[1] ...`, left to right; rest is argument `first` on.
Expected<Node> fold(Op op, Node acc, const Args& rest, size_t first) {
    for (size_t i = 0; i < rest.size(); ++i) {
        auto v = number_at(rest, i, first);
        if (!v) return v;
        acc = arith(op, acc, *v);
    }
    return acc;
}

// == and != compare numbers by value (1 equals 1.0), anything else structurally.
bool equal(const Node& a, const Node& b) noexcept {
    if (a.is_number() && b.is_number()) {
        if (a.type() == Type::Int && b.type() == Type::Int) return *a.try_int() == *b.try_int();
        return as_double(a) == as_double(b);
    }
    return structurally_equal(a, b);
}
bool less(const Node& a, const Node& b) noexcept {
    if (a.type() == Type::Int && b.type() == Type::Int) return *a.try_int() < *b.try_int();
    return as_double(a) < as_double(b);
}
bool less_eq(const Node& a, const Node& b) noexcept {
    if (a.type() == Type::Int && b.type() == Type::Int) return *a.try_int() <= *b.try_int();
    return as_double(a) <= as_double(b);
}
bool greater(const Node& a, const Node& b) noexcept {
    return less(b, a);
}
bool greater_eq(const Node& a, const Node& b) noexcept {
    return less_eq(b, a);
}

// `cmp` over every adjacent pair of a, b and the rest (numbers, argument 3 on), each evaluated
// once, stopping at the first pair that fails.
template <class Cmp>
Expected<bool> chain(const Node& a, const Node& b, const Args& rest, Cmp cmp) {
    if (!cmp(a, b)) return false;
    if (rest.size() == 0) return true;
    Node prev = b;
    for (size_t i = 0; i < rest.size(); ++i) {
        auto v = number_at(rest, i, 3);
        if (!v) return Unexpected(std::move(v).error());
        if (!cmp(prev, *v)) return false;
        prev = std::move(*v);
    }
    return true;
}

// The operators as natives, each its own function so the operation is a constant: `+ *`
// fold two numbers or more, `< <= > >=` chain over two numbers or more.
template <Op op>
constexpr auto folded = [](const Number& a, const Number& b, Numbers more) {
    return fold(op, arith(op, a.value, b.value), more.rest, 3);
};
template <bool (*cmp)(const Node&, const Node&) noexcept>
constexpr auto ordered = [](const Number& a, const Number& b, Numbers more) {
    return chain(a.value, b.value, more.rest, cmp);
};

// A built-in's doc text: under BAKER_SHIPPING nothing, so the literal is dropped where it is
// written, as the reference it is for is a tool's.
struct Doc {
    constexpr Doc() = default;
    constexpr Doc(
        [[maybe_unused]] const char* text) noexcept {  // NOLINT(google-explicit-constructor)
        if constexpr (!kShipping) view = text;
    }
    std::string_view view;
};

template <class F>
void add(Context& ctx, Symbol name, F&& f, Doc doc = {}) {
    auto r = ctx.func(name, std::forward<F>(f));
    if (r && !doc.view.empty()) r->doc(doc.view);
}

}  // namespace

void register_builtins(Context& ctx) {
    // --- Control. ---
    add(
        ctx, "if",
        [](bool cond, Raw<Node> then, std::optional<Raw<Node>> otherwise) -> Expected<Node> {
            if (cond) return then.eval();
            if (otherwise) return otherwise->eval();
            return Node();
        },
        "`{if cond then [else]}`: evaluates `then` if `cond` is true, else `else` if given "
        "(else 0). Only the branch taken is evaluated.");
    add(
        ctx, "set",
        [](Raw<Var> place, Node value) -> Expected<Node> {
            auto r = place.store(value);
            if (!r) return Unexpected(std::move(r).error());
            return value;
        },
        "`{set $var value}` or, in a method, `{set [property] value}`: stores the value and "
        "returns it.");
    add(
        ctx, "do",
        [](Args a) -> Expected<Node> {
            Context::Impl& impl = a.context().impl();
            Rebind rebind(impl);
            size_t from = 0;
            if (a.size() > 0 && a.raw(0).type() == Type::Array) {
                from = 1;
                for (const Node& b : a.raw(0).try_array()->nodes()) {
                    if (b.type() == Type::Var) {
                        impl.bind(b.var_name(), unset_node());
                        continue;
                    }
                    const Array* pair = b.try_array();
                    if (!pair || pair->size() != 2 || (*pair)[0].type() != Type::Var)
                        return Unexpected(
                            a.error("do binds $variables or ($variable value)", ErrorCode::Syntax));
                    auto v = impl.eval((*pair)[1]);
                    if (!v) return v;
                    impl.bind((*pair)[0].var_name(), std::move(*v));
                }
            }
            Node last;
            for (size_t i = from; i < a.size(); ++i) {
                auto r = a.eval(i);
                if (!r) return r;
                last = std::move(*r);
            }
            return last;
        },
        "`{do ($a ($b 1)) body...}`: binds the variables (unset, or to a value) for the "
        "duration of the body, restores them after, and returns the body's last value. "
        "Without the list, just runs the body.");
    add(
        ctx, "func", [](Args a) { return define(a, false); },
        "`{func name ($params...) body...}`: defines a script function. `(@status stub)`, "
        "`(@status provisional)`, `(@status deprecated replacement)` and `(@doc \"...\")` may "
        "follow the parameters.");
    add(
        ctx, "override", [](Args a) { return define(a, true); },
        "As `func`, but may replace a native function, which it logs.");

    // --- Arithmetic: variadic, folded left. Ints stay ints (wrapping on overflow)
    // until a float joins. ---
    add(ctx, "+", folded<Op::Add>, "Sum of two numbers or more, left to right: `{+ 1 2 3}` is 6.");
    add(
        ctx, "-",
        [](const Number& a, Numbers more) -> Expected<Node> {
            if (more.rest.size() > 0) return fold(Op::Sub, a.value, more.rest, 2);
            if (const auto i = a.value.try_int()) return Node(wrap(0 - static_cast<uint64_t>(*i)));
            return Node(-*a.value.try_float());
        },
        "Difference, left to right: `{- 10 2 3}` is 5; with one argument, negation.");
    add(ctx, "*", folded<Op::Mul>, "Product of two numbers or more: `{* 2 3 4}` is 24.");
    add(ctx, "/", [](double a, double b) { return a / b; }, "Quotient, always a float.");
    add(
        ctx, "%",
        [](int64_t x, int64_t y) -> Expected<int64_t> {
            if (y == 0) return Unexpected(make_error(ErrorCode::Range, "'%' by zero"));
            if (y == -1) return 0;  // INT64_MIN % -1 overflows
            return x % y;
        },
        "Remainder of int division, with the sign of the dividend.");

    // --- Assignment: `{$x += 1}`, `{[hp] -= $n}`: load, apply, store, return the value. ---
    auto assign = [](Op op) {
        return [op](Raw<Var> place, const Number& by) -> Expected<Node> {
            const Node current = place.load();
            if (!place.is_set() || !current.is_number())
                return Unexpected(make_error(
                    place.is_set() ? ErrorCode::Type : ErrorCode::NotFound,
                    (place.is_property() ? Str("[...]") : "$" + Str(place.name().str())) +
                        (place.is_set()
                             ? " holds " + Str(type_name(current.type())) + ", not a number"
                             : Str(" is not set"))));
            Node value = arith(op, current, by.value);
            if (auto r = place.store(value); !r) return Unexpected(std::move(r).error());
            return value;
        };
    };
    add(ctx, "+=", assign(Op::Add),
        "`{+= $x n}`, or in a method `{+= [property] n}`: adds, stores and returns the new "
        "value.");
    add(ctx, "-=", assign(Op::Sub), "`{-= $x n}`: subtracts, stores and returns the new value.");
    add(ctx, "*=", assign(Op::Mul), "`{*= $x n}`: multiplies, stores and returns the new value.");
    add(ctx, "/=", assign(Op::Div), "`{/= $x n}`: divides (a float), stores and returns it.");

    // --- Comparison: chained over every adjacent pair, each argument evaluated once, stopping
    // at the first pair that fails: `{0.3 <= $value <= 0.7}`. ---
    add(
        ctx, "==", [](const Node& a, const Node& b) { return equal(a, b); },
        "Equality: numbers by value (1 equals 1.0), anything else by structure.");
    add(ctx, "!=", [](const Node& a, const Node& b) { return !equal(a, b); }, "Inequality.");
    add(ctx, "<", ordered<less>, "Less than, over every adjacent pair: `{< 1 $x 10}`.");
    add(ctx, "<=", ordered<less_eq>,
        "Less than or equal, over every adjacent pair: `{<= 0.3 $value 0.7}` is a band.");
    add(ctx, ">", ordered<greater>, "Greater than, over every adjacent pair.");
    add(ctx, ">=", ordered<greater_eq>, "Greater than or equal, over every adjacent pair.");

    // --- Logic. and/or stop at the first argument that decides the result. ---
    add(
        ctx, "and",
        [](Args a) -> Expected<bool> {
            for (size_t i = 0; i < a.size(); ++i) {
                auto v = a.get<bool>(i);
                if (!v || !*v) return v;
            }
            return true;
        },
        "True if every argument is; stops at the first false one.");
    add(
        ctx, "or",
        [](Args a) -> Expected<bool> {
            for (size_t i = 0; i < a.size(); ++i) {
                auto v = a.get<bool>(i);
                if (!v || *v) return v;
            }
            return false;
        },
        "True if any argument is; stops at the first true one.");
    add(ctx, "not", [](bool v) { return !v; }, "Negation.");
}

}  // namespace baker
