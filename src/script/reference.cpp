// The generated script reference and load-time checking: both read a Context's
// registrations, and neither evaluates anything.

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
    static bool reads_values(std::span<const std::string_view> types, const Node& n) {
        const Array* a = n.try_array();
        return detail::reads_as(
            types, a ? Values(a->nodes(), a) : Values(std::span<const Node>(&n, 1), nullptr));
    }

    // Whether `o` accepts every literal argument of `command`, from index `first`.
    static bool fits(const detail::Overload& o, const Array& command, size_t first) {
        const size_t n = command.size() - first;
        for (size_t i = 0; i < n && i < o.params.size(); ++i) {
            const detail::ParamInfo& p = o.params[i];
            if (p.kind == detail::ParamKind::Rest) return true;
            if (evaluable(command[i + first])) continue;
            if (p.match ? !p.match(command[i + first], true)
                        : !p.values().empty() && !reads_values(p.values(), command[i + first]))
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

// --- Machine-readable output (the schema is docs/check-json.md) ---

namespace {

using detail::json_names;
using detail::json_string;

void json_sites(Str& out, Vec<CheckReport::Site> sites) {
    std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b) {
        const int c = a.file.str().compare(b.file.str());
        return c != 0 ? c < 0 : a.line < b.line;
    });
    out += '[';
    for (size_t i = 0; i < sites.size(); ++i) {
        out += i ? ", {\"file\": " : "{\"file\": ";
        json_string(out, sites[i].file.str());
        out += ", \"line\": " + detail::decimal(sites[i].line) + "}";
    }
    out += ']';
}

// Errors, warnings, and findings: sorted by file, line, column, then message.
void json_errors(Str& out, const char* key, const List<Error>& errors) {
    Vec<const Error*> sorted;
    for (const Error& e : errors) sorted.push_back(&e);
    std::sort(sorted.begin(), sorted.end(), [](const Error* a, const Error* b) {
        if (const int c = a->file().str().compare(b->file().str()); c != 0) return c < 0;
        if (a->line() != b->line()) return a->line() < b->line();
        if (a->column() != b->column()) return a->column() < b->column();
        return a->message() < b->message();
    });
    out += "  \"" + Str(key) + "\": [";
    for (size_t i = 0; i < sorted.size(); ++i) {
        const Error& e = *sorted[i];
        out += i ? ",\n    {" : "\n    {";
        out += "\"code\": ";
        json_string(out, error_code_name(e.code()));
        out += ", \"file\": ";
        json_string(out, e.file().str());
        out += ", \"line\": " + detail::decimal(e.line()) +
               ", \"column\": " + detail::decimal(e.column()) + ", \"path\": ";
        json_string(out, e.path());
        out += ", \"message\": ";
        json_string(out, e.message());
        out += '}';
    }
    out += sorted.empty() ? "]" : "\n  ]";
}

}  // namespace

