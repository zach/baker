// The tool commands (host/commands.hpp), run from a game's own binary.

#include <baker/host/commands.hpp>
#include <baker/host/live.hpp>
#include <baker/image.hpp>
#include <baker/tools/console.hpp>
#include <baker/tools/format.hpp>
#include <baker/tools/layers.hpp>
#include <baker/tools/reference.hpp>
#include <baker/tweak.hpp>

#include "common/containers.hpp"
#include "data/table_build.hpp"
#include "data/text.hpp"
#include "host/scenarios.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <random>
#include <tuple>

#if BAKER_HOST_SHAPE
#include <baker/shape.hpp>
#include <baker/tools/rules.hpp>
#endif

namespace fs = std::filesystem;

namespace baker::host {
namespace {

// A command's output, gathered while it runs, then sent where the host asked (run_command).
thread_local Str* t_output = nullptr;

void out(std::string_view text) {
    if (t_output) t_output->append(text);
}
void err(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stderr);
}

bool write_whole(std::string_view path, std::string_view text);  // below
TextSink stdout_sink() {
    return {[](void*, std::string_view text) { out(text); }, nullptr};
}

// The game's data, as the commands see it: a fresh Context with the game's functions, and
// the stack opened without watching.
struct Loaded {
    explicit Loaded(const CommandHooks& h) : hooks(h), live(ctx, h.config) {}
    Loaded(const Loaded&) = delete;
    Loaded& operator=(const Loaded&) = delete;
    ~Loaded() {
        if (bound && hooks.unbind) hooks.unbind(ctx);
    }
    const CommandHooks& hooks;
    Context ctx;
    Live live;
    bool bound = false;
};

bool load(Loaded& l, const CommandLine& line) {
    if (l.hooks.bind) l.hooks.bind(l.ctx);
    l.bound = true;
    // A shipping Live binds no config.*; the commands are tool code, and eval offers
    // them in every build. Bound before the open, as Live binds them before #autorun.
    if constexpr (kShipping)
        if (auto r = tools::bind_config_tools(l.ctx, l.live.config()); !r) {
            err(describe(r.error()));
            err("\n");
            return false;
        }
    LiveOptions o;
    o.project = line.project;
    o.built = l.hooks.built;
    o.reader = l.hooks.reader;
    // A directory the host's lister does not have is the disk's, as for scenarios.
    if (l.hooks.list)
        o.lister = [&l](std::string_view dir, const Callback<void(std::string_view)>& entry) {
            return l.hooks.list(dir, entry) || LoadSession().lister()(dir, entry);
        };
    o.open = l.hooks.open;
    o.tables = l.hooks.tables;
    o.watch = false;
    o.changed = l.hooks.changed;
    o.log = [](const Error& e) {
        err(describe(e));
        err("\n");
    };
    return static_cast<bool>(l.live.open(o));
}

// A whole number written in decimal digits.
std::optional<uint64_t> whole(std::string_view s) {
    if (s.empty()) return std::nullopt;
    uint64_t n = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        n = n * 10 + static_cast<uint64_t>(c - '0');
    }
    return n;
}

// The scenarios and objectives the project names (its file's, or the stack built in code).
Expected<void> read_book(const Loaded& l, sim::Book& book) {
    Project read;
    const Project* project = l.hooks.built;
    if (!l.live.project_file().empty()) {
        auto p = read_project(l.live.project_file().str(), l.hooks.reader);
        if (!p) return Unexpected(std::move(p).error());
        read = std::move(*p);
        project = &read;
    }
    if (!project) return {};
    book.step_name = project->step_name;
    for (const List<ProjectEntry>* list : {&project->objectives, &project->scenarios})
        for (const ProjectEntry& e : *list)
            if (auto r = book.read(e.value.str(), l.hooks.reader, l.hooks.list); !r) return r;
    return {};
}

// The project the stack opened from: its file, read again into `storage`, else the one the
// host built; null for neither.
const Project* project_of(const Loaded& l, Project& storage) {
    if (!l.live.project_file().empty())
        if (auto p = read_project(l.live.project_file().str(), l.hooks.reader)) {
            storage = std::move(*p);
            return &storage;
        }
    return l.hooks.built;
}

#if BAKER_HOST_SHAPE
// The shapes the enabled layers declare, each layer's read alone with the stack's packs,
// as the validator reads them.
void add_shapes(const Config& cfg, shape::Set& set, shape::Report& report) {
    for (const Symbol layer : cfg.layers())
        if (cfg.layer_enabled(layer.str()))
            if (const Ref<Array> root = cfg.layer_root(layer.str()))
                set.add(cfg.read_session(), report, root->file());
}

// The project's (assets ...), for the (asset KIND) fields.
void add_asset_kinds(const Project* project, shape::Set& set) {
    if (!project) return;
    Vec<shape::AssetKind> kinds;
    for (const ProjectAssetKind& k : project->assets) kinds.push_back({k.name, k.extensions});
    set.add_asset_kinds({kinds.data(), kinds.size()});
}

// An asset the merged stack names, and whether the lister finds it: each file's directory is
// listed once per run, through the host's lister or the disk. Paths are relative to
// the project file's directory.
struct AssetEdge {
    shape::AssetUse use;
    Str target;  // joined with the project's directory
    bool exists;
};
Vec<AssetEdge> asset_edges(const Loaded& l, const shape::Set& set) {
    struct Dir {
        Str path;
        Vec<Str> names;
        bool listed;
    };
    Vec<Dir> dirs;
    const auto base_name = [](std::string_view p) {
        const size_t slash = p.find_last_of("/\\");
        return slash == std::string_view::npos ? p : p.substr(slash + 1);
    };
    const std::string_view root = detail::parent_path(l.live.project_file().str());
    LoadSession disk;
    const LoadSession::Lister& lister = l.hooks.list ? l.hooks.list : disk.lister();
    Vec<AssetEdge> edges;
    for (const shape::AssetUse& u : shape::asset_uses(*l.live.config().root(), set)) {
        const bool absolute = !u.target.empty() && (u.target.front() == '/' ||
                                                    (u.target.size() > 1 && u.target[1] == ':'));
        Str target = absolute || root.empty() ? Str(u.target) : Str(root) + "/" + u.target;
        const std::string_view dir = detail::parent_path(target.view());
        auto d = std::find_if(dirs.begin(), dirs.end(),
                              [&](const Dir& x) { return x.path.view() == dir; });
        if (d == dirs.end()) {
            Dir fresh{Str(dir), {}, false};
            fresh.listed =
                lister(dir.empty() ? std::string_view(".") : dir,
                       [&](std::string_view p) { fresh.names.emplace_back(base_name(p)); });
            dirs.push_back(std::move(fresh));
            d = dirs.end() - 1;
        }
        const std::string_view name = base_name(target.view());
        const bool exists =
            d->listed && std::any_of(d->names.begin(), d->names.end(),
                                     [&](const Str& n) { return n.view() == name; });
        edges.push_back({u, std::move(target), exists});
    }
    return edges;
}

// A record's path as the check names it, with dots, as rows and the JSON spell paths.
Str dotted(Symbol path) {
    Str out(path.str());
    for (char& c : out)
        if (c == '/') c = '.';
    return out;
}
#endif

#if BAKER_HOST_SHAPE
// Every (@aka old) of the merged stack that no key_of resolved through and no layer's own
// tree names as a key: a finding, since a shipped overlay may still name it.
void unused_aliases(const Config& cfg, const shape::Report& shaped, CheckReport& report) {
    struct Alias {
        Symbol old;
        const Array* record;
        Str path;
    };
    Vec<Alias> aliases;
    Vec<Str> segments;
    const auto walk = [&](auto&& self, const Array& a) -> void {
        const uint32_t first = a.tag().empty() ? 0 : 1;
        for (uint32_t i = first; i < a.size(); ++i) {
            const Array* c = a[i].try_array();
            if (!c) continue;
            segments.push_back(c->tag().empty() ? detail::decimal(static_cast<int64_t>(i - first))
                                                : Str(c->tag().str()));
            if (c->meta_first())
                if (const Array* aka = c->meta(Symbol("@aka")))
                    for (const Node& old : aka->values())
                        if (const auto s = old.try_sym()) {
                            Str path;
                            for (const Str& seg : segments) path += (path.empty() ? "" : "/") + seg;
                            aliases.push_back({*s, c, path});
                        }
            self(self, *c);
            segments.pop_back();
        }
    };
    walk(walk, *cfg.root());
    if (aliases.empty()) return;
    Vec<Symbol> used(shaped.aliases_used.begin(), shaped.aliases_used.end());
    const auto tags_of = [&](auto&& self, const Array& a) -> void {
        const uint32_t first = a.tag().empty() ? 0 : 1;
        for (uint32_t i = first; i < a.size(); ++i)
            if (const Array* c = a[i].try_array()) {
                if (!c->tag().empty()) used.push_back(c->tag());
                self(self, *c);
            }
    };
    for (const Symbol layer : cfg.layers())
        if (const Ref<Array> root = cfg.layer_root(layer.str())) tags_of(tags_of, *root);
    Vec<Error> findings(report.findings.begin(), report.findings.end());
    for (const Alias& a : aliases) {
        if (std::find(used.begin(), used.end(), a.old) != used.end()) continue;
        Error e = make_error(ErrorCode::NotFound,
                             "alias '" + Str(a.old.str()) + "' of " + a.path +
                                 " is named by no layer or key_of: a shipped overlay may still "
                                 "name it; drop the (@aka ...) once none does",
                             a.record->file(), a.record->line());
        e.set_path(a.path);
        findings.push_back(e);
    }
    report.findings = List<Error>(std::span<const Error>(findings.data(), findings.size()));
}
#endif

