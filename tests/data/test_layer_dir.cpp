// A directory of layers (Config::push_layer_dir): mods, DLC and plugin packs, each a layer
// found by its file, in order.dta's order, listed again at every reload.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/project.hpp>
#include <baker/std_interop.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "dev_only.hpp"
#include "temp_dir.hpp"

using namespace baker;
using baker::test::TempDir;

namespace {

std::vector<std::string> names(const Config& cfg) {
    std::vector<std::string> out;
    for (const Symbol& s : cfg.layers()) out.emplace_back(s.str());
    return out;
}

Ref<Array> tree(const char* text) {
    auto t = read_string(text);
    REQUIRE(t);
    return *t;
}

// A Config that keeps its warnings.
struct Stack {
    std::vector<std::string> warnings;  // before the Config, whose handler appends to it
    Config cfg;
    Stack() {
        cfg.set_warning_handler([this](const Error& e) { warnings.emplace_back(to_string(e)); });
    }
};

}  // namespace

TEST_CASE("each file and folder of a directory is a layer, in order.dta's order") {
    TempDir dir;
    dir.write("base.dta", "(speed 1)\n");
    dir.write("mods/a.dta", "(speed 2)\n(from a)\n");
    dir.write("mods/b.dta", "(speed 3)\n(from b)\n");
    dir.write("mods/zed/layer.dta", "#include more.dta\n(from zed)\n");
    dir.write("mods/zed/more.dta", "(lives 9)\n");
    dir.write("mods/notes.txt", "not a layer\n");
    dir.write("mods/.hidden/layer.dta", "(from hidden)\n");
    dir.write("mods/order.dta", "(order zed missing b)\n");
    dir.write("top.dta", "(top 1)\n");
    Stack s;
    REQUIRE(s.cfg.push_layer("base", dir.path("base.dta")));
    REQUIRE(s.cfg.push_layer_dir("mods", dir.path("mods")));
    REQUIRE(s.cfg.push_layer("top", dir.path("top.dta")));
    // Listed first, in its order; the rest after, in name order.
    CHECK(names(s.cfg) ==
          std::vector<std::string>{"base", "mods", "zed", "b", "a", "top", "scratch"});
    CHECK(s.cfg.get<Symbol>("from") == Symbol("a"));
    CHECK(s.cfg.get<int>("speed") == 2);
    CHECK(s.cfg.get<int>("lives") == 9);  // a folder's layer.dta includes beside itself
    CHECK(s.cfg.source("lives")->layer == Symbol("zed"));
    REQUIRE(s.warnings.size() == 1);
    CHECK(s.warnings[0].find("order.dta lists 'missing', which") != std::string::npos);
    CHECK(s.warnings[0].find("order.dta:1") != std::string::npos);
    // stack_files() names the directory, its order.dta, and each layer's files.
    std::vector<std::string> files;
    for (const StackFile& f : s.cfg.stack_files())
        if (f.layer == Symbol("mods") || f.layer == Symbol("zed"))
            files.push_back(std::string(f.layer.str()) + " " +
                            std::filesystem::path(f.path.str()).filename().generic_string() +
                            (f.directory ? " dir" : "") + (f.present ? "" : " absent"));
    CHECK(files == std::vector<std::string>{"mods mods dir", "mods order.dta", "zed layer.dta",
                                            "zed more.dta"});
    // Without an order.dta, name order.
    TempDir plain;
    plain.write("m/b.dta", "(x 1)\n");
    plain.write("m/a.dta", "(x 2)\n");
    Config cfg;
    REQUIRE(cfg.push_layer_dir("m", plain.path("m")));
    CHECK(names(cfg) == std::vector<std::string>{"m", "a", "b", "scratch"});
}

