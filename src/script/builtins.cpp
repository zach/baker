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
        const Array* meta = a.raw(i).try_array();
        if (!meta || meta->empty()) break;
        if ((*meta)[0].is("status") && meta->size() >= 2) {
            const Symbol s = (*meta)[1].try_sym().value_or(Symbol());
            status = s == "stub"          ? Status::Stub
                     : s == "provisional" ? Status::Provisional
                     : s == "deprecated"  ? Status::Deprecated
                                          : Status::Normal;
            if (meta->size() >= 3) replacement = (*meta)[2].try_sym().value_or(Symbol());
        } else if ((*meta)[0].is("doc") && meta->size() >= 2) {
            doc = (*meta)[1].try_str().value_or("");
        } else {
            break;
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
        detail::ParamInfo{&kScriptParam, detail::ParamKind::Value, nullptr, false});
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

// == and != compare numbers by value (1 equals 1.0), anything else structurally.
bool equal(const Node& a, const Node& b) noexcept {
    if (a.is_number() && b.is_number()) {
        if (a.type() == Type::Int && b.type() == Type::Int) return *a.try_int() == *b.try_int();
        return *a.try_float() == *b.try_float();
    }
    return structurally_equal(a, b);
}

template <class F>
void add(Context& ctx, Symbol name, F&& f, std::string_view doc = {}) {
    auto r = ctx.func(name, std::forward<F>(f));
    if (r && !doc.empty()) r->doc(doc);
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
                        impl.bind(b.var_name(), kUnset);
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
        "`{func name ($params...) body...}`: defines a script function. `(status stub)`, "
        "`(status provisional)`, `(status deprecated replacement)` and `(doc \"...\")` may "
        "follow the parameters.");
    add(
        ctx, "override", [](Args a) { return define(a, true); },
        "As `func`, but may replace a native function, which it logs.");

    // --- Arithmetic. Ints stay ints (wrapping on overflow); a float makes it float. ---
    add(ctx, "+", [](int64_t a, int64_t b) { return wrap(uint64_t(a) + uint64_t(b)); }, "Sum.");
    add(ctx, "+", [](double a, double b) { return a + b; });
    add(
        ctx, "-", [](int64_t a, int64_t b) { return wrap(uint64_t(a) - uint64_t(b)); },
        "Difference, or with one argument, negation.");
    add(ctx, "-", [](double a, double b) { return a - b; });
    add(ctx, "-", [](int64_t a) { return wrap(0 - uint64_t(a)); });
    add(ctx, "-", [](double a) { return -a; });
    add(ctx, "*", [](int64_t a, int64_t b) { return wrap(uint64_t(a) * uint64_t(b)); }, "Product.");
    add(ctx, "*", [](double a, double b) { return a * b; });
    add(ctx, "/", [](double a, double b) { return a / b; }, "Quotient, always a float.");
    add(
        ctx, "%",
        [](int64_t x, int64_t y) -> Expected<int64_t> {
            if (y == 0) return Unexpected(make_error(ErrorCode::Range, "'%' by zero"));
            if (y == -1) return 0;  // INT64_MIN % -1 overflows
            return x % y;
        },
        "Remainder of int division, with the sign of the dividend.");

    // --- Comparison. ---
    add(
        ctx, "==", [](const Node& a, const Node& b) { return equal(a, b); },
        "Equality: numbers by value (1 equals 1.0), anything else by structure.");
    add(ctx, "!=", [](const Node& a, const Node& b) { return !equal(a, b); }, "Inequality.");
    add(ctx, "<", [](int64_t a, int64_t b) { return a < b; }, "Less than.");
    add(ctx, "<", [](double a, double b) { return a < b; });
    add(ctx, "<=", [](int64_t a, int64_t b) { return a <= b; }, "Less than or equal.");
    add(ctx, "<=", [](double a, double b) { return a <= b; });
    add(ctx, ">", [](int64_t a, int64_t b) { return a > b; }, "Greater than.");
    add(ctx, ">", [](double a, double b) { return a > b; });
    add(ctx, ">=", [](int64_t a, int64_t b) { return a >= b; }, "Greater than or equal.");
    add(ctx, ">=", [](double a, double b) { return a >= b; });

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
