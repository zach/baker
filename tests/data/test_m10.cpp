// M10: harness instrumentation (§10): loop statistics and
// the edit journal, and the repetition report.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/repeats.hpp>
#include <baker/std_interop.hpp>

#include "alloc_counter.hpp"
#include "temp_dir.hpp"

#include <string>
#include <vector>

using namespace baker;
using baker::test::TempDir;

namespace {

// Advances a fixed step per call. A step of 1 µs keeps every sample exact: the statistics
// keep microseconds.
struct FakeClock {
    uint64_t t = 0;
    uint64_t step = 1000;
    static uint64_t tick(void* self) {
        auto* c = static_cast<FakeClock*>(self);
        return c->t += c->step;
    }
};

[[maybe_unused]] std::vector<JournalEntry> journal(const Config& cfg) {
    std::vector<JournalEntry> out;
    cfg.journal([&](const JournalEntry& e) { out.push_back(e); });
    return out;
}

[[maybe_unused]] const LoopRow* row(const List<LoopRow>& rows, std::string_view ends_with) {
    for (const LoopRow& r : rows)
        if (r.name.str().ends_with(ends_with)) return &r;
    return nullptr;
}

}  // namespace

#if !BAKER_SHIPPING
TEST_CASE(
    "loop statistics: edit-to-see per file and per layer, to the reload's end, then to "
    "mark_visible") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("d.dta", "(speed 1)\n")));
    REQUIRE(cfg.push_layer("patch", dir.write("p.dta", "(jump 1)\n")));
    FakeClock clock;
    cfg.set_time_source(FakeClock::tick, &clock);

    // Every stage reads the clock, so a reload's times are whole microseconds.
    clock.t = 100000;
    dir.write("d.dta", "(speed 2)\n");
    REQUIRE(cfg.reload({.trigger = "watcher"_sym, .origin_ns = 50000}) == true);
    const auto rec = cfg.last_reload();
    REQUIRE(rec);
    CHECK(rec->layer == Symbol("default"));
    CHECK(rec->visible_ns == 0);
    const uint64_t first_edit = rec->end_ns - 50000, first_time = rec->end_ns - rec->start_ns;

    LoopStats s = cfg.loop_stats();
    CHECK(s.reloads == 1);
    REQUIRE(s.files.size() == 1);
    CHECK(s.files[0].name.str().ends_with("d.dta"));
    CHECK(s.files[0].reloads == 1);
    CHECK(s.files[0].edits == 1);
    CHECK(s.files[0].edit_median_ns == first_edit);  // to the reload's end: no frame yet
    CHECK(s.files[0].times == 1);
    CHECK(s.files[0].reload_median_ns == first_time);
    REQUIRE(s.layers.size() == 1);
    CHECK(s.layers[0].name == Symbol("default"));
    CHECK(s.layers[0].edit_median_ns == first_edit);

    // The frame that shows it, one tick later.
    cfg.mark_visible();
    CHECK(cfg.last_reload()->visible_ns == rec->end_ns + 1000);
    const uint64_t seen = first_edit + 1000;
    s = cfg.loop_stats();
    CHECK(s.files[0].edit_median_ns == seen);
    CHECK(s.files[0].edit_worst_ns == seen);
    CHECK(s.layers[0].edit_median_ns == seen);
    // Nothing waits now: the next frame reads no clock.
    const uint64_t before = clock.t;
    cfg.mark_visible();
    CHECK(clock.t == before);

    // A host polling a hundred times pushes nothing out.
    for (int i = 0; i < 100; ++i) REQUIRE(cfg.reload() == false);
    s = cfg.loop_stats();
    CHECK(s.reloads == 101);
    CHECK(s.unchanged == 100);
    CHECK(s.files[0].reloads == 1);
    CHECK(s.files[0].edits == 1);

    // Three more edits, saved 1, 5, and 3 µs before their reloads began: with the first,
    // four samples, of which the median is the upper middle one and the worst the first.
    int n = 3;
    std::vector<uint64_t> edits{seen};
    for (int k : {1, 5, 3}) {
        dir.write("d.dta", "(speed " + std::to_string(n++) + ")\n");
        const uint64_t origin = clock.t - uint64_t(k) * 1000;
        REQUIRE(cfg.reload({.origin_ns = origin}) == true);
        edits.push_back(cfg.last_reload()->end_ns - origin);
    }
    CHECK(edits[2] > edits[3]);  // the samples differ as their origins do
    CHECK(edits[3] > edits[1]);
    s = cfg.loop_stats();
    CHECK(s.files[0].reloads == 4);
    CHECK(s.files[0].edits == 4);
    CHECK(s.files[0].edit_median_ns == edits[2]);  // sorted: edits[1], edits[3], edits[2], seen
    CHECK(s.files[0].edit_worst_ns == seen);
    // A reload without an origin counts, and has a reload time, but no edit-to-see.
    dir.write("d.dta", "(speed 9)\n");
    REQUIRE(cfg.reload() == true);
    s = cfg.loop_stats();
    CHECK(s.files[0].reloads == 5);
    CHECK(s.files[0].edits == 4);
    CHECK(s.files[0].times == 5);

    // A failed reload counts against its file, its layer, and the total.
    dir.write("p.dta", "(jump\n");
    CHECK_FALSE(cfg.reload({.origin_ns = clock.t}));
    s = cfg.loop_stats();
    CHECK(s.failures == 1);
    REQUIRE(s.files.size() == 2);
    CHECK(s.files[0].name.str().ends_with("p.dta"));  // most recent first
    CHECK(s.files[0].failures == 1);
    CHECK(s.files[0].edits == 0);
    CHECK(s.files[0].times == 0);
    const LoopRow* patch = row(s.layers, "patch");
    REQUIRE(patch);
    CHECK(patch->failures == 1);
    CHECK(cfg.last_reload()->outcome == ReloadOutcome::ParseError);
}