int report_check(CheckReport& report, const Loaded& l, const LoadSession* packs) {
    report.load = List<Error>(l.live.load_errors());
    tools::write_json(report, stdout_sink(), packs);
    out("\n");
    return report.ok() ? 0 : kCommandFailed;
}

int check(const CommandLine& line, const CommandHooks& hooks) {
    Loaded l(hooks);
    CheckReport report;
    // A call to a function nobody defined is demand to Baker: a word the data asks for. In
    // a tool's or an agent's edit it is nearly always a typo, so it fails the check unless
    // declared a stub.
    report.unresolved_fails = true;
    // Data that does not load is still a report: the JSON says why, and the exit status
    // stays "could not run".
    if (!load(l, line)) {
        report_check(report, l, nullptr);
        return kCommandError;
    }
    const Config& cfg = l.live.config();
    auto unreadable = [&](const Error& e) {
        report.load = List<Error>(std::span<const Error>(&e, 1));
        tools::write_json(report, stdout_sink(), &cfg.read_session());
        out("\n");
        err(describe(e));
        err("\n");
        return kCommandError;
    };
    // The project's scenarios and objectives, which an agent writes as it writes data, and
    // the game's own extra files.
    sim::Book book;
    if (auto r = read_book(l, book); !r) return unreadable(r.error());
    List<Ref<Array>> trees;
    if (hooks.extra_trees) {
        auto t = hooks.extra_trees(cfg);
        if (!t) return unreadable(t.error());
        trees = std::move(*t);
    }
    Vec<const Array*> extra;
    for (const Ref<Array>& t : book.trees) extra.push_back(t.get());
    for (const Ref<Array>& t : trees) extra.push_back(t.get());
    check(cfg, l.ctx, report, {.unresolved_fails = true},
          std::span<const Array* const>(extra.data(), extra.size()));
    for (const CheckReport::Demand& d : report.demand)
        if (!d.stub) {
            const CheckReport::Site* site = d.sites.empty() ? nullptr : d.sites.begin();
            err("check: '" + Str(d.name.str()) + "' is not defined (" +
                Str(site ? site->file.str() : "?") + ":" + detail::decimal(site ? site->line : 0) +
                ")\n");
        }
#if BAKER_HOST_SHAPE
    // The shapes, over the merged data: a value out of range or of the wrong type fails the
    // check; a field no shape declares is reported, as demand for a word, and passes.
    Project read;
    const Project* project = project_of(l, read);
    shape::Set shapes;
    shape::Report shaped;
    add_shapes(cfg, shapes, shaped);
    add_asset_kinds(project, shapes);
    shape::check(*cfg.root(), shapes, shaped);
    shape::unused(*cfg.root(), shapes, shaped);  // a shape at the wrong depth checks nothing
    unused_aliases(cfg, shaped, report);         // an (@aka old) nothing names
    tools::check_rules(*cfg.root(), shapes, l.ctx, shaped);  // a broken rule fails the check
    {  // an asset the lister cannot find fails the check; so does a record a baked
       // table could not read whole
        Vec<Error> errors(shaped.errors.begin(), shaped.errors.end());
        for (const Error& e : cfg.table_errors()) errors.push_back(e);
        for (const AssetEdge& e : asset_edges(l, shapes)) {
            if (e.exists) continue;
            Error missing = make_error(ErrorCode::NotFound,
                                       "asset \"" + Str(e.use.target) + "\" of " +
                                           dotted(e.use.path) + " is not there (" + e.target + ")",
                                       e.use.file, e.use.line);
            missing.set_path(e.use.path.str());
            errors.push_back(std::move(missing));
        }
        shaped.errors = List<Error>({errors.data(), errors.size()});
    }
    for (const Error& e : shaped.errors) err("shape: " + Str(to_string(e)) + "\n");
    for (const Error& e : shaped.warnings) err("shape warning: " + Str(to_string(e)) + "\n");
    // The views the game compiled, against the contract layers' shapes.
    if (!hooks.views.empty()) {
        shape::Report drift;
        if (!project || project->views.layers.empty()) {
            const Error e = make_error(
                ErrorCode::NotFound,
                "the game compiled " + detail::decimal(static_cast<int64_t>(hooks.views.size())) +
                    " views, but the project names no (views (layers ...)) to check them against");
            drift.warnings = List<Error>(std::span<const Error>(&e, 1));
        } else {
            const auto roots = shape::contract_roots(cfg, project->views.layers.items());
            const shape::Set contract =
                shape::contract_set(cfg.read_session(), roots.items(), drift);
            shape::check_views(contract, hooks.views, drift);
        }
        Vec<Error> errors(shaped.errors.begin(), shaped.errors.end());
        Vec<Error> warnings(shaped.warnings.begin(), shaped.warnings.end());
        for (const Error& e : drift.errors) {
            err("view: " + Str(to_string(e)) + "\n");
            errors.push_back(e);
        }
        for (const Error& e : drift.warnings) {
            err(Str(line.strict_views ? "view: " : "view warning: ") + Str(to_string(e)) + "\n");
            (line.strict_views ? errors : warnings).push_back(e);
        }
        shaped.errors = List<Error>(std::span<const Error>(errors.data(), errors.size()));
        shaped.warnings = List<Error>(std::span<const Error>(warnings.data(), warnings.size()));
    }
    report.shape_errors = List<Error>(shaped.errors.items());
    report.shape_warnings = List<Error>(shaped.warnings.items());
#endif
    return report_check(report, l, &cfg.read_session());
}

// assets: every asset the merged stack names, as baker.assets/1 (docs/assets-json.md),
// sorted by file, line and path; exit 1 when one is not there.
int assets(const CommandLine& line, const CommandHooks& hooks) {
    Loaded l(hooks);
    if (!load(l, line)) return kCommandError;
#if BAKER_HOST_SHAPE
    const Config& cfg = l.live.config();
    Project read;
    const Project* project = project_of(l, read);
    shape::Set shapes;
    shape::Report declared;
    add_shapes(cfg, shapes, declared);
    add_asset_kinds(project, shapes);
    Vec<AssetEdge> edges = asset_edges(l, shapes);
    std::stable_sort(edges.begin(), edges.end(), [](const AssetEdge& a, const AssetEdge& b) {
        if (a.use.file != b.use.file) return a.use.file.str() < b.use.file.str();
        if (a.use.line != b.use.line) return a.use.line < b.use.line;
        return a.use.path.str() < b.use.path.str();
    });
    Str json = R"({"schema": "baker.assets/1", "root": )";
    detail::json_string(json, detail::parent_path(l.live.project_file().str()));
    json += ", \"assets\": [";
    bool missing = false;
    for (size_t i = 0; i < edges.size(); ++i) {
        const AssetEdge& e = edges[i];
        json += i ? ",\n  {\"from\": {\"file\": " : "\n  {\"from\": {\"file\": ";
        detail::json_string(json, e.use.file.str());
        json += ", \"line\": " + detail::decimal(e.use.line) + ", \"path\": ";
        detail::json_string(json, dotted(e.use.path).view());
        json += ", \"id\": ";
        char hex[RecordId::kDigits];
        e.use.id.write(hex);
        detail::json_string(
            json, e.use.id.empty() ? std::string_view() : std::string_view(hex, RecordId::kDigits));
        json += "}, \"field\": ";
        detail::json_string(json, e.use.field.str());
        json += ", \"kind\": ";
        detail::json_string(json, e.use.kind.str());
        json += ", \"target\": ";
        detail::json_string(json, e.use.target);
        json += Str(", \"exists\": ") + (e.exists ? "true" : "false") + "}";
        missing |= !e.exists;
    }
    out(json + (edges.empty() ? "]}\n" : "\n]}\n"));
    return missing ? kCommandFailed : 0;
#else
    err("assets: this build has no baker::shape\n");
    return kCommandError;
#endif
}

