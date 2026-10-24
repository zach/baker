// Defines a host supplies, who made each define, and changing them on a live stack.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/project.hpp>
#include <baker/std_interop.hpp>

#include <string>
#include <vector>
#include "ok.hpp"
#include "temp_dir.hpp"

using namespace baker;
using baker::test::ok;
using baker::test::TempDir;

namespace {

std::vector<std::string> made(const LoadSession& s) {
    std::vector<std::string> out;
    for (const ActiveDefine& d : s.active_defines()) {
        std::string line = std::string(d.name.str()) + " " + std::string(define_from_name(d.from));
        if (d.from == DefineFrom::File)
            line += " " + std::string(d.file.str()) + ":" + std::to_string(d.line);
        out.push_back(line);
    }
    return out;
}

}  // namespace

TEST_CASE("a host's defines are seen by #ifdef and listed as the host's") {
    TempDir dir;
    dir.write("base.dta",
              "#define TUNED\n#ifdef server\n(mode (tick 30))\n#else\n(mode (tick 60))\n#endif\n");
    const Project p = ok(read_project(
        dir.write("baker-project.dta", "(project (defines PC) (layers (base \"base.dta\")))\n")));
    Config cfg({.tweaks = nullptr});
    static constexpr std::string_view kDefines[] = {"server", "steam"};
    REQUIRE(ok(open_project(cfg, p, {.defines = kDefines})).empty());
    CHECK(cfg.get<int>("mode.tick") == 30);
    CHECK(made(cfg.read_session()) ==
          std::vector<std::string>{"PC project", "server host", "steam host",
                                   "TUNED file " + dir.path("base.dta") + ":1"});
}

TEST_CASE("set_defines reads again exactly the layers that tested a changed define") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    cfg.session().define(Symbol("server"));
    REQUIRE(cfg.push_layer("plain", dir.write("plain.dta", "(rules (rounds 3))\n")));
    REQUIRE(cfg.push_layer("modes", dir.write("modes.dta",
                                              "#ifdef server\n(mode (tick 30))\n#else\n"
                                              "(mode (tick 60))\n#endif\n")));
    REQUIRE(cfg.push_layer("top", dir.write("top.dta", "(rules (time 90))\n")));
    CHECK(cfg.get<int>("mode.tick") == 30);
    const Ref<Array> plain = cfg.layer_root("plain"), modes = cfg.layer_root("modes"),
                     top = cfg.layer_root("top");

    static constexpr std::string_view kDemo[] = {"demo"};
    REQUIRE(cfg.set_defines(kDemo));
    CHECK(cfg.get<int>("mode.tick") == 60);
    CHECK(cfg.layer_root("plain").get() == plain.get());  // kept: tested nothing
    CHECK(cfg.layer_root("modes").get() != modes.get());  // read again
    CHECK(cfg.layer_root("top").get() == top.get());
    CHECK(made(cfg.read_session()) == std::vector<std::string>{"demo host"});
    if (const auto r = cfg.last_reload()) CHECK(r->trigger == Symbol("defines"));

    // The same set changes nothing; another brings the first back.
    const Ref<Array> root = cfg.root();
    REQUIRE(cfg.set_defines(kDemo));
    CHECK(cfg.root().get() == root.get());
    static constexpr std::string_view kServer[] = {"server"};
    REQUIRE(cfg.set_defines(kServer));
    CHECK(cfg.get<int>("mode.tick") == 30);
}

TEST_CASE("a file's own define of a name stands when the host drops it") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    cfg.session().define(Symbol("server"));
    REQUIRE(cfg.push_layer("base", dir.write("base.dta", "#define server\n(a 1)\n")));
    REQUIRE(cfg.push_layer("plain", dir.write("plain.dta", "(b 2)\n")));
    REQUIRE(cfg.push_layer(
        "modes", dir.write("modes.dta", "#ifdef server\n(mode 30)\n#else\n(mode 60)\n#endif\n")));
    static constexpr std::string_view kNone[] = {"other"};
    REQUIRE(cfg.set_defines(kNone));
    CHECK(cfg.get<int>("mode") == 30);  // base.dta still defines it, below
}

TEST_CASE("a layer that does not read under the new defines leaves everything as it was") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    cfg.session().define(Symbol("server"));
    REQUIRE(cfg.push_layer(
        "modes", dir.write("modes.dta", "#ifdef server\n(mode 30)\n#else\n(mode 60\n#endif\n")));
    static constexpr std::string_view kNone[] = {"other"};
    CHECK_FALSE(cfg.set_defines(kNone));
    CHECK(cfg.get<int>("mode") == 30);
    CHECK(made(cfg.session()) == std::vector<std::string>{"server host"});
}
