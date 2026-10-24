// M3b: reload telemetry, layer options and toggles, scratch, layer effect and diff,
// write_back_to insertion, promote, and snapshots.
#include <doctest.h>
#include <baker/cache.hpp>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>
#include <baker/write.hpp>

#include "alloc_counter.hpp"
#include "temp_dir.hpp"

#include <string>
#include <vector>
#include "dev_only.hpp"
#include "ok.hpp"

using namespace baker;
using baker::test::diff_lines;
using baker::test::ok;
using baker::test::TempDir;

namespace {

// Advances a fixed step per call, so every stage's duration is a known multiple of it.
struct FakeClock {
    uint64_t t = 0;
    uint64_t step = 10;
    static uint64_t tick(void* self) {
        auto* c = static_cast<FakeClock*>(self);
        return c->t += c->step;
    }
};

[[maybe_unused]] Ref<Array> parse(const char* text) {
    auto r = read_string(text, "overlay.dta");
    REQUIRE(r);
    return *r;
}

[[maybe_unused]] std::vector<ReloadRecord> history(const Config& cfg) {
    std::vector<ReloadRecord> out;
    cfg.each_reload([&](const ReloadRecord& r) { out.push_back(r); });
    return out;
}

const char* const kBase =
    "; shared defaults\n"
    "(box\n"
    "   (width 2 (note \"2-3 reads well\"))   ; tuned in playtest 4\n"
    "   (height 1))\n"
    "(audio (volume 0.8) (channels 64))\n";

}  // namespace

// --- Change 1: reload telemetry ---

#if !BAKER_SHIPPING
TEST_CASE("a reload's six stages are timed exactly with a fake time source") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", "(speed 1)\n")));
    dir.write("default.dta", "(speed 2)\n");
    FakeClock clock;
    cfg.set_time_source(FakeClock::tick, &clock);
    REQUIRE(cfg.reload({.trigger = "watcher"_sym, .origin_ns = 3}) == true);

    // One file: begin, scan end, read start and end, parse end, merge, bind, notify.
    const auto r = cfg.last_reload();
    REQUIRE(r);
    CHECK(r->trigger == Symbol("watcher"));
    CHECK(r->origin_ns == 3);
    CHECK(r->start_ns == 10);
    CHECK(r->scan_ns == 10);
    CHECK(r->read_ns == 10);
    CHECK(r->parse_ns == 20);
    CHECK(r->merge_ns == 10);
    CHECK(r->bind_ns == 10);
    CHECK(r->notify_ns == 10);
    CHECK(r->end_ns == 80);  // the end of the notify stage: the stages tile the reload
    CHECK(r->files_reloaded == 1);
    CHECK(r->first_file.str().ends_with("default.dta"));
    CHECK(r->outcome == ReloadOutcome::Ok);
    CHECK(r->epoch == cfg.epoch());

    REQUIRE(cfg.reload() == false);
    CHECK(cfg.last_reload()->outcome == ReloadOutcome::Unchanged);
    CHECK(cfg.last_reload()->trigger == Symbol("reload"));
    CHECK(cfg.last_reload()->epoch == r->epoch);
}

TEST_CASE("a write-back's stages tile its record, its write included") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", "(speed 1)\n")));
    REQUIRE(cfg.set("speed", Node(2)));
    FakeClock clock;
    cfg.set_time_source(FakeClock::tick, &clock);
    REQUIRE(cfg.write_back("speed"));
    const auto r = cfg.last_reload();
    REQUIRE(r);
    CHECK(r->trigger == Symbol("write_back"));
    CHECK(r->write_ns == 10);  // from the record's start to the end of the write
    CHECK(r->scan_ns + r->write_ns + r->read_ns + r->parse_ns + r->merge_ns + r->bind_ns +
              r->notify_ns ==
          r->end_ns - r->start_ns);

    REQUIRE(cfg.set("speed", Node(3)));
    REQUIRE(cfg.promote("scratch", "default"));
    const auto p = cfg.last_reload();
    REQUIRE(p);
    CHECK(p->write_ns == 10);
    CHECK(p->scan_ns + p->write_ns + p->read_ns + p->parse_ns + p->merge_ns + p->bind_ns +
              p->notify_ns ==
          p->end_ns - p->start_ns);
    CHECK(dir.read("default.dta") == "(speed 3)\n");
}

