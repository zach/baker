// Rules between values (<baker/tools/rules.hpp>): declared in shapes, evaluated here.

#include <baker/tools/rules.hpp>

#include "common/containers.hpp"
#include "data/text.hpp"

#include <algorithm>

namespace baker::tools {

namespace {

// The tree check_rules is checking, for {data}: one per thread, as a Context is.
thread_local const Array* checked = nullptr;

Expected<Node> not_checking(const Args& a) {
    return Unexpected(
        a.error("reads the tree a rule is checked against, and no rule is being "
                "checked",
                ErrorCode::Refused));
}

// The path arguments `first` to `last` name, each evaluated: a symbol (dotted or not), an
// integer index, or a string.
Expected<Str> path_of(const Args& a, size_t first, size_t last) {
    Str path;
    for (size_t i = first; i < last; ++i) {
        auto v = a.eval(i);
        if (!v) return Unexpected(std::move(v).error());
        Str seg;
        if (const auto s = v->try_sym())
            seg = s->str();
        else if (const auto t = v->try_str())
            seg = *t;
        else if (v->type() == Type::Int)
            seg = detail::text_of(*v);
        else
            return Unexpected(a.error("takes a path of symbols, strings, or indices, not " +
                                      detail::preview(*v)));
        if (!path.empty()) path += '.';
        path += seg;
    }
    if (path.empty()) return Unexpected(a.error("needs a path"));
    return path;
}

Expected<Node> data(Args a) {
    if (!checked) return not_checking(a);
    auto path = path_of(a, 0, a.size());
    if (!path) return Unexpected(std::move(path).error());
    const Array* at = checked->try_get(Path(path->view()));
    if (!at)
        return Unexpected(a.error("the checked tree has no '" + *path + "'", ErrorCode::NotFound));
    const Values v = at->values();
    if (v.size() == 1) return v[0];
    return Node(Array::make_copy(std::span<const Node>(v.begin(), v.end())));
}

Expected<Node> data_sum(Args a) {
    if (!checked) return not_checking(a);
    if (a.size() < 1 || a.size() > 2) return Unexpected(a.error("takes a path and a field"));
    auto path = path_of(a, 0, 1);  // the second argument, if any, is the field
    if (!path) return Unexpected(std::move(path).error());
    std::optional<Symbol> field;
    if (a.size() == 2) {
        auto f = a.eval(1);
        if (!f) return Unexpected(std::move(f).error());
        if (!f->try_sym())
            return Unexpected(a.error("takes a field name, not " + detail::preview(*f)));
        field = *f->try_sym();
    }
    const Array* table = checked->try_get(Path(path->view()));
    if (!table)
        return Unexpected(a.error("the checked tree has no '" + *path + "'", ErrorCode::NotFound));
    int64_t whole = 0;
    double sum = 0;
    bool ints = true;
    for (const Node& c : table->values()) {
        const Array* child = c.try_array();
        if (!child) continue;
        const Array* holder = field ? child->try_find(*field) : child;
        const Values v = holder ? holder->values() : Values();
        const Node* n = v.size() >= 1 ? &v[0] : nullptr;
        if (!n || !n->is_number())
            return Unexpected(a.error("'" + *path + "." + Str(child->tag().str()) +
                                      (field ? "." + Str(field->str()) : Str()) +
                                      "' is not a number to sum"));
        if (n->type() == Type::Int)
            whole += *n->try_int();
        else
            ints = false;
        sum += *n->try_float();
    }
    return ints ? Node(whole) : Node(sum);
}

// The $variables `n` mentions, each once, in the order written.
void mentioned(const Node& n, Vec<Symbol>& out) {
    if (const Symbol v = n.var_name(); !v.empty()) {
        if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
        return;
    }
    if (const Array* a = n.array())
        for (const Node& c : *a) mentioned(c, out);
}

}  // namespace

void declare_rule_functions(Context& ctx) {
    if (ctx.has_func("data")) return;
    (void)ctx.func("data", &data);
    (void)ctx.func("data.sum", &data_sum);
}

void check_rules(const Array& tree, const shape::Set& shapes, Context& ctx, shape::Report& report) {
    declare_rule_functions(ctx);
    Vec<Error> errors(report.errors.begin(), report.errors.end());
    Vec<Error> warnings(report.warnings.begin(), report.warnings.end());
    // Each rule's $variables, and their cells, found once per check rather than per record.
    struct Reads {
        const shape::Rule* rule;
        size_t first, count;  // into `cells`
    };
    Vec<Reads> reads;
    Vec<VarRef> cells;
    Vec<Symbol> names;
    for (const shape::Shape& s : shapes.shapes())
        for (const shape::Rule& rule : s.rules) {
            names.clear();
            mentioned(rule.expression, names);
            reads.push_back({&rule, cells.size(), names.size()});
            for (const Symbol name : names) cells.push_back(ctx.var(name));
        }
    const Array* previous = checked;
    checked = &tree;
    Vec<Node> saved;
    Vec<bool> was_set;
    shape::each_ruled(
        tree, shapes, [&](const Array& record, const shape::Shape& s, std::string_view path) {
            for (const shape::Rule& rule : s.rules) {
                const Reads& r = *std::find_if(reads.begin(), reads.end(),
                                               [&](const Reads& x) { return x.rule == &rule; });
                const std::span<VarRef> vars(cells.data() + r.first, r.count);
                // The record's fields as $variables, the Context's own put back after.
                saved.clear();
                was_set.clear();
                for (VarRef& v : vars) {
                    was_set.push_back(v.is_set());
                    saved.push_back(v.value());
                    const Array* f = record.try_find(v.name());
                    if (!f) {
                        v.unset();
                        continue;
                    }
                    const Values vals = f->values();
                    v = vals.size() == 1 ? vals[0]
                                         : Node(Array::make_copy(
                                               std::span<const Node>(vals.begin(), vals.end())));
                }
                auto result = ctx.eval(rule.expression);
                // The values the rule read, for its message: only when it fails.
                auto shown = [&] {
                    Str out;
                    for (const VarRef& v : vars)
                        if (v.is_set())
                            out += (out.empty() ? "$" : ", $") + Str(v.name().str()) + " " +
                                   detail::text_of(v.value());
                    return out;
                };
                auto located = [&](ErrorCode code, const Str& message) {
                    Error e = make_error(code, message, record.file(), record.line());
                    e.set_path(path);
                    return e;
                };
                bool holds = false;
                const bool answered =
                    result && Values(std::span<const Node>(&*result, 1), nullptr).read(0, holds);
                if (!answered || !holds) {
                    if (!answered) {
                        errors.push_back(located(
                            ErrorCode::Type, "rule \"" + Str(rule.message) +
                                                 "\" does not evaluate: " +
                                                 (result ? "it gives " + detail::preview(*result) +
                                                               ", not true or false"
                                                         : Str(result.error().message()))));
                    } else {
                        Str message(rule.message);
                        if (const Str values = shown(); !values.empty())
                            message += " (" + values + ")";
                        (rule.warn ? warnings : errors)
                            .push_back(located(ErrorCode::Range, message));
                    }
                }
                for (size_t i = 0; i < vars.size(); ++i) {
                    if (was_set[i])
                        vars[i] = saved[i];
                    else
                        vars[i].unset();
                }
            }
        });
    checked = previous;
    report.errors = List<Error>({errors.data(), errors.size()});
    report.warnings = List<Error>({warnings.data(), warnings.size()});
}

Config::Validator rules_validator(Context& ctx) {
    return [&ctx](const ValidatorStack& stack, const Callback<void(const Error&)>& warn) {
        shape::Set set;
        shape::Report read;  // the shapes' own errors are shape::validator()'s to report
        for (const Ref<Array>& tree : stack.layers) set.add(*tree, read, &stack.session);
        shape::Report report;
        check_rules(stack.root, set, ctx, report);
        if (!warn) return;
        for (const Error& e : report.errors) warn(e);
        for (const Error& w : report.warnings) warn(w);
    };
}

}  // namespace baker::tools
