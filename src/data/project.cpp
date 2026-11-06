// Project files: a Config's layers, pack levels, include directories, and defines,
// read from one file that the game and its tools share.

#include <baker/project.hpp>

#include "common/containers.hpp"
#include "text.hpp"

#include <algorithm>
#include <cstdio>

namespace baker {
namespace {

// Where each value of the file is written, from the source map of its one read.
class Places {
public:
    explicit Places(const SourceMap& map) : map_(map) {}

    [[nodiscard]] ProjectPosition of(const Array& a, size_t i) const {
        return place(map_.span(a, i));
    }
    [[nodiscard]] ProjectPosition of(const Array& a) const { return place(map_.extent(a)); }

    [[nodiscard]] Error at(const Array& a, std::string_view message) const {
        Error e = make_error(ErrorCode::Syntax, message, a.file(), a.line());
        if (const ProjectPosition p = of(a); p.column > 0) e.set_column(p.column);
        return e;
    }

private:
    // Lines and columns as the parser's errors count them: columns in code points, a
    // byte-order mark aside.
    [[nodiscard]] ProjectPosition place(const SourceSpan* span) const {
        if (!span || !span->literal()) return {};
        const auto text = map_.text(span->file);
        if (!text) return {};
        const size_t end = std::min<size_t>(span->begin, text->size());
        size_t from = 0;
        if (text->substr(0, 3) == "\xEF\xBB\xBF") from = std::min<size_t>(3, end);
        ProjectPosition p{1, 1};
        for (size_t i = from; i < end; ++i) {
            const auto c = static_cast<unsigned char>((*text)[i]);
            if (c == '\n') {
                ++p.line;
                p.column = 1;
            } else if ((c & 0xC0) != 0x80) {
                ++p.column;
            }
        }
        return p;
    }