// bake --baker-stack FILE: the contract layers ((views (layers ...))) loaded as the
// game loads them, with its tables registered, written as one stack image for a shipped
// build to open in place of the layers' text.
int bake(const CommandLine& line, const CommandHooks& hooks) {
    if (line.stack.empty()) {
        err("bake writes the stack image: bake --baker-stack game.datb\n");
        return kCommandError;
    }
    const std::string_view from = !line.project.empty() ? line.project : std::string_view(".");
    Project read;
    const Project* project = hooks.built;
    if (const Symbol file = find_project(from, hooks.reader); !file.empty()) {
        auto p = read_project(file.str(), hooks.reader);
        if (!p) {
            err("bake: " + Str(describe(p.error())) + "\n");
            return kCommandError;
        }
        read = std::move(*p);
        project = &read;
    }
    if (!project || project->views.layers.empty()) {
        err("bake: the project names no (views (layers ...)): the image holds the contract "
            "layers\n");
        return kCommandError;
    }
    // The contract layers alone: every other layer is left out of the load.
    CommandHooks mine = hooks;
    for (const ProjectLayer& l : project->layers) {
        bool contract = false;
        for (const Symbol c : project->views.layers) contract = contract || c == l.name;
        if (!contract && !mine.open.skip.add(l.name.str())) {
            err("bake: too many layers to leave out of the image\n");
            return kCommandError;
        }
    }
    Loaded l(mine);
    if (!load(l, line)) return kCommandError;
    Str image;
    if (auto r = write_stack_image(l.live.config(), project->views.layers.items(),
                                   detail::str_out(image));
        !r) {
        err("bake: " + Str(describe(r.error())) + "\n");
        return kCommandError;
    }
    if (!write_whole(line.stack, image.view())) {
        err("bake: cannot write '" + Str(line.stack) + "'\n");
        return kCommandError;
    }
    out("wrote " + Str(line.stack) + ": " +
        detail::decimal(static_cast<int64_t>(project->views.layers.size())) + " layers, " +
        detail::decimal(static_cast<int64_t>(detail::table_slots(l.live.config()).size())) +
        " tables\n");
    return 0;
}

int reference(const CommandLine& line, const CommandHooks& hooks) {
    Loaded l(hooks);
    if (!load(l, line)) return kCommandError;
    const Config& cfg = l.live.config();
    if (!line.json) {
        tools::reference(l.ctx, stdout_sink(), &cfg.read_session());
        return 0;
    }
    // The three documents in one object: the vocabulary, the tweaks, and the shapes the
    // enabled layers declare.
    Str ref, tweaks, shapes = "null";
    tools::write_reference_json(l.ctx, detail::str_out(ref), &cfg.read_session());
    baker::write_json(TweakRegistry::global(), detail::str_out(tweaks));
#if BAKER_HOST_SHAPE
    shape::Set set;
    shape::Report report;
    add_shapes(cfg, set, report);
    shapes.clear();
    shape::write_json(set, detail::str_out(shapes));
#endif
    out("{\"reference\": " + ref + ", \"tweaks\": " + tweaks + ", \"shapes\": " + shapes + "}\n");
    return 0;
}

int eval(const CommandLine& line, const CommandHooks& hooks) {
    if (line.commands.empty()) {
        err("eval needs a command: eval \"{...}\"\n");
        return kCommandError;
    }
    Loaded l(hooks);
    if (!load(l, line)) return kCommandError;
    l.ctx.var("seed") = Node(static_cast<int64_t>(line.seed));
    if (hooks.reset) hooks.reset(l.ctx, line.seed);
    int status = 0;
    if (line.json) {
        // One baker.reply/1.2 line per command, as a running game's console answers a
        // request: a tool reads a headless game and a live one the same way.
        int64_t id = 0;
        for (const std::string_view command : line.commands) {
            Str reply;
            eval_request(l.ctx, "(" + detail::decimal(++id) + " " + Str(command) + ")",
                         detail::str_out(reply));
            out(reply);
            if (reply.find(R"(,"ok":false,)") != Str::npos) status = kCommandFailed;
        }
        return status;
    }
    for (const std::string_view command : line.commands) {
        auto r = l.ctx.eval(command);
        if (!r) {
            err(describe(r.error()));
            err("\n");
            status = kCommandFailed;
            continue;
        }
        if (auto text = r->as_str())
            out(*text);
        else
            out(detail::text_of(*r));
        out("\n");
    }
    return status;
}

// Values one after another, as a row writes them: (aim 0 0)'s "0 0", (aim (0 0))'s "(0 0)";
// "()" for none. With `list`, the list a verb changes, in parentheses: "(coin gem)".
Str values_text(const RowValues& values, bool list = false) {
    Str s;
    for (const Node& v : values) {
        if (!s.empty()) s += ' ';
        s += detail::text_of(v);
    }
    return list || values.empty() ? "(" + s + ")" : s;
}
bool composes(const LayerEffectEntry& e) {
    return e.op == Symbol("#append") || e.op == Symbol("#drop");
}

// The row of `root` an effect entry was read from: the first with its key, or for a later
// row that composes (a key's second #append), the one on the entry's line.
const Array* row_of(const Array* root, const LayerEffectEntry& e) {
    const Path path(e.path->view());
    const Array* a = root && path.valid() ? root->try_get(path) : nullptr;
    if (!a || a->line() == e.source.line || path.empty()) return a;
    const Array* parent =
        path.size() == 1 ? root : root->try_get(Path(detail::text_of(path, path.size() - 1)));
    for (size_t i = 0; parent && i < parent->size(); ++i)
        if (const Array* c = (*parent)[i].try_array();
            c && c->tag() == path[path.size() - 1].name && c->line() == e.source.line)
            return c;
    return a;
}

// Each value the layer sets as a row, (/player.speed 4.5), its value below in a comment:
// text a layer file can hold. A verb reads bare in its slot, and metadata
// follows. A key no row can spell is a comment.
void print_rows(const Config& cfg, std::string_view layer, const List<LayerEffectEntry>& effect,
                std::string_view heading) {
    out("; " + Str(heading) + "\n");
    const Ref<Array> root = cfg.layer_root(layer);
    for (const LayerEffectEntry& e : effect) {
        const std::string_view key = e.path->view();
        if (!e.declares.empty()) {  // a declaration is written as itself, not as a row
            out(Str(e.declares.str()) + " " + values_text(e.values) + "\n");
            continue;
        }
        const Array* a = row_of(root.get(), e);
        bool spelled = a && !key.empty() && key[0] != '*';
        for (const char c : key)
            spelled = spelled && !detail::is_atom_end(static_cast<unsigned char>(c));
        if (!spelled) {
            out("; " + Str(key) + " cannot be written as a row\n");
            continue;
        }
        Str row = "(/" + Str(key);
        const Values values = a->values();
        for (size_t i = 0; i < values.size(); ++i)
            row += " " + (i == 0 && !e.op.empty() ? Str(e.op.str()) : detail::text_of(values[i]));
        for (const Array& m : a->metadata())
            row += " " + detail::text_of(Node(Ref<Array>(const_cast<Array*>(&m))));
        row += ")  ; ";
        row += e.below ? "was " + values_text(*e.below, composes(e)) : Str("new");
        if (e.shadowed) row += ", shadowed";
        if (e.stale) row += ", stale";
        if (e.folded) row += ", " + detail::decimal(e.folded) + " more folded into it";
        out(row + "\n");
    }
}

// What a layer's rows say of themselves: "# path: note" for each (@note ...), a patch's
// preamble, which git apply and patch skip.
Str notes_of(const Config& cfg, std::string_view layer) {
    Str out;
    auto effect = tools::layer_effect(cfg, layer);
    const Ref<Array> root = cfg.layer_root(layer);
    for (const LayerEffectEntry& e : effect ? *effect : List<LayerEffectEntry>()) {
        const Array* a = row_of(root.get(), e);
        const Array* note = a ? a->meta(Symbol("@note")) : nullptr;
        if (!note) continue;
        Str text;
        for (const Node& v : note->values()) {
            if (!text.empty()) text += ' ';
            text += v.try_str() ? Str(*v.try_str()) : detail::text_of(v);
        }
        out += "# " + Str(e.path->view()) + ": " + text + "\n";
    }
    return out;
}

// One line per value the layer sets: path, the value below it, its value, where; or, with
// `rows`, each as a row.
int print_effect(const Config& cfg, std::string_view layer, std::string_view heading,
                 bool rows = false) {
    auto effect = tools::layer_effect(cfg, layer);
    if (!effect) {
        err(to_string(effect.error()));
        err("\n");
        return kCommandError;
    }
    if (effect->empty()) {
        out("layer '" + Str(layer) + "' changes nothing\n");
        return 0;
    }
    if (rows) {
        print_rows(cfg, layer, *effect, heading);
        return 0;
    }
    out(Str(heading) + "\n");
    for (const LayerEffectEntry& e : *effect) {
        const bool verb = !e.op.empty() && e.op != Symbol("#remove");  // #remove is the value
        const Str what = e.declares.empty() ? Str() : Str(e.declares.str()) + " ";
        out("  " + what + Str(e.path->view()) + "  " +
            (e.below ? values_text(*e.below, verb) : Str("(new)")) + " -> " +
            (verb ? Str(e.op.str()) + " " : Str()) + values_text(e.values) + "  (" +
            Str(e.source.file.str()) + ":" + detail::decimal(e.source.line) + ")" +
            (e.shadowed ? "  shadowed" : "") + (e.stale ? "  stale" : "") +
            (e.folded ? "  folded " + detail::decimal(e.folded) : Str()) + "\n");
    }
    return 0;
}