void write_json(const CheckReport& report, TextSink text, const LoadSession* packs) {
    Str out = "{\n  \"schema\": \"baker.check/1.3\",\n";
    out += Str("  \"passed\": ") + (report.ok() ? "true" : "false") + ",\n";
    json_errors(out, "errors", report.errors);
    out += ",\n";
    json_errors(out, "warnings", report.warnings);
    out += ",\n";

    // Demand: by name, then the class it was sent to.
    Vec<const CheckReport::Demand*> demand;
    for (const CheckReport::Demand& d : report.demand) demand.push_back(&d);
    std::sort(demand.begin(), demand.end(), [](const auto* a, const auto* b) {
        if (const int c = a->name.str().compare(b->name.str()); c != 0) return c < 0;
        return a->on.str() < b->on.str();
    });
    out += "  \"demand\": [";
    for (size_t i = 0; i < demand.size(); ++i) {
        const CheckReport::Demand& d = *demand[i];
        out += i ? ",\n    {\"name\": " : "\n    {\"name\": ";
        json_string(out, d.name.str());
        out += ", \"on\": ";
        json_string(out, d.on.str());
        out += Str(", \"stub\": ") + (d.stub ? "true" : "false") + ", \"sites\": ";
        json_sites(out, Vec<CheckReport::Site>(d.sites.begin(), d.sites.end()));
        out += '}';
    }
    out += demand.empty() ? "],\n" : "\n  ],\n";

    // Repeats: in find_repeats' order, which depends only on the data.
    out += "  \"repeats\": [";
    for (size_t i = 0; i < report.repeats.size(); ++i) {
        const Repeat& r = report.repeats[i];
        Str form;
        write(r.form, str_out(form));
        out += i ? ",\n    {\"form\": " : "\n    {\"form\": ";
        json_string(out, form);
        out += ", \"nodes\": " + detail::decimal(r.nodes) + ", \"sites\": ";
        Vec<CheckReport::Site> sites;
        for (const RepeatSite& s : r.sites) sites.push_back({s.file, s.line});
        json_sites(out, std::move(sites));
        out += '}';
    }
    out += report.repeats.empty() ? "],\n" : "\n  ],\n";

    json_errors(out, "findings", report.findings);
    out += ",\n";
    json_errors(out, "load", report.load);
    out += ",\n";
    json_errors(out, "shape_errors", report.shape_errors);
    out += ",\n";
    json_errors(out, "shape_warnings", report.shape_warnings);
    out += ",\n";
    json_errors(out, "tweak_errors", report.tweak_errors);
    out += ",\n";
    json_errors(out, "tweak_warnings", report.tweak_warnings);
    out += ",\n";

    // Packs: the session's, by name; their harness metadata is the harness register.
    Vec<const Pack*> ps;
    if (packs)
        for (const Pack& p : packs->packs()) ps.push_back(&p);
    std::sort(ps.begin(), ps.end(), [](const Pack* a, const Pack* b) {
        if (const int c = a->name.str().compare(b->name.str()); c != 0) return c < 0;
        return a->file.str() < b->file.str();
    });
    out += "  \"packs\": [";
    for (size_t i = 0; i < ps.size(); ++i) {
        const Pack& p = *ps[i];
        out += i ? ",\n    {\"name\": " : "\n    {\"name\": ";
        json_string(out, p.name.str());
        out += ", \"level\": ";
        json_string(out, p.level.str());
        out += ", \"file\": ";
        json_string(out, p.file.str());
        out += ", \"requires\": ";
        json_names(out, p.requires_packs.items());
        out += ", \"owners\": ";
        json_names(out, p.owners.items());
        out += ", \"for\": ";
        json_names(out, p.audience.items());
        out += ", \"stage\": ";
        json_string(out, p.stage.str());
        out += '}';
    }
    out += ps.empty() ? "]\n}\n" : "\n  ]\n}\n";
    text(out);
}