TEST_CASE("promote writes what the Config reads, however the source repeats a tag") {
    TempDir dir;
    for (const int keys : {4, 40}) {
        Config cfg({.tweaks = nullptr});
        REQUIRE(cfg.push_layer("default", dir.write("default.dta", "(k0 0)\n")));
        std::string text;
        for (int i = 0; i < keys; ++i)
            text += "(k" + std::to_string(i) + " " + std::to_string(i * 10) + ")\n";
        // A repeated key folds into the first, its values winning; a repeated section
        // brings its own keys.
        text += "(k1 99)\n(sec (a 1))\n(sec (b 2))\n";
        REQUIRE(cfg.push_layer("top", parse(text.c_str())));
        REQUIRE(cfg.get<int>("k1").value_or(-1) == 99);
        REQUIRE(cfg.get<int>("sec.b").value_or(-1) == 2);
        REQUIRE(cfg.promote("top", "default"));
        const std::string after = dir.read("default.dta");
        CHECK(after.find("(k1 99)") != std::string::npos);
        CHECK(after.find("(k1 10)") == std::string::npos);
        CHECK(after.find("(a 1)") != std::string::npos);
        CHECK(after.find("(b 2)") != std::string::npos);
        // The target alone now reads as the stack did.
        REQUIRE(cfg.set_layer_enabled("top", false));
        CHECK(cfg.get<int>("k1").value_or(-1) == 99);
        CHECK(cfg.get<int>("sec.a").value_or(-1) == 1);
        CHECK(cfg.get<int>("sec.b").value_or(-1) == 2);
    }
}

TEST_CASE("a layer removes a key from the layers below with (key #remove)") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("base", dir.write("base.dta",
                                             "(menu (debug_menu 1) (items play options credits))\n"
                                             "(enemies (grunt (hp 10)) (exploit_boss (hp 1)))\n"
                                             "(cheats (god 1))\n")));
    REQUIRE(cfg.push_layer("tu", dir.write("tu.dta",
                                           "(menu (debug_menu #remove))\n"
                                           "(enemies (exploit_boss #remove))\n"
                                           "(cheats #remove)\n(missing #remove)\n")));
    CHECK(to_string(*cfg.root()) ==
          "((menu (items play options credits)) (enemies (grunt (hp 10))))");
    CHECK_FALSE(cfg.get<int>("menu.debug_menu"));
    CHECK(cfg.get<int>("enemies.grunt.hp") == 10);

    // Removing the layer brings the keys back; a layer above may set a key again.
    REQUIRE(cfg.push_layer("dlc", dir.write("dlc.dta", "(cheats (god 2))\n")));
    CHECK(cfg.get<int>("cheats.god") == 2);
    REQUIRE(cfg.remove_layer("tu"));
    CHECK(cfg.get<int>("menu.debug_menu") == 1);
    CHECK(cfg.get<int>("enemies.exploit_boss.hp") == 1);

    // A key new to the stack keeps none of the removals inside it.
    REQUIRE(cfg.push_layer("season", dir.write("season.dta", "(event (x 1) (y #remove))\n")));
    CHECK(to_string(*ok(cfg.root()->find_array("event"))) == "(x 1)");

    // A layer read from a cache keeps its removals: the flag is in the array header.
    auto layer = ok(read_string("(menu (debug_menu #remove))", "tu.dta"));
    CHECK(layer->has_removal());
    std::string bytes;
    REQUIRE(write_cache(*layer, string_out(bytes)));
    auto cached = ok(read_cache_bytes(bytes, "tu.datb"));
    CHECK(cached->has_removal());
    CHECK(to_string(*merge(*ok(read_string("(menu (debug_menu 1) (x 2))", "b.dta")), *cached)) ==
          "((menu (x 2)))");

    // A marker in the bottom layer removes nothing and is not in the tree.
    Config alone({.tweaks = nullptr});
    REQUIRE(alone.push_layer("only", dir.write("only.dta", "(a (b #remove) (c 1))")));
    CHECK(to_string(*alone.root()) == "((a (c 1)))");
}