    const SourceMap& map_;
};

// A path, or a string given as a symbol, from a value: "data/x.dta" or data.
bool text_of(const Node& n, std::string_view& out) {
    if (auto s = n.try_str()) {
        out = *s;
        return true;
    }
    if (auto s = n.try_sym()) {
        out = s->str();
        return true;
    }
    return false;
}

Symbol joined(std::string_view dir, std::string_view path) {
    if (dir.empty() || detail::is_absolute_path(path)) return Symbol(path);
    return Symbol(detail::join_path(dir, path).view());
}

// Each value of `a` (after its tag) as an entry, a path joined with `dir` if `paths`.
Expected<List<ProjectEntry>> entries(const Array& a, const Places& places, std::string_view dir,
                                     bool paths) {
    Vec<ProjectEntry> out;
    for (size_t i = 1; i < a.size(); ++i) {
        std::string_view text;
        if (!text_of(a[i], text))
            return Unexpected(
                places.at(a, "(" + Str(a.tag().str()) + " ...) lists names or paths"));
        out.push_back({paths ? joined(dir, text) : Symbol(text), places.of(a, i)});
    }
    return List<ProjectEntry>(std::span<const ProjectEntry>(out.data(), out.size()));
}

bool exists(const LoadSession::Reader& reader, std::string_view path) {
    if (reader) {
        return reader(path, TextSink{});  // the text is not wanted
    }
    std::FILE* f = std::fopen(detail::CPath(path).c_str(), "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}

}  // namespace

Symbol Project::tool_path(std::string_view build_dir) const {
    constexpr std::string_view kBuild = "$build";
    const std::string_view path = tool.str();
    if (!path.starts_with(kBuild)) return tool;
    std::string_view rest = path.substr(kBuild.size());
    while (!rest.empty() && (rest.front() == '/' || rest.front() == '\\')) rest.remove_prefix(1);
    const Symbol base = !build_dir.empty() ? Symbol(build_dir)
                        : !build.empty()   ? build
                                           : joined(detail::parent_path(file.str()), "build");
    return rest.empty() ? base : joined(base.str(), rest);
}

Expected<Project> read_project(std::string_view path, const LoadSession::Reader& reader) {
    SourceMap map;
    LoadSession session;
    if (reader) session.set_reader(reader);
    session.text_only = session.refuse_includes = true;  // a project file is the whole project
    session.source_map = &map;                           // where each entry is written
    auto tree = read_file(path, session);
    if (!tree) return Unexpected(std::move(tree).error());
    const Places places(map);
    const Array* root = (*tree)->try_find("project");
    if (!root) {
        // At the file's first form, if it has one.
        const ProjectPosition first = (*tree)->empty() ? ProjectPosition{} : places.of(**tree, 0);
        Error e = make_error(ErrorCode::NotFound, "a project file holds (project (layers ...) ...)",
                             Symbol(path), first.line > 0 ? first.line : 1);
        if (first.column > 0) e.set_column(first.column);
        return Unexpected(std::move(e));
    }
    const std::string_view dir = detail::parent_path(path);
    Project p;
    p.file = Symbol(path);
    bool has_layers = false;
    const Array* views_part = nullptr;
    for (size_t i = 1; i < root->size(); ++i) {
        const Array* part = (*root)[i].try_array();
        if (!part || part->empty())
            return Unexpected(places.at(*root, "(project ...) holds (key ...) forms"));
        const Symbol key = part->tag();
        if (key == Symbol("pack_levels") || key == Symbol("defines")) {
            auto list = entries(*part, places, dir, false);
            if (!list) return Unexpected(std::move(list).error());
            (key == Symbol("defines") ? p.defines : p.pack_levels) = std::move(*list);
        } else if (key == Symbol("include_dirs") || key == Symbol("scenarios") ||
                   key == Symbol("objectives")) {
            auto list = entries(*part, places, dir, true);
            if (!list) return Unexpected(std::move(list).error());
            (key == Symbol("scenarios")    ? p.scenarios
             : key == Symbol("objectives") ? p.objectives
                                           : p.include_dirs) = std::move(*list);
        } else if (key == Symbol("tool")) {
            std::string_view file;
            if (part->size() < 2 || !text_of((*part)[1], file))
                return Unexpected(places.at(*part, R"(the tool is (tool "path" "argument"...))"));
            // A path into the build directory stays as written, for tool_path().
            p.tool = file.starts_with("$build") ? Symbol(file) : joined(dir, file);
            p.tool_at = places.of(*part, 1);
            Vec<Symbol> args;
            for (size_t k = 2; k < part->size(); ++k) {
                std::string_view arg;
                if (!text_of((*part)[k], arg))
                    return Unexpected(places.at(*part, "a tool's arguments are strings"));
                args.emplace_back(arg);
            }
            p.tool_args = List<Symbol>(std::span<const Symbol>(args.data(), args.size()));
        } else if (key == Symbol("format")) {
            for (size_t k = 1; k < part->size(); ++k) {
                const Array* f = (*part)[k].try_array();
                const std::optional<int64_t> n =
                    f && f->size() == 2 ? (*f)[1].try_int() : std::nullopt;
                const bool indent = f && f->tag() == Symbol("indent");
                if (!n || (!indent && !(f && f->tag() == Symbol("width"))) || *n < 0 || *n > 1000)
                    return Unexpected(places.at(
                        f ? *f : *part, "the layout is (format (indent N) (width N)), 0 to 1000"));
                (indent ? p.format.indent : p.format.width) = static_cast<int>(*n);
            }
        } else if (key == Symbol("step_name")) {
            const auto name = part->size() == 2 ? (*part)[1].try_sym() : std::nullopt;
            if (!name) return Unexpected(places.at(*part, "the step's name is (step_name NAME)"));
            p.step_name = *name;
        } else if (key == Symbol("console")) {
            const std::optional<int64_t> port =
                part->size() == 2 ? (*part)[1].try_int() : std::nullopt;
            if (!port || *port < 1 || *port > 65535)
                return Unexpected(places.at(*part, "the console is (console PORT), 1 to 65535"));
            p.console_port = static_cast<uint16_t>(*port);
            p.console_at = places.of(*part, 1);
        } else if (key == Symbol("build")) {
            std::string_view where;
            if (part->size() != 2 || !text_of((*part)[1], where))
                return Unexpected(places.at(*part, "the build directory is (build \"path\")"));
            p.build = joined(dir, where);
            p.build_at = places.of(*part, 1);
        } else if (key == Symbol("views")) {
            views_part = part;
            p.views.at = places.of(*part, 0);
            Vec<Symbol> view_layers, view_shapes;
            for (size_t k = 1; k < part->size(); ++k) {
                const Array* v = (*part)[k].try_array();
                const Symbol vk = v ? v->tag() : Symbol();
                if (vk == Symbol("layers") || vk == Symbol("shapes")) {
                    for (size_t m = 1; m < v->size(); ++m) {
                        const auto name = (*v)[m].try_sym();
                        if (!name)
                            return Unexpected(places.at(
                                *v, "(views (layers NAME...)) and (shapes NAME...) hold names"));
                        (vk == Symbol("layers") ? view_layers : view_shapes).push_back(*name);
                    }
                } else if (vk == Symbol("namespace")) {
                    std::string_view ns;
                    if (v->size() != 2 || !text_of((*v)[1], ns) || ns.empty())
                        return Unexpected(places.at(*v, "the namespace is (namespace NAME)"));
                    p.views.ns = Symbol(ns);
                } else {
                    return Unexpected(places.at(v ? *v : *part,
                                                "views are (views (layers NAME...) [(shapes "
                                                "NAME...)] [(namespace NAME)])"));
                }
            }
            if (view_layers.empty())
                return Unexpected(
                    places.at(*part, "(views ...) names its contract layers: (layers NAME...)"));
            p.views.layers =
                List<Symbol>(std::span<const Symbol>(view_layers.data(), view_layers.size()));
            p.views.shapes =
                List<Symbol>(std::span<const Symbol>(view_shapes.data(), view_shapes.size()));
        } else if (key == Symbol("assets")) {
            Vec<ProjectAssetKind> kinds;
            for (size_t k = 1; k < part->size(); ++k) {
                const Array* kind = (*part)[k].try_array();
                const Symbol name = kind ? kind->tag() : Symbol();
                if (name.empty() || name.is_meta())
                    return Unexpected(places.at(
                        kind ? *kind : *part,
                        "assets are (assets (KIND \".ext\" ...) ...), each kind a symbol"));
                for (const ProjectAssetKind& seen : kinds)
                    if (seen.name == name)
                        return Unexpected(places.at(
                            *kind, "asset kind '" + Str(name.str()) + "' is declared twice"));
                Vec<Symbol> extensions;
                for (size_t m = 1; m < kind->size(); ++m) {
                    const auto ext = (*kind)[m].try_str();
                    if (!ext || ext->size() < 2 || ext->front() != '.')
                        return Unexpected(places.at(
                            *kind, "asset kind '" + Str(name.str()) +
                                       "' lists its extensions as strings, each with its dot"));
                    extensions.emplace_back(*ext);
                }
                kinds.push_back({name, List<Symbol>({extensions.data(), extensions.size()}),
                                 places.of(*kind, 0)});
            }
            p.assets = List<ProjectAssetKind>({kinds.data(), kinds.size()});
        } else if (key == Symbol("reference")) {
            std::string_view file;
            if (part->size() != 2 || !text_of((*part)[1], file))
                return Unexpected(places.at(*part, "the reference is (reference \"path\")"));
            p.reference = joined(dir, file);
            p.reference_at = places.of(*part, 1);
        } else if (key == Symbol("layers")) {
            has_layers = true;
            Vec<ProjectLayer> layers;
            for (size_t j = 1; j < part->size(); ++j) {
                const Array* l = (*part)[j].try_array();
                std::string_view file;
                // (name (dir "path")): a directory of layers.
                const Array* in = l && l->size() >= 2 ? (*l)[1].try_array() : nullptr;
                const bool is_dir =
                    in && in->tag() == Symbol("dir") && in->size() == 2 && text_of((*in)[1], file);
                if (!l || l->size() < 2 || l->tag().empty() || (!is_dir && !text_of((*l)[1], file)))
                    return Unexpected(
                        places.at(l ? *l : *part,
                                  "a layer is (name \"path\" [optional] [disabled] [dev_only] "
                                  "[(when COND)]), or (name (dir \"path\") ...) for a directory "
                                  "of layers"));
                ProjectLayer layer{.name = l->tag(),
                                   .path = joined(dir, file),
                                   .dir = is_dir,
                                   .name_at = places.of(*l, 0),
                                   .path_at = is_dir ? places.of(*in, 1) : places.of(*l, 1)};
                for (size_t k = 2; k < l->size(); ++k) {
                    if ((*l)[k].is(Symbol("optional")))
                        layer.optional = true;
                    else if ((*l)[k].is(Symbol("disabled")))
                        layer.enabled = false;
                    else if ((*l)[k].is(Symbol("dev_only")))
                        layer.dev_only = true;
                    else if (const Array* w = (*l)[k].try_array();
                             w && w->tag() == Symbol("when") && w->size() == 2 &&
                             layer.when.empty())
                        layer.when = Symbol(detail::text_of((*w)[1]));  // checked at the push
                    else
                        return Unexpected(places.at(
                            *l, "layer '" + Str(l->tag().str()) +
                                    "': after the path, only optional, disabled, dev_only and one "
                                    "(when COND)"));
                }
                for (const ProjectLayer& seen : layers)
                    if (seen.name == layer.name)
                        return Unexpected(
                            places.at(*l, "two layers named '" + Str(layer.name.str()) + "'"));
                layers.push_back(layer);
            }
            p.layers =
                List<ProjectLayer>(std::span<const ProjectLayer>(layers.data(), layers.size()));
        } else {
            return Unexpected(
                places.at(*part, "a project has no '" + Str(key.str()) +
                                     "': it holds pack_levels, include_dirs, "
                                     "defines, reference, tool, build, console, step_name, format, "
                                     "scenarios, objectives, views, and layers"));
        }
    }
    if (!has_layers) return Unexpected(places.at(*root, "a project needs (layers ...)"));
    // A contract layer is one of the project's, and one that every build has: a
    // view's shape set is one per build.
    for (const Symbol name : p.views.layers) {
        const ProjectLayer* layer = nullptr;
        for (const ProjectLayer& l : p.layers)
            if (l.name == name) layer = &l;
        if (!layer)
            return Unexpected(places.at(*views_part, "(views (layers ...)) names '" +
                                                         Str(name.str()) +
                                                         "', which (layers ...) does not have"));
        if (layer->dev_only || !layer->when.empty())
            return Unexpected(places.at(
                *views_part, "(views (layers ...)) names '" + Str(name.str()) +
                                 "', which is dev_only or under (when COND): a view's shapes "
                                 "are the same in every build"));
    }
    return p;
}

Symbol find_project(std::string_view path, const LoadSession::Reader& reader) {
    std::string_view dir = path;
    for (;;) {
        const Str candidate = detail::join_path(dir, kProjectFileName);
        if (exists(reader, candidate.view())) return Symbol(candidate.view());
        if (dir.empty()) return {};
        const std::string_view up = detail::parent_path(dir);
        if (up == dir) return {};  // the root ("/", "C:/", "user://") is its own parent
        dir = up;
    }
}

Expected<List<ProjectProblem>> open_project(Config& cfg, const Project& project,
                                            const ProjectOptions& options) {
    if (options.skip.overflowed())
        return Unexpected(make_error(
            ErrorCode::Refused, "more than " +
                                    detail::decimal(static_cast<int64_t>(LayerNames::kCapacity)) +
                                    " layers to leave out (ProjectOptions::skip)"));
    cfg.set_layout(project.format);  // what it writes, laid out as the project's files
    LoadSession& session = cfg.session();
    if (!project.pack_levels.empty()) {
        Vec<Symbol> levels;
        for (const ProjectEntry& e : project.pack_levels) levels.push_back(e.value);
        session.set_pack_levels(std::span<const Symbol>(levels.data(), levels.size()));
    }
    for (const ProjectEntry& e : project.include_dirs) session.add_include_dir(e.value.str());
    for (const ProjectEntry& e : project.defines) session.define(e.value, DefineFrom::Project);
    for (const std::string_view d : options.defines) session.define(Symbol(d), DefineFrom::Host);
    Vec<ProjectProblem> problems;  // allocates only for a problem
    // A stack image: the leading layers it holds come from it, the rest as today.
    size_t from_image = 0;
    if (!cfg.stack_image().empty()) {
        auto taken = cfg.push_image_layers(project.layers.items());
        if (!taken) return Unexpected(std::move(taken).error());
        from_image = *taken;
    }
    for (size_t i = from_image; i < project.layers.size(); ++i) {
        const ProjectLayer& l = project.layers[i];
        if (options.skip.contains(l.name.str())) continue;
        // A development layer never reaches a shipping build, whatever its file says.
        if (l.dev_only && (kShipping || options.skip_dev_only)) continue;
        const LayerOptions o{.optional = l.optional, .enabled = l.enabled, .when = l.when.str()};
        if (auto r = l.dir ? cfg.push_layer_dir(l.name.str(), l.path.str(), o)
                           : cfg.push_layer(l.name.str(), l.path.str(), o);
            !r) {
            if (!options.go_on) return Unexpected(std::move(r).error());
            problems.push_back({l.name, std::move(r).error()});
        }
    }
    return List<ProjectProblem>(std::span<const ProjectProblem>(problems.data(), problems.size()));
}

}  // namespace baker
