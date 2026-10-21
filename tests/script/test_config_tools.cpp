// bind_config_tools: the Config's tooling as script functions.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/script.hpp>
#include <baker/std_interop.hpp>
#include <baker/std_script.hpp>

#include "temp_dir.hpp"

#include <string>
#include "ok.hpp"

using namespace baker;
using baker::test::ok;
using baker::test::TempDir;

namespace {

std::string eval(Context& ctx, std::string_view text) {
    return to_string(ok(ctx.eval(text, "test")));
}

const char* const kDefault =
    "(box\n"
    "   (width 2)\n"
    "   (height 1))\n"
    "(phase 10 20 30)\n";

struct Fixture {
    TempDir dir;
    TweakRegistry reg;
    Config cfg{{.tweaks = &reg}};
    Context ctx;
    explicit Fixture(ConfigToolOptions options = {}) {
        REQUIRE(cfg.push_layer("default", dir.write("default.dta", kDefault)));
        REQUIRE(cfg.push_layer("local", dir.write("local.dta", "(box (width 3))\n")));
        REQUIRE(bind_config_tools(ctx, cfg, options));
    }
    std::string file(const char* name) const { return dir.path(name); }
};

}  // namespace

TEST_CASE("the reading tools: get, source, explain, layers, effect, diff") {
    Fixture f;
    CHECK(eval(f.ctx, "{config.get box.width}") == "3");
    CHECK(eval(f.ctx, "{config.get \"box.height\"}") == "1");  // a path as a string, too
    CHECK(eval(f.ctx, "{config.get phase}") == "(10 20 30)");
    CHECK(eval(f.ctx, "{config.source box.width}") ==
          "(source (layer local) (file \"" + f.file("local.dta") + "\") (line 1))");
    CHECK(eval(f.ctx, "{config.explain box.width}") ==
          "(((layer default) (value 2) (standing shadowed) (file \"" + f.file("default.dta") +
              "\") (line 2)) ((layer local) (value 3) (standing wins) (file \"" +
              f.file("local.dta") + "\") (line 1)))");
    CHECK(eval(f.ctx, "{config.layers}") ==
          "(((layer default) (enabled 1)) ((layer local) (enabled 1)) ((layer scratch) (enabled "
          "1)))");
    CHECK(eval(f.ctx, "{config.effect local}") ==
          "(((path box.width) (value 3) (below 2) (file \"" + f.file("local.dta") +
              "\") (line 1) (shadowed 0)))");
    CHECK(eval(f.ctx, "{config.diff default local}").find("((path box.width) (a 2) (b 3))") !=
          std::string::npos);

    // Failures are the Config's, located.
    CHECK_FALSE(f.ctx.eval("{config.get box.depth}", "test"));
    CHECK_FALSE(f.ctx.eval("{config.effect nosuch}", "test"));
}

TEST_CASE("a tool reads the results as JSON through eval_request") {
    Fixture f;
    std::string reply;
    eval_request(f.ctx, "(1 {config.explain box.width})", string_out(reply));
    CHECK(reply.find(R"j("value":[{"layer":"default","value":2,"standing":"shadowed",)j") !=
          std::string::npos);
    CHECK(reply.find(R"j({"layer":"local","value":3,"standing":"wins",)j") != std::string::npos);
}

TEST_CASE("check and the reference know the tools") {
    Fixture f;
    const auto tree = ok(read_string("(on_hover {config.get})\n", "tool.dta"));
    const CheckReport report = check(*tree, f.ctx);
    REQUIRE(report.errors.size() == 1);
    CHECK(report.errors[0].line() == 1);
    const std::string ref = reference(f.ctx);
    CHECK(ref.find("### `config.explain`") != std::string::npos);
    CHECK(ref.find("Every contributor to a path's value") != std::string::npos);
}

