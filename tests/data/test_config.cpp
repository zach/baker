// Config: merge, layers, provenance, reload, callbacks, tweaks, live edits, write-back.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>
#include <baker/write.hpp>

#include "alloc_counter.hpp"
#include "merge.hpp"
#include "temp_dir.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include "ok.hpp"

using namespace baker;
using baker::test::ok;
namespace fs = std::filesystem;
using baker::test::diff_lines;
using baker::test::TempDir;

namespace {

Ref<Array> parse(const char* text) {
    auto r = read_string(text, "t.dta");
    REQUIRE(r);
    return *r;
}

std::string str_of(const Array& a) {
    return to_string(a);
}

const char* const kDefault =
    "; engine defaults\n"
    "[kDefaultVolume 0.8]\n"
    "(renderer\n"
    "   (screen_size 1280 720)\n"
    "   (clear_color 0.1 0.1 0.12)\n"
    "   (shadows (enabled 1) (resolution 2048)))\n"
    "(audio\n"
    "   (master_volume kDefaultVolume)   ; from the macro\n"
    "   (channels 64))\n"
    "(box (width 2 (range 0 10) (note \"good values are 2-3\")) (height 1))\n";

const char* const kApp =
    "; app overrides\n"
    "(renderer (screen_size 1920 1080) (shadows (resolution 4096)))\n"
    "(audio (channels 32))\n";

}  // namespace

TEST_CASE("overwrite merge: leaves win, tagged arrays recurse") {
    // The slide's example, with the leaves winning.
    auto r = merge(*parse("(a (b 1))"), *parse("(a (b 2) (c 2))"));
    CHECK(str_of(*r) == "((a (b 2) (c 2)))");
    // Values replace values; children are kept and merged.
    r = merge(*parse("(width 2 (range 0 10))"), *parse("(width 3)"));
    CHECK(str_of(*r) == "((width 3 (range 0 10)))");
    r = merge(*parse("(clear_color 0.1 0.1 0.12)"), *parse("(clear_color 1 0)"));
    CHECK(str_of(*r) == "((clear_color 1 0))");
    r = merge(*parse("(points (1 2) (3 4))"), *parse("(points (5 6))"));
    CHECK(str_of(*r) == "((points (5 6)))");  // untagged arrays are values
    r = merge(*parse("(s (a 1) (b 2))"), *parse("(s (b 3))"));
    CHECK(str_of(*r) == "((s (a 1) (b 3)))");
    // Nothing changes: the base itself comes back.
    auto base = parse("(a (b 1))");
    CHECK(merge(*base, *parse("(a)")).get() == base.get());
    CHECK(merge(*base, *Array::make(0)).get() == base.get());
    // Fill-missing keeps existing values.
    r = merge(*parse("(a (b 1))"), *parse("(a (b 2) (c 2))"), MergePolicy::FillMissing);
    CHECK(str_of(*r) == "((a (b 1) (c 2)))");
}

TEST_CASE("a large merge, indexed, gives what small merges give") {
    // Past 32 incoming keys merge indexes the existing ones. Merging the same keys
    // 20 at a time takes the scanning path; both must agree, duplicate tags included.
    std::string base = "(dup 0) (dup 1) ", over = "(dup 2) (dup 3) ";
    for (int i = 0; i < 150; ++i)
        base += "(k" + std::to_string(i) + " " + std::to_string(i) + " (x 1)) ";
    for (int i = 100; i < 250; ++i)
        over += "(k" + std::to_string(i) + " -" + std::to_string(i) + " (y 2)) ";
    over += "(k120 (z 3)) 7";
    const auto b = parse(base.c_str()), o = parse(over.c_str());
    for (const MergePolicy policy : {MergePolicy::Overwrite, MergePolicy::FillMissing}) {
        Ref<Array> step = b;
        for (uint32_t from = 0; from < o->size(); from += 20) {
            const auto part =
                Array::make_copy(o->nodes().subspan(from, std::min<size_t>(20, o->size() - from)));
            step = merge(*step, *part, policy);
        }
        CHECK(str_of(*merge(*b, *o, policy)) == str_of(*step));
    }
}

TEST_CASE("wide arrays at two depths merge as narrow ones do") {
    // The root and one section each pass 32 incoming keys: each depth indexes, while the
    // depth above holds its own index.
    std::string base, over;
    for (int i = 0; i < 60; ++i) base += "(k" + std::to_string(i) + " 1) ";
    std::string wide_b = "(wide", wide_o = "(wide";
    for (int i = 0; i < 50; ++i) {
        wide_b += " (w" + std::to_string(i) + " 1 (deep 1))";
        wide_o += " (w" + std::to_string(i + 25) + " 2)";
    }
    base += wide_b + ") ";
    over += wide_o + ") ";
    for (int i = 30; i < 90; ++i) over += "(k" + std::to_string(i) + " 3) ";
    const auto b = parse(base.c_str()), o = parse(over.c_str());
    const Ref<Array> merged = merge(*b, *o);
    // Narrow: one key at a time.
    Ref<Array> step = b;
    for (uint32_t i = 0; i < o->size(); ++i)
        step = merge(*step, *Array::make_copy(o->nodes().subspan(i, 1)));
    CHECK(str_of(*merged) == str_of(*step));
    CHECK(merged->get<int>("wide.w30.0") == 2);
    CHECK(merged->get<int>("wide.w30.deep.0") == 1);
    CHECK(merged->get<int>("wide.w10.0") == 1);
    CHECK(merged->get<int>("k45.0") == 3);
}

TEST_CASE("a layer that sets a whole section shares it, and a merge allocates little") {
    const auto base = parse("(s (speed 1) (count 2)) (t (x 1))");
    const auto over = parse("(s (speed 3) (count 4))");
    const Ref<Array> merged = merge(*base, *over);
    CHECK(merged->try_find("s") == over->try_find("s"));  // the layer's own array
    CHECK(merged->try_find("t") == base->try_find("t"));
    // A Config merging layers that override whole sections allocates the same for 20
    // sections as for 200: only what it keeps, the roots.
    auto allocations_for = [](int sections) {
        std::string lo, hi;
        for (int i = 0; i < sections; ++i) {
            lo += "(s" + std::to_string(i) + " (speed 1) (count 1) (sub (x 1) (y 1)))";
            if (i % 2 == 0)
                hi += "(s" + std::to_string(i) + " (speed 2) (count 2) (sub (x 2) (y 2)))";
        }
        Config cfg({.tweaks = nullptr});
        REQUIRE(cfg.push_layer("lo", parse(lo.c_str())));
        REQUIRE(cfg.push_layer("hi", parse(hi.c_str())));
        REQUIRE(cfg.set_layer_enabled("hi", false));
        REQUIRE(cfg.set_layer_enabled("hi", true));  // warm the scratch
        REQUIRE(cfg.set_layer_enabled("hi", false));
        const test::AllocationScope scope;
        (void)cfg.set_layer_enabled("hi", true);
        return scope.count();
    };
    if constexpr (!kShipping)  // shipping refuses toggling a layer
        if (test::counting_allocations()) CHECK(allocations_for(200) == allocations_for(20));
}