TEST_CASE("a layer that does not read stands empty, located, and the rest stand") {
    TempDir dir;
    dir.write("mods/a.dta", "(a 1)\n");
    dir.write("mods/bad.dta", "(b 1\n");
    dir.write("mods/c.dta", "(c 1)\n");
    Stack s;
    REQUIRE(s.cfg.push_layer_dir("mods", dir.path("mods")));
    CHECK(names(s.cfg) == std::vector<std::string>{"mods", "a", "bad", "c", "scratch"});
    CHECK(s.cfg.get<int>("a") == 1);
    CHECK(s.cfg.get<int>("c") == 1);
    REQUIRE(s.warnings.size() == 1);
    CHECK(s.warnings[0].find("bad.dta:") != std::string::npos);
    // A name already taken is left out, with a warning naming its file.
    dir.write("other/mods.dta", "(m 1)\n");
    dir.write("other/scratch.dta", "(m 1)\n");
    dir.write("other/fine.dta", "(m 1)\n");
    s.warnings.clear();
    REQUIRE(s.cfg.push_layer_dir("other", dir.path("other")));
    CHECK(names(s.cfg) ==
          std::vector<std::string>{"mods", "a", "bad", "c", "other", "fine", "scratch"});
    CHECK(s.warnings.size() == 2);
    // An order.dta that does not parse, or holds something else, refuses the push.
    dir.write("third/order.dta", "(order a\n");
    CHECK_FALSE(s.cfg.push_layer_dir("third", dir.path("third")));
    dir.write("third/order.dta", "(sequence a)\n");
    const auto r = s.cfg.push_layer_dir("third", dir.path("third"));
    REQUIRE_FALSE(r);
    CHECK(to_string(r.error()).find("order.dta holds (order NAME ...)") != std::string::npos);
    // A directory that is not there: an error, or with `optional` an empty one.
    CHECK_FALSE(s.cfg.push_layer_dir("none", dir.path("none")));
    REQUIRE(s.cfg.push_layer_dir("none", dir.path("none"), {.optional = true}));
    CHECK(names(s.cfg) ==
          std::vector<std::string>{"mods", "a", "bad", "c", "other", "fine", "none", "scratch"});
}

TEST_CASE("a directory's layers are placed, removed and popped together") {
    TempDir dir;
    dir.write("mods/a.dta", "(a 1)\n");
    dir.write("mods/b.dta", "(b 1)\n");
    dir.write("x.dta", "(x 1)\n");
    Config cfg;
    REQUIRE(cfg.push_layer_dir("mods", dir.path("mods")));
    REQUIRE(cfg.push_layer("under", tree("(u 1)"), {.below = "mods"}));
    REQUIRE(cfg.push_layer("over", tree("(o 1)"), {.above = "mods"}));
    CHECK(names(cfg) == std::vector<std::string>{"under", "mods", "a", "b", "over", "scratch"});
    CHECK_FALSE(cfg.push_layer("between", tree("(o 1)"), {.above = "a"}));
    CHECK_FALSE(cfg.remove_layer("a"));
    REQUIRE(cfg.remove_layer("over"));
    REQUIRE(cfg.pop_layer());  // the directory, with its layers
    CHECK(names(cfg) == std::vector<std::string>{"under", "scratch"});
    REQUIRE(cfg.push_layer_dir("mods", dir.path("mods")));
    REQUIRE(cfg.remove_layer("mods"));
    CHECK(names(cfg) == std::vector<std::string>{"under", "scratch"});
    const LayerPolicy policy;
    CHECK_FALSE(cfg.push_layer_dir("mods", dir.path("mods"), {.policy = &policy}));
}

