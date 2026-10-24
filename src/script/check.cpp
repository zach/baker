// Load-time checking: it reads a Context's registrations and evaluates nothing. The
// generated reference, which reads them too, is baker::tools' (src/tools/reference.cpp).

#include <baker/config.hpp>
#include <baker/write.hpp>

#include "common/containers.hpp"
#include "internal.hpp"

#include <algorithm>
#include <cstdio>

namespace baker {
namespace {

bool evaluable(const Node& n) noexcept {
    return n.type() == Type::Command || n.type() == Type::Var || n.type() == Type::Property;
}

struct Checker {
    // A function defined by {func} or {override} in the checked tree.
    struct Defined {
        uint32_t arity = 0;
        bool stub = false;
        Symbol file;
    };

    // What check() accumulates, as growable lists; it becomes the CheckReport at the end.
    struct DemandBuild {
        Symbol name;
        bool stub;
        Vec<CheckReport::Site> sites;
        Symbol on;
    };
    struct Build {
        Vec<Error> errors;
        Vec<DemandBuild> demand;
        Vec<Error> warnings;
    };

    Context::Impl& impl;
    Build& report;
    const LoadSession* packs;
    HashMap<Symbol, Defined> defined;
    HashMap<Symbol, Vec<Symbol>> classes;  // {class} in the tree: handlers
    HashMap<Symbol, Symbol> objects;       // {new Class name} in the tree
    HashMap<Str, size_t> demand_index;
    Vec<std::pair<Symbol, Symbol>> warned;  // (pack, function)

    static Str key(Symbol name, Symbol on) { return Str(on.str()) + "." + Str(name.str()); }

    void demand(Symbol name, bool stub, const Array& at, Symbol on = {}) {
        auto [it, inserted] = demand_index.try_emplace(key(name, on), report.demand.size());
        if (inserted) report.demand.push_back({name, stub, {}, on});
        report.demand[it->second].sites.push_back({at.file(), at.line()});
    }

    void error(const Array& at, std::string_view message) {
        report.errors.push_back(make_error(ErrorCode::Type, message, at.file(), at.line()));
    }

    void collect(const Array& a, bool is_command) {
        if (a.height() > 1)
            for (const Node& n : a.nodes())
                if (const Array* c = n.array()) collect(*c, n.type() == Type::Command);
        if (!is_command || a.size() < 2) return;
        if ((a[0].is("class") || a[0].is("extend")) && a[1].try_sym()) {
            auto& handlers = classes[*a[1].try_sym()];
            for (size_t i = 2; i < a.size(); ++i)
                if (const Array* h = a[i].try_array(); h && !h->empty() && (*h)[0].try_sym())
                    handlers.push_back(*(*h)[0].try_sym());
            return;
        }
        if (a[0].is("new") && a.size() >= 3 && a[1].try_sym() && a[2].try_sym()) {
            objects[*a[2].try_sym()] = *a[1].try_sym();
            return;
        }
        if (a.size() < 3 || (!a[0].is("func") && !a[0].is("override"))) return;
        const auto name = a[1].try_sym();
        const Array* params = a[2].try_array();
        if (!name || !params) return;
        bool stub = false;
        for (size_t i = 3; i < a.size(); ++i) {
            const Array* m = a[i].try_array();
            if (!m || m->size() < 2) break;
            if ((*m)[0].is("status")) stub = (*m)[1].is("stub");
        }
        defined[*name] = {params->size(), stub, a.file()};
    }

    // A literal argument against the value types of a parameter of a type only described,
    // as Values::read reads a compound value: an array as its contents, else the one value.
    static bool reads_values(const detail::ParamInfo& p, const Node& n) {
        const Array* a = n.try_array();
        // A list's elements, unless one is evaluated: that is left to the call.
        if (!p.element().type.empty())
            return a && (std::any_of(a->begin(), a->end(), evaluable) ||
                         detail::reads_list(p.element(), Values(a->nodes(), a)));
        return detail::reads_as(
            p.values(), p.count(),
            a ? Values(a->nodes(), a) : Values(std::span<const Node>(&n, 1), nullptr));
    }

