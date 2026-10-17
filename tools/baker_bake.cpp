// Writes the binary cache of a DTA file, for a build step.
//
//   baker_bake [-D NAME]... [-I DIR]... INPUT.dta OUTPUT.datb
//
// The cache records the files the load read and the defines given here, so a game's
// read_cached() or cache_fresh() with the same defines finds it fresh. Exits 1 on any
// error, naming it.

#include <baker/cache.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    baker::LoadSession session;
    session.record_for_cache = true;
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "-D" || arg == "-I") && i + 1 < argc) {
            if (arg == "-D")
                session.define(argv[++i]);
            else
                session.add_include_dir(argv[++i]);
        } else {
            paths.push_back(arg);
        }
    }
    if (paths.size() != 2) {
        std::fprintf(stderr, "usage: baker_bake [-D NAME]... [-I DIR]... INPUT.dta OUTPUT.datb\n");
        return 2;
    }
    auto root = baker::read_file(paths[0], session);
    if (!root) {
        std::fprintf(stderr, "%s\n", baker::to_string(root.error()).data());
        return 1;
    }
    if (auto r = baker::write_cache(**root, paths[1], &session); !r) {
        std::fprintf(stderr, "%s\n", baker::to_string(r.error()).data());
        return 1;
    }
    return 0;
}