BAKER_DEV_TEST_CASE("every reload lists the directory again") {
    TempDir dir;
    dir.write("mods/a.dta", "(a 1)\n");
    dir.write("mods/b.dta", "(b 1)\n");
    dir.write("top.dta", "(top 1)\n");
    Stack s;
    REQUIRE(s.cfg.push_layer_dir("mods", dir.path("mods")));
    REQUIRE(s.cfg.push_layer("top", dir.path("top.dta")));
    const uint64_t epoch = s.cfg.stack_files_epoch();
    CHECK_FALSE(*s.cfg.reload());  // nothing changed
    // A file added becomes a layer; the files list moves.
    dir.write("mods/c.dta", "(c 1)\n");
    REQUIRE(*s.cfg.reload());
    CHECK(names(s.cfg) == std::vector<std::string>{"mods", "a", "b", "c", "top", "scratch"});
    CHECK(s.cfg.get<int>("c") == 1);
    CHECK(s.cfg.stack_files_epoch() != epoch);
    CHECK(s.cfg.last_reload()->layer == Symbol("mods"));
    // One removed goes; order.dta reorders them.
    std::filesystem::remove(dir.path("mods/b.dta"));
    REQUIRE(*s.cfg.reload());
    CHECK(names(s.cfg) == std::vector<std::string>{"mods", "a", "c", "top", "scratch"});
    CHECK_FALSE(s.cfg.get<int>("b"));
    dir.write("mods/order.dta", "(order c)\n");
    REQUIRE(*s.cfg.reload());
    CHECK(names(s.cfg) == std::vector<std::string>{"mods", "c", "a", "top", "scratch"});
    // A folder that gains a layer.dta becomes a layer.
    dir.write("mods/pack/readme.md", "later\n");
    REQUIRE(*s.cfg.reload());  // the listing changed, though no layer did
    CHECK(names(s.cfg).size() == 5);
    dir.write("mods/pack/layer.dta", "(packed 1)\n");
    REQUIRE(*s.cfg.reload());
    CHECK(names(s.cfg) == std::vector<std::string>{"mods", "c", "a", "pack", "top", "scratch"});
    // An order.dta broken by a save fails the reload and changes nothing.
    dir.write("mods/order.dta", "(order c\n");
    CHECK_FALSE(s.cfg.reload());
    CHECK(names(s.cfg) == std::vector<std::string>{"mods", "c", "a", "pack", "top", "scratch"});
    // A layer that does not read is read again once it is fixed.
    dir.write("mods/order.dta", "(order c)\n");
    dir.write("mods/a.dta", "(a 2\n");
    s.warnings.clear();
    REQUIRE(*s.cfg.reload());
    CHECK(s.warnings.size() == 1);
    CHECK_FALSE(s.cfg.get<int>("a"));
    CHECK(s.cfg.get<int>("c") == 1);
    dir.write("mods/a.dta", "(a 3)\n");
    REQUIRE(*s.cfg.reload());
    CHECK(s.cfg.get<int>("a") == 3);
}

BAKER_DEV_TEST_CASE("a layer turned off stays off across a rescan; a directory turns them all") {
    TempDir dir;
    dir.write("mods/a.dta", "(a 1)\n");
    dir.write("mods/b.dta", "(b 1)\n");
    Config cfg;
    REQUIRE(cfg.push_layer_dir("mods", dir.path("mods")));
    REQUIRE(cfg.set_layer_enabled("a", false));
    dir.write("mods/c.dta", "(c 1)\n");
    REQUIRE(*cfg.reload());
    CHECK_FALSE(cfg.layer_enabled("a"));
    CHECK(cfg.layer_enabled("c"));
    CHECK_FALSE(cfg.get<int>("a"));
    // Off as a whole: every layer, and one found later too.
    REQUIRE(cfg.set_layer_enabled("mods", false));
    CHECK_FALSE(cfg.get<int>("b"));
    dir.write("mods/d.dta", "(d 1)\n");
    REQUIRE(*cfg.reload());
    CHECK_FALSE(cfg.layer_enabled("d"));
    REQUIRE(cfg.set_layer_enabled("mods", true));
    CHECK(cfg.get<int>("a") == 1);
    CHECK(cfg.get<int>("d") == 1);
    // An optional directory that appears is listed at the next reload.
    REQUIRE(cfg.push_layer_dir("later", dir.path("later"), {.optional = true}));
    dir.write("later/x.dta", "(x 1)\n");
    REQUIRE(*cfg.reload());
    CHECK(cfg.get<int>("x") == 1);
}