TEST_CASE("#remove is a key's only value, prints back, and #merge ignores it") {
    CHECK(to_string(*ok(read_string("(a (b #remove))", "r.dta"))) == "((a (b #remove)))");
    // Elsewhere the symbol is quoted, so it reads back (found by the fuzzer).
    for (const char* text : {"('#remove')", "(a '#remove' 1)", "{f '#remove'}", "'#remove'"}) {
        auto tree = ok(read_string(text, "q.dta"));
        CHECK(structurally_equal(*tree, *ok(read_string(to_dta(*tree), "back.dta"))));
    }
    for (const char* bad : {"(a #remove 1)", "(a 1 #remove)", "(#remove)", "#remove", "{f #remove}",
                            "(a (b #remove) #remove)"}) {
        auto r = read_string(bad, "bad.dta");
        REQUIRE_FALSE(r);
        CHECK(std::string(r.error().message()).find("(key #remove)") != std::string::npos);
    }
    TempDir dir;
    dir.write("over.dta", "(a (b #remove) (c 2))");
    const std::string main = dir.write("main.dta", "(a (b 1))\n#merge over.dta\n");
    CHECK(to_string(*ok(read_file(main))) == "((a (b 1) (c 2)))");
}

TEST_CASE("a layer can be placed below or above another") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("base", dir.write("base.dta", "(songs (song_a (bpm 120)))\n")));
    REQUIRE(cfg.push_layer("tu2", dir.write("tu2.dta", "(songs (song_c (bpm 141)))\n")));
    // A DLC installed after the title update still goes below it, so the fix wins.
    const std::string pack1 = dir.write("pack1.dta", "(songs (song_c (bpm 140) (title c)))\n");
    REQUIRE(cfg.push_layer("pack1", pack1, {.below = "tu2"}));
    CHECK(strings(cfg.layers()) == std::vector<std::string>{"base", "pack1", "tu2", "scratch"});
    CHECK(cfg.get<int>("songs.song_c.bpm") == 141);
    CHECK(cfg.get<Symbol>("songs.song_c.title") == Symbol("c"));

    // A layer placed under others is read first, and they are read again with its macros.
    REQUIRE(cfg.push_layer("top", dir.write("top.dta",
                                            "#ifdef HARD\n(level hard)\n#else\n"
                                            "(level easy)\n#endif\n")));
    CHECK(cfg.get<Symbol>("level") == Symbol("easy"));
    REQUIRE(cfg.push_layer("mode", dir.write("mode.dta", "#define HARD\n"), {.above = "pack1"}));
    CHECK(strings(cfg.layers()) ==
          std::vector<std::string>{"base", "pack1", "mode", "tu2", "top", "scratch"});
    CHECK(cfg.get<Symbol>("level") == Symbol("hard"));

    // An overlay can be placed too; "scratch" as the anchor is the top.
    REQUIRE(cfg.push_layer("hotfix", ok(read_string("(songs (song_a (bpm 121)))", "h.dta")),
                           {.below = "base"}));
    CHECK(cfg.get<int>("songs.song_a.bpm") == 120);  // below base: base wins
    REQUIRE(cfg.push_layer("live", ok(read_string("(songs (song_a (bpm 122)))", "l.dta")),
                           {.below = "scratch"}));
    CHECK(cfg.get<int>("songs.song_a.bpm") == 122);

    CHECK(cfg.push_layer("x", pack1, {.below = "nope"}).error().code() == ErrorCode::NotFound);
    CHECK(cfg.push_layer("y", pack1, {.below = "base", .above = "tu2"}).error().code() ==
          ErrorCode::Refused);
    CHECK(strings(cfg.layers()).size() == 8);  // neither refusal changed the stack
}

TEST_CASE("telemetry adds no allocation per reload once the history exists") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", "(speed 1)\n")));
    auto allocations_for = [&](size_t history) {
        cfg.set_reload_history(history);
        for (int i = 0; i < 3; ++i) (void)cfg.reload();  // fills (and wraps) the ring
        test::AllocationScope scope;
        for (int i = 0; i < 20; ++i) (void)cfg.reload();
        return scope.count();
    };
    if (!test::counting_allocations()) return;
    const size_t without = allocations_for(0);
    const size_t with = allocations_for(2);
    CHECK(with == without);
    CHECK(history(cfg).size() == 2);
}