TEST_CASE("loop statistics keep the 32 most recently changed files") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    std::string main;
    for (int i = 0; i < 33; ++i) {
        dir.write("f" + std::to_string(i) + ".dta", "(f" + std::to_string(i) + " 0)\n");
        main += "#include f" + std::to_string(i) + ".dta\n";
    }
    REQUIRE(cfg.push_layer("default", dir.write("main.dta", main)));
    for (int i = 0; i < 33; ++i) {
        dir.write("f" + std::to_string(i) + ".dta", "(f" + std::to_string(i) + " 1)\n");
        REQUIRE(cfg.reload() == true);
    }
    const LoopStats s = cfg.loop_stats();
    CHECK(s.files.size() == 32);
    CHECK(s.files[0].name.str().ends_with("f32.dta"));
    CHECK_FALSE(row(s.files, "/f0.dta"));  // the least recently changed gave up its row
    CHECK(row(s.files, "/f1.dta"));
    REQUIRE(s.layers.size() == 1);
    CHECK(s.layers[0].reloads == 33);
}

TEST_CASE("mark_visible and a reload that feeds the statistics allocate nothing") {
    if (!test::counting_allocations()) return;
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("d.dta", "(speed 1)\n")));
    dir.write("d.dta", "(speed 2)\n");
    REQUIRE(cfg.reload({.origin_ns = 1}));  // makes the history and the table
    test::AllocationScope scope;
    cfg.mark_visible();
    for (int i = 0; i < 10; ++i) cfg.mark_visible();
    CHECK(scope.count() == 0);
}
TEST_CASE("the edit journal: old and new values, provenance, and each edit's fate") {
    TempDir dir;
    TweakRegistry reg;
    Tweak<float> width{"box.width", 1.0f, {}, reg};
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer(
        "default", dir.write("default.dta", "; shared\n(box\n   (width 2)\n   (height 1))\n")));
    REQUIRE(cfg.push_layer("local", dir.write("local.dta", "(audio (volume 0.5))\n")));
    FakeClock clock;
    cfg.set_time_source(FakeClock::tick, &clock);

    // A tweak tuned live: unsaved() lists it (M10's second criterion), and the journal says
    // from what, from where, and when.
    REQUIRE(width.set(3.0f));
    CHECK(strings(cfg.unsaved()) == std::vector<std::string>{"box/width"});
    auto j = journal(cfg);
    REQUIRE(j.size() == 1);
    CHECK(j[0].path == Symbol("box/width"));
    CHECK(j[0].before_count == 1);
    CHECK(j[0].before[0].try_int() == 2);
    CHECK(j[0].after_count == 1);
    CHECK(j[0].after[0].try_float() == 3.0);
    CHECK(j[0].source.layer == Symbol("default"));
    CHECK(j[0].source.line == 3);
    CHECK(j[0].time_ns > 0);
    CHECK(j[0].fate == EditFate::Unsaved);

    // Tuned again before saving (set's fast path): the first is superseded, and the old
    // value now comes from scratch.
    REQUIRE(cfg.set("box.width", Node(4)));
    j = journal(cfg);
    REQUIRE(j.size() == 2);
    CHECK(j[0].fate == EditFate::Superseded);
    CHECK(j[1].before[0].try_float() == 3.0);
    CHECK(j[1].after[0].try_int() == 4);
    CHECK(j[1].source.layer == Symbol("scratch"));
    CHECK(j[1].fate == EditFate::Unsaved);

    // Saved: written, with the file and line.
    REQUIRE(cfg.write_back("box.width"));
    j = journal(cfg);
    CHECK(j[1].fate == EditFate::Written);
    CHECK(j[1].written.file.str().ends_with("default.dta"));
    CHECK(j[1].written.line == 3);
    CHECK(cfg.unsaved().empty());

    // Kept for one user: written into local.
    REQUIRE(cfg.set("box.height", Node(7)));
    REQUIRE(cfg.write_back_to("box.height", "local"));
    j = journal(cfg);
    CHECK(j.back().fate == EditFate::Written);
    CHECK(j.back().written.file.str().ends_with("local.dta"));

    // Reverted: dropped from scratch.
    REQUIRE(cfg.set("audio.volume", Node(0.9)));
    REQUIRE(cfg.clear_scratch("audio.volume"));
    CHECK(journal(cfg).back().fate == EditFate::Reverted);

    // A value of more than four nodes keeps its first four, and its count; a new path had
    // no value before.
    const Node six[6] = {1, 2, 3, 4, 5, 6};
    REQUIRE(cfg.set("box.sizes", std::span<const Node>(six)));
    CHECK(journal(cfg).back().before_count == 0);
    CHECK(journal(cfg).back().after_count == 6);
    CHECK(journal(cfg).back().after[3].try_int() == 4);

    // Promoted from scratch: written.
    REQUIRE(cfg.promote("scratch", "local"));
    CHECK(journal(cfg).back().fate == EditFate::Written);
    CHECK(journal(cfg).back().written.file.str().ends_with("local.dta"));
}