    // Whether `o` accepts every literal argument of `command`, from index `first`.
    static bool fits(const detail::Overload& o, const Array& command, size_t first) {
        const size_t n = command.size() - first;
        for (size_t i = 0; i < n && i < o.params.size(); ++i) {
            const detail::ParamInfo& p = o.params[i];
            if (p.kind == detail::ParamKind::Rest) return true;
            if (evaluable(command[i + first])) continue;
            const Node& arg = command[i + first];
            if (p.match
                    ? !p.match(arg, true)
                    : (!p.values().empty() || !p.element().values.empty()) && !reads_values(p, arg))
                return false;
            // A literal list's elements, when none is evaluated: a native's too.
            if (p.match && !p.element().values.empty())
                if (const Array* a = arg.try_array();
                    a && std::none_of(a->begin(), a->end(), evaluable) &&
                    !detail::reads_list(p.element(), Values(a->nodes(), a)))
                    return false;
        }
        return true;
    }

    // Arity and literal types against an overload set.
    void overloads(Symbol name, const Vec<detail::Overload>& set, const Array& command,
                   size_t first) {
        const uint32_t n = command.size() - static_cast<uint32_t>(first);
        const detail::Overload* arity_ok = nullptr;
        for (const detail::Overload& o : set) {
            if (n < o.min_args || n > o.max_args) continue;
            if (fits(o, command, first)) return;
            arity_ok = &o;
        }
        if (!arity_ok) {
            if (set.size() == 1)
                error(command, arity_message(name, set[0].min_args, set[0].max_args, n));
            else
                error(command, "no overload of " + quoted(name) + " takes " + arity_text(n, n));
            return;
        }
        Str given;
        for (size_t i = first; i < command.size(); ++i) {
            if (i > first) given += ' ';
            given += evaluable(command[i]) ? "?" : type_name(command[i].type());
        }
        Str have;
        for (const detail::Overload& o : set)
            if (n >= o.min_args && n <= o.max_args) have += " " + signature_text(o);
        error(command, quoted(name) + " is given (" + given + "); it takes" + have);
    }

    // {object method ...} where the object's class is known: a named object, or one the
    // tree creates with {new Class name}.
    void send(const Array& command, Symbol cls, const detail::TypeRecord* type) {
        const auto method = command.size() > 1 ? command[1].try_sym() : std::nullopt;
        if (!method) return;
        for (const char* m : {"get", "set", "type", "name"})
            if (*method == Symbol(m)) return;
        auto handles = [&](Symbol c) {
            if (auto it = classes.find(c); it != classes.end())
                for (Symbol h : it->second)
                    if (h == *method) return true;
            const ScriptClass* sc = impl.script_class(c);
            return sc && sc->handler(*method);
        };
        if (handles(cls)) return;
        if (type) {
            for (const detail::TypeRecord* b : type->bases)
                if (handles(b->symbol)) return;
            if (const detail::MethodEntry* m = type->method(*method)) {
                if (m->status == Status::Stub) demand(*method, true, command, cls);
                overloads(*method, m->overloads, command, 2);
                return;
            }
        }
        demand(*method, false, command, cls);
    }

    // The one-way rule: softer may call harder, never the reverse.
    void one_way(const Array& command, Symbol name, Symbol defined_in) {
        const Pack* mine = packs->pack_of(command.file());
        const Pack* theirs = mine ? packs->pack_of(defined_in) : nullptr;
        if (!theirs || theirs->rank <= mine->rank) return;
        const std::pair<Symbol, Symbol> key{mine->name, name};
        if (std::find(warned.begin(), warned.end(), key) != warned.end()) return;
        warned.push_back(key);
        report.warnings.push_back(
            make_error(ErrorCode::Refused,
                       "pack " + quoted(mine->name) + " (level " + Str(mine->level.str()) +
                           ") calls " + quoted(name) + " from the softer pack " +
                           quoted(theirs->name) + " (level " + Str(theirs->level.str()) + ")",
                       command.file(), command.line()));
    }