TEST_CASE("a reload that finds nothing changed allocates nothing") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    REQUIRE(cfg.reload());  // warm: the scan buffer
    test::AllocationScope scope;
    for (int i = 0; i < 10; ++i) CHECK(cfg.reload() == false);
    if (test::counting_allocations()) CHECK(scope.count() == 0);
}

BAKER_DEV_TEST_CASE("a merge copies no session when the top layer's is unchanged") {
    auto allocations_for = [](int macros) {
        TempDir dir;
        std::string defs;
        for (int i = 0; i < macros; ++i)
            defs += "#define M" + std::to_string(i) + " ((v " + std::to_string(i) + "))\n";
        Config cfg({.tweaks = nullptr});
        REQUIRE(cfg.push_layer("defs", dir.write("defs.dta", defs + "(a 1)")));
        REQUIRE(cfg.push_layer("over", parse("(a 2)")));
        REQUIRE(cfg.set_layer_enabled("over", false));
        REQUIRE(cfg.set_layer_enabled("over", true));
        REQUIRE(cfg.set_layer_enabled("over", false));
        size_t count = 0;
        {
            const test::AllocationScope scope;  // no doctest assertion inside: they allocate
            (void)cfg.set_layer_enabled("over", true);
            count = scope.count();
        }
        CHECK(cfg.read_session().is_defined(Symbol("M0")));
        return count;
    };
    if (test::counting_allocations()) CHECK(allocations_for(100) == allocations_for(5));
}

TEST_CASE("the history ring wraps, keeping the newest records in order") {
    Config cfg({.tweaks = nullptr});
    cfg.set_reload_history(3);
    for (int i = 0; i < 5; ++i) REQUIRE(cfg.push_layer("l" + std::to_string(i), parse("(x 1)")));
    const auto h = history(cfg);
    REQUIRE(h.size() == 3);
    CHECK(h[0].epoch + 1 == h[1].epoch);
    CHECK(h[1].epoch + 1 == h[2].epoch);
    CHECK(h[2].epoch == cfg.epoch());
    CHECK(h[2].trigger == Symbol("layer"));
    CHECK(h[2].scan_ns == 0);  // a layer change reads no files
    CHECK(h[2].read_ns == 0);
    CHECK(h[2].parse_ns == 0);
}

TEST_CASE("a parse error records ParseError and changes nothing") {
    TempDir dir;
    TweakRegistry reg;
    Tweak<int> speed{"speed", 0, {}, reg};
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", "(speed 1)\n")));
    dir.write("default.dta", "(speed 2\n");
    FakeClock clock;
    cfg.set_time_source(FakeClock::tick, &clock);
    CHECK_FALSE(cfg.reload());
    const auto r = cfg.last_reload();
    REQUIRE(r);
    CHECK(r->outcome == ReloadOutcome::ParseError);
    CHECK(r->bind_ns == 0);
    CHECK(r->notify_ns == 0);
    CHECK(int(speed) == 1);
}

TEST_CASE("records count migrations and punts, keep the origin, and reach the host") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("d", dir.write("d.dta", "(a (x 1)) (b (x 1)) (c (x 1))\n")));
    int rebuilds = 0;
    cfg.on_reload("a", [&] { ++rebuilds; });  // punts
    cfg.on_reload("b", [&] { ++rebuilds; }, [] { return Reload::Migrated; });
    cfg.on_reload("c", [&] { ++rebuilds; }, [] { return Reload::Punt; });
    std::vector<ReloadRecord> seen;
    const auto id = cfg.on_reload_complete([&](const ReloadRecord& r) { seen.push_back(r); });
    dir.write("d.dta", "(a (x 2)) (b (x 2)) (c (x 2))\n");
    REQUIRE(cfg.reload({.trigger = "console"_sym, .origin_ns = 12345}));
    const auto r = cfg.last_reload();
    CHECK(r->migrated == 1);
    CHECK(r->punted == 2);
    CHECK(rebuilds == 2);
    CHECK(r->origin_ns == 12345);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].trigger == Symbol("console"));
    cfg.remove_on_reload(id);
    REQUIRE(cfg.push_layer("p", parse("(a (x 3))")));
    CHECK(seen.size() == 1);
}
#else
TEST_CASE("under BAKER_SHIPPING telemetry is empty but its API stays") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("d", dir.write("d.dta", "(a 1)\n")));
    CHECK_FALSE(cfg.last_reload());
    int visited = 0;
    cfg.each_reload([&](const ReloadRecord&) { ++visited; });
    CHECK(visited == 0);
    CHECK(cfg.toggle_layer("d").error().code() == ErrorCode::Refused);
    CHECK(cfg.set("a", Node(2)).error().code() == ErrorCode::Refused);
    CHECK(cfg.promote("d", "d").error().code() == ErrorCode::Refused);
}
#endif