TEST_CASE("a directory is listed and read through the host's lister and reader") {
    std::map<std::string, std::string> files = {
        {"pak://mods/a.dta", "(a 1)\n"},
        {"pak://mods/order.dta", "(order pack a)\n"},
        {"pak://mods/pack/layer.dta", "#include extra.dta\n(packed 1)\n"},
        {"pak://mods/pack/extra.dta", "(extra 1)\n"},
    };
    Config cfg;
    cfg.session().set_reader([&](std::string_view path, TextSink out) {
        const auto it = files.find(std::string(path));
        if (it == files.end()) return false;
        out(it->second);
        return true;
    });
    int listed = 0;
    cfg.session().set_lister([&](std::string_view dir, Callback<void(std::string_view)> entry) {
        ++listed;
        const std::string prefix = std::string(dir) + "/";
        std::vector<std::string> seen;
        for (const auto& [path, text] : files) {
            if (path.rfind(prefix, 0) != 0) continue;
            const std::string rest = path.substr(prefix.size());
            const std::string first = prefix + rest.substr(0, rest.find('/'));
            if (std::find(seen.begin(), seen.end(), first) == seen.end()) {
                seen.push_back(first);
                entry(first);
            }
        }
        return !seen.empty();
    });
    REQUIRE(cfg.push_layer_dir("mods", "pak://mods"));
    CHECK(names(cfg) == std::vector<std::string>{"mods", "pack", "a", "scratch"});
    CHECK(cfg.get<int>("extra") == 1);
    CHECK(listed == 1);
    if constexpr (!kShipping) {
        files["pak://mods/b.dta"] = "(b 1)\n";
        REQUIRE(*cfg.reload());
        CHECK(cfg.get<int>("b") == 1);
    }
}

TEST_CASE("a project names a directory of layers; dev_only leaves it out of shipping") {
    TempDir dir;
    dir.write("data/default.dta", "(speed 1)\n");
    dir.write("mods/fast.dta", "(speed 5)\n");
    dir.write("experiments/tuning.dta", "(speed 9)\n");
    dir.write("baker-project.dta",
              "(project (layers\n"
              "   (default \"data/default.dta\")\n"
              "   (mods (dir \"mods\"))\n"
              "   (agent (dir \"experiments\") optional dev_only)))\n");
    auto project = read_project(dir.path("baker-project.dta"));
    REQUIRE(project);
    REQUIRE(project->layers.size() == 3);
    CHECK(project->layers[1].dir);
    CHECK(project->layers[1].path_at.line == 3);
    CHECK(project->layers[2].dir);
    CHECK(project->layers[2].dev_only);
    Config cfg;
    REQUIRE(open_project(cfg, *project));
    if constexpr (kShipping) {
        CHECK(names(cfg) == std::vector<std::string>{"default", "mods", "fast", "scratch"});
        CHECK(cfg.get<int>("speed") == 5);
    } else {
        CHECK(names(cfg) ==
              std::vector<std::string>{"default", "mods", "fast", "agent", "tuning", "scratch"});
        CHECK(cfg.get<int>("speed") == 9);
    }
    Config skipped;
    REQUIRE(open_project(skipped, *project, {.skip_dev_only = true}));
    CHECK(skipped.get<int>("speed") == 5);
    // A malformed entry is located.
    dir.write("bad/baker-project.dta", "(project (layers (mods (dir))))\n");
    const auto bad = read_project(dir.path("bad/baker-project.dta"));
    REQUIRE_FALSE(bad);
    CHECK(to_string(bad.error()).find("(name (dir \"path\") ...)") != std::string::npos);
}