namespace {
// The session's packs and origins, as the harness register:
// who keeps each pack, for whom, how far it has evolved; and which tools generate which files.
Str harness_register(const LoadSession& session) {
    auto names = [](const List<Symbol>& list) {
        Str s;
        for (const Symbol n : list) s += (s.empty() ? "" : ", ") + Str(n.str());
        return s.empty() ? Str("-") : s;
    };
    Str out;
    Vec<const Pack*> ps;
    for (const Pack& p : session.packs()) ps.push_back(&p);
    std::sort(ps.begin(), ps.end(),
              [](const Pack* a, const Pack* b) { return a->name.str() < b->name.str(); });
    if (!ps.empty()) {
        out += "\n## Packs\n\n| Pack | Level | Requires | Owners | For | Stage | File |\n";
        out += "|---|---|---|---|---|---|---|\n";
        for (const Pack* p : ps)
            out += "| `" + Str(p->name.str()) + "` | " + Str(p->level.str()) + " | " +
                   names(p->requires_packs) + " | " + names(p->owners) + " | " +
                   names(p->audience) + " | " +
                   (p->stage.empty() ? Str("-") : Str(p->stage.str())) + " | " +
                   Str(p->file.str()) + " |\n";
    }
    Vec<const Origin*> os;
    for (const Origin& o : session.origins()) os.push_back(&o);
    std::sort(os.begin(), os.end(), [](const Origin* a, const Origin* b) {
        if (const int c = a->tool.str().compare(b->tool.str()); c != 0) return c < 0;
        return a->file.str() < b->file.str();
    });
    if (!os.empty()) {
        out += "\n## Origins\n\nFiles generated by tools outside the config:\n\n";
        out += "| Tool | Source | File |\n|---|---|---|\n";
        for (const Origin* o : os)
            out += "| " + (o->tool.empty() ? Str("-") : Str(o->tool.str())) + " | " +
                   Str(o->source.str()) + " | " + Str(o->file.str()) + " |\n";
    }
    return out;
}

// A status as the reference prints it: empty for Normal, else its name, and a deprecated
// entry's replacement. Functions and methods alike.
Str status_text(Status status, Symbol replacement) {
    switch (status) {
        case Status::Normal: return {};
        case Status::Stub: return "stub";
        case Status::Provisional: return "provisional";
        case Status::Deprecated:
            return replacement.empty() ? Str("deprecated")
                                       : "deprecated: use `" + Str(replacement.str()) + "`";
    }
    return {};
}

Str reference_text(const Context& ctx, const LoadSession* packs) {
    const Context::Impl& impl = ctx.impl();
    Vec<const Function*> fs;
    for (const auto& f : impl.functions)
        if (!f->overloads.empty()) fs.push_back(f.get());
    std::sort(fs.begin(), fs.end(),
              [](const Function* a, const Function* b) { return a->name.str() < b->name.str(); });
    Str out =
        "# Script reference\n\n"
        "Generated by `baker::reference()` from what a Context registers. Do not edit it by\n"
        "hand: the test suite regenerates it and fails when it and the code disagree.\n\n"
        "A parameter in [brackets] is optional, `unevaluated` is passed as written, and\n"
        "`values...` takes the rest of the arguments.\n\n## Functions\n";
    for (const Function* f : fs) {
        out += "\n### `" + Str(f->name.str()) + "`\n\n";
        for (const detail::Overload& o : f->overloads) {
            Str sig = signature_text(o);
            sig = sig.size() > 2 ? " " + sig.substr(1, sig.size() - 2) : "";
            out += "- `{" + Str(f->name.str()) + sig + "}` → " + Str(o.result) + "\n";
        }
        out += "\n";
        out += f->tier == Tier::Native ? "*Native" : "*Script";
        if (const Str st = status_text(f->status, f->replacement); !st.empty()) out += ", " + st;
        out += ".*";
        if (!f->doc.empty()) out += " " + f->doc;
        out += "\n";
    }
    Vec<const detail::TypeRecord*> types = impl.types;
    Vec<const ScriptClass*> scripts;
    for (const auto& c : impl.classes)
        if (!impl.type_named(c->name)) scripts.push_back(c.get());
    if (!types.empty() || !scripts.empty()) {
        std::sort(types.begin(), types.end(),
                  [](const auto* a, const auto* b) { return a->name < b->name; });
        std::sort(scripts.begin(), scripts.end(),
                  [](const auto* a, const auto* b) { return a->name.str() < b->name.str(); });
        out +=
            "\n## Types\n\nEvery object also has `{obj get name}`, `{obj set name value}`, "
            "`{obj type}` and `{obj name}`.\n";
        auto handlers = [&](Symbol cls, const Str& shown) {
            if (const ScriptClass* c = impl.script_class(cls))
                for (const ScriptClass::Handler& h : c->handlers) {
                    out += "- `{<" + shown + "> " + Str(h.name.str());
                    for (Symbol p : h.body.params) out += " $" + Str(p.str());
                    out += "}` (script)\n";
                }
        };
        for (const detail::TypeRecord* t : types) {
            out += "\n### `" + t->name + "`\n\n";
            if (!t->bases.empty()) out += "Derives from `" + t->bases[0]->name + "`.\n\n";
            Vec<const detail::MethodEntry*> ms;
            for (const detail::MethodEntry& m : t->methods) ms.push_back(&m);
            std::sort(ms.begin(), ms.end(),
                      [](const auto* a, const auto* b) { return a->name.str() < b->name.str(); });
            for (const detail::MethodEntry* m : ms)
                for (const detail::Overload& o : m->overloads) {
                    Str sig = signature_text(o);
                    sig = sig.size() > 2 ? " " + sig.substr(1, sig.size() - 2) : "";
                    out += "- `{<" + t->name + "> " + Str(m->name.str()) + sig + "}` → " +
                           Str(o.result);
                    if (&o == &m->overloads.front()) {
                        if (const Str st = status_text(m->status, m->replacement); !st.empty())
                            out += " *(" + st + ")*";
                        if (!m->doc.empty()) out += ": " + m->doc;
                    }
                    if (!m->message.empty()) out += " (message `" + Str(m->message) + "`)";
                    out += "\n";
                }
            for (const detail::PropertyEntry& p : t->properties)
                out += "- property `" + Str(p.name.str()) + "`: " + Str(p.type) +
                       (p.doc.empty() ? "" : ": " + p.doc) + "\n";
            handlers(t->symbol, t->name);
        }
        for (const ScriptClass* c : scripts) {
            out += "\n### `" + Str(c->name.str()) + "` (script class)\n\n";
            handlers(c->name, Str(c->name.str()));
        }
    }
    if (!impl.globals.empty()) {
        Vec<Global> gs = impl.globals;
        std::sort(gs.begin(), gs.end(),
                  [](const Global& a, const Global& b) { return a.name.str() < b.name.str(); });
        out += "\n## Globals\n";
        for (const Global& g : gs) {
            out += "\n### `$" + Str(g.name.str()) + "`\n";
            if (!g.doc.empty()) out += "\n" + g.doc + "\n";
        }
    }
    if (packs) out += harness_register(*packs);
    return out;
}

// --- The reference as JSON (baker.reference/1.2, docs/reference-json.md) ---

const char* param_kind_name(detail::ParamKind k) {
    switch (k) {
        case detail::ParamKind::Value: return "value";
        case detail::ParamKind::Raw: return "raw";
        case detail::ParamKind::Optional: return "optional";
        case detail::ParamKind::Rest: return "rest";
        case detail::ParamKind::Implicit: return "implicit";
    }
    return "value";
}

const char* status_name(Status s) {
    switch (s) {
        case Status::Normal: return "normal";
        case Status::Stub: return "stub";
        case Status::Provisional: return "provisional";
        case Status::Deprecated: return "deprecated";
    }
    return "normal";
}

void json_overloads(Str& out, const Vec<detail::Overload>& overloads) {
    out += "\"overloads\": [";
    for (size_t i = 0; i < overloads.size(); ++i) {
        const detail::Overload& o = overloads[i];
        out += i ? ", {\"params\": [" : "{\"params\": [";
        for (size_t j = 0; j < o.params.size(); ++j) {
            const detail::ParamInfo& p = o.params[j];
            out += j ? ", {\"type\": " : "{\"type\": ";
            json_string(out, p.type());
            out += ", \"kind\": ";
            json_string(out, param_kind_name(p.kind));
            out += Str(", \"raw\": ") + (p.raw ? "true" : "false") + ", \"values\": ";
            if (p.values().empty()) {
                out += "null";
            } else {
                out += '[';
                for (size_t k = 0; k < p.values().size(); ++k) {
                    if (k) out += ", ";
                    json_string(out, p.values()[k]);
                }
                out += ']';
            }
            out += '}';
        }
        out += "], \"result\": ";
        json_string(out, o.result);
        out += ", \"min_args\": " + detail::decimal(o.min_args) + ", \"max_args\": ";
        out += o.max_args == UINT32_MAX ? Str("null") : detail::decimal(o.max_args);
        out += '}';
    }
    out += ']';
}

void json_status(Str& out, Status status, Symbol replacement, std::string_view doc) {
    out += "\"status\": ";
    json_string(out, status_name(status));
    out += ", \"replacement\": ";
    json_string(out, replacement.str());
    out += ", \"doc\": ";
    json_string(out, doc);
}

Str reference_json(const Context& ctx, const LoadSession* packs) {
    const Context::Impl& impl = ctx.impl();
    Str out = "{\n  \"schema\": \"baker.reference/1.2\",\n  \"functions\": [";
    Vec<const Function*> fs;
    for (const auto& f : impl.functions)
        if (!f->overloads.empty()) fs.push_back(f.get());
    std::sort(fs.begin(), fs.end(),
              [](const Function* a, const Function* b) { return a->name.str() < b->name.str(); });
    for (size_t i = 0; i < fs.size(); ++i) {
        const Function& f = *fs[i];
        out += i ? ",\n    {\"name\": " : "\n    {\"name\": ";
        json_string(out, f.name.str());
        out += ", \"tier\": ";
        json_string(out, f.tier == Tier::Native ? "native" : "script");
        out += ", ";
        json_status(out, f.status, f.replacement, f.doc);
        out += ", \"file\": ";
        json_string(out, f.file.str());
        out += ", \"line\": " + detail::decimal(f.line) + ", ";
        json_overloads(out, f.overloads);
        out += '}';
    }
    out += fs.empty() ? "],\n" : "\n  ],\n";

    // Types: native ones with their methods, properties, and script handlers; then the
    // classes only script defined.
    Vec<const detail::TypeRecord*> types = impl.types;
    std::sort(types.begin(), types.end(),
              [](const auto* a, const auto* b) { return a->name < b->name; });
    auto handlers = [&](Symbol cls) {
        out += "\"handlers\": [";
        if (const ScriptClass* c = impl.script_class(cls))
            for (size_t i = 0; i < c->handlers.size(); ++i) {
                if (i) out += ", ";
                json_string(out, c->handlers[i].name.str());
            }
        out += ']';
    };
    out += "  \"types\": [";
    for (size_t i = 0; i < types.size(); ++i) {
        const detail::TypeRecord& t = *types[i];
        out += i ? ",\n    {\"name\": " : "\n    {\"name\": ";
        json_string(out, t.name);
        out += ", \"bases\": [";
        for (size_t b = 0; b < t.bases.size(); ++b) {
            if (b) out += ", ";
            json_string(out, t.bases[b]->name);
        }
        out += "], \"methods\": [";
        Vec<const detail::MethodEntry*> ms;
        for (const detail::MethodEntry& m : t.methods) ms.push_back(&m);
        std::sort(ms.begin(), ms.end(),
                  [](const auto* a, const auto* b) { return a->name.str() < b->name.str(); });
        for (size_t m = 0; m < ms.size(); ++m) {
            out += m ? ", {\"name\": " : "{\"name\": ";
            json_string(out, ms[m]->name.str());
            out += ", ";
            json_status(out, ms[m]->status, ms[m]->replacement, ms[m]->doc);
            out += ", \"message\": ";
            json_string(out, ms[m]->message);
            out += ", ";
            json_overloads(out, ms[m]->overloads);
            out += '}';
        }
        out += "], \"properties\": [";
        for (size_t p = 0; p < t.properties.size(); ++p) {
            out += p ? ", {\"name\": " : "{\"name\": ";
            json_string(out, t.properties[p].name.str());
            out += ", \"type\": ";
            json_string(out, t.properties[p].type);
            out += ", \"doc\": ";
            json_string(out, t.properties[p].doc);
            out += '}';
        }
        out += "], ";
        handlers(t.symbol);
        out += '}';
    }
    out += types.empty() ? "],\n" : "\n  ],\n";

    Vec<const ScriptClass*> scripts;
    for (const auto& c : impl.classes)
        if (!impl.type_named(c->name)) scripts.push_back(c.get());
    std::sort(scripts.begin(), scripts.end(),
              [](const auto* a, const auto* b) { return a->name.str() < b->name.str(); });
    out += "  \"script_classes\": [";
    for (size_t i = 0; i < scripts.size(); ++i) {
        out += i ? ",\n    {\"name\": " : "\n    {\"name\": ";
        json_string(out, scripts[i]->name.str());
        out += ", ";
        handlers(scripts[i]->name);
        out += '}';
    }
    out += scripts.empty() ? "],\n" : "\n  ],\n";

    Vec<Global> gs = impl.globals;
    std::sort(gs.begin(), gs.end(),
              [](const Global& a, const Global& b) { return a.name.str() < b.name.str(); });
    out += "  \"globals\": [";
    for (size_t i = 0; i < gs.size(); ++i) {
        out += i ? ",\n    {\"name\": " : "\n    {\"name\": ";
        json_string(out, gs[i].name.str());
        out += ", \"doc\": ";
        json_string(out, gs[i].doc);
        out += '}';
    }
    out += gs.empty() ? "],\n" : "\n  ],\n";

    // Named objects, so a tool knows `{clock ...}` is a call on one: those present now.
    struct Named {
        Symbol name;
        std::string_view type;
    };
    Vec<Named> named;
    for (const Context::Impl::Slot& sl : impl.slots)
        if (sl.object && !sl.name.empty())
            named.push_back({sl.name, sl.script ? sl.script->name.str() : sl.object->type_name()});
    std::sort(named.begin(), named.end(),
              [](const Named& a, const Named& b) { return a.name.str() < b.name.str(); });
    out += "  \"objects\": [";
    for (size_t i = 0; i < named.size(); ++i) {
        out += i ? ",\n    {\"name\": " : "\n    {\"name\": ";
        json_string(out, named[i].name.str());
        out += ", \"type\": ";
        json_string(out, named[i].type);
        out += '}';
    }
    out += named.empty() ? "],\n" : "\n  ],\n";

    // Packs and origins, as check's JSON writes them (docs/check-json.md).
    Vec<const Pack*> ps;
    Vec<const Origin*> os;
    if (packs) {
        for (const Pack& p : packs->packs()) ps.push_back(&p);
        for (const Origin& o : packs->origins()) os.push_back(&o);
    }
    std::sort(ps.begin(), ps.end(), [](const Pack* a, const Pack* b) {
        if (const int c = a->name.str().compare(b->name.str()); c != 0) return c < 0;
        return a->file.str() < b->file.str();
    });
    std::sort(os.begin(), os.end(), [](const Origin* a, const Origin* b) {
        if (const int c = a->tool.str().compare(b->tool.str()); c != 0) return c < 0;
        return a->file.str() < b->file.str();
    });
    out += "  \"packs\": [";
    for (size_t i = 0; i < ps.size(); ++i) {
        const Pack& p = *ps[i];
        out += i ? ",\n    {\"name\": " : "\n    {\"name\": ";
        json_string(out, p.name.str());
        out += ", \"level\": ";
        json_string(out, p.level.str());
        out += ", \"file\": ";
        json_string(out, p.file.str());
        out += ", \"requires\": ";
        json_names(out, p.requires_packs.items());
        out += ", \"owners\": ";
        json_names(out, p.owners.items());
        out += ", \"for\": ";
        json_names(out, p.audience.items());
        out += ", \"stage\": ";
        json_string(out, p.stage.str());
        out += '}';
    }
    out += ps.empty() ? "],\n" : "\n  ],\n";
    out += "  \"origins\": [";
    for (size_t i = 0; i < os.size(); ++i) {
        out += i ? ",\n    {\"tool\": " : "\n    {\"tool\": ";
        json_string(out, os[i]->tool.str());
        out += ", \"source\": ";
        json_string(out, os[i]->source.str());
        out += ", \"file\": ";
        json_string(out, os[i]->file.str());
        out += '}';
    }
    out += os.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}
}  // namespace