TEST_CASE("edit mode records where every node is written") {
    const std::string text = "[M 5]\n(a 1 \"two\" 'three four' M\n  (b 2.5))\n";
    LoadSession s;
    SourceMap map;
    s.source_map = &map;
    auto root = read_string(text, "e.dta", s);
    REQUIRE(root);
    const Array& a = *(**root)[0].try_array();
    auto token = [&](size_t i) {
        const SourceSpan* sp = map.span(a, i);
        REQUIRE(sp);
        return text.substr(sp->begin, sp->end - sp->begin);
    };
    CHECK(token(0) == "a");
    CHECK(token(1) == "1");
    CHECK(token(2) == "\"two\"");
    CHECK(token(3) == "'three four'");
    CHECK(map.span(a, 4)->macro == Symbol("M"));  // spliced from macro M
    CHECK(token(4) == "M");
    CHECK(token(5) == "(b 2.5)");
    CHECK(map.span(a, 1)->literal());
    CHECK_FALSE(map.span(a, 4)->literal());
    const SourceSpan* ext = map.extent(a);
    REQUIRE(ext);
    CHECK(text.substr(ext->begin, ext->end - ext->begin) ==
          "(a 1 \"two\" 'three four' M\n  (b 2.5))");
    CHECK(*map.text(Symbol("e.dta")) == text);
    CHECK(map.extent(**root)->end == text.size());

    // Gameplay mode records nothing and builds the same tree.
    auto plain = read_string(text, "e.dta");
    REQUIRE(plain);
    CHECK(structurally_equal(**plain, **root));
}

TEST_CASE("an overwriting merge keeps spans with their nodes") {
    const std::string base = "(a 1 (b 2))\n", over = "(a 3)\n";
    LoadSession s;
    SourceMap map;
    s.source_map = &map;
    auto rb = read_string(base, "base.dta", s);
    auto ro = read_string(over, "over.dta", s);
    REQUIRE(rb);
    REQUIRE(ro);
    const Array& a = *(**rb)[0].try_array();
    const Array& o = *(**ro)[0].try_array();
    Vec<Node> nodes(a.begin(), a.end());
    const auto known = map.spans(a);
    REQUIRE(known);
    Vec<SourceSpan> spans(known->data(), known->data() + known->size());
    detail::MergeScratch scratch;
    REQUIRE(detail::merge_into(nodes, &spans, 1, o, 1, MergePolicy::Overwrite, &map, scratch));
    REQUIRE(nodes.size() == 3);
    REQUIRE(spans.size() == 3);
    CHECK(nodes[1].try_int() == 3);
    CHECK(spans[1].file == Symbol("over.dta"));
    CHECK(over.substr(spans[1].begin, spans[1].end - spans[1].begin) == "3");
    CHECK(spans[2].file == Symbol("base.dta"));
    CHECK(base.substr(spans[2].begin, spans[2].end - spans[2].begin) == "(b 2)");
}

TEST_CASE("a source map read again keeps the text as it is now") {
    LoadSession s;
    SourceMap map;
    s.source_map = &map;
    REQUIRE(read_string("(a 1)\n", "f.dta", s));
    REQUIRE(read_string("(a 22)\n", "f.dta", s));
    CHECK(*map.text(Symbol("f.dta")) == "(a 22)\n");
}

TEST_CASE("edit mode follows #include and #merge") {
    TempDir dir;
    dir.write("inc.dta", "(from_inc 7)\n");
    dir.write("base.dta", "(s (x 1) (y 2))\n(only_base 3)\n");
    const std::string main =
        dir.write("main.dta", "#include inc.dta\n(s (x 10))\n#merge base.dta\n");
    LoadSession session;
    SourceMap map;
    session.source_map = &map;
    auto root = read_file(main, session);
    REQUIRE(root);
    const Array& r = **root;
    CHECK(to_string(r) == "((from_inc 7) (s (x 10) (y 2)) (only_base 3))");
    const SourceSpan* inc = map.span(r, 0);
    REQUIRE(inc);
    CHECK(inc->file.str().ends_with("inc.dta"));
    // (s ...) was rebuilt by #merge; its spans travel with its nodes.
    const Array& s = *r[1].try_array();
    REQUIRE(map.spans(s));
    CHECK(map.span(s, 1)->file.str().ends_with("main.dta"));  // (x 10)
    CHECK(map.span(s, 2)->file.str().ends_with("base.dta"));  // (y 2), merged in
    CHECK(map.span(r, 2)->file.str().ends_with("base.dta"));
}

TEST_CASE("layers merge, and removing one restores every value") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kDefault)));
    REQUIRE(cfg.push_layer("app", dir.write("app.dta", kApp)));
    CHECK(strings(cfg.layers()) == std::vector<std::string>{"default", "app", "scratch"});
    CHECK(cfg.get<int>("renderer.screen_size.0") == 1920);
    CHECK(cfg.get<int>("renderer.shadows.resolution") == 4096);
    CHECK(cfg.get<int>("renderer.shadows.enabled") == 1);
    CHECK(cfg.get<double>("audio.master_volume") == 0.8);
    CHECK(cfg.get<int>("audio.channels") == 32);

    const Ref<Array> before = cfg.root();
    REQUIRE(
        cfg.push_layer("patch-2026-10-11",
                       parse("(audio (channels 16)) (renderer (clear_color 1 0 0)) (new_key 5)")));
    CHECK(cfg.get<int>("audio.channels") == 16);
    CHECK(cfg.get<double>("renderer.clear_color.1") == 0.0);
    CHECK(cfg.get<int>("new_key") == 5);
    REQUIRE(cfg.remove_layer("patch-2026-10-11"));
    CHECK(structurally_equal(*cfg.root(), *before));
    CHECK_FALSE(cfg.get<int>("new_key"));

    // Removing a middle layer re-merges the rest; popping removes the top.
    const uint64_t epoch = cfg.epoch();
    REQUIRE(cfg.push_layer("top", parse("(audio (channels 8))")));
    REQUIRE(cfg.remove_layer("app"));
    CHECK(cfg.get<int>("audio.channels") == 8);
    CHECK(cfg.get<int>("renderer.screen_size.0") == 1280);
    REQUIRE(cfg.pop_layer());
    CHECK(cfg.get<int>("audio.channels") == 64);
    CHECK(cfg.epoch() == epoch + 3);

    CHECK(error_code_name(cfg.remove_layer("nope").error().code()) == "not found");
    CHECK(cfg.push_layer("default", parse("(x 1)")).error().code() == ErrorCode::Refused);
    CHECK(cfg.push_layer("bad", dir.path("missing.dta")).error().code() == ErrorCode::Io);
}

