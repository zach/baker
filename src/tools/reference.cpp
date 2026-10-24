// The generated script reference, and the check report and the data's script functions for
// tools: each reads a Context's registrations, and none evaluates anything.

#include <baker/config.hpp>
#include <baker/tools/reference.hpp>
#include <baker/write.hpp>

#include "access.hpp"

#include <algorithm>
#include <cstdio>

namespace baker::tools {

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

namespace {
// `  "defines": [...]`: the flags that stood for the read, in the order made, each with who
// made it ("host", "project", or "file" with where), for a tool to show which were on.
void json_defines(Str& out, const LoadSession* session) {
    const std::span<const ActiveDefine> ds =
        session ? session->active_defines() : std::span<const ActiveDefine>();
    out += "  \"defines\": [";
    for (size_t i = 0; i < ds.size(); ++i) {
        out += i ? ",\n    {\"name\": " : "\n    {\"name\": ";
        json_string(out, ds[i].name.str());
        out += ", \"origin\": ";
        json_string(out, define_from_name(ds[i].from));
        out += ", \"file\": ";
        json_string(out, ds[i].file.str());
        out += ", \"line\": " + detail::decimal(ds[i].line) + '}';
    }
    out += ds.empty() ? "]\n" : "\n  ]\n";
}
}  // namespace

void write_json(const CheckReport& report, TextSink text, const LoadSession* packs) {
    Str out = "{\n  \"schema\": \"baker.check/1.4\",\n";
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
    out += ps.empty() ? "],\n" : "\n  ],\n";
    json_defines(out, packs);
    out += "}\n";
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
        "Generated by `baker::tools::reference()` from what a Context registers. Do not edit it\n"
        "by hand: the test suite regenerates it and fails when it and the code disagree.\n\n"
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

// --- The reference as JSON (baker.reference/1.5, docs/reference-json.md) ---

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
                out += "null, \"count\": null";
            } else {
                out += '[';
                for (size_t k = 0; k < p.values().size(); ++k) {
                    if (k) out += ", ";
                    json_string(out, p.values()[k]);
                }
                out += "], \"count\": ";
                json_string(out, detail::count_name(p.count()));
            }
            detail::json_list_element(out, p.element());
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
    Str out = "{\n  \"schema\": \"baker.reference/1.5\",\n  \"functions\": [";
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
    out += os.empty() ? "],\n" : "\n  ],\n";
    json_defines(out, packs);
    out += "}\n";
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

}  // namespace baker::tools