int diff(const CommandLine& line, const CommandHooks& hooks) {
    Loaded l(hooks);
    if (!load(l, line)) return kCommandError;
    return print_effect(l.live.config(), line.layer,
                        "layer '" + Str(line.layer) + "' changes:", line.rows);
}

// A file's lines, each with its newline; the last may have none.
Vec<std::string_view> lines_of(std::string_view text) {
    Vec<std::string_view> lines;
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = text.find('\n', at);
        const size_t next = end == std::string_view::npos ? text.size() : end + 1;
        lines.push_back(text.substr(at, next - at));
        at = next;
    }
    return lines;
}

// A unified diff of `before` to `after`, named `name` under a/ and b/ (git apply, patch -p1),
// with three lines of context; empty when they are the same. The common head and tail are
// cut first, so the line-by-line comparison runs only over what changed.
Str unified_diff(std::string_view before, std::string_view after, std::string_view name) {
    if (before == after) return {};
    const Vec<std::string_view> a = lines_of(before), b = lines_of(after);
    size_t head = 0;
    while (head < a.size() && head < b.size() && a[head] == b[head]) ++head;
    size_t tail = 0;
    while (tail < a.size() - head && tail < b.size() - head &&
           a[a.size() - 1 - tail] == b[b.size() - 1 - tail])
        ++tail;
    // The longest common subsequence of the middles, then the edit script over all lines.
    const size_t n = a.size() - head - tail, m = b.size() - head - tail;
    Vec<uint32_t> lcs((n + 1) * (m + 1), 0);
    auto at = [&](size_t i, size_t j) -> uint32_t& { return lcs[i * (m + 1) + j]; };
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;)
            at(i, j) = a[head + i] == b[head + j] ? at(i + 1, j + 1) + 1
                                                  : std::max(at(i + 1, j), at(i, j + 1));
    struct Op {
        char kind;      // ' ', '-', '+'
        size_t ai, bi;  // the line's index in a (for ' ' and '-') and in b (for ' ' and '+')
    };
    Vec<Op> ops;
    for (size_t i = 0; i < head; ++i) ops.push_back({' ', i, i});
    size_t i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && a[head + i] == b[head + j]) {
            ops.push_back({' ', head + i++, head + j++});
        } else if (i < n && (j == m || at(i + 1, j) >= at(i, j + 1))) {
            ops.push_back({'-', head + i++, head + j});  // removals first, as diff -u
        } else {
            ops.push_back({'+', head + i, head + j++});
        }
    }
    for (size_t k = 0; k < tail; ++k)
        ops.push_back({' ', a.size() - tail + k, b.size() - tail + k});

    Str text = "--- a/" + Str(name) + "\n+++ b/" + Str(name) + "\n";
    // Hunks: runs of changes no more than twice the context apart, with the context around.
    const size_t context = 3;
    Vec<size_t> changes;
    for (size_t k = 0; k < ops.size(); ++k)
        if (ops[k].kind != ' ') changes.push_back(k);
    for (size_t c = 0; c < changes.size();) {
        size_t last = c;
        while (last + 1 < changes.size() && changes[last + 1] - changes[last] <= 2 * context + 1)
            ++last;
        const size_t start = changes[c] > context ? changes[c] - context : 0;
        const size_t end = std::min(ops.size(), changes[last] + context + 1);
        size_t a_count = 0, b_count = 0;
        for (size_t x = start; x < end; ++x) {
            a_count += ops[x].kind != '+' ? 1 : 0;
            b_count += ops[x].kind != '-' ? 1 : 0;
        }
        // A side with no lines in the hunk names the line before it, as diff -u does.
        const size_t a_start = ops[start].ai + (a_count ? 1 : 0);
        const size_t b_start = ops[start].bi + (b_count ? 1 : 0);
        text += "@@ -" + detail::decimal(static_cast<int64_t>(a_start)) + "," +
                detail::decimal(static_cast<int64_t>(a_count)) + " +" +
                detail::decimal(static_cast<int64_t>(b_start)) + "," +
                detail::decimal(static_cast<int64_t>(b_count)) + " @@\n";
        for (size_t x = start; x < end; ++x) {
            const std::string_view l = ops[x].kind == '+' ? b[ops[x].bi] : a[ops[x].ai];
            text += ops[x].kind;
            text += l;
            if (l.empty() || l.back() != '\n') text += "\n\\ No newline at end of file\n";
        }
        c = last + 1;
    }
    return text;
}

// Through the host's reader when it gave one, as the stack was read.
bool read_whole(const Str& path, Str& text, const LoadSession::Reader& reader) {
    text.clear();
    if (reader) return reader(path.view(), detail::str_out(text));
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append({buf, n});
    std::fclose(f);
    return true;
}