TEST_CASE("layers above see macros from layers below, and lose them with the layer") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("macros", dir.write("m.dta", "[kSize 7]\n")));
    REQUIRE(cfg.push_layer("use", dir.write("u.dta", "(size kSize)\n")));
    CHECK(cfg.get<int>("size") == 7);
    REQUIRE(cfg.remove_layer("macros"));
    CHECK(cfg.get<Symbol>("size") == Symbol("kSize"));  // now just a symbol
}

TEST_CASE("source names the layer, file, and line") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kDefault)));
    REQUIRE(cfg.push_layer("app", dir.write("app.dta", kApp)));
    REQUIRE(cfg.push_layer("patch", parse("(renderer (clear_color 1 0 0))")));

    Source s = *cfg.source("renderer.screen_size");
    CHECK(s.layer == "app");
    CHECK(s.file.str().ends_with("app.dta"));
    CHECK(s.line == 2);
    s = *cfg.source("renderer.shadows.enabled");
    CHECK(s.layer == "default");
    CHECK(s.file.str().ends_with("default.dta"));
    CHECK(s.line == 6);
    s = *cfg.source("audio.master_volume");
    CHECK(s.layer == "default");
    CHECK(s.line == 8);
    s = *cfg.source("renderer.clear_color.2");
    CHECK(s.layer == "patch");
    CHECK(s.file == Symbol("t.dta"));     // the overlay was parsed from a string named t.dta
    s = *cfg.source("renderer.shadows");  // a container: the topmost layer that has it
    CHECK(s.layer == "app");

#if !BAKER_SHIPPING
    REQUIRE(cfg.set("renderer.screen_size", std::array<int, 2>{800, 600}));
    s = *cfg.source("renderer.screen_size");
    CHECK(s.layer == "scratch");
    CHECK(cfg.get<int>("renderer.screen_size.1") == 600);
    CHECK(strings(cfg.unsaved()) == std::vector<std::string>{"renderer/screen_size"});
#endif

    CHECK(cfg.source("renderer.nope").error().code() == ErrorCode::NotFound);
    CHECK(cfg.source("a..b").error().code() == ErrorCode::Syntax);
}

TEST_CASE("source names a generated file's origin and tool") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    dir.write("econ.dta", "(origin \"economy.xlsx\" (tool sheets-export))\n(econ (price 5))\n");
    REQUIRE(
        cfg.push_layer("default", dir.write("default.dta", "(shop (tax 2))\n#include econ.dta\n")));
    REQUIRE(cfg.push_layer("patch", dir.write("patch.dta", "(shop (tax 3))\n")));

    Source s = *cfg.source("econ.price");
    CHECK(s.layer == "default");
    CHECK(s.file.str().ends_with("econ.dta"));
    CHECK(s.line == 2);
    CHECK(s.origin == Symbol("economy.xlsx"));
    CHECK(s.tool == Symbol("sheets-export"));
    s = *cfg.source("shop.tax");  // written by hand
    CHECK(s.origin.empty());
    CHECK(s.tool.empty());
    // Layer tooling reports it too.
    const auto effect = *cfg.layer_effect("default");
    const auto price = std::find_if(effect.begin(), effect.end(), [](const LayerEffectEntry& e) {
        return e.path == Symbol("econ.price");
    });
    REQUIRE(price != effect.end());
    CHECK(price->source.tool == Symbol("sheets-export"));
}

#if !BAKER_SHIPPING  // reload is inert under BAKER_SHIPPING (tested below)
TEST_CASE("reload sees a file changed on disk") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    dir.write("inc.dta", "(volume 3)\n");
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", "(speed 1)\n#include inc.dta\n")));
    REQUIRE(cfg.push_layer("app", dir.write("app.dta", "(speed 2)\n")));
    CHECK(cfg.get<int>("speed") == 2);
    CHECK(cfg.reload() == false);  // nothing changed

    dir.write("app.dta", "(speed 5)\n");
    CHECK(cfg.reload() == true);
    CHECK(cfg.get<int>("speed") == 5);

    dir.write("inc.dta", "(volume 4)\n");  // an included file counts too
    CHECK(cfg.reload() == true);
    CHECK(cfg.get<int>("volume") == 4);

    // A broken edit changes nothing, and the error says where.
    const Ref<Array> good = cfg.root();
    dir.write("app.dta", "(speed 6\n");
    auto r = cfg.reload();
    REQUIRE_FALSE(r);
    CHECK(r.error().file().str().ends_with("app.dta"));
    CHECK(cfg.root().get() == good.get());
    CHECK(cfg.get<int>("speed") == 5);
    dir.write("app.dta", "(speed 7)\n");
    CHECK(cfg.reload() == true);
    CHECK(cfg.get<int>("speed") == 7);
    // The old tree is still readable by anyone who held it.
    CHECK(good->get<int>("speed") == 5);
}

TEST_CASE("reload callbacks fire per group; punting rebuilds, migrating does not") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("d", dir.write("d.dta", "(renderer (msaa 4)) (audio (channels 8))\n")));
    int renderer_rebuilds = 0, audio_rebuilds = 0, audio_migrations = 0;
    cfg.on_reload("renderer", [&] { ++renderer_rebuilds; });  // punt by default: no code
    cfg.on_reload(
        "audio", [&] { ++audio_rebuilds; },
        [&] {
            ++audio_migrations;
            return cfg.get<int>("audio.channels") == 16 ? Reload::Migrated : Reload::Punt;
        });

    dir.write("d.dta", "(renderer (msaa 8)) (audio (channels 8))\n");
    REQUIRE(cfg.reload());
    CHECK(renderer_rebuilds == 1);
    CHECK(audio_migrations == 0);  // its group did not change

    dir.write("d.dta", "(renderer (msaa 8)) (audio (channels 16))\n");
    REQUIRE(cfg.reload());
    CHECK(renderer_rebuilds == 1);
    CHECK(audio_migrations == 1);
    CHECK(audio_rebuilds == 0);  // migrated

    dir.write("d.dta", "(renderer (msaa 8)) (audio (channels 2))\n");
    REQUIRE(cfg.reload());
    CHECK(audio_migrations == 2);
    CHECK(audio_rebuilds == 1);  // migration declined: punt

    // Layer changes fire the same callbacks.
    REQUIRE(cfg.push_layer("p", parse("(renderer (msaa 2))")));
    CHECK(renderer_rebuilds == 2);
    REQUIRE(cfg.pop_layer());
    CHECK(renderer_rebuilds == 3);
}