// --- Change 2: layers, scratch, effect, diff, write_back_to, promote ---

#if !BAKER_SHIPPING
TEST_CASE("toggling a layer off and on restores every value and source exactly") {
    TempDir dir;
    TweakRegistry reg;
    Tweak<float> width{"box.width", 0, {}, reg};
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    REQUIRE(cfg.push_layer("experiment",
                           dir.write("exp.dta", "(box (width 3)) (audio (volume 0.5))\n")));
    const Ref<Array> with = cfg.root();
    const Source s_width = *cfg.source("box.width");
    CHECK(s_width.layer == "experiment");
    CHECK(float(width) == 3.0f);

    CHECK(cfg.toggle_layer("experiment") == false);
    CHECK_FALSE(cfg.layer_enabled("experiment"));
    CHECK(float(width) == 2.0f);
    CHECK(cfg.source("box.width")->layer == "default");  // never a disabled layer
    CHECK(cfg.get<double>("audio.volume") == 0.8);
    CHECK(cfg.last_reload()->trigger == Symbol("layer"));

    CHECK(cfg.toggle_layer("experiment") == true);
    CHECK(structurally_equal(*cfg.root(), *with));
    const Source back = *cfg.source("box.width");
    CHECK(back.layer == s_width.layer);
    CHECK(back.file == s_width.file);
    CHECK(back.line == s_width.line);
    CHECK(float(width) == 3.0f);
    CHECK(strings(cfg.layers()) == std::vector<std::string>{"default", "experiment", "scratch"});
    CHECK(cfg.toggle_layer("nope").error().code() == ErrorCode::NotFound);

    // A layer pushed disabled keeps its place but contributes nothing.
    REQUIRE(cfg.push_layer("off", parse("(box (width 9))"), {.enabled = false}));
    CHECK(float(width) == 3.0f);
}

TEST_CASE("an optional layer with a missing file is empty, and appears on reload") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    REQUIRE(cfg.push_layer("local", dir.path("local/sam.dta"), {.optional = true}));
    CHECK(cfg.get<int>("box.height") == 1);
    CHECK(cfg.push_layer("bad", dir.path("missing.dta")).error().code() == ErrorCode::Io);
    dir.write("local/sam.dta", "(box (height 5))\n");
    REQUIRE(cfg.reload() == true);
    CHECK(cfg.get<int>("box.height") == 5);
    CHECK(cfg.source("box.height")->layer == "local");
}

TEST_CASE("set on a scalar already in scratch allocates nothing and touches only its tweak") {
    TempDir dir;
    TweakRegistry reg;
    Tweak<float> width{"box.width", 0, {}, reg};
    Tweak<int> height{"box.height", 0, {}, reg};
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    int width_changes = 0, height_changes = 0;
    width.on_change([&] { ++width_changes; });
    height.on_change([&] { ++height_changes; });

    REQUIRE(cfg.set("box.width", Node(2.5)));  // first set: creates the scratch entry
    CHECK(float(width) == 2.5f);
    const uint64_t epoch = cfg.epoch();
    const auto record = cfg.last_reload();
    test::AllocationScope scope;
    REQUIRE(cfg.set("box.width", Node(2.75)));
    const size_t allocations = scope.count();
    if (test::counting_allocations()) CHECK(allocations == 0);
    CHECK(float(width) == 2.75f);
    CHECK(width_changes == 2);
    CHECK(height_changes == 0);
    CHECK(cfg.epoch() == epoch + 1);  // a live edit, though not a full re-merge
    CHECK(cfg.last_reload()->epoch == record->epoch);
    CHECK(cfg.get<double>("box.width") == 2.75);  // lookups see scratch first
    CHECK(cfg.source("box.width")->layer == "scratch");
    CHECK(cfg.root()->get<double>("box.width") == 2.0);  // root(): as of the last merge

    // Toggling scratch is a full re-merge like any layer.
    CHECK(cfg.toggle_layer("scratch") == false);
    CHECK(float(width) == 2.0f);
    CHECK(cfg.get<double>("box.width") == 2.0);
    CHECK(cfg.toggle_layer("scratch") == true);
    CHECK(float(width) == 2.75f);
    REQUIRE(cfg.clear_scratch("box.width"));
    CHECK(float(width) == 2.0f);
    CHECK(cfg.unsaved().empty());
}