bool write_whole(std::string_view path, std::string_view text) {
    std::FILE* f = std::fopen(Str(path).c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

int send(const CommandLine& line, const CommandHooks& hooks, const Str& output, int status);

int promote(const CommandLine& line, const CommandHooks& hooks) {
    Loaded l(hooks);
    if (!load(l, line)) return kCommandError;
    Config& cfg = l.live.config();
    auto report_refusals = [](const WriteReport& r) {
        for (const Error& e : r.refused) err("refused: " + Str(to_string(e)) + "\n");
    };
    if (!line.patch.empty()) {
        // The real promotion, with the Config's writer capturing each file instead of
        // writing it: nothing on disk is written, created, or locked.
        Vec<std::pair<Str, Str>> captured;
        cfg.set_writer([&captured](std::string_view path, std::string_view text) {
            captured.emplace_back(Str(path), Str(text));
            return true;
        });
        auto report = cfg.promote(line.layer, line.to);
        if (!report) {
            err(describe(report.error()));
            err("\n");
            return kCommandError;
        }
        report_refusals(*report);
        std::sort(captured.begin(), captured.end(),
                  [](const auto& x, const auto& y) { return x.first.view() < y.first.view(); });
        fs::path root = line.patch_root.empty()
                            ? fs::path(Str(l.live.project_file().str()).c_str()).parent_path()
                            : fs::path(Str(line.patch_root).c_str());
        if (root.empty()) root = ".";  // a project file named without a directory
        Str patch = report->ok() ? notes_of(cfg, line.layer) : Str();
        if (report->ok())  // all or nothing, as --baker-write is
            for (const auto& [path, text] : captured) {
                Str before;
                (void)read_whole(path, before, l.hooks.reader);
                std::error_code ec;
                const fs::path rel = fs::relative(fs::path(path.c_str()), root, ec);
                patch += unified_diff(before, text,
                                      ec ? std::string_view(path.view()) : rel.generic_string());
            }
        if (!write_whole(line.patch, patch)) {
            err("cannot write " + Str(line.patch) + "\n");
            return kCommandError;
        }
        out("wrote " + Str(line.patch) + ": " +
            detail::decimal(static_cast<int64_t>(report->ok() ? captured.size() : 0)) +
            " files would change;\napply it from " + Str(root.generic_string()) +
            " with git apply -p1 (or patch -p1)\n");
        return report->ok() ? 0 : kCommandFailed;
    }
    if (line.json) {
        // The promotion's transaction as baker.changes/1: what it writes and each
        // value it changes. Without --baker-write, the writer captures the files instead,
        // so the records are what the promotion would make and nothing is written.
        if (!line.write) cfg.set_writer([](std::string_view, std::string_view) { return true; });
        cfg.set_change_history(1);
        auto report = cfg.promote(line.layer, line.to);
        if (!report) {
            err(describe(report.error()));
            err("\n");
            return kCommandError;
        }
        report_refusals(*report);
        Str json;
        tools::write_changes_json(cfg, detail::str_out(json));
        return send(line, hooks, json, report->ok() ? 0 : kCommandFailed);
    }
    if (!line.write)
        return print_effect(
            cfg, line.layer,
            "would write into " + Str(line.to) + " (rerun with --baker-write to write):",
            line.rows);
    auto report = cfg.promote(line.layer, line.to);
    if (!report) {
        err(describe(report.error()));
        err("\n");
        return kCommandError;
    }
    report_refusals(*report);
    if (!report->ok()) return kCommandFailed;
    for (const WrittenLine& w : report->written)
        out("wrote " + Str(w.file.str()) + ":" + detail::decimal(w.line) + "\n");
    out("layer '" + Str(line.layer) + "' is unchanged: empty it once the promotion is reviewed\n");
    return 0;
}

// The output, sent where the command line or the host asked: the file --baker-output names, else
// the host's sink, else stdout, so an engine that owns stdout keeps the result apart.
int send(const CommandLine& line, const CommandHooks& hooks, const Str& output, int status) {
    if (!line.output.empty()) {
        if (!write_whole(line.output, output)) {
            err("cannot write " + Str(line.output) + "\n");
            return kCommandError;
        }
    } else if (hooks.output) {
        hooks.output(output.view());
    } else {
        std::fwrite(output.data(), 1, output.size(), stdout);
    }
    return status;
}

// sim, for a game that steps on demand: a SimRun driven with its reset and step.
int run_scenarios(const CommandLine& line, const CommandHooks& hooks) {
    if (!hooks.step) {
        err("sim: this game gives no step (CommandHooks::step), so it cannot run scenarios\n");
        return kCommandError;
    }
    SimRun run(line, hooks);
    for (SimRun::Wants w; (w = run.wants()) != SimRun::Wants::Done;) {
        if (w == SimRun::Wants::Reset) {
            if (hooks.reset) hooks.reset(run.context(), run.seed());
            run.ready();
        } else {
            hooks.step(run.context(), run.step_seconds());
            run.step();
        }
    }
    return run.status();
}

// Which objectives moved between two `sim --baker-results` directories, without running anything.
// Lays out files as Baker writes them (tools::format_text), with the project's layout.
// --- format's rewrites: fields under a name their shape renamed, and record IDs. ---

// A rewrite format wants, located as the tree locates the array (its file and line): resolved
// to a byte edit once the file's text is read, at the bracket on that line whose tag spells
// `tag`, in order.
struct Want {
    enum class Kind : uint8_t {
        Rename,     // the tag token becomes `text`
        InsertId,   // " (@id text)" after the tag
        ReplaceId,  // the record's (@id ...) value becomes `text`
        Value,      // each value token of the bracket spelling `value` becomes `text`
        AppendAka,  // " (@aka text)" before the record's close, or into its (@aka ...)
    };
    Kind kind;
    Symbol file;
    int32_t line;
    Symbol tag;    // the field's old name, or the record's key
    Str text;      // the field's name now, or the ID
    Symbol value;  // Value: the key as written
    bool done = false;
};

struct Rewrite {
    uint32_t begin, end;
    Str text;
};

// Whether the token spells `name`: as a symbol, or quoted.
bool spells(std::string_view text, const tools::TextToken& t, Symbol name) {
    const std::string_view s = text.substr(t.begin, t.end - t.begin);
    if (t.kind == tools::TextTokenKind::Symbol) return s == name.str();
    if (t.kind == tools::TextTokenKind::QuotedSymbol && s.size() >= 2)
        return s.substr(1, s.size() - 2) == name.str();
    return false;
}

// Resolves the wants of `file` against its text, every want a bracket answers to (a rename
// wants three of one record's). A row's path, (/a.b 1), is not a tag the structure spells,
// so a field a row names stays as written (check still warns).
void resolve(std::string_view text, const tools::TextStructure& s, Symbol file, Vec<Want>& wants,
             Vec<Rewrite>& out) {
    const auto n = static_cast<int32_t>(s.brackets.size());
    for (int32_t b = 0; b < n; ++b) {
        const tools::TextBracket& br = s.brackets[static_cast<size_t>(b)];
        if (br.tag < 0) continue;
        const tools::TextToken& tag = s.tokens[static_cast<size_t>(br.tag)];
        for (Want& w : wants) {
            if (w.done || w.file != file || w.line != tag.line || !spells(text, tag, w.tag))
                continue;
            w.done = true;
            // The bracket's child tagged `meta`, (@id ...) or (@aka ...), as a bracket index.
            const auto child = [&](Symbol meta) -> int32_t {
                for (int32_t c = b + 1; c < n && s.brackets[static_cast<size_t>(c)].begin < br.end;
                     ++c) {
                    const tools::TextBracket& cb = s.brackets[static_cast<size_t>(c)];
                    if (cb.parent == b && cb.tag >= 0 &&
                        spells(text, s.tokens[static_cast<size_t>(cb.tag)], meta))
                        return c;
                }
                return -1;
            };
            if (w.kind == Want::Kind::Rename) {
                out.push_back({tag.begin, tag.end, w.text});
            } else if (w.kind == Want::Kind::InsertId) {
                out.push_back({tag.end, tag.end, " (@id " + w.text + ")"});
            } else if (w.kind == Want::Kind::ReplaceId) {
                // The record's (@id ...), whose value is replaced; absent: inserted.
                const int32_t c = child("@id");
                if (c < 0) {
                    out.push_back({tag.end, tag.end, " (@id " + w.text + ")"});
                } else {
                    const tools::TextBracket& cb = s.brackets[static_cast<size_t>(c)];
                    const tools::TextToken& id_tag = s.tokens[static_cast<size_t>(cb.tag)];
                    const auto next = static_cast<size_t>(cb.tag) + 1;
                    if (next < s.tokens.size() &&
                        s.tokens[next].kind != tools::TextTokenKind::Close &&
                        s.tokens[next].begin < cb.end)
                        out.push_back({s.tokens[next].begin, s.tokens[next].end, w.text});
                    else
                        out.push_back({id_tag.end, id_tag.end, " " + w.text});
                }
            } else if (w.kind == Want::Kind::Value) {
                for (size_t t = static_cast<size_t>(br.tag) + 1;
                     t < s.tokens.size() && s.tokens[t].begin < br.end; ++t)
                    if (spells(text, s.tokens[t], w.value))
                        out.push_back({s.tokens[t].begin, s.tokens[t].end, w.text});
            } else {  // AppendAka: into the record's (@aka ...), else a new one before its close
                const int32_t c = child("@aka");
                const tools::TextBracket& into = c < 0 ? br : s.brackets[static_cast<size_t>(c)];
                if (!into.closed) {
                    w.done = false;  // an unclosed bracket: the text does not parse
                    continue;
                }
                out.push_back(
                    {into.end - 1, into.end - 1, c < 0 ? " (@aka " + w.text + ")" : " " + w.text});
            }
        }
    }
}

Str splice(std::string_view text, Vec<Rewrite>& edits) {
    std::stable_sort(edits.begin(), edits.end(),
                     [](const Rewrite& a, const Rewrite& b) { return a.begin < b.begin; });
    Str out;
    uint32_t from = 0;
    for (const Rewrite& e : edits) {
        if (e.begin < from) continue;  // two wants at one token: the first wins
        out += text.substr(from, e.begin - from);
        out += e.text;
        from = e.end;
    }
    out += text.substr(from);
    return out;
}

// Whether `file`, as the tree names it, is the file named `path` on the command line.
bool same_file(Symbol file, const Str& path) {
    if (file.str() == path.view()) return true;
    std::error_code ec;
    return fs::equivalent(fs::path(Str(file.str()).c_str()), fs::path(path.c_str()), ec) && !ec;
}

#if BAKER_HOST_SHAPE
// Every field the stack spells by a name its shape renamed: each enabled layer's own tree is
// checked, not the merged one, so an overlay's spelling is found beside the base's. A layer
// alone lacks most fields; only its renames are read.
void want_renames(const Config& cfg, Vec<Want>& wants) {
    shape::Set set;
    shape::Report declared;
    add_shapes(cfg, set, declared);
    for (const Symbol layer : cfg.layers()) {
        if (!cfg.layer_enabled(layer.str())) continue;
        const Ref<Array> root = cfg.layer_root(layer.str());
        if (!root) continue;
        shape::Report r;
        shape::check(*root, set, r);
        for (const shape::Renamed& m : r.renames)
            wants.push_back({Want::Kind::Rename, m.file, m.line, m.old, Str(m.name.str()), {}});
    }
}
#endif

// The array at `path` (dotted, from the root) below `root`, or null.
const Array* at_path(const Array* root, std::string_view path) {
    const Array* t = root;
    for (size_t at = 0; t && at <= path.size();) {
        const size_t dot = std::min(path.find('.', at), path.size());
        t = t->try_find(Symbol(path.substr(at, dot - at)));
        at = dot + 1;
    }
    return t;
}

// A fresh ID from the system's random source: the library mints none.
Str mint_id() {
    std::random_device rd;
    const auto word = [&rd] { return (static_cast<uint64_t>(rd()) << 32) | rd(); };
    RecordId id{word(), word()};
    id.hi = (id.hi & 0x0fffffffffffffffULL) | (static_cast<uint64_t>(0xA + id.lo % 6) << 60);
    char hex[RecordId::kDigits];
    id.write(hex);
    return Str(std::string_view(hex, RecordId::kDigits));
}

// An ID for each record of `table` (a path from the root) that has none, in the lowest
// enabled layer holding the record, where it is declared; a record repeating another's ID (a
// copy) gets its own. False if no layer has the table.
bool want_ids(const Config& cfg, std::string_view table, Vec<Want>& wants) {
    Vec<Symbol> declared;
    Vec<RecordId> used;
    bool found = false;
    for (const Symbol layer : cfg.layers()) {
        if (!cfg.layer_enabled(layer.str())) continue;
        const Ref<Array> root = cfg.layer_root(layer.str());
        const Array* t = at_path(root.get(), table);
        if (!t) continue;
        found = true;
        for (uint32_t i = t->tag().empty() ? 0 : 1; i < t->size(); ++i) {
            const Array* r = (*t)[i].try_array();
            if (!r || r->tag().empty() || r->tag().is_meta()) continue;
            if (std::find(declared.begin(), declared.end(), r->tag()) != declared.end()) continue;
            declared.push_back(r->tag());
            const RecordId id = r->record_id();
            if (!id.empty() && id.valid() &&
                std::find(used.begin(), used.end(), id) == used.end()) {
                used.push_back(id);
                continue;
            }
            const bool has = r->meta("@id") != nullptr;
            wants.push_back({has ? Want::Kind::ReplaceId : Want::Kind::InsertId,
                             r->file(),
                             r->line(),
                             r->tag(),
                             mint_id(),
                             {}});
        }
    }
    return found;
}

// Whether `text` is one symbol to the lexer: a key a record can be renamed to.
bool one_symbol(std::string_view text) {
    const tools::TextStructure s = tools::read_structure(text);
    return s.errors.empty() && s.tokens.size() == 1 &&
           s.tokens[0].kind == tools::TextTokenKind::Symbol && s.tokens[0].begin == 0 &&
           s.tokens[0].end == text.size();
}

// rename OLD NEW [--baker-write]: the record at path OLD takes the key NEW in the
// layer that declares it, keeps OLD as an alias, gets an ID if it has none, and every
// (key_of ...) value naming it in any enabled layer is rewritten. Without --baker-write,
// the edits are listed and nothing is written; with a refusal, nothing is written.
int rename(const CommandLine& line, const CommandHooks& hooks) {
    if (line.rest.size() != 2) {
        err("rename takes OLD, a record's path, and NEW, its key: rename weapons.laser plasma\n");
        return kCommandError;
    }
    const std::string_view old_path = line.rest[0], to = line.rest[1];
    const size_t dot = old_path.rfind('.');
    const std::string_view table_path =
        dot == std::string_view::npos ? "" : old_path.substr(0, dot);
    const Symbol key(dot == std::string_view::npos ? old_path : old_path.substr(dot + 1));
    if (key.empty() || !one_symbol(key.str())) {
        err("rename: '" + Str(old_path) + "' does not name a record\n");
        return kCommandError;
    }
    if (!one_symbol(to) || to.front() == '@') {
        err("rename: '" + Str(to) + "' is not a key: one symbol, as a record's tag is written\n");
        return kCommandError;
    }
    Loaded l(hooks);
    if (!load(l, line)) return kCommandError;
    const Config& cfg = l.live.config();
    const Ref<Array> merged = cfg.root();
    const Array* table = table_path.empty() ? merged.get() : at_path(merged.get(), table_path);
    const Array* record = table ? table->try_find(key) : nullptr;
    if (!record) {
        err("rename: no record '" + Str(old_path) + "' in the stack\n");
        return kCommandError;
    }
    // The key must be free: no sibling has it, as its key or as an alias.
    if (table->try_find(Symbol(to))) {
        err("rename: '" + Str(to) + "' is already a key of " +
            Str(table_path.empty() ? "the root" : table_path) + "\n");
        return kCommandError;
    }
    for (uint32_t i = table->tag().empty() ? 0 : 1; i < table->size(); ++i)
        if (const Array* sib = (*table)[i].try_array())
            if (const Array* aka = sib->meta("@aka"))
                for (const Node& v : aka->values())
                    if (v.is(Symbol(to))) {
                        err("rename: '" + Str(to) + "' is an alias of '" + Str(sib->tag().str()) +
                            "' (its (@aka ...)); remove it first\n");
                        return kCommandError;
                    }
    // The declaring layer: the lowest enabled one whose own tree holds the record by key.
    Vec<Want> wants;
    bool declared = false;
    for (const Symbol layer : cfg.layers()) {
        if (!cfg.layer_enabled(layer.str())) continue;
        const Ref<Array> root = cfg.layer_root(layer.str());
        const Array* r = root ? at_path(root.get(), old_path) : nullptr;
        if (!r) continue;
        wants.push_back({Want::Kind::Rename, r->file(), r->line(), key, Str(to), {}});
        wants.push_back({Want::Kind::AppendAka, r->file(), r->line(), key, Str(key.str()), {}});
        const RecordId id = r->record_id();
        if (id.empty() || !id.valid())
            wants.push_back({Want::Kind::InsertId, r->file(), r->line(), key, mint_id(), {}});
        declared = true;
        break;
    }
    if (!declared) {
        err("rename: no layer's own file declares '" + Str(old_path) +
            "' by that key (a layer built in code, or one naming it by an alias)\n");
        return kCommandError;
    }
    // Every (key_of TABLE) value naming the key, in every enabled layer.
#if BAKER_HOST_SHAPE
    {
        shape::Set set;
        shape::Report declared_shapes;
        add_shapes(cfg, set, declared_shapes);
        for (const Symbol layer : cfg.layers()) {
            if (!cfg.layer_enabled(layer.str())) continue;
            const Ref<Array> root = cfg.layer_root(layer.str());
            if (!root) continue;
            for (const shape::KeyUse& u : shape::key_uses(*root, set)) {
                if (u.key != key) continue;
                Str path;
                for (const Symbol seg : u.table) path += (path.empty() ? "" : ".") + Str(seg.str());
                if (path.view() != table_path) continue;
                const bool twice = std::any_of(wants.begin(), wants.end(), [&](const Want& w) {
                    return w.kind == Want::Kind::Value && w.file == u.file && w.line == u.line &&
                           w.tag == u.field;
                });
                if (!twice)
                    wants.push_back({Want::Kind::Value, u.file, u.line, u.field, Str(to), key});
            }
        }
    }
#endif
    // Each file's edits, from its text; a want no bracket spells (a row's path, a macro's
    // expansion) refuses the whole rename.
    Vec<Symbol> files;
    for (const Want& w : wants)
        if (std::find(files.begin(), files.end(), w.file) == files.end()) files.push_back(w.file);
    Vec<std::pair<Symbol, Str>> written;
    for (const Symbol file : files) {
        Str text;
        if (!read_whole(Str(file.str()), text, hooks.reader)) {
            err("rename: cannot read '" + Str(file.str()) + "'\n");
            return kCommandError;
        }
        const tools::TextStructure s = tools::read_structure(text.view(), file);
        Vec<Rewrite> rewrites;
        resolve(text.view(), s, file, wants, rewrites);
        written.emplace_back(file, splice(text.view(), rewrites));
    }
    int status = 0;
    for (const Want& w : wants) {
        Str what;
        switch (w.kind) {
            case Want::Kind::Rename: what = Str(w.tag.str()) + " -> " + w.text; break;
            case Want::Kind::AppendAka: what = "(@aka " + w.text + ")"; break;
            case Want::Kind::InsertId: what = "(@id " + w.text + ")"; break;
            case Want::Kind::Value:
                what = "(" + Str(w.tag.str()) + " ...): " + Str(w.value.str()) + " -> " + w.text;
                break;
            case Want::Kind::ReplaceId: break;
        }
        if (!w.done) {
            err("rename: cannot place " + what + " at " + Str(w.file.str()) + ":" +
                detail::decimal(w.line) +
                " (a row's path, or a macro's expansion): edit it there\n");
            status = kCommandError;
            continue;
        }
        out(Str(w.file.str()) + ":" + detail::decimal(w.line) + "  " + what + "\n");
    }
    if (status != 0) return status;
    if (!line.write) {
        out("rerun with --baker-write to write\n");
        return 0;
    }
    for (const auto& [file, text] : written) {
        if (!write_whole(file.str(), text.view())) {
            err("rename: cannot write '" + Str(file.str()) + "'\n");
            return kCommandError;
        }
        out("wrote " + Str(file.str()) + "\n");
    }
    return 0;
}

int format(const CommandLine& line, const CommandHooks& hooks) {
    // The project that governs them: the one named, else the first file's, else here's.
    const std::string_view from = !line.project.empty() ? line.project
                                  : !line.rest.empty()  ? line.rest[0]
                                                        : std::string_view(".");
    WriteOptions layout = hooks.built ? hooks.built->format : WriteOptions{};
    const Symbol project = find_project(from, hooks.reader);
    if (!project.empty()) {
        auto p = read_project(project.str(), hooks.reader);
        if (!p) {
            err("format: " + Str(describe(p.error())) + "\n");
            return kCommandError;
        }
        layout = p->format;
    }
    Vec<Str> files;
    for (const std::string_view f : line.rest) files.emplace_back(f);
    // The stack, for its own files, for the shapes' renames and for the IDs. Named files
    // without a stack are laid out and nothing more.
    Loaded l(hooks);
    bool loaded = false;
    if (files.empty() || !project.empty() || hooks.built || !line.mint.empty()) {
        loaded = load(l, line);
        if (!loaded && (files.empty() || !line.mint.empty())) return kCommandError;
        if (!loaded) err("format: the stack did not load, so no field is renamed\n");
    }
    if (files.empty()) {  // the stack's own files, under the project's directory
        const std::string_view dir = detail::parent_path(l.live.project_file().str());
        for (const StackFile& f : l.live.config().stack_files())
            if (f.present && (dir.empty() || f.path.str().starts_with(dir)) &&
                std::none_of(files.begin(), files.end(),
                             [&](const Str& s) { return s.view() == f.path.str(); }))
                files.emplace_back(f.path.str());
    }
    Vec<Want> wants;
    if (loaded) {
#if BAKER_HOST_SHAPE
        want_renames(l.live.config(), wants);
#endif
        if (!line.mint.empty() && !want_ids(l.live.config(), line.mint, wants)) {
            err("format: no layer of the stack has a table '" + Str(line.mint) + "'\n");
            return kCommandError;
        }
    }
    int status = 0;
    Str json = R"({"schema": "baker.format/1.1", "files": [)";
    for (size_t i = 0; i < files.size(); ++i) {
        Str text, laid;
        if (!read_whole(files[i], text, hooks.reader)) {
            err("format: cannot read '" + files[i] + "'\n");
            return kCommandError;
        }
        // The rewrites wanted in this file, by the names the tree gave it, then the layout.
        Vec<Rewrite> rewrites;
        if (!wants.empty()) {
            Vec<std::pair<Symbol, bool>> matched;  // each name the wants give this file
            std::optional<tools::TextStructure> s;
            for (const Want& w : wants) {
                if (w.done) continue;
                auto m = std::find_if(matched.begin(), matched.end(),
                                      [&](const auto& x) { return x.first == w.file; });
                if (m == matched.end()) {
                    matched.emplace_back(w.file, same_file(w.file, files[i]));
                    m = matched.end() - 1;
                }
                if (!m->second) continue;
                if (!s) s = tools::read_structure(text.view(), w.file);
                resolve(text.view(), *s, w.file, wants, rewrites);
            }
        }
        if (!rewrites.empty()) text = splice(text.view(), rewrites);
        const tools::Formatted r =
            tools::format_text(text.view(), detail::str_out(laid), {.layout = layout});
        for (const Error& e : r.errors) err("format: " + Str(describe(e)) + "\n");
        if (!r.errors.empty()) status = kCommandFailed;
        const bool changed = r.changed() || !rewrites.empty();
        if (line.json) {
            json += i ? ",\n  {\"file\": " : "\n  {\"file\": ";
            detail::json_string(json, files[i].view());
            json += Str(", \"changed\": ") + (changed ? "true" : "false") +
                    ", \"rewrites\": " + detail::decimal(static_cast<int64_t>(rewrites.size())) +
                    ", \"line_end\": ";
            detail::json_string(json, r.line_end);
            json += ", \"edits\": [";
            for (size_t k = 0; k < r.edits.size(); ++k) {
                const tools::FormatEdit& e = r.edits[k];
                json += (k ? ", [" : "[") + detail::decimal(e.begin) + ", " +
                        detail::decimal(e.end) + ", " + detail::decimal(e.newlines) + ", " +
                        detail::decimal(e.spaces) + "]";
            }
            json += "], \"errors\": " + Str(r.errors.empty() ? "0" : "1") + "}";
        }
        if (!changed) continue;
        if (line.check || line.json) {
            if (line.check) {
                if (!line.json) out(files[i] + "\n");
                status = kCommandFailed;
            }
        } else if (!write_whole(files[i].view(), laid.view())) {
            err("format: cannot write '" + files[i] + "'\n");
            return kCommandError;
        }
    }
    if (line.json) out(json + (files.empty() ? "]}\n" : "\n]}\n"));
    const auto left =
        std::count_if(wants.begin(), wants.end(), [](const Want& w) { return !w.done; });
    if (left != 0)
        err("format: " + detail::decimal(static_cast<int64_t>(left)) +
            " rewrites are in files not named here, or at a row's path\n");
    return status;
}

int compare(const CommandLine& line) {
    if (line.before.empty() || line.after.empty()) {
        err("compare needs --baker-before DIR --baker-after DIR, each written by sim "
            "--baker-results\n");
        return kCommandError;
    }
    Vec<sim::Run> before, after;
    Vec<sim::Median> medians_before, medians_after;
    for (const auto& [dir, runs, medians] : {std::tuple{line.before, &before, &medians_before},
                                             std::tuple{line.after, &after, &medians_after}}) {
        const Str file((fs::path(Str(dir).c_str()) / "results.dta").generic_string());
        if (auto r = sim::read_results(file.view(), *runs, *medians); !r) {
            err(describe(r.error()));
            err("\n");
            return kCommandError;
        }
    }
    bool broke = false;
    out(sim::compare_json(before, medians_before, after, medians_after, broke));
    return broke ? kCommandFailed : 0;
}

}  // namespace

// A sim in progress: the stack, the scenarios chosen, the run being stepped, and the output so
// far, which every call gathers while it runs.
struct SimRun::Impl {
    Impl(CommandLine l, const CommandHooks& h) : line(std::move(l)), loaded(h) {}
    CommandLine line;
    Loaded loaded;
    sim::Book book;
    Vec<const sim::Scenario*> chosen;
    size_t scenario = 0;  // into chosen
    uint64_t seed = 0, last = 0;
    std::optional<sim::Stepper> stepper;
    Vec<sim::Run> runs;
    Str output;
    Wants wants = Wants::Done;
    int status = kCommandError;

    // A line a person reads; the JSON has the rest.
    void print(const sim::Run& r) const {
        if (line.json) return;
        Str t = Str(r.scenario.str()) + " seed " + detail::decimal(static_cast<int64_t>(r.seed)) +
                ": " +
                (!r.finished  ? "STOPPED"
                 : r.passed() ? "passed"
                              : "FAILED");
        if (!r.ended.empty() && r.ended != Symbol("length"))  // the game's own end
            t += " (" +
                 Str(r.ended == Symbol("until") ? std::string_view("ended") : r.ended.str()) +
                 " at " +
                 Str(book.step_name.empty() ? std::string_view("step") : book.step_name.str()) +
                 " " + detail::decimal(static_cast<int64_t>(r.steps)) + ")";
        for (const sim::ObjectiveResult& o : r.objectives)
            t += "  " + Str(o.name.str()) + " " +
                 (o.failed ? "(" + Str(to_string(o.error)) + ")"
                           : sim::number_text(o.value) + (o.pass ? "" : " (fail)"));
        for (const Error& e : r.errors) t += "\n  error: " + Str(to_string(e));
        out(t + "\n");
    }
    void start() {
        stepper.emplace(loaded.ctx, book, *chosen[scenario], seed, line.step_hz);
        wants = Wants::Reset;
    }
    // The run has its last step: judged, then the next, or the end.
    void judge() {
        runs.push_back(stepper->judge());
        print(runs.back());
        if (seed < last) {
            ++seed;
        } else if (++scenario < chosen.size()) {
            seed = line.seed;
        } else {
            finish(false);
            return;
        }
        start();
    }
    void finish(bool stopped) {
        stepper.reset();
        const Vec<sim::Median> medians = sim::judge_medians(loaded.ctx, book, runs);
        if (!line.json)
            for (const sim::Median& m : medians)
                out(Str(m.scenario.str()) + " median of " + detail::decimal(m.seeds) +
                    " seeds: " + (m.pass ? "passed" : "FAILED") + "  " + Str(m.objective.str()) +
                    " " + sim::number_text(m.value) +
                    (m.failed ? "  " + Str(m.error.message()) : Str()) + "\n");
        const Str json = sim::results_json(runs, medians, book.step_name);
        if (line.json) out(json);
        status = stopped ? kCommandError : sim::all_passed(runs, medians) ? 0 : kCommandFailed;
        if (!line.results.empty()) {
            std::error_code ec;
            const fs::path dir(Str(line.results).c_str());
            fs::create_directories(dir, ec);
            if (!write_whole((dir / "results.json").generic_string(), json) ||
                !write_whole((dir / "results.dta").generic_string(),
                             sim::results_dta(runs, medians))) {
                err("sim: cannot write to " + Str(line.results) + "\n");
                status = kCommandError;
            }
        }
        done();
    }
    // The stack and the scenarios chosen; false, with the reason on stderr, if it cannot run.
    bool open() {
        if (line.command != "sim") {
            err("SimRun runs sim alone\n");
            return false;
        }
        if (!load(loaded, line)) return false;
        if (auto e = read_book(loaded, book); !e) {
            err(describe(e.error()));
            err("\n");
            return false;
        }
        // --baker-scenario picks by scenario name, or by its file's name without .dta.
        for (const sim::Scenario& s : book.scenarios) {
            const Str stem(fs::path(Str(s.file.str()).c_str()).stem().generic_string());
            bool wanted = line.scenarios.empty();
            for (const std::string_view n : line.scenarios)
                wanted = wanted || n == s.name.str() || n == stem.view();
            if (wanted) chosen.push_back(&s);
        }
        if (chosen.empty()) {
            err(book.scenarios.empty() ? "sim: the project names no scenarios (scenarios ...)\n"
                                       : "sim: no scenario by that name\n");
            return false;
        }
        seed = line.seed;
        last = line.seed_last ? line.seed_last : line.seed;
        return true;
    }
    // The end: the output sent, as run_command sends it.
    void done() {
        wants = Wants::Done;
        status = send(line, loaded.hooks, output, status);
    }
};

namespace {
// The output a SimRun call prints goes to its run, while the call lasts.
class Gather {
public:
    explicit Gather(Str& into) : outer_(t_output) { t_output = &into; }
    ~Gather() { t_output = outer_; }
    Gather(const Gather&) = delete;
    Gather& operator=(const Gather&) = delete;

private:
    Str* outer_;
};
}  // namespace

SimRun::SimRun(const CommandLine& line, const CommandHooks& hooks) : impl_(new Impl(line, hooks)) {
    const Gather gather(impl_->output);
    if (impl_->open())
        impl_->start();
    else
        impl_->done();
}

SimRun::~SimRun() {
    delete impl_;
}

SimRun::Wants SimRun::wants() const noexcept {
    return impl_->wants;
}
uint64_t SimRun::seed() const noexcept {
    return impl_->seed;
}
double SimRun::step_seconds() const noexcept {
    return impl_->stepper ? impl_->stepper->dt() : 0.0;
}
Context& SimRun::context() noexcept {
    return impl_->loaded.ctx;
}
int SimRun::status() const noexcept {
    return impl_->status;
}

void SimRun::ready() {
    Impl& r = *impl_;
    if (r.wants != Wants::Reset) return;
    const Gather gather(r.output);
    r.stepper->begin();
    r.wants = Wants::Steps;
    if (r.stepper->done()) r.judge();
}

void SimRun::step() {
    Impl& r = *impl_;
    if (r.wants != Wants::Steps) return;
    const Gather gather(r.output);
    r.stepper->advance();
    if (r.stepper->done()) r.judge();
}

void SimRun::stop() {
    Impl& r = *impl_;
    if (r.wants == Wants::Done) return;
    const Gather gather(r.output);
    if (r.wants == Wants::Steps) {  // a run waiting for its reset has not started
        r.runs.push_back(r.stepper->stop());
        r.print(r.runs.back());
    }
    r.finish(true);
}

namespace {
// Where an option that takes text keeps it, or null; `a` is its name after --baker-.
std::string_view* text_option(CommandLine& line, std::string_view a) {
    const std::pair<std::string_view, std::string_view*> options[] = {
        {"project", &line.project}, {"layer", &line.layer},   {"from", &line.layer},
        {"to", &line.to},           {"patch", &line.patch},   {"output", &line.output},
        {"results", &line.results}, {"before", &line.before}, {"after", &line.after},
        {"mint-ids", &line.mint},   {"stack", &line.stack}};
    for (const auto& [name, into] : options)
        if (a == name) return into;
    return nullptr;
}
}  // namespace

Expected<CommandLine> parse_command_line(int argc, const char* const* argv) {
    CommandLine line;
    if (argc > 1) line.command = argv[1];
    Vec<std::string_view> commands, rest, scenarios;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        // The kit's flags begin --baker-; everything else is the host's, or a command.
        constexpr std::string_view kPrefix = "--baker-";
        if (!arg.starts_with(kPrefix)) {
            (!arg.empty() && arg.front() == '{' ? commands : rest).push_back(arg);
            continue;
        }
        const std::string_view a = arg.substr(kPrefix.size());
        auto value = [&](std::string_view& into) -> bool {
            if (i + 1 >= argc) return false;
            into = argv[++i];
            return true;
        };
        auto missing = [&] {
            return Unexpected(make_error(ErrorCode::Syntax, Str(arg) + " needs a value"));
        };
        if (a == "json") {
            line.json = true;
        } else if (a == "write") {
            line.write = true;
        } else if (a == "rows") {
            line.rows = true;
        } else if (a == "check") {
            line.check = true;
        } else if (a == "strict-views") {
            line.strict_views = true;
        } else if (std::string_view* into = text_option(line, a)) {
            if (!value(*into)) return missing();
        } else if (a == "seed" || a == "step-hz") {
            std::string_view s;
            if (!value(s)) return missing();
            const auto n = whole(s);
            if (!n || (a == "step-hz" && *n == 0))
                return Unexpected(make_error(ErrorCode::Syntax, Str(arg) + " needs a number"));
            if (a == "seed")
                line.seed = *n;
            else
                line.step_hz = static_cast<double>(*n);
        } else if (a == "seeds") {
            std::string_view s;
            if (!value(s)) return missing();
            const size_t dash = s.find('-');
            const auto first = whole(s.substr(0, dash));
            const auto last = dash == std::string_view::npos ? first : whole(s.substr(dash + 1));
            if (!first || !last || *last < *first)
                return Unexpected(make_error(ErrorCode::Syntax, "--baker-seeds needs A-B, A <= B"));
            line.seed = *first;
            line.seed_last = *last;
        } else if (a == "scenario") {
            std::string_view s;
            if (!value(s)) return missing();
            scenarios.push_back(s);
        } else {  // a misspelled flag of the kit's is an error, never the host's
            return Unexpected(make_error(ErrorCode::Syntax, "unknown flag " + Str(arg)));
        }
    }
    line.commands =
        List<std::string_view>(std::span<const std::string_view>(commands.data(), commands.size()));
    line.rest = List<std::string_view>(std::span<const std::string_view>(rest.data(), rest.size()));
    line.scenarios = List<std::string_view>(
        std::span<const std::string_view>(scenarios.data(), scenarios.size()));
    return line;
}