#endif

TEST_CASE("callbacks may change the Config and their own lists while notified") {
    if constexpr (kShipping) return;  // set() and reload() are refused
    TweakRegistry reg;
    Tweak<int> a{"t.a", 1, {}, reg};
    Tweak<int> b{"t.b", 1, {}, reg};
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("d", parse("(t (a 1) (b 1))")));

    SUBCASE("a callback that sets another tweak notifies it, once") {
        int b_changes = 0;
        a.on_change([&] { REQUIRE(cfg.set("t.b", {Node(int64_t(a) * 10)})); });
        b.on_change([&] { ++b_changes; });
        REQUIRE(cfg.set("t.a", {Node(2)}));
        CHECK(int(b) == 20);
        CHECK(b_changes == 1);
    }
    SUBCASE("two tweaks whose callbacks set each other do not loop") {
        int a_changes = 0, b_changes = 0;
        a.on_change([&] {
            ++a_changes;
            REQUIRE(cfg.set("t.b", {Node(int64_t(a) + 1)}));
        });
        b.on_change([&] {
            ++b_changes;
            REQUIRE(cfg.set("t.a", {Node(int64_t(b) + 1)}));
        });
        REQUIRE(cfg.set("t.a", {Node(10)}));
        CHECK(a_changes == 1);  // a's own change from b's callback does not notify again
        CHECK(b_changes == 1);
        CHECK(int(b) == 11);
        CHECK(int(a) == 12);  // the value changed; only the notification was elided
    }
    SUBCASE("a callback added from inside a callback runs from the next change") {
        int first = 0, second = 0;
        a.on_change([&] {
            ++first;
            if (first == 1) a.on_change([&] { ++second; });
        });
        REQUIRE(cfg.set("t.a", {Node(2)}));
        CHECK(first == 1);
        CHECK(second == 0);
        REQUIRE(cfg.set("t.a", {Node(3)}));
        CHECK(first == 2);
        CHECK(second == 1);
    }
    SUBCASE("a reload callback may remove itself and others while they are notified") {
        TempDir dir;
        Config files({.tweaks = nullptr});
        REQUIRE(files.push_layer("d", dir.write("d.dta", "(g (v 1))\n")));
        int one = 0, two = 0, done = 0;
        Config::ReloadId id_one = 0, id_two = 0, id_done = 0;
        id_one = files.on_reload("g", [&] {
            ++one;
            files.remove_on_reload(id_one);
            files.remove_on_reload(id_two);
        });
        id_two = files.on_reload("g", [&] { ++two; });
        id_done = files.on_reload_complete([&](const ReloadRecord&) {
            ++done;
            files.remove_on_reload(id_done);
        });
        dir.write("d.dta", "(g (v 2))\n");
        REQUIRE(files.reload());
        CHECK(one == 1);
        CHECK(done == 1);
        dir.write("d.dta", "(g (v 3))\n");
        REQUIRE(files.reload());
        CHECK(one == 1);  // removed
        CHECK(two == 1);  // ran in the round it was removed in: the round walks a copy
        CHECK(done == 1);
    }
}

TEST_CASE("tweaks bind, rebind, and fall back to their defaults") {
    TweakRegistry reg;
    Tweak<float> width{"box.width", 2.0f, {.range = {0, 10}, .step = 1, .desc = "Box width"}, reg};
    Tweak<int> missing{"box.depth", 9, {}, reg};
    Tweak<std::string> title{"app.title", "untitled", {}, reg};
    Tweak<std::array<float, 3>> tint{"look.tint", {1, 1, 1}, {.range = {0, 1}}, reg};
    CHECK(reg.size() == 4);
    CHECK(float(width) == 2.0f);
    CHECK_FALSE(width.bound());

    int width_changes = 0;
    width.on_change([&] { ++width_changes; });

    TempDir dir;
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kDefault)));
    CHECK(width.bound());
    CHECK(float(width) == 2.0f);
    CHECK(width_changes == 1);  // became bound
    CHECK_FALSE(missing.bound());
    CHECK(int(missing) == 9);
    CHECK(title.get() == "untitled");

    REQUIRE(cfg.push_layer(
        "app", parse("(box (width 3.5)) (app (title \"Demo\")) (look (tint 1 0.5 0))")));
    CHECK(float(width) == 3.5f);
    CHECK(width_changes == 2);
    CHECK(title.get() == "Demo");
    CHECK(tint.value()[1] == 0.5f);
    CHECK(tint.kind() == "color");  // inferred: 3 numbers ranged 0..1

    // Metadata: the data file's wins over the code's, and note exists only in data.
    CHECK(width.range().max == 10);
    CHECK(width.step() == 1);
    CHECK(width.desc() == "Box width");
    CHECK(width.note() == "good values are 2-3");
    CHECK(width.group() == "box");

    REQUIRE(cfg.pop_layer());
    CHECK(float(width) == 2.0f);
    CHECK(title.get() == "untitled");  // absent again: the default
    CHECK_FALSE(title.bound());

    // A tweak created after loading binds at once.
    Tweak<int> height{"box.height", 0, {}, reg};
    CHECK(height.bound());
    CHECK(int(height) == 1);

    // A value of the wrong type warns and keeps the default.
    std::vector<Error> warnings;
    cfg.set_warning_handler([&](const Error& e) { warnings.push_back(e); });
    REQUIRE(cfg.push_layer("bad", parse("(box (width wide))")));
    CHECK(float(width) == 2.0f);
    CHECK_FALSE(width.bound());
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].message() == "'width' is symbol, not float");
}

TEST_CASE("a tweak's value stops before the metadata written after it") {
    TweakRegistry reg;
    Tweak<std::array<float, 3>> tint{"look.tint", {1, 1, 1}, {}, reg};
    Tweak<std::vector<int>> sizes{"look.sizes", {}, {}, reg};
    TempDir dir;
    Config cfg({.tweaks = &reg});
    std::vector<Error> warnings;
    cfg.set_warning_handler([&](const Error& e) { warnings.push_back(e); });
    REQUIRE(cfg.push_layer(
        "default", dir.write("default.dta",
                             "(look\n"
                             "   (tint 0.9 0.5 0.2 (range 0 1) (desc \"Tint\") (note \"warm\"))\n"
                             "   (sizes 1 2 3 (step 1)))\n")));
    CHECK(warnings.empty());
    REQUIRE(tint.bound());
    CHECK(tint.value()[0] == 0.9f);
    CHECK(tint.value()[2] == 0.2f);
    CHECK(tint.kind() == "color");  // three numbers, ranged 0..1 in the data
    CHECK(tint.desc() == "Tint");
    CHECK(sizes.get() == std::vector<int>{1, 2, 3});

    if constexpr (!kShipping) {
        // A live edit and its write-back change the numbers and keep the metadata.
        REQUIRE(tint.set({0.8f, 0.5f, 0.25f}));
        REQUIRE(tint.write_back());
        CHECK(dir.read("default.dta").find("(tint 0.8 0.5 0.25 (range 0 1) (desc \"Tint\")") !=
              std::string::npos);
        CHECK(tint.value()[2] == 0.25f);
    }

    // Only trailing arrays with a metadata tag are left out: other tagged elements are
    // the value's own.
    Tweak<std::vector<Node>> parts{"look.parts", {}, {}, reg};
    REQUIRE(cfg.push_layer("parts", parse("(look (parts 1 (x 2) (range 0 1)))")));
    CHECK(parts.get().size() == 2);
}