TEST_CASE("every live edit moves the epoch, with the journal off too") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    cfg.set_journal_history(0);
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    REQUIRE(cfg.push_layer("exp", dir.write("exp.dta", "(audio (volume 0.6))")));
    uint64_t epoch = cfg.epoch();
    const auto moved = [&] {
        const bool m = cfg.epoch() != epoch;
        epoch = cfg.epoch();
        return m;
    };
    REQUIRE(cfg.set("box.width", Node(2.5)));
    CHECK(moved());
    REQUIRE(cfg.set("box.width", Node(2.5)));  // the fast path, the same value again
    CHECK(moved());
    const ConfigSnapshot snap = cfg.snapshot();
    CHECK_FALSE(moved());
    REQUIRE(cfg.clear_scratch("box.width"));
    CHECK(moved());
    REQUIRE(cfg.restore(snap));
    CHECK(moved());
    REQUIRE(cfg.write_back("box.width"));
    CHECK(moved());
    REQUIRE(cfg.promote("exp", "default"));
    CHECK(moved());
    CHECK_FALSE(moved());  // nothing changed, nothing moves
}

TEST_CASE("a shorter scratch value hides the layers' longer one") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", "(size 1 2 3)")));
    const Node five(5);
    REQUIRE(cfg.set(Path("size"), std::span<const Node>(&five, 1)));
    CHECK(cfg.get<int>("size.0") == 5);
    auto past = cfg.get<int>("size.2");
    REQUIRE_FALSE(past);
    CHECK(past.error().code() == ErrorCode::NotFound);
    CHECK(cfg.get<int>("size.2", 9) == 9);
}

TEST_CASE("set while scratch is disabled waits for it to be enabled") {
    TempDir dir;
    TweakRegistry reg;
    Tweak<int> w{"w", 0, {}, reg};
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", "(w 7)")));
    REQUIRE(cfg.set_layer_enabled("scratch", false));
    REQUIRE(cfg.set("w", Node(99)));
    CHECK(int(w) == 7);
    CHECK(cfg.get<int>("w") == 7);
    REQUIRE(cfg.set("w", Node(98)));  // the fast path, now the entry exists
    CHECK(int(w) == 7);
    REQUIRE(cfg.set_layer_enabled("scratch", true));
    CHECK(int(w) == 98);
    CHECK(cfg.get<int>("w") == 98);
}

TEST_CASE("set then write_back: one line, comments and note kept, scratch emptied") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    REQUIRE(cfg.set("box.width", Node(2.4)));
    CHECK(strings(cfg.unsaved()) == std::vector<std::string>{"box/width"});
    const std::string before = dir.read("default.dta");
    auto report = cfg.write_back("box.width");
    REQUIRE(report);
    CHECK(diff_lines(before, dir.read("default.dta")) ==
          std::vector<std::string>{
              "-   (width 2 (note \"2-3 reads well\"))   ; tuned in playtest 4",
              "+   (width 2.4 (note \"2-3 reads well\"))   ; tuned in playtest 4"});
    REQUIRE(report->written.size() == 1);
    CHECK(report->written[0].file.str().ends_with("default.dta"));
    CHECK(report->written[0].line == 3);
    CHECK(cfg.unsaved().empty());
    const Source s = *cfg.source("box.width");
    CHECK(s.layer == "default");
    CHECK(s.line == 3);
    CHECK(cfg.last_reload()->trigger == Symbol("write_back"));
    CHECK(cfg.last_reload()->files_reloaded == 1);
}