TEST_CASE("restore reverts the unsaved edits its scratch does not hold") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("d.dta", "(a 1) (b 1)\n")));
    REQUIRE(cfg.set("a", Node(2)));
    const ConfigSnapshot snap = cfg.snapshot();
    REQUIRE(cfg.set("b", Node(2)));
    REQUIRE(cfg.restore(snap));
    const auto j = journal(cfg);
    REQUIRE(j.size() == 2);
    CHECK(j[0].fate == EditFate::Unsaved);   // a: the snapshot's scratch holds it
    CHECK(j[1].fate == EditFate::Reverted);  // b: gone
}

TEST_CASE("the journal is a ring, and an edit on set's fast path allocates nothing") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("d.dta", "(a 1)\n")));
    cfg.set_journal_history(3);
    for (int i = 2; i <= 6; ++i) REQUIRE(cfg.set("a", Node(i)));
    auto j = journal(cfg);
    REQUIRE(j.size() == 3);  // the newest three, oldest first
    CHECK(j[0].after[0].try_int() == 4);
    CHECK(j[2].after[0].try_int() == 6);
    CHECK(j[1].fate == EditFate::Superseded);
    CHECK(j[2].fate == EditFate::Unsaved);
    {
        test::AllocationScope scope;
        for (int i = 0; i < 100; ++i) REQUIRE(cfg.set("a", Node(i)));
        if (test::counting_allocations()) CHECK(scope.count() == 0);
    }
    j = journal(cfg);
    CHECK(j[2].after[0].try_int() == 99);
    CHECK(j[2].before[0].try_int() == 98);

    cfg.set_journal_history(0);  // off
    REQUIRE(cfg.set("a", Node(7)));
    CHECK(journal(cfg).empty());
}
#endif

#if BAKER_SHIPPING
TEST_CASE("under BAKER_SHIPPING loop statistics are empty and mark_visible does nothing") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("d", dir.write("d.dta", "(a 1)\n")));
    cfg.mark_visible();
    const LoopStats s = cfg.loop_stats();
    CHECK(s.reloads == 0);
    CHECK(s.files.empty());
    CHECK(s.layers.empty());
    CHECK(cfg.set("a", Node(2)).error().code() == ErrorCode::Refused);
    int entries = 0;
    cfg.journal([&](const JournalEntry&) { ++entries; });
    CHECK(entries == 0);
}
#endif

