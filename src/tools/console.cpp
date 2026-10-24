// bind_config_tools: a Config's tooling as script functions, for consoles, editors, and
// agents. Each result is data, so it prints as Baker text and
// projects to JSON through eval_request.

#include <baker/curve.hpp>
#include <baker/tools/console.hpp>
#include <baker/tools/layers.hpp>

#include "common/containers.hpp"

namespace baker {
namespace {

// A path or a layer name, written as a symbol (box.width) or a string ("phase.2").
// Context::declare knows the type by its name, "name", and checks it alike.
struct Name {
    std::string_view text;
};

}  // namespace

template <>
struct Convert<Name> {
    static constexpr std::string_view name = "name";
    static constexpr bool scalar = true;
    static bool from(const Values& v, Name& out) noexcept {
        if (v.empty()) return false;
        if (const auto s = v[0].try_sym()) {
            out.text = s->str();
            return true;
        }
        if (const auto s = v[0].try_str()) {
            out.text = *s;
            return true;
        }
        return false;
    }
};

namespace tools {
namespace {

Node text_node(std::string_view s) {
    return Node(String::make(s));
}

Node tagged(const char* tag, Node value) {
    return Node(array(Symbol(tag), std::move(value)));
}

Node list(Vec<Node>& items) {
    return Node(Array::make_from(std::span<Node>(items.data(), items.size())));
}

// (layer L) (file F) (line N) [(origin O) (tool T)], appended to `out`.
void add_source(Vec<Node>& out, const Source& s, bool with_layer = true) {
    if (with_layer) out.push_back(tagged("layer", Node(s.layer)));
    out.push_back(tagged("file", text_node(s.file.str())));
    out.push_back(tagged("line", Node(int64_t{s.line})));
    if (!s.origin.empty()) out.push_back(tagged("origin", text_node(s.origin.str())));
    if (!s.tool.empty()) out.push_back(tagged("tool", Node(s.tool)));
}

Symbol standing_name(Standing s) {
    switch (s) {
        case Standing::Wins: return Symbol("wins");
        case Standing::Shadowed: return Symbol("shadowed");
        case Standing::Removes: return Symbol("removes");
        case Standing::Disabled: return Symbol("disabled");
    }
    return {};
}

// The tools' own state: the Config, and the snapshots taken by name. Shared by the bound
// functions and freed with the last of them.
struct Tools {
    Tools(Config& c, const Allocator& a) : cfg(c), snapshots(a) {}
    Config& cfg;
    Vec<std::pair<Symbol, ConfigSnapshot>> snapshots;
};

Expected<Node> explain_node(Config& cfg, std::string_view path) {
    auto entries = explain(cfg, Path(path));
    if (!entries) return Unexpected(std::move(entries).error());
    Vec<Node> out;
    for (const ExplainEntry& e : *entries) {
        Vec<Node> fields;
        fields.push_back(tagged("layer", Node(e.layer)));
        fields.push_back(tagged("value", e.value));
        fields.push_back(tagged("standing", Node(standing_name(e.standing))));
        add_source(fields, e.source, false);
        if (e.section) {  // (chosen_by server ifdef): the define behind the value
            Vec<Node> by;
            by.emplace_back(Symbol("chosen_by"));
            by.emplace_back(e.section->name);
            by.emplace_back(Symbol(e.section->else_branch ? "else"
                                   : e.section->negated   ? "ifndef"
                                                          : "ifdef"));
            fields.push_back(list(by));
        }
        out.push_back(list(fields));
    }
    return list(out);
}

Expected<Node> report_node(Expected<WriteReport> r) {
    if (!r) return Unexpected(std::move(r).error());
    Vec<Node> written, refused;
    for (const WrittenLine& w : r->written) {
        Vec<Node> f;
        f.push_back(tagged("file", text_node(w.file.str())));
        f.push_back(tagged("line", Node(int64_t{w.line})));
        written.push_back(list(f));
    }
    for (const Error& e : r->refused) {
        Vec<Node> f;
        f.push_back(tagged("message", text_node(e.message())));
        f.push_back(tagged("file", text_node(e.file().str())));
        f.push_back(tagged("line", Node(int64_t{e.line()})));
        refused.push_back(list(f));
    }
    Vec<Node> out;
    out.emplace_back(array(Symbol("written"), list(written)));
    out.emplace_back(array(Symbol("refused"), list(refused)));
    return list(out);
}

}  // namespace

Expected<void> bind_config_tools(Context& ctx, Config& cfg, const ConfigToolOptions& options) {
    const Rc<Tools> tools = Rc<Tools>::make(ctx.allocator(), cfg, ctx.allocator());
    Expected<void> status;
    auto bind = [&](const char* name, auto&& f, const char* doc) {
        if (!status) return;
        auto r = ctx.func(Symbol(name), std::forward<decltype(f)>(f));
        if (!r) {
            status = Unexpected(std::move(r).error());
            return;
        }
        r->doc(doc);
    };

    // --- Reading: in every build. ---
    bind(
        "config.get",
        [tools](Name path) -> Expected<Node> {
            auto entries = explain(tools->cfg, Path(path.text));
            if (!entries) return Unexpected(std::move(entries).error());
            for (const ExplainEntry& e : *entries)
                if (e.standing == Standing::Wins && e.layer != "code") return e.value;
            return Unexpected(make_error(
                ErrorCode::NotFound, "nothing sets '" + Str(path.text) + "'; its default stands"));
        },
        "The value at a path, as get() reads it: several values come as one array.");
    bind(
        "curve.sample",
        [tools](Name path, double x) -> Expected<double> {
            auto curve = tools->cfg.get<Curve>(Path(path.text));
            if (!curve) return Unexpected(std::move(curve).error());
            return curve->sample(x);
        },
        "A curve's value at x: the path's values read as a curve (points (x y), with an "
        "optional interpolation and extension).");
    bind(
        "config.source",
        [tools](Name path) -> Expected<Node> {
            auto s = tools->cfg.source(Path(path.text));
            if (!s) return Unexpected(std::move(s).error());
            Vec<Node> out;
            out.emplace_back(Symbol("source"));
            add_source(out, *s);
            return list(out);
        },
        "Where a path's value comes from: (source (layer L) (file F) (line N)), with "
        "(origin O) (tool T) when a tool generated the file.");
    bind(
        "config.explain", [tools](Name path) { return explain_node(tools->cfg, path.text); },
        "Every contributor to a path's value, bottom to top: each entry's layer, value, "
        "standing (wins, shadowed, removes, disabled), file, and line.");
    bind(
        "config.layers",
        [tools]() -> Node {
            Vec<Node> out;
            for (const Symbol l : tools->cfg.layers()) {
                Vec<Node> f;
                f.push_back(tagged("layer", Node(l)));
                f.push_back(tagged("enabled", Node(int64_t{tools->cfg.layer_enabled(l.str())})));
                out.push_back(list(f));
            }
            return list(out);
        },
        "The layers bottom to top, ending with scratch, each with whether it is enabled.");
    bind(
        "config.effect",
        [tools](Name layer) -> Expected<Node> {
            auto effect = layer_effect(tools->cfg, layer.text);
            if (!effect) return Unexpected(std::move(effect).error());
            Vec<Node> out;
            for (const LayerEffectEntry& e : *effect) {
                Vec<Node> f;
                f.push_back(tagged("path", Node(e.path)));
                f.push_back(tagged("value", e.value));
                if (e.below) f.push_back(tagged("below", *e.below));
                add_source(f, e.source, false);
                f.push_back(tagged("shadowed", Node(int64_t{e.shadowed})));
                out.push_back(list(f));
            }
            return list(out);
        },
        "What a layer changes: each path, its value, the value below it, its file and line, "
        "and whether a layer above shadows it.");
    bind(
        "config.diff",
        [tools](Name a, Name b) -> Expected<Node> {
            auto diff = tools::diff(tools->cfg, a.text, b.text);
            if (!diff) return Unexpected(std::move(diff).error());
            Vec<Node> out;
            for (const DiffEntry& d : *diff) {
                Vec<Node> f;
                f.push_back(tagged("path", Node(d.path)));
                if (d.a) f.push_back(tagged("a", *d.a));
                if (d.b) f.push_back(tagged("b", *d.b));
                out.push_back(list(f));
            }
            return list(out);
        },
        "The paths two layers set differently, with each one's value (absent: not set).");

    if constexpr (!kShipping) {
        // --- Live edits and telemetry: development builds only. ---
        bind(
            "config.set",
            [tools](Name path, Args values) -> Expected<Node> {
                Vec<Node> nodes;
                for (size_t i = 0; i < values.size(); ++i) {
                    auto v = values.eval(i);
                    if (!v) return Unexpected(std::move(v).error());
                    nodes.push_back(*v);
                }
                if (nodes.empty())
                    return Unexpected(make_error(ErrorCode::Type, "config.set needs a value"));
                auto r = tools->cfg.set(Path(path.text),
                                        std::span<const Node>(nodes.data(), nodes.size()));
                if (!r) return Unexpected(std::move(r).error());
                return nodes.size() == 1 ? nodes[0] : list(nodes);
            },
            "Tunes a path live, in the scratch layer; returns the value set.");
        bind(
            "config.clear_scratch",
            [tools](Name path) -> Expected<Node> {
                // A path with no live edit changes nothing, not even a merge. All of scratch
                // is never this call with the path left out: a tool probing whether the
                // command exists must not drop every edit.
                bool edited = false;
                for (const Symbol p : tools->cfg.unsaved())
                    edited = edited || Path(p.str()).matches(path.text);
                if (!edited) return Node(int64_t{0});
                auto r = tools->cfg.clear_scratch(path.text);
                if (!r) return Unexpected(std::move(r).error());
                return Node(int64_t{1});
            },
            "Drops a path's live edit, so its value is the files' again, and the journal "
            "records it reverted; returns 1 if there was an edit to drop, 0 if not.");
        bind(
            "config.toggle",
            [tools](Name layer) -> Expected<Node> {
                auto r = tools->cfg.toggle_layer(layer.text);
                if (!r) return Unexpected(std::move(r).error());
                return Node(int64_t{*r});
            },
            "Turns a layer (or scratch) off or on; returns 1 if it is now enabled.");
        bind(
            "config.reload",
            [tools]() -> Expected<Node> {
                auto r = tools->cfg.reload({.trigger = Symbol("console")});
                if (!r) return Unexpected(std::move(r).error());
                return Node(int64_t{*r});
            },
            "Re-reads changed files now; returns 1 if anything changed.");
        bind(
            "config.digest",
            [tools]() -> Node {
                char hex[17];
                uint64_t d = tools->cfg.digest();
                for (int i = 15; i >= 0; --i, d >>= 4) hex[i] = "0123456789abcdef"[d & 15];
                return Node(std::string_view(hex, 16));
            },
            "The digest of the data as the game reads it, live edits included, as 16 hex "
            "digits: the same on every machine with the same data.");
        bind(
            "config.epoch",
            [tools]() -> Node { return Node(static_cast<int64_t>(tools->cfg.epoch())); },
            "A number that moves on every reload, layer change and live edit: a tool asks for "
            "it, and for the values again only when it moved.");
        bind(
            "config.unsaved",
            [tools]() -> Node {
                Vec<Node> out;
                for (const Symbol p : tools->cfg.unsaved()) out.emplace_back(p);
                return list(out);
            },
            "The paths tuned live and not yet written back.");
        bind(
            "config.journal",
            [tools](std::optional<int64_t> since_ns) -> Node {
                static const char* const kFates[] = {"unsaved", "written", "reverted",
                                                     "superseded"};
                Vec<Node> out;
                tools->cfg.journal([&](const JournalEntry& e) {
                    if (since_ns && e.time_ns < static_cast<uint64_t>(*since_ns)) return;
                    // Packed as explain packs values: one bare, several as an array,
                    // none left out.
                    auto packed = [](const Node* nodes, uint32_t count) {
                        Vec<Node> v(nodes, nodes + count);
                        return count == 1 ? v[0] : list(v);
                    };
                    Vec<Node> f;
                    f.push_back(tagged("path", Node(e.path)));
                    f.push_back(tagged("time_ns", Node(static_cast<int64_t>(e.time_ns))));
                    if (e.before_count)
                        f.push_back(tagged("before", packed(e.before, e.before_count)));
                    if (e.after_count) f.push_back(tagged("after", packed(e.after, e.after_count)));
                    f.push_back(tagged("fate", Node(Symbol(kFates[int(e.fate)]))));
                    add_source(f, e.source);
                    out.push_back(list(f));
                });
                return list(out);
            },
            "The live edits of this session, oldest first, each with its old and new values, "
            "where the old one came from, and its fate; [since_ns] keeps the later ones.");
        bind(
            "config.loop",
            [tools]() -> Node {
                const LoopStats s = tools->cfg.loop_stats();
                auto rows = [](const List<LoopRow>& rs) {
                    Vec<Node> out;
                    for (const LoopRow& r : rs) {
                        Vec<Node> f;
                        f.push_back(tagged("name", text_node(r.name.str())));
                        f.push_back(tagged("reloads", Node(int64_t{r.reloads})));
                        f.push_back(tagged("failures", Node(int64_t{r.failures})));
                        f.push_back(
                            tagged("edit_median_ns", Node(static_cast<int64_t>(r.edit_median_ns))));
                        f.push_back(
                            tagged("edit_worst_ns", Node(static_cast<int64_t>(r.edit_worst_ns))));
                        f.push_back(tagged("reload_median_ns",
                                           Node(static_cast<int64_t>(r.reload_median_ns))));
                        out.push_back(list(f));
                    }
                    return list(out);
                };
                Vec<Node> out;
                out.push_back(tagged("reloads", Node(int64_t{s.reloads})));
                out.push_back(tagged("unchanged", Node(int64_t{s.unchanged})));
                out.push_back(tagged("failures", Node(int64_t{s.failures})));
                out.push_back(tagged("files", rows(s.files)));
                out.push_back(tagged("layers", rows(s.layers)));
                return list(out);
            },
            "Edit-to-see and reload times, per file and per layer.");
        bind(
            "config.snapshot",
            [tools](Name named) -> Node {
                const Symbol name(named.text);
                for (auto& [n, snap] : tools->snapshots)
                    if (n == name) {
                        snap = tools->cfg.snapshot();
                        return Node(name);
                    }
                tools->snapshots.emplace_back(name, tools->cfg.snapshot());
                return Node(name);
            },
            "Captures the whole state (layers, toggles, scratch) under a name.");
        bind(
            "config.restore",
            [tools](Name named) -> Expected<Node> {
                const Symbol name(named.text);
                for (const auto& [n, snap] : tools->snapshots)
                    if (n == name) {
                        auto r = tools->cfg.restore(snap);
                        if (!r) return Unexpected(std::move(r).error());
                        return Node(name);
                    }
                return Unexpected(
                    make_error(ErrorCode::NotFound, "no snapshot named '" + Str(name.str()) + "'"));
            },
            "Returns to a named snapshot.");
        if (options.allow_write) {
            bind(
                "config.write_back",
                [tools](Name path) { return report_node(tools->cfg.write_back(Path(path.text))); },
                "Writes a live-tuned value back to the line it came from; returns the lines "
                "written and any refusals.");
            bind(
                "config.promote",
                [tools](Name from, Name to) {
                    return report_node(tools->cfg.promote(from.text, to.text));
                },
                "Writes everything a layer changes into another layer's file, or nothing; "
                "returns the lines written and any refusals.");
        }
    }
    return status;
}

}  // namespace tools
}  // namespace baker