TEST_CASE("registering a tweak allocates nothing") {
    TweakRegistry reg;
    test::AllocationScope scope;
    {
        Tweak<float> t{"a.b", 1.0f, {.desc = "d"}, reg};
        CHECK(float(t) == 1.0f);
    }
    if (test::counting_allocations()) CHECK(scope.count() == 0);
    CHECK(reg.size() == 0);
}

TEST_CASE("live edits through tweaks, and write-back to the source line") {
    TweakRegistry reg;
    Tweak<float> width{"box.width", 2.0f, {}, reg};
    TempDir dir;
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kDefault)));
    REQUIRE(cfg.push_layer("app", dir.write("app.dta", kApp)));

#if BAKER_SHIPPING
    CHECK(width.set(2.7f).error().code() == ErrorCode::Refused);
#else
    REQUIRE(width.set(2.7f));
    CHECK(float(width) == 2.7f);
    CHECK(width.source()->layer == "scratch");
    CHECK(cfg.get<double>("box.width") == 2.7);  // written as the float's decimal

    const std::string before = dir.read("default.dta");
    REQUIRE(width.write_back());
    const std::string after = dir.read("default.dta");
    CHECK(diff_lines(before, after) ==
          std::vector<std::string>{
              "-(box (width 2 (range 0 10) (note \"good values are 2-3\")) (height 1))",
              "+(box (width 2.7 (range 0 10) (note \"good values are 2-3\")) (height 1))"});
    CHECK(width.source()->layer == "default");  // saved: no longer live
    CHECK(width.source()->line == 10);
    CHECK(cfg.unsaved().empty());
    CHECK(float(width) == 2.7f);
    CHECK(width.note() == "good values are 2-3");

    // Writing back an unchanged value changes nothing.
    REQUIRE(cfg.write_back("box.width"));
    CHECK(dir.read("default.dta") == after);

    // An override in a higher layer is written there, as one token.
    REQUIRE(cfg.set("renderer.screen_size.1", Node(1200)));
    REQUIRE(cfg.write_back("renderer.screen_size.1"));
    CHECK(dir.read("app.dta") ==
          "; app overrides\n(renderer (screen_size 1920 1200) (shadows (resolution 4096)))\n"
          "(audio (channels 32))\n");
    CHECK(cfg.unsaved().empty());
#endif
}

#if !BAKER_SHIPPING
TEST_CASE("write-back refuses what it cannot edit exactly") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kDefault)));
    REQUIRE(cfg.push_layer("patch", parse("(renderer (clear_color 1 0 0))")));

    // A value spliced in from a macro.
    REQUIRE(cfg.set("audio.master_volume", Node(0.5)));
    auto r = cfg.write_back("audio.master_volume");
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::Refused);
    CHECK(r.error().message() ==
          "'audio/master_volume' comes from macro 'kDefaultVolume'; edit the macro's definition "
          "instead");
    CHECK(r.error().file().str().ends_with("default.dta"));
    CHECK(r.error().line() == 8);
    CHECK(cfg.unsaved().size() == 1);  // still live

    // A value from an overlay layer.
    REQUIRE(cfg.set("renderer.clear_color.0", Node(0.5)));
    r = cfg.write_back("renderer.clear_color");
    CHECK(r.error().message() ==
          "'renderer/clear_color' comes from layer 'patch', which has no source file");

    // A file edited since it was read.
    const std::string edited = dir.read("default.dta") + "; edited by hand\n";
    dir.write("default.dta", edited);
    REQUIRE(cfg.set("audio.channels", Node(4)));
    r = cfg.write_back("audio.channels");
    CHECK(r.error().code() == ErrorCode::Refused);
    CHECK(
        r.error().message().ends_with("has changed since it was read; reload before writing back"));
    CHECK(dir.read("default.dta") == edited);  // untouched
    REQUIRE(cfg.reload());
    REQUIRE(cfg.write_back("audio.channels"));
    CHECK(dir.read("default.dta").find("(channels 4))") != std::string::npos);

    // A value no layer's file has.
    REQUIRE(cfg.set("fresh.key", Node(1)));
    CHECK(cfg.write_back("fresh.key").error().code() == ErrorCode::Refused);
}

TEST_CASE("write-back into an array assembled by #merge is refused") {
    TempDir dir;
    dir.write("base.dta", "(s (w 1 (range 0 2)))\n");
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("main", dir.write("main.dta", "(s (w 5))\n#merge base.dta\n")));
    // (w 5) gained (range 0 2) from base.dta, so it was rebuilt; its values are main.dta's
    // tokens, carried along with their spans.
    REQUIRE(cfg.set("s.w", Node(6)));
    REQUIRE(cfg.write_back("s.w"));
    CHECK(dir.read("main.dta") == "(s (w 6))\n#merge base.dta\n");
}

TEST_CASE("write_back_to writes into another layer, adding the key if needed") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kDefault)));
    REQUIRE(cfg.push_layer("local", dir.write("local.dta", "; my machine\n(box (height 3))")));
    REQUIRE(cfg.set("box.width", Node(4)));
    REQUIRE(cfg.write_back_to("box.width", "local"));
    CHECK(dir.read("local.dta") == "; my machine\n(box (height 3) (width 4))");
    CHECK(cfg.source("box.width")->layer == "local");
    CHECK(cfg.unsaved().empty());

    REQUIRE(cfg.set("audio.channels", Node(2)));
    REQUIRE(cfg.write_back_to("audio.channels", "local"));
    CHECK(dir.read("local.dta") ==
          "; my machine\n(box (height 3) (width 4))\n(audio (channels 2))\n");
    CHECK(cfg.get<int>("audio.channels") == 2);

    // Into a layer below the one that supplies the value: written, but still shadowed, so
    // the live edit stays.
    REQUIRE(cfg.set("box.height", Node(9)));
    REQUIRE(cfg.write_back_to("box.height", "default"));
    CHECK(dir.read("default.dta").find("(height 9))") != std::string::npos);
    CHECK(strings(cfg.unsaved()) == std::vector<std::string>{"box/height"});
    CHECK(cfg.get<int>("box.height") == 9);
}