    void call(const Array& command) {
        const auto name = command[0].try_sym();
        if (!name) return;  // an object computed at run time
        const uint32_t n = command.size() - 1;
        const Function* f = impl.find(*name);
        if (packs) {
            if (auto it = defined.find(*name); it != defined.end())
                one_way(command, *name, it->second.file);
            else if (f && f->tier == Tier::Script)
                one_way(command, *name, f->file);
        }
        if (f && !f->overloads.empty()) {
            if (f->status == Status::Stub) demand(*name, true, command);
            overloads(*name, f->overloads, command, 1);
            return;
        }
        if (const Context::Impl::Slot* s = impl.slot(impl.named(*name))) {
            send(command, Symbol(impl.class_name(*s)), s->script ? nullptr : &s->object->type());
            return;
        }
        if (const Symbol type = impl.declared_object(*name); !type.empty()) {
            send(command, type, impl.type_named(type));
            return;
        }
        if (auto it = objects.find(*name); it != objects.end()) {
            send(command, it->second, impl.type_named(it->second));
            return;
        }
        if (auto it = defined.find(*name); it != defined.end()) {
            if (it->second.stub) demand(*name, true, command);
            if (n != it->second.arity)
                error(command, arity_message(*name, it->second.arity, it->second.arity, n));
            return;
        }
        demand(*name, false, command);
    }

