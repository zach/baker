// Writes the binary cache of a DTA file, for a build step, or
// the C++ views of a project's shapes:
//
//   baker_bake [-D NAME]... [-I DIR]... [--drop-metadata] INPUT.dta OUTPUT.datb
//   baker_bake --views OUT_DIR --project baker-project.dta [--depfile FILE] [--stamp FILE]
//              [-D NAME]...
//
// The cache records the files the load read and the defines given here, so a game's
// read_cached() or cache_fresh() with the same defines finds it fresh. --drop-metadata
// leaves out every (@...) child, for a shipped build that reads no metadata.
//
// --views reads the project's contract layers ((views (layers ...)) in the project file),
// and writes OUT_DIR/<shape>.hpp, <shape>_read.hpp, views.hpp and tables.hpp for each view,
// a file only when its text changed; --depfile writes a Make-style dependency line naming every
// file read, and --stamp touches a file when done, for a build's rule (cmake/BakerViews.cmake).
// Exits 1 on any error, naming it.

#include <baker/cache.hpp>
#include <baker/parse.hpp>
#include <baker/project.hpp>
#include <baker/std_interop.hpp>
#if BAKER_BAKE_VIEWS
#include <baker/shape.hpp>
#endif

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int fail(std::string_view message) {
    std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
    return 1;
}

// Writes `text` to `path` unless the file already holds it: an unchanged header keeps its
// time stamp, so nothing that includes it rebuilds.
bool write_if_changed(const std::string& path, const std::string& text) {
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            const std::string old((std::istreambuf_iterator<char>(in)), {});
            if (old == text) return true;
        }
    }
    std::ofstream out(path, std::ios::binary);
    out << text;
    return static_cast<bool>(out);
}

int bake_cache(baker::LoadSession& session, const std::vector<std::string>& paths,
               bool drop_metadata) {
    if (paths.size() != 2) {
        std::fprintf(stderr,
                     "usage: baker_bake [-D NAME]... [-I DIR]... [--drop-metadata] INPUT.dta "
                     "OUTPUT.datb\n       baker_bake --views OUT_DIR --project FILE [--depfile "
                     "FILE] [--stamp FILE]\n");
        return 2;
    }
    session.record_for_cache = true;
    auto root = baker::read_file(paths[0], session);
    if (!root) return fail(baker::to_string(root.error()));
    session.drop_metadata = drop_metadata;  // the tree's metadata and the shapes' notes
    if (auto r = baker::write_cache(**root, paths[1], &session); !r)
        return fail(baker::to_string(r.error()));
    return 0;
}