TEST_CASE("write_back_to creates an absent personal file, or inserts beside siblings") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    REQUIRE(cfg.push_layer("local", dir.path("local/sam.dta"), {.optional = true}));
    REQUIRE(cfg.set("box.width", Node(4)));
    auto r = cfg.write_back_to("box.width", "local");
    REQUIRE(r);
    CHECK(dir.read("local/sam.dta") == "(box (width 4))\n");
    CHECK(cfg.source("box.width")->layer == "local");
    CHECK(cfg.unsaved().empty());

    // Into a populated file: a multi-line ancestor gets a new line indented like its
    // last child; nothing else changes.
    const std::string mine =
        "; mine\n"
        "(audio\n"
        "   (channels 2)   ; quiet\n"
        "   (rate 44100))\n"
        "(box (width 4))\n";
    dir.write("local/sam.dta", mine);
    REQUIRE(cfg.reload());
    REQUIRE(cfg.set("audio.volume", Node(0.25)));
    REQUIRE(cfg.write_back_to("audio.volume", "local"));
    CHECK(dir.read("local/sam.dta") ==
          "; mine\n"
          "(audio\n"
          "   (channels 2)   ; quiet\n"
          "   (rate 44100)\n"
          "   (volume 0.25))\n"
          "(box (width 4))\n");
    // A one-line ancestor gets the entry on its line.
    REQUIRE(cfg.set("box.height", Node(7)));
    REQUIRE(cfg.write_back_to("box.height", "local"));
    CHECK(dir.read("local/sam.dta").ends_with("(box (width 4) (height 7))\n"));
    // A missing positional segment is refused, located.
    auto bad = cfg.write_back_to("box.width.3", "local");
    CHECK_FALSE(bad);

    // A multi-line ancestor whose last child is followed by a comment line.
    dir.write("local/sam.dta", "(audio\n   (channels 2) ; quiet\n)\n");
    REQUIRE(cfg.reload());
    REQUIRE(cfg.set("audio.volume", Node(0.5)));
    REQUIRE(cfg.write_back_to("audio.volume", "local"));
    CHECK(dir.read("local/sam.dta") == "(audio\n   (channels 2) ; quiet\n   (volume 0.5)\n)\n");
}

TEST_CASE("write-back refuses a file changed on disk since it was read") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    REQUIRE(cfg.push_layer("local", dir.write("local.dta", "(box (height 3))\n")));
    dir.write("local.dta", "(box (height 3)) ; edited by hand\n");
    REQUIRE(cfg.set("box.width", Node(5)));
    auto r = cfg.write_back_to("box.width", "local");
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::Refused);
    CHECK(r.error().file().str().ends_with("local.dta"));
    CHECK(dir.read("local.dta") == "(box (height 3)) ; edited by hand\n");
}

TEST_CASE("promote writes all entries or none, and reports every refusal") {
    TempDir dir;
    dir.write("net.dta", "(net (port 80))\n");
    const std::string base = "#include net.dta\n(box (width 2))\n";
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", base)));
    REQUIRE(cfg.push_layer("exp", dir.write("exp.dta",
                                            "(box (width 3) (depth 4))\n"
                                            "(net (timeout 5))\n"
                                            "(net2 (x 1))\n")));
    auto r = cfg.promote("exp", "default");
    REQUIRE(r);
    CHECK_FALSE(r->ok());
    REQUIRE(r->refused.size() == 1);  // net's array comes from net.dta, not default.dta
    CHECK(r->refused[0].code() == ErrorCode::Refused);
    CHECK(r->refused[0].file().str().ends_with("net.dta"));
    CHECK(r->refused[0].line() == 1);
    CHECK(r->written.empty());
    CHECK(dir.read("default.dta") == base);  // nothing written

    // Without the unwritable entry, every entry lands, as one reviewable diff.
    dir.write("exp.dta", "(box (width 3) (depth 4))\n(net2 (x 1))\n");
    REQUIRE(cfg.reload());
    r = cfg.promote("exp", "default");
    REQUIRE(r);
    CHECK(r->ok());
    CHECK(r->written.size() == 3);
    CHECK(dir.read("default.dta") == "#include net.dta\n(box (width 3) (depth 4))\n(net2 (x 1))\n");
    CHECK(strings(cfg.layers()) ==
          std::vector<std::string>{"default", "exp", "scratch"});  // exp stays
    REQUIRE(cfg.remove_layer("exp"));
    CHECK(cfg.get<int>("box.depth") == 4);
    CHECK(cfg.source("box.depth")->layer == "default");
}