TEST_CASE("freeze ends the initial load") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("d", dir.write("d.dta", "(a 1)\n")));
    cfg.freeze();
    CHECK(cfg.push_layer("p", parse("(a 2)")).error().code() == ErrorCode::Refused);
    CHECK(cfg.set("a", Node(3)).error().code() == ErrorCode::Refused);
    dir.write("d.dta", "(a 5)\n");
    CHECK(cfg.reload() == false);
    CHECK(cfg.get<int>("a") == 1);

    Config live({.live_layers = true, .tweaks = nullptr});
    REQUIRE(live.push_layer("d", dir.path("d.dta")));
    live.freeze();
    REQUIRE(live.push_layer("patch", parse("(a 2)")));  // the explicit live-ops path
    CHECK(live.get<int>("a") == 2);
}
#endif

#if BAKER_SHIPPING
TEST_CASE("under BAKER_SHIPPING reload is inert and dev_only tweaks keep their defaults") {
    TweakRegistry reg;
    Tweak<int> cheat{"debug.god_mode", 0, {.dev_only = true}, reg};
    Tweak<int> speed{"player.speed", 1, {}, reg};
    TempDir dir;
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("d", dir.write("d.dta", "(debug (god_mode 1)) (player (speed 5))\n")));
    CHECK(int(cheat) == 0);
    CHECK_FALSE(cheat.bound());
    CHECK(int(speed) == 5);
    dir.write("d.dta", "(debug (god_mode 1)) (player (speed 9))\n");
    CHECK(cfg.reload() == false);
    CHECK(int(speed) == 5);
    CHECK(speed.set(7).error().code() == ErrorCode::Refused);
    CHECK(cfg.write_back("player.speed").error().code() == ErrorCode::Refused);
}
#endif

TEST_CASE("the short forms in one place") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    ok(cfg.load(dir.write("default.dta", kDefault)));
    ok(cfg.merge(dir.write("app.dta", kApp)));
    CHECK(cfg.get<int>("audio.channels") == 32);
    CHECK(cfg.get<int>("audio.missing", 3) == 3);
    CHECK(ok(cfg.source("audio.channels")).file.str().ends_with("app.dta"));
    CHECK_FALSE(cfg.push_layer("x", dir.path("nope.dta")));
    CHECK_FALSE(cfg.get<int>("audio.nope"));
    if constexpr (kShipping) {
        CHECK(cfg.set("audio.channels", 4).error().code() == ErrorCode::Refused);
    } else {
        ok(cfg.set("audio.channels", 4));
        ok(cfg.write_back("audio.channels"));
        CHECK(dir.read("app.dta").find("(audio (channels 4))") != std::string::npos);
        CHECK(ok(cfg.reload()) == false);
    }
}

#if !BAKER_SHIPPING
namespace {
// Every path in `a` that ends on an array with values: menu/item/price, ...
void value_paths(const Array& a, const std::string& prefix, std::vector<std::string>& out) {
    for (const Array& child : a.subarrays()) {
        const auto tag = child.empty() ? std::nullopt : child[0].try_sym();
        if (!tag) continue;
        const std::string path =
            (prefix.empty() ? "" : prefix + "/") + "'" + std::string(tag->str()) + "'";
        bool values = false;
        for (size_t i = 1; i < child.size(); ++i) {
            const Array* c = child[i].try_array();
            values = values || !c || c->empty() || !(*c)[0].try_sym();
        }
        if (values) out.push_back(path);
        value_paths(child, path, out);
    }
}
}  // namespace

TEST_CASE("every corpus config value writes back as one line, or is refused with a location") {
    TempDir dir;
    for (const char* name : {"config_default.dta", "config_app.dta", "macros.dta"}) {
        std::ifstream in(fs::path(BAKER_CORPUS_DIR) / name, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        dir.write(name, ss.str());
    }
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("app", dir.path("config_app.dta")));
    std::vector<std::string> paths;
    value_paths(*cfg.root(), "", paths);
    REQUIRE(paths.size() >= 10);
    size_t written = 0, refused_count = 0;
    for (const std::string& path_text : paths) {
        CAPTURE(path_text);
        const Path path(path_text);
        REQUIRE(cfg.set(path, Node(4242)));
        auto source = cfg.source(path);
        REQUIRE(source);
        std::map<std::string, std::string> before;
        for (const char* name : {"config_default.dta", "config_app.dta", "macros.dta"})
            before[name] = dir.read(name);
        auto r = cfg.write_back(path);
        if (!r) {
            CHECK(r.error().code() == ErrorCode::Refused);
            CHECK_FALSE(r.error().file().empty());
            CHECK(r.error().line() > 0);
            ++refused_count;
            continue;
        }
        size_t changed_lines = 0;
        for (const auto& [name, text] : before)
            changed_lines += diff_lines(text, dir.read(name)).size() / 2;
        CHECK(changed_lines == 1);
        CHECK(cfg.get<int>(path) == 4242);
        const auto unsaved = cfg.unsaved();  // refused paths stay live; this one is saved
        CHECK(std::find(unsaved.begin(), unsaved.end(), Symbol(to_string(path))) == unsaved.end());
        ++written;
    }
    CHECK(written >= 8);
    CHECK(refused_count >= 2);  // kDefaultVolume and kMaxPlayers are macros
    MESSAGE("wrote " << written << ", refused " << refused_count);
}
#endif