#if !BAKER_SHIPPING
TEST_CASE("the live tools: set, unsaved, journal, toggle, snapshot, restore, reload") {
    Fixture f;
    CHECK(eval(f.ctx, "{config.set box.width 4.5}") == "4.5");
    CHECK(eval(f.ctx, "{config.set phase 1 2}") == "(1 2)");
    CHECK(f.cfg.get<float>("box.width") == 4.5f);
    CHECK(eval(f.ctx, "{config.unsaved}") == "(box/width phase)");
    const std::string journal = eval(f.ctx, "{config.journal}");
    CHECK(journal.find("(path box/width)") != std::string::npos);
    CHECK(journal.find("(before 3) (after 4.5) (fate unsaved) (layer local)") != std::string::npos);
    CHECK(journal.find("(before (10 20 30)) (after (1 2)) (fate unsaved) (layer default)") !=
          std::string::npos);

    CHECK(eval(f.ctx, "{config.snapshot tuned}") == "tuned");
    CHECK(eval(f.ctx, "{config.toggle local}") == "0");
    CHECK(eval(f.ctx, "{config.toggle scratch}") == "0");
    CHECK(f.cfg.get<int>("box.width") == 2);
    CHECK(eval(f.ctx, "{config.restore tuned}") == "tuned");
    CHECK(f.cfg.get<float>("box.width") == 4.5f);
    CHECK_FALSE(f.ctx.eval("{config.restore nosuch}", "test"));

    CHECK(eval(f.ctx, "{config.reload}") == "0");
    f.dir.write("default.dta", "(box (width 2) (height 7))\n(phase 10 20 30)\n");
    CHECK(eval(f.ctx, "{config.reload}") == "1");
    CHECK(f.cfg.get<int>("box.height") == 7);
    CHECK(eval(f.ctx, "{config.loop}").rfind("((reloads 2) (unchanged 1) (failures 0)", 0) == 0);
}

TEST_CASE("config.clear drops one path's live edit, and says whether there was one") {
    Fixture f;
    REQUIRE(f.ctx.eval("{config.set box.width 4.5}", "test"));
    REQUIRE(f.ctx.eval("{config.set box.height 9}", "test"));
    CHECK(eval(f.ctx, "{config.clear box.width}") == "1");
    CHECK(f.cfg.get<int>("box.width") == 3);   // the files' value: local's
    CHECK(f.cfg.get<int>("box.height") == 9);  // the other edit stays
    CHECK(eval(f.ctx, "{config.unsaved}") == "(box/height)");
    CHECK(eval(f.ctx, "{config.journal}").find("(after 4.5) (fate reverted)") != std::string::npos);
    // Nothing to drop: no edit there, or no value at all. Neither changes anything.
    const uint64_t epoch = f.cfg.epoch();
    CHECK(eval(f.ctx, "{config.clear box.width}") == "0");
    CHECK(eval(f.ctx, "{config.clear no.such.path}") == "0");
    CHECK(f.cfg.epoch() == epoch);
    CHECK(eval(f.ctx, "{config.unsaved}") == "(box/height)");
    CHECK_FALSE(f.ctx.eval("{config.clear}", "test"));  // no path: an error, never all
    CHECK(eval(f.ctx, "{config.unsaved}") == "(box/height)");
}

TEST_CASE("config.epoch moves on every change, and only then") {
    Fixture f;
    const std::string before = eval(f.ctx, "{config.epoch}");
    CHECK(before == std::to_string(f.cfg.epoch()));
    CHECK(eval(f.ctx, "{config.epoch}") == before);  // asking changes nothing
    REQUIRE(f.ctx.eval("{config.set box.width 4.5}", "test"));
    const std::string after = eval(f.ctx, "{config.epoch}");
    CHECK(after != before);
    CHECK(eval(f.ctx, "{config.toggle local}") == "0");
    CHECK(eval(f.ctx, "{config.epoch}") != after);
}

TEST_CASE("the writers are bound only when allowed, and write through the Config") {
    {
        Fixture f;
        CHECK_FALSE(f.ctx.has_func("config.write_back"));
        CHECK_FALSE(f.ctx.has_func("config.promote"));
    }
    Fixture f({.allow_write = true});
    REQUIRE(f.ctx.has_func("config.write_back"));
    REQUIRE(f.ctx.eval("{config.set box.width 5}", "test"));
    CHECK(eval(f.ctx, "{config.write_back box.width}") ==
          "((written (((file \"" + f.file("local.dta") + "\") (line 1)))) (refused ()))");
    CHECK(f.dir.read("local.dta") == "(box (width 5))\n");
    CHECK(eval(f.ctx, "{config.unsaved}") == "()");
}
#else
TEST_CASE("a shipping build binds the reading tools only") {
    Fixture f({.allow_write = true});
    CHECK(f.ctx.has_func("config.explain"));
    CHECK_FALSE(f.ctx.has_func("config.set"));
    CHECK_FALSE(f.ctx.has_func("config.clear"));
    CHECK_FALSE(f.ctx.has_func("config.write_back"));
}
#endif
