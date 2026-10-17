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
    for (size_t i = 1; i < root->size(); ++i) {
        const Array* part = (*root)[i].try_array();
        if (!part || part->empty())
            return Unexpected(places.at(*root, "(project ...) holds (key ...) forms"));
        const Symbol key = part->tag();
        if (key == Symbol("pack_levels") || key == Symbol("defines")) {
            auto list = entries(*part, places, dir, false);
            if (!list) return Unexpected(std::move(list).error());
            (key == Symbol("defines") ? p.defines : p.pack_levels) = std::move(*list);
        } else if (key == Symbol("include_dirs")) {
            auto list = entries(*part, places, dir, true);
            if (!list) return Unexpected(std::move(list).error());
            p.include_dirs = std::move(*list);
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
                if (!l || l->size() < 2 || l->tag().empty() || !text_of((*l)[1], file))
                    return Unexpected(places.at(
                        l ? *l : *part, "a layer is (name \"path\" [optional] [disabled])"));
                ProjectLayer layer{.name = l->tag(),
                                   .path = joined(dir, file),
                                   .name_at = places.of(*l, 0),
                                   .path_at = places.of(*l, 1)};
                for (size_t k = 2; k < l->size(); ++k) {
                    if ((*l)[k].is(Symbol("optional")))
                        layer.optional = true;
                    else if ((*l)[k].is(Symbol("disabled")))
                        layer.enabled = false;
                    else
                        return Unexpected(
                            places.at(*l, "layer '" + Str(l->tag().str()) +
                                              "': after the path, only optional and disabled"));
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
            return Unexpected(places.at(*part, "a project has no '" + Str(key.str()) +
                                                   "': it holds pack_levels, include_dirs, "
                                                   "defines, reference, and layers"));
        }
    }
    if (!has_layers) return Unexpected(places.at(*root, "a project needs (layers ...)"));
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
    LoadSession& session = cfg.session();
    if (!project.pack_levels.empty()) {
        Vec<Symbol> levels;
        for (const ProjectEntry& e : project.pack_levels) levels.push_back(e.value);
        session.set_pack_levels(std::span<const Symbol>(levels.data(), levels.size()));
    }
    for (const ProjectEntry& e : project.include_dirs) session.add_include_dir(e.value.str());
    for (const ProjectEntry& e : project.defines) session.define(e.value);
    Vec<ProjectProblem> problems;  // allocates only for a problem
    for (const ProjectLayer& l : project.layers) {
        bool skipped = false;
        for (const std::string_view s : options.skip) skipped = skipped || l.name.str() == s;
        if (skipped) continue;
        if (auto r = cfg.push_layer(l.name.str(), l.path.str(),
                                    {.optional = l.optional, .enabled = l.enabled});
            !r) {
            if (!options.go_on) return Unexpected(std::move(r).error());
            problems.push_back({l.name, std::move(r).error()});
        }
    }
    return List<ProjectProblem>(std::span<const ProjectProblem>(problems.data(), problems.size()));
}

}  // namespace baker