#if BAKER_BAKE_VIEWS
int bake_views(baker::LoadSession& session, const std::string& out_dir,
               const std::string& project_path, const std::string& depfile,
               const std::string& stamp) {
    namespace fs = std::filesystem;
    // Every file the reads touch, for the depfile: the session's reader, wrapped.
    std::vector<std::string> read;
    const auto reading = [&](std::string_view path, baker::TextSink out) {
        std::ifstream in(std::string(path), std::ios::binary);
        if (!in) return false;
        read.emplace_back(path);
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        out(text);
        return true;
    };
    auto project = baker::read_project(project_path, reading);
    if (!project) return fail(baker::to_string(project.error()));
    if (project->views.layers.empty())
        return fail("baker_bake --views: " + project_path +
                    " has no (views (layers ...)): nothing to generate");
    session.set_reader(reading);
    {
        std::vector<baker::Symbol> levels;
        for (const baker::ProjectEntry& e : project->pack_levels) levels.push_back(e.value);
        if (!levels.empty()) session.set_pack_levels(levels);
    }
    for (const baker::ProjectEntry& e : project->include_dirs)
        session.add_include_dir(e.value.str());
    for (const baker::ProjectEntry& e : project->defines)
        session.define(e.value, baker::DefineFrom::Project);
    // The contract layers' reads, bottom to top; a directory layer is each of its .dta files.
    std::vector<baker::Symbol> roots;
    std::vector<baker::Ref<baker::Array>> trees;  // kept: the declarations live in them
    for (const baker::Symbol name : project->views.layers) {
        const baker::ProjectLayer* layer = nullptr;
        for (const baker::ProjectLayer& l : project->layers)
            if (l.name == name) layer = &l;
        if (!layer) return fail("baker_bake --views: no layer named " + std::string(name.str()));
        std::vector<std::string> files;
        if (layer->dir) {
            std::error_code ec;
            for (const fs::directory_entry& e : fs::directory_iterator(layer->path.str(), ec))
                if (e.is_regular_file() && e.path().extension() == ".dta")
                    files.push_back(e.path().generic_string());
            std::sort(files.begin(), files.end());
        } else {
            files.emplace_back(layer->path.str());
        }
        for (const std::string& file : files) {
            auto tree = baker::read_file(file, session);
            if (!tree) {
                if (layer->optional && !fs::exists(file)) continue;
                return fail(baker::to_string(tree.error()));
            }
            // The root as the parser recorded it (its normalized path), which the
            // declarations name: not the path as the project joined it.
            roots.push_back((*tree)->file());
            trees.push_back(std::move(*tree));
        }
    }
    baker::shape::Report report;
    const baker::shape::Set set = baker::shape::contract_set(session, roots, report);
    const auto listed = project->views.shapes;
    const auto views = baker::shape::view_shapes(set, listed.items(), report);
    if (!report.errors.empty()) {
        for (const baker::Error& e : report.errors)
            std::fprintf(stderr, "%s\n", baker::to_string(e).data());
        return 1;
    }
    for (const baker::Error& w : report.warnings)
        std::fprintf(stderr, "%s\n", baker::to_string(w).data());
    const baker::shape::ViewOptions options{.ns = project->views.ns.empty()
                                                      ? std::string_view("baker_views")
                                                      : project->views.ns.str()};
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    const auto write = [&](const std::string& file, const std::string& text) {
        const std::string path = out_dir + "/" + file;
        if (!write_if_changed(path, text)) return fail("baker_bake --views: cannot write " + path);
        return 0;
    };
    for (const baker::Symbol v : views) {
        std::string header, reader;
        baker::shape::write_view_header(set, v, options, baker::string_out(header));
        baker::shape::write_view_reader(set, v, options, baker::string_out(reader));
        if (int r = write(std::string(v.str()) + ".hpp", header)) return r;
        if (int r = write(std::string(v.str()) + "_read.hpp", reader)) return r;
    }
    std::string umbrella, tables;
    baker::shape::write_views_umbrella(set, views.items(), options, baker::string_out(umbrella));
    if (int r = write("views.hpp", umbrella)) return r;
    baker::shape::write_tables_header(set, views.items(), options, baker::string_out(tables));
    if (int r = write("tables.hpp", tables)) return r;
    if (!depfile.empty()) {
        // Make syntax: the stamp (or the umbrella) depends on every file read; a space in
        // a path is escaped.
        std::string line = (stamp.empty() ? out_dir + "/views.hpp" : stamp) + ":";
        for (const std::string& p : read) {
            line += ' ';
            for (const char c : p) {
                if (c == ' ') line += '\\';
                line += c;
            }
        }
        line += '\n';
        std::ofstream out(depfile, std::ios::binary);
        if (!(out << line)) return fail("baker_bake --views: cannot write " + depfile);
    }
    if (!stamp.empty()) {
        std::ofstream out(stamp, std::ios::binary | std::ios::trunc);
        if (!out) return fail("baker_bake --views: cannot write " + stamp);
    }
    return 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    baker::LoadSession session;
    std::vector<std::string> paths;
    std::string views_dir, project_path, depfile, stamp;
    bool drop_metadata = false;
    bool views = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--drop-metadata") {
            drop_metadata = true;
        } else if (arg == "--views" && has_value) {
            views = true;
            views_dir = argv[++i];
        } else if (arg == "--project" && has_value) {
            project_path = argv[++i];
        } else if (arg == "--depfile" && has_value) {
            depfile = argv[++i];
        } else if (arg == "--stamp" && has_value) {
            stamp = argv[++i];
        } else if ((arg == "-D" || arg == "-I") && has_value) {
            if (arg == "-D")
                session.define(argv[++i]);
            else
                session.add_include_dir(argv[++i]);
        } else {
            paths.push_back(arg);
        }
    }
    if (views) {
#if BAKER_BAKE_VIEWS
        if (project_path.empty() || !paths.empty())
            return fail(
                "usage: baker_bake --views OUT_DIR --project FILE [--depfile FILE] "
                "[--stamp FILE] [-D NAME]...");
        return bake_views(session, views_dir, project_path, depfile, stamp);
#else
        return fail("baker_bake: this build has no baker::shape, so it generates no views");
#endif
    }
    return bake_cache(session, paths, drop_metadata);
}