namespace {

int dispatch(const CommandLine& line, const CommandHooks& hooks) {
    if (line.command == "check") return check(line, hooks);
    if (line.command == "reference") return reference(line, hooks);
    if (line.command == "assets") return assets(line, hooks);
    if (line.command == "bake") return bake(line, hooks);
    if (line.command == "eval") return eval(line, hooks);
    if (line.command == "compare") return compare(line);
    if (line.command == "format") return format(line, hooks);
    if (line.command == "diff" || line.command == "promote" || line.command == "rename") {
        if constexpr (kShipping) {
            err(Str(line.command) + ": not in a shipping build\n");
            return kCommandError;
        } else {
            return line.command == "diff"      ? diff(line, hooks)
                   : line.command == "promote" ? promote(line, hooks)
                                               : rename(line, hooks);
        }
    }
    return -1;
}

}  // namespace

int run_command(const CommandLine& line, const CommandHooks& hooks) {
    if (line.command == "sim") return run_scenarios(line, hooks);  // a SimRun sends its own
    Str output;
    int status = 0;
    {
        const Gather gather(output);
        status = dispatch(line, hooks);
    }
    return status < 0 ? status : send(line, hooks, output, status);
}

std::string_view command_help() noexcept {
    return "The kit's flags begin --baker-; any other flag is the game's.\n"
           "  check [--baker-strict-views]   baker.check/1.5 JSON; exit 1 on an error, a load\n"
           "                                 problem, a shape error, a missing asset, or an\n"
           "                                 undefined function;\n"
           "                                 --baker-strict-views: a compiled view's drift too\n"
           "  assets                         every asset the stack names, as baker.assets/1;\n"
           "                                 exit 1 when one is not there\n"
           "  bake --baker-stack FILE        the contract layers and the baked tables as one\n"
           "                                 stack image, for a shipped build to open\n"
           "  reference [--baker-json]       the script vocabulary, as Markdown; --baker-json:\n"
           "                                 {reference, tweaks, shapes} as Baker's JSON\n"
           "  diff [--baker-layer L] [--baker-rows]\n"
           "                                 what layer L (default agent) changes; --baker-rows:\n"
           "                                 as rows a layer file can hold\n"
           "  promote [--baker-from L] [--baker-to T] [--baker-write | --baker-patch FILE]\n"
           "          [--baker-rows]         layer L into T (default default); --baker-write\n"
           "                                 writes; --baker-patch writes the change as a\n"
           "                                 unified diff; --baker-rows: the change as rows\n"
           "  eval [--baker-seed N] [--baker-json] \"{command}\"...\n"
           "                                 each command's result, one a line, on the game\n"
           "                                 reset with the seed (default 1); --baker-json: one\n"
           "                                 baker.reply/1.2 line each, as the console answers\n"
           "  sim [--baker-scenario NAME]... [--baker-seed N | --baker-seeds A-B]\n"
           "      [--baker-step-hz N] [--baker-json] [--baker-results DIR]\n"
           "                                 the project's scenarios, judged by its objectives;\n"
           "                                 --baker-json: baker.sim/1.3; --baker-results: what\n"
           "                                 compare reads\n"
           "  compare --baker-before DIR --baker-after DIR\n"
           "                                 which objectives moved: baker.compare/1\n"
           "  rename OLD NEW [--baker-write]  the record at path OLD keyed NEW where it is\n"
           "                                 declared, (@aka OLD) kept, an (@id ...) minted,\n"
           "                                 every key_of naming it rewritten; --baker-write\n"
           "                                 writes, else the edits are listed\n"
           "  format [--baker-check] [--baker-json] [--baker-mint-ids TABLE] [FILE...]\n"
           "                                 lays out the files (no FILE: the stack's own) as\n"
           "                                 Baker writes them, spelling each field by its\n"
           "                                 shape's name; --baker-mint-ids: an (@id ...) for\n"
           "                                 each record of TABLE without one; --baker-check:\n"
           "                                 exit 1, listing each file that would change\n"
           "every command also takes --baker-output FILE: its output there, not on stdout\n";
}

}  // namespace baker::host