TEST_CASE("restore returns every value and source to the snapshot") {
    TempDir dir;
    TweakRegistry reg;
    Tweak<float> width{"box.width", 0, {}, reg};
    Tweak<int> height{"box.height", 0, {}, reg};
    Tweak<double> volume{"audio.volume", 0, {}, reg};
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    REQUIRE(cfg.push_layer("exp", parse("(audio (volume 0.6))")));
    REQUIRE(cfg.set("box.height", Node(4)));
    const ConfigSnapshot snap = cfg.snapshot();
    REQUIRE(cfg.restore(snap));  // a full merge, so root() includes the fast-path set
    const std::vector<const char*> paths{"box.width", "box.height", "audio.volume",
                                         "audio.channels"};
    std::vector<Source> sources;
    for (const char* p : paths) sources.push_back(*cfg.source(p));
    const Ref<Array> root = cfg.root();
    int box_rebuilds = 0, audio_rebuilds = 0;
    cfg.on_reload("box", [&] { ++box_rebuilds; });
    cfg.on_reload("audio", [&] { ++audio_rebuilds; });

    REQUIRE(cfg.push_layer("more", parse("(box (width 8))")));
    REQUIRE(cfg.toggle_layer("exp"));
    REQUIRE(cfg.set("box.width", Node(9)));
    REQUIRE(cfg.set("box.height", Node(10)));
    REQUIRE(cfg.set("audio.channels", Node(2)));
    box_rebuilds = audio_rebuilds = 0;
    const uint64_t epoch = cfg.epoch();

    REQUIRE(cfg.restore(snap));
    CHECK(box_rebuilds == 1);
    CHECK(audio_rebuilds == 1);
    CHECK(cfg.epoch() == epoch + 1);
    CHECK(cfg.last_reload()->trigger == Symbol("restore"));
    CHECK(structurally_equal(*cfg.root(), *root));
    CHECK(float(width) == 2.0f);
    CHECK(int(height) == 4);
    CHECK(double(volume) == 0.6);
    for (size_t i = 0; i < paths.size(); ++i) {
        CAPTURE(paths[i]);
        const Source s = *cfg.source(paths[i]);
        CHECK(s.layer == sources[i].layer);
        CHECK(s.file == sources[i].file);
        CHECK(s.line == sources[i].line);
    }
    CHECK(strings(cfg.layers()) == std::vector<std::string>{"default", "exp", "scratch"});
    CHECK(strings(cfg.unsaved()) == std::vector<std::string>{"box/height"});
}

TEST_CASE("restore never reads disk, and write-back of restored values checks the file") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    const ConfigSnapshot snap = cfg.snapshot();
    dir.write("default.dta", "(box (width 7))\n");
    REQUIRE(cfg.reload());
    CHECK(cfg.get<int>("box.width") == 7);
    REQUIRE(cfg.restore(snap));
    CHECK(cfg.get<int>("box.width") == 2);  // from the snapshot, not the disk
    REQUIRE(cfg.set("box.width", Node(3)));
    auto r = cfg.write_back("box.width");
    REQUIRE_FALSE(r);
    CHECK(
        r.error().message().ends_with("has changed since it was read; reload before writing back"));
    REQUIRE(cfg.reload());                  // the file differs from the restored state
    CHECK(cfg.get<int>("box.width") == 3);  // scratch still wins
}

TEST_CASE("taking a snapshot copies no array") {
    // Bookkeeping only (the state, the layer list and its strings, one scratch entry), so the
    // cost does not grow with the trees. Compared rather than bounded: debug STLs add their own.
    auto snapshot_allocations = [](int entries) {
        TempDir dir;
        std::string text;
        for (int i = 0; i < entries; ++i)
            text += "(k" + std::to_string(i) + " (v " + std::to_string(i) + "))\n";
        Config cfg({.tweaks = nullptr});
        REQUIRE(cfg.push_layer("big", dir.write("big.dta", text)));
        REQUIRE(cfg.set("k1.v", Node(5)));
        test::AllocationScope scope;
        const ConfigSnapshot snap = cfg.snapshot();
        const size_t allocations = scope.count();
        CHECK_FALSE(snap.empty());
        return allocations;
    };
    const size_t small = snapshot_allocations(2);
    const size_t big = snapshot_allocations(500);
    if (test::counting_allocations()) {
        CHECK(big == small);
        CHECK(small > 0);
    }
}
#endif