    void walk(const Array& a, bool is_command) {
        if (is_command && !a.empty()) call(a);
        if (a.height() > 1)
            for (const Node& n : a.nodes())
                if (const Array* c = n.array()) walk(*c, n.type() == Type::Command);
    }
};

}  // namespace

void check(std::span<const Array* const> trees, const Context& ctx, CheckReport& report,
           const CheckOptions& options) {
    // The report accumulates: start from what it holds.
    Checker::Build b;
    b.errors.assign(report.errors.begin(), report.errors.end());
    b.warnings.assign(report.warnings.begin(), report.warnings.end());
    for (const CheckReport::Demand& d : report.demand)
        b.demand.push_back({d.name, d.stub, {d.sites.begin(), d.sites.end()}, d.on});
    Checker c{ctx.impl(), b, options.packs, {}, {}, {}, {}, {}};
    for (size_t i = 0; i < b.demand.size(); ++i)
        c.demand_index[Checker::key(b.demand[i].name, b.demand[i].on)] = i;
    // Every tree's definitions first, so a call in one file to a function another defines
    // counts as defined.
    for (const Array* tree : trees)
        if (tree) c.collect(*tree, false);
    for (const Array* tree : trees)
        if (tree) c.walk(*tree, false);
    Vec<CheckReport::Demand> demand;
    for (const Checker::DemandBuild& d : b.demand)
        demand.push_back({d.name, d.stub, List<CheckReport::Site>(d.sites), d.on});
    report.unresolved_fails = report.unresolved_fails || options.unresolved_fails;
    report.errors = List<Error>(b.errors);
    report.warnings = List<Error>(b.warnings);
    report.demand = List<CheckReport::Demand>(demand);

    // Repetition, across this call's trees.
    const List<Repeat> found = find_repeats(trees, options.repeats);
    if (!found.empty()) {
        Vec<Repeat> repeats(report.repeats.begin(), report.repeats.end());
        repeats.insert(repeats.end(), found.begin(), found.end());
        report.repeats = List<Repeat>(std::span<const Repeat>(repeats.data(), repeats.size()));
    }
    // Packs one person keeps: the single-gardener finding, once a pack.
    if (options.packs) {
        Vec<Error> findings(report.findings.begin(), report.findings.end());
        for (const Pack& p : options.packs->packs()) {
            if (p.owners.size() != 1) continue;  // none declared, or enough
            if (std::any_of(findings.begin(), findings.end(),
                            [&](const Error& f) { return f.file() == p.file; }))
                continue;
            findings.push_back(make_error(ErrorCode::Refused,
                                          "pack " + quoted(p.name) + " has one owner, " +
                                              Str(p.owners[0].str()) + ": no one else keeps it",
                                          p.file));
        }
        report.findings = List<Error>(findings);
    }
}

void check(const Array& tree, const Context& ctx, CheckReport& report,
           const CheckOptions& options) {
    const Array* trees[1] = {&tree};
    check(std::span<const Array* const>(trees, 1), ctx, report, options);
}

CheckReport check(const Array& tree, const Context& ctx, const CheckOptions& options) {
    CheckReport report;
    check(tree, ctx, report, options);
    return report;
}

void check(const Config& cfg, const Context& ctx, CheckReport& report, const CheckOptions& options,
           std::span<const Array* const> extra) {
    // The #autorun commands, as one tree: each keeps its own file and line.
    const List<Node> commands = cfg.autorun();
    const Ref<Array> autorun = Array::make_copy(commands.items());
    Vec<const Array*> trees;
    trees.push_back(cfg.root().get());
    trees.push_back(autorun.get());
    for (const Array* t : extra) trees.push_back(t);
    CheckOptions o = options;
    if (!o.packs) o.packs = &cfg.read_session();
    check(std::span<const Array* const>(trees.data(), trees.size()), ctx, report, o);
    // The data against the tweaks the Config binds: what the game declares tunable.
    if (const TweakRegistry* tweaks = cfg.tweak_registry()) {
        TweakFindings f = check_tweaks(*cfg.root(), *tweaks);
        report.tweak_errors = std::move(f.errors);
        report.tweak_warnings = std::move(f.warnings);
    }
}

Callback<void(const ValidatorStack&, const Callback<void(const Error&)>&)> check_validator(
    const Context& ctx, const CheckOptions& options) {
    CheckOptions o = options;
    o.repeats.min_repeats = 0;  // the repetition report is for tools
    return [&ctx, o](const ValidatorStack& stack, const Callback<void(const Error&)>& warn) {
        if (!warn) return;
        CheckReport report;
        CheckOptions with = o;
        if (!with.packs) with.packs = &stack.session;
        check(stack.root, ctx, report, with);
        for (const Error& e : report.errors) warn(e);
        for (const Error& w : report.warnings) warn(w);
    };
}

void write(const CheckReport& report, TextSink text) {
    Str out;
    for (const Error& e : report.load) out += "load: " + Str(baker::to_string(e)) + "\n";
    for (const Error& e : report.errors) out += Str(baker::to_string(e)) + "\n";
    for (const Error& e : report.shape_errors) out += "shape: " + Str(baker::to_string(e)) + "\n";
    for (const Error& e : report.shape_warnings)
        out += "shape warning: " + Str(baker::to_string(e)) + "\n";
    for (const Error& e : report.tweak_errors) out += "tweak: " + Str(baker::to_string(e)) + "\n";
    for (const Error& e : report.tweak_warnings)
        out += "tweak warning: " + Str(baker::to_string(e)) + "\n";
    for (const Error& w : report.warnings) out += "warning: " + Str(baker::to_string(w)) + "\n";
    for (const CheckReport::Demand& d : report.demand) {
        out += "demand: " + quoted(d.name);
        if (!d.on.empty()) out += " on " + Str(d.on.str());
        out += Str(d.stub ? " is a stub" : " is not defined") + ", called at";
        for (size_t i = 0; i < d.sites.size(); ++i) {
            out += i ? ", " : " ";
            out += Str(d.sites[i].file.str()) + ":" + detail::decimal(d.sites[i].line);
        }
        out += "\n";
    }
    for (const Repeat& r : report.repeats) {
        Str form;
        write(r.form, str_out(form));
        if (form.size() > 60) form = form.substr(0, 57) + "...";
        out += "repeat: " + detail::decimal(static_cast<int64_t>(r.sites.size())) + " times, " +
               detail::decimal(r.nodes) + " nodes: " + form + " at";
        for (size_t i = 0; i < r.sites.size(); ++i) {
            out += i ? ", " : " ";
            out += Str(r.sites[i].file.str()) + ":" + detail::decimal(r.sites[i].line);
        }
        out += "\n";
    }
    for (const Error& f : report.findings) out += "finding: " + Str(baker::to_string(f)) + "\n";
    text(out);
}

}  // namespace baker