// --- The repetition report ---

namespace {

Ref<Array> read(std::string_view text, std::string_view name, LoadSession& s) {
    auto r = read_string(text, name, s);
    REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : std::string(to_string(r.error()))));
    return *r;
}

std::string sites(const Repeat& r) {
    std::string out;
    for (const RepeatSite& s : r.sites)
        out += (out.empty() ? "" : " ") + std::string(s.file.str()) + ":" + std::to_string(s.line);
    return out;
}

}  // namespace

TEST_CASE("a subtree repeated in three files is reported with all three locations") {
    LoadSession s;
    const auto a = read("(level1\n  (enemy (hp 30) (speed 2) (loot coin)))", "a.dta", s);
    const auto b = read("(level2 (x 1))\n(enemy (hp 30) (speed 2) (loot coin))", "b.dta", s);
    const auto c = read("\n\n(boss\n (adds\n  (enemy (hp 30) (speed 2) (loot coin))))", "c.dta", s);
    const auto d = read("(enemy (hp 30) (speed 3) (loot coin))", "d.dta", s);  // differs
    const Array* trees[] = {a.get(), b.get(), c.get(), d.get()};
    const List<Repeat> r = find_repeats(trees);
    REQUIRE(r.size() == 1);
    CHECK(to_string(r[0].form) == "(enemy (hp 30) (speed 2) (loot coin))");
    CHECK(r[0].nodes == 10);
    CHECK(sites(r[0]) == "a.dta:2 b.dta:2 c.dta:5");

    // Twice is not a repetition, and min_repeats 0 turns the report off.
    const Array* two[] = {a.get(), b.get()};
    CHECK(find_repeats(two).empty());
    CHECK(find_repeats(trees, {.min_repeats = 0}).empty());
    CHECK(find_repeats(two, {.min_repeats = 2}).size() == 1);
}

TEST_CASE("repetition: only the largest, a macro counts once, commands apart, stable order") {
    LoadSession s;
    // (hp 30) is inside every (enemy ...), and once more on its own: it is reported, with
    // all four places, because one is not inside a reported subtree. (speed 2) is only ever
    // inside one, so it is not.
    const auto t = read(
        "(a (enemy (hp 30) (speed 2)))\n"
        "(b (enemy (hp 30) (speed 2)))\n"
        "(c (enemy (hp 30) (speed 2)))\n"
        "(d (hp 30))",
        "t.dta", s);
    List<Repeat> r = find_repeats(*t, {.min_nodes = 2});
    REQUIRE(r.size() == 2);
    CHECK(to_string(r[0].form) == "(enemy (hp 30) (speed 2))");
    CHECK(to_string(r[1].form) == "(hp 30)");
    CHECK(sites(r[1]) == "t.dta:1 t.dta:2 t.dta:3 t.dta:4");

    // A macro's expansions share its array: a word already made.
    const auto m = read(
        "[ENEMY (enemy (hp 30) (speed 2) (loot coin))]\n"
        "(a ENEMY) (b ENEMY) (c ENEMY)",
        "m.dta", s);
    CHECK(find_repeats(*m).empty());

    // A command and an array with the same contents are different things.
    const auto k = read(
        "(a {spawn grunt 3 near gate})\n(b {spawn grunt 3 near gate})\n"
        "(c (spawn grunt 3 near gate))\n(d {spawn grunt 3 near gate})",
        "k.dta", s);
    r = find_repeats(*k, {.min_nodes = 5});
    REQUIRE(r.size() == 1);
    CHECK(r[0].form.type() == Type::Command);
    CHECK(sites(r[0]) == "k.dta:1 k.dta:2 k.dta:4");

    // The same report on every run: nothing depends on where things are in memory.
    const auto again = read(
        "(a (enemy (hp 30) (speed 2)))\n(b (enemy (hp 30) (speed 2)))\n"
        "(c (enemy (hp 30) (speed 2)))\n(d (hp 30))",
        "t.dta", s);
    const List<Repeat> r2 = find_repeats(*again, {.min_nodes = 2});
    REQUIRE(r2.size() == 2);
    CHECK(to_string(r2[0].form) == "(enemy (hp 30) (speed 2))");
    CHECK(sites(r2[1]) == "t.dta:1 t.dta:2 t.dta:3 t.dta:4");
}