TEST_CASE("cache_dir: file layers load through caches outside edit mode") {
    TempDir dir;
    fs::create_directories(dir.path("cache"));
    const std::string base =
        dir.write("base.dta", "#include tuning.dta\n#define WIDE\n(screen (width 1280))");
    dir.write("tuning.dta", "(speed 3)");
    const std::string top = dir.write("top.dta", "#ifdef WIDE (screen (width 2560)) #endif");
    const std::string cache_dir = dir.path("cache");
    int writes = 0;
    auto make = [&] {
        auto cfg = std::make_unique<Config>(
            ConfigOptions{.edit_mode = false, .tweaks = nullptr, .cache_dir = cache_dir});
        cfg->set_writer([&](std::string_view path, std::string_view text) {
            ++writes;
            std::ofstream(std::string(path), std::ios::binary) << text;
            return true;
        });
        return cfg;
    };

    auto first = make();
    REQUIRE(first->push_layer("base", base));
    REQUIRE(first->push_layer("top", top));  // sees WIDE, replayed or parsed
    CHECK(first->get<int>("screen.width") == 2560);
    CHECK(first->get<int>("speed") == 3);
    if constexpr (kShipping) {  // shipping writes no caches, and reads the text without one
        CHECK(writes == 0);
        return;
    }
    CHECK(writes == 2);  // one cache per layer
    CHECK(std::distance(fs::directory_iterator(cache_dir), fs::directory_iterator()) == 2);

    // Another Config reads both layers from their caches, with the same values and sources.
    auto second = make();
    REQUIRE(second->push_layer("base", base));
    REQUIRE(second->push_layer("top", top));
    CHECK(writes == 2);
    CHECK(second->get<int>("screen.width") == 2560);
    CHECK(second->get<int>("speed") == 3);
    CHECK(ok(second->source("speed")).file == Symbol(dir.path("tuning.dta")));
    CHECK(ok(second->source("screen.width")).layer == Symbol("top"));

    if constexpr (!kShipping) {
        // A changed include is read from text on reload, and its layer's cache rewritten.
        dir.write("tuning.dta", "(speed 4)");
        REQUIRE(second->reload());
        CHECK(second->get<int>("speed") == 4);
        CHECK(writes >= 3);
        const int after = writes;
        auto third = make();
        REQUIRE(third->push_layer("base", base));
        CHECK(third->get<int>("speed") == 4);
        CHECK(writes == after);
    }

    // Edit mode reads text, and writes no cache.
    Config edit({.edit_mode = true, .tweaks = nullptr, .cache_dir = cache_dir});
    edit.set_writer([&](std::string_view, std::string_view) {
        ++writes;
        return true;
    });
    const int before = writes;
    REQUIRE(edit.push_layer("base", base));
    CHECK(writes == before);
}

TEST_CASE("cache_dir names a cache by the normalized path") {
    if constexpr (kShipping) return;  // shipping writes no caches
    TempDir dir;
    fs::create_directories(dir.path("cache"));
    dir.write("dlc/axe.dta", "(axe (damage 10))");
    const std::string cache_dir = dir.path("cache");
    int writes = 0;
    auto load = [&](const std::string& path) {
        Config cfg({.edit_mode = false, .tweaks = nullptr, .cache_dir = cache_dir});
        cfg.set_writer([&](std::string_view p, std::string_view text) {
            ++writes;
            std::ofstream(std::string(p), std::ios::binary) << text;
            return true;
        });
        REQUIRE(cfg.push_layer("axe", path));
        CHECK(cfg.get<int>("axe.damage") == 10);
    };
    load(dir.path("dlc/axe.dta"));
    load(dir.path("./dlc/../dlc/axe.dta"));  // spelled otherwise: the same cache
    CHECK(writes == 1);
    CHECK(std::distance(fs::directory_iterator(cache_dir), fs::directory_iterator()) == 1);
}

TEST_CASE("cache_dir reads a cache through a custom reader, and from disk without one") {
    if constexpr (kShipping) return;  // shipping writes no caches
    TempDir dir;
    fs::create_directories(dir.path("cache"));
    const std::string text = dir.write("pak.dta", "(pak (size 7))");
    const std::string cache_dir = dir.path("cache");
    std::vector<std::string> asked;
    auto load = [&](bool logged) {
        Config cfg({.edit_mode = false, .tweaks = nullptr, .cache_dir = cache_dir});
        cfg.set_writer([&](std::string_view p, std::string_view bytes) {
            std::ofstream(std::string(p), std::ios::binary) << bytes;
            return true;
        });
        // A pack file system's reader: every file, the cache too, must come through it.
        if (logged)
            cfg.session().set_reader([&](std::string_view p, TextSink out) {
                asked.emplace_back(p);
                std::ifstream f{std::string(p), std::ios::binary};
                if (!f) return false;
                const std::string bytes{std::istreambuf_iterator<char>(f), {}};
                out(bytes);
                return true;
            });
        REQUIRE(cfg.push_layer("pak", text));
        CHECK(cfg.get<int>("pak.size") == 7);
    };
    load(false);  // writes the cache
    load(false);  // the disk's reader: the cache is read straight into its arena
    load(true);
    const auto is_cache = [](const std::string& p) { return p.ends_with(".datb"); };
    CHECK(std::count_if(asked.begin(), asked.end(), is_cache) == 1);
    CHECK(std::count(asked.begin(), asked.end(), text) == 1);  // freshness reads the text
}

TEST_CASE("Config forwards the one-way rule's warnings") {
    TempDir dir;
    dir.write("task.dta", "(pack boss_task (level task))\n#define PHASES (3)");
    const std::string game =
        dir.write("game.dta", "(pack rules (level game))\n#require task.dta\n(b PHASES)");
    Config cfg({.tweaks = nullptr});
    cfg.session().set_pack_levels({"game", "task"});
    std::vector<std::string> warnings;
    cfg.set_warning_handler([&](const Error& e) { warnings.emplace_back(to_string(e)); });
    REQUIRE(cfg.push_layer("game", game));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("uses macro 'PHASES'") != std::string::npos);
}

TEST_CASE("Config::autorun: each enabled file layer's #autorun commands, bottom to top") {
    TempDir dir;
    const std::string base = dir.write("base.dta", "#autorun {first}\n(a 1)");
    const std::string top = dir.write("top.dta", "#autorun {second}\n#autorun {third}\n(a 2)");
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("base", base));
    REQUIRE(cfg.push_layer("top", top));
    REQUIRE(cfg.push_layer("overlay", *read_string("(a 3)", "overlay")));  // none of its own
    auto heads = [&] {
        std::vector<std::string> out;
        for (const Node& n : cfg.autorun()) out.emplace_back(to_string(n));
        return out;
    };
    CHECK(heads() == std::vector<std::string>{"{first}", "{second}", "{third}"});
    CHECK(cfg.session().autorun().empty());  // the layers' sessions hold them, not this one

    if constexpr (!kShipping) {
        REQUIRE(cfg.set_layer_enabled("top", false));
        CHECK(heads() == std::vector<std::string>{"{first}"});
        REQUIRE(cfg.set_layer_enabled("top", true));
        dir.write("top.dta", "#autorun {fourth}\n(a 2)");
        REQUIRE(cfg.reload());
        CHECK(heads() == std::vector<std::string>{"{first}", "{fourth}"});  // the new read's
    }
}