namespace {

void find_funcs(const Array& a, Vec<Definition>& out) {
    if (a.size() >= 2 && a.tag() == Symbol("func"))
        if (const auto name = a[1].try_sym()) out.push_back({*name, a.file(), a.line()});
    for (const Node& n : a.values())
        if (const Array* inner = n.array()) find_funcs(*inner, out);
}

}  // namespace

List<Definition> script_functions(std::span<const Node> autorun) {
    Vec<Definition> out;
    for (const Node& command : autorun)
        if (const Array* a = command.array()) find_funcs(*a, out);
    return List<Definition>(std::span<const Definition>(out.data(), out.size()));
}

void write_reference_json(const Context& ctx, TextSink out, const LoadSession* packs) {
    out(reference_json(ctx, packs));
}

void reference(const Context& ctx, TextSink out, const LoadSession* packs) {
    out(reference_text(ctx, packs));
}

Expected<void> write_reference(const Context& ctx, std::string_view path,
                               const LoadSession* packs) {
    const Str text = reference_text(ctx, packs);
    const Str p(path);
    std::FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return Unexpected(make_error(ErrorCode::Io, "cannot write " + p));
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !ok)
        return Unexpected(make_error(ErrorCode::Io, "cannot write " + p));
    return {};
}

}  // namespace baker