TEST_CASE("a tweak's path made at runtime is kept by the symbol") {
    TweakRegistry reg;
    std::vector<std::unique_ptr<Tweak<float>>> tunables;  // as a scripting host keeps them
    {
        std::string built = "enemy.";
        built += "speed";  // made at runtime, and gone after this block
        tunables.push_back(std::make_unique<Tweak<float>>(Symbol(built), 1.0f, TweakMeta{}, reg));
        const std::string_view desc_view = "Enemy speed";
        tunables.push_back(std::make_unique<Tweak<float>>(
            std::string_view(built.data(), 5), 2.0f,  // a view converts to a Symbol: "enemy"
            TweakMeta{.desc = Symbol(desc_view).str()}, reg));
        built.assign(built.size(), 'x');  // the buffer the paths came from is overwritten
    }
    CHECK(tunables[0]->path() == "enemy.speed");
    CHECK(tunables[1]->path() == "enemy");
    CHECK(tunables[1]->desc() == "Enemy speed");
    TempDir dir;
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", "(enemy (speed 4.5))\n")));
    CHECK(float(*tunables[0]) == 4.5f);
    CHECK(tunables[0]->bound());
    CHECK(ok(tunables[0]->source()).line == 1);
    tunables.clear();  // detached, the registry empty again
    CHECK(reg.size() == 0);
}

TEST_CASE("a Config lists the files its stack was read from, for a host's own watcher") {
    test::TempDir dir;
    const std::string base = dir.write("base.dta", "#include parts.dta\n(speed 1)\n");
    dir.write("parts.dta", "(jump 2)\n");
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", base));
    REQUIRE(cfg.push_layer("local", dir.path("local.dta"), {.optional = true}));
    REQUIRE(cfg.push_layer_text("received/patch.dta", "(speed 3)"));  // no file

    auto listed = [&] {
        std::vector<std::string> out;
        for (const StackFile& f : cfg.stack_files())
            out.push_back(std::string(f.layer.str()) + " " +
                          std::filesystem::path(std::string(f.path.str())).filename().string() +
                          (f.present ? "" : " (missing)"));
        return out;
    };
    CHECK(listed() == std::vector<std::string>{"default base.dta", "default parts.dta",
                                               "local local.dta (missing)"});
    const uint64_t epoch = cfg.stack_files_epoch();

    if constexpr (!kShipping) {
        // A reload that reads the same files keeps the list and its epoch.
        dir.write("parts.dta", "(jump 3)\n");
        REQUIRE(cfg.reload());
        CHECK(cfg.get<int>("jump") == 3);
        CHECK(cfg.stack_files_epoch() == epoch);

        // One that reads other files moves it.
        dir.write("more.dta", "(lives 3)\n");
        dir.write("parts.dta", "#include more.dta\n(jump 3)\n");
        REQUIRE(cfg.reload());
        CHECK(cfg.stack_files_epoch() != epoch);
        CHECK(listed() == std::vector<std::string>{"default base.dta", "default parts.dta",
                                                   "default more.dta",
                                                   "local local.dta (missing)"});

        // A layer's file appearing, and a layer leaving, move it too.
        const uint64_t before = cfg.stack_files_epoch();
        dir.write("local.dta", "(speed 9)\n");
        REQUIRE(cfg.reload());
        CHECK(cfg.stack_files_epoch() != before);
        CHECK(listed().back() == "local local.dta");
        REQUIRE(cfg.remove_layer("local"));
        CHECK(listed().size() == 3);
    }
}

#if !BAKER_SHIPPING
TEST_CASE("write-back keeps an integer token's form when a float value is whole") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta",
                                                "(player (height 96) (speed 2.5) (lives 3) "
                                                "(jump 4) (huge 1) (weird 1))\n")));
    REQUIRE(cfg.set("player.height", Node(120.0)));  // a host's float field
    REQUIRE(cfg.set("player.speed", Node(3.0)));     // a float token stays a float
    REQUIRE(cfg.set("player.lives", Node(3.0)));     // the same number: no edit
    REQUIRE(cfg.set("player.jump", Node(4.5)));      // a fraction is written as one
    REQUIRE(cfg.set("player.huge", Node(1e30)));     // no int64 holds it
    REQUIRE(cfg.set("player.weird", Node(std::numeric_limits<double>::infinity())));
    REQUIRE(cfg.promote("scratch", "default"));
    CHECK(dir.read("default.dta") ==
          "(player (height 120) (speed 3.0) (lives 3) (jump 4.5) (huge 1e+30) (weird inf))\n");
    CHECK(cfg.get<double>("player.height") == 120.0);  // reads the same either way
    CHECK(cfg.get<int>("player.height") == 120);

    // write_back alike.
    REQUIRE(cfg.set("player.height", Node(150.0)));
    REQUIRE(cfg.write_back("player.height"));
    CHECK(dir.read("default.dta").rfind("(player (height 150) (speed 3.0)", 0) == 0);
}
#endif

TEST_CASE(
    "a tool keeps a Config and reloads: unchanged layers are kept, the result a fresh load's "
    "") {
    if constexpr (kShipping) return;  // reload() is refused
    std::map<std::string, std::string> files = {{"mem/default.dta", "(a 1)\n(b 2)\n"},
                                                {"mem/local.dta", "(b 3)\n"}};
    const LoadSession::Reader reader = [&](std::string_view p, TextSink out) {
        const auto it = files.find(std::string(p));
        if (it == files.end()) return false;
        out(it->second);
        return true;
    };
    auto open = [&](Config& cfg) -> Expected<void> {
        cfg.session().set_reader(reader);
        if (auto r = cfg.push_layer("default", "mem/default.dta"); !r) return r;
        return cfg.push_layer("local", "mem/local.dta");
    };
    Config kept({.tweaks = nullptr});
    REQUIRE(open(kept));
    const Ref<Array> below = kept.layer_root("default");

    // An edit to the top layer, by its text alone: no file times.
    files["mem/local.dta"] = "(b 4)\n(c 5)\n";
    REQUIRE(ok(kept.reload()));
    CHECK(kept.layer_root("default").get() == below.get());  // not read again
    Config fresh({.tweaks = nullptr});
    REQUIRE(open(fresh));
    CHECK(to_string(*kept.root()) == to_string(*fresh.root()));
    CHECK(kept.source("b")->line == fresh.source("b")->line);

    // Text that does not parse: the same error a fresh load gives, and the next edit reads.
    files["mem/local.dta"] = "(b 4\n";
    const auto broken = kept.reload();
    REQUIRE_FALSE(broken);
    Config fresh_broken({.tweaks = nullptr});
    const auto fresh_error = open(fresh_broken);
    REQUIRE_FALSE(fresh_error);
    CHECK(to_string(broken.error()) == to_string(fresh_error.error()));
    files["mem/local.dta"] = "(b 6)\n";
    REQUIRE(ok(kept.reload()));
    CHECK(kept.get<int>("b") == 6);
    CHECK(kept.layer_root("default").get() == below.get());
}
