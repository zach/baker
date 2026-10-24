// The host kit's scenario runner: a game with no window, stepped and judged.
#include "host/scenarios.hpp"

#include <baker/parse.hpp>

#include <doctest.h>

#include "temp_dir.hpp"

#include <map>
#include <string>

namespace sim = baker::host::sim;

namespace {

// A counter that steps, and the script words to drive and read it.
struct Counter {
    double total = 0, rate = 1;
    uint64_t seed = 0;

    void bind(baker::Context& ctx) {
        (void)ctx.func("counter.total", [this] { return total; });
        (void)ctx.func("counter.rate", [this](double r) { rate = r; });
    }
    [[nodiscard]] sim::Game game() {
        return {.reset =
                    [this](baker::Context&, uint64_t s) {
                        total = 0;
                        rate = 1;
                        seed = s;
                    },
                .step = [this](baker::Context&, double dt) { total += rate * dt; }};
    }
};

struct Fixture {
    baker::test::TempDir dir;
    sim::Book book;
    baker::Error error;

    bool load(const std::string& metrics, const std::string& scenario) {
        dir.write("metrics/m.dta", metrics);
        const std::string s = dir.write("s.dta", scenario);
        for (const std::string& path : {dir.path("metrics"), s})
            if (auto r = book.read(path, {}); !r) {
                error = r.error();
                return false;
            }
        return true;
    }
    [[nodiscard]] std::string message() const { return std::string(error.message()); }
};

const char* kMetrics = R"(
(objective counter.reached (desc "Ten seconds at rate 1 reach 10")
   (measure {counter.total}) (pass {>= $value 9.99}))
(objective counter.small (measure {counter.total}) (pass {< $value 5})))";

}  // namespace

TEST_CASE("sim: steps a scenario's seconds and judges it") {
    Fixture f;
    REQUIRE_MESSAGE(f.load(kMetrics, "(scenario ten (seconds 10) (objectives counter.reached))"),
                    f.message());
    REQUIRE(f.book.objectives.size() == 2);
    REQUIRE(f.book.scenarios.size() == 1);
    CHECK(f.book.objectives[0].desc == "Ten seconds at rate 1 reach 10");
    CHECK(f.book.scenarios[0].line == 1);
    CHECK(f.book.scenarios[0].file.str().ends_with("s.dta"));

    Counter c;
    baker::Context ctx;
    c.bind(ctx);
    const sim::Run r = sim::simulate(ctx, c.game(), f.book, f.book.scenarios[0], 7, 60);
    CHECK(r.steps == 600);
    CHECK(c.seed == 7);
    REQUIRE(r.objectives.size() == 1);  // the scenario names only one
    CHECK(r.objectives[0].value == doctest::Approx(10.0));
    CHECK(r.objectives[0].pass);
    CHECK(r.objectives[0].line == 2);  // where the objective is declared
    CHECK(r.passed());
}

TEST_CASE("sim: start and at commands run at their times; a failing objective fails the run") {
    Fixture f;
    REQUIRE_MESSAGE(f.load(kMetrics, R"((scenario timed (seconds 4)
        (start {counter.rate 2})
        (at 2 {counter.rate 0}))   ; 2 s at rate 2, then still
    )"),
                    f.message());
    Counter c;
    baker::Context ctx;
    c.bind(ctx);
    const sim::Run r = sim::simulate(ctx, c.game(), f.book, f.book.scenarios[0], 1, 60);
    REQUIRE(r.objectives.size() == 2);  // no (objectives ...): all of them
    CHECK(r.objectives[0].value == doctest::Approx(4.0));
    CHECK_FALSE(r.objectives[0].pass);  // counter.reached wanted 10
    CHECK(r.objectives[1].pass);        // counter.small
    CHECK_FALSE(r.passed());
}

TEST_CASE("sim: the same scenario and seed give the same result") {
    Fixture f;
    REQUIRE(f.load(kMetrics, "(scenario ten (seconds 10))"));
    Counter c;
    baker::Context ctx;
    c.bind(ctx);
    baker::Vec<sim::Run> a, b;
    a.push_back(sim::simulate(ctx, c.game(), f.book, f.book.scenarios[0], 3, 60));
    b.push_back(sim::simulate(ctx, c.game(), f.book, f.book.scenarios[0], 3, 60));
    CHECK(sim::results_dta(a, {}) == sim::results_dta(b, {}));  // reset between runs
}

TEST_CASE("sim: errors are reported, located, not crashed on") {
    Fixture f;
    REQUIRE(f.load(kMetrics, "(scenario bad (seconds 1) (start {no.such}) (objectives nope))"));
    Counter c;
    baker::Context ctx;
    c.bind(ctx);
    const sim::Run r = sim::simulate(ctx, c.game(), f.book, f.book.scenarios[0], 1, 60);
    CHECK_FALSE(r.passed());
    REQUIRE(r.errors.size() == 2);  // the unknown function, and the unknown objective
    CHECK(r.errors[0].file().str().ends_with("s.dta"));
    CHECK(r.errors[1].message().find("nope") != std::string_view::npos);
    CHECK(r.errors[1].line() == 1);

    Fixture g;
    CHECK_FALSE(
        g.load("\n(objective broken (measure {counter.total}))", "(scenario s (seconds 1))"));
    CHECK(g.message().find("pass") != std::string::npos);
    CHECK(g.error.line() == 2);
    Fixture h;
    CHECK_FALSE(h.load(kMetrics, "(scenario s)"));
    CHECK(h.message().find("seconds") != std::string::npos);

    // A measure that gives no number fails its objective, not the runner.
    Fixture m;
    REQUIRE(
        m.load("(objective words (measure {\"text\"}) (pass {1}))", "(scenario s (seconds 1))"));
    const sim::Run w = sim::simulate(ctx, c.game(), m.book, m.book.scenarios[0], 1, 60);
    REQUIRE(w.objectives.size() == 1);
    CHECK(w.objectives[0].failed);
    CHECK_FALSE(w.passed());
}

TEST_CASE("sim: a path that does not exist reads nothing") {
    sim::Book book;
    CHECK(book.read("no/such/dir", {}));
    CHECK(book.scenarios.empty());
}

TEST_CASE("sim: results are data that reads back, located") {
    Fixture f;
    REQUIRE(f.load(kMetrics, "(scenario bad (seconds 1) (start {no.such \"a \\q quote\"}))"));
    Counter c;
    baker::Context ctx;
    c.bind(ctx);
    baker::Vec<sim::Run> runs;
    runs.push_back(sim::simulate(ctx, c.game(), f.book, f.book.scenarios[0], 1, 60));
    runs.push_back(sim::simulate(ctx, c.game(), f.book, f.book.scenarios[0], 2, 60));
    const std::string file =
        f.dir.write("out/results.dta", std::string(sim::results_dta(runs, {}).view()));
    baker::Vec<sim::Run> back;
    baker::Vec<sim::Median> medians;
    REQUIRE(sim::read_results(file, back, medians));
    REQUIRE(back.size() == 2);
    CHECK(back[1].seed == 2);
    CHECK(back[0].steps == 60);
    CHECK(back[0].file == runs[0].file);
    CHECK(back[0].objectives[1].line == runs[0].objectives[1].line);
    REQUIRE(back[0].errors.size() == 1);
    CHECK(back[0].errors[0].message() == runs[0].errors[0].message());

    const std::string json(sim::results_json(runs, {}).view());
    CHECK(json.find("\"schema\": \"baker.sim/1.3\"") != std::string::npos);
}

TEST_CASE("sim: an objective (over median) is judged by its median over a scenario's seeds") {
    Fixture f;
    // Each seed's total is the seed: 1 to 5, so the median is 3.
    REQUIRE_MESSAGE(f.load(R"(
        (objective counter.typical (over median) (measure {counter.total}) (pass {<= $value 3.5}))
        (objective counter.strict (over median) (measure {counter.total}) (pass {<= $value 2.5})))",
                           "(scenario seeded (seconds 1) (start {counter.rate $seed}))"),
                    f.message());
    REQUIRE(f.book.objectives.size() == 2);
    CHECK(f.book.objectives[0].over_median);
    Counter c;
    baker::Context ctx;
    c.bind(ctx);
    baker::Vec<sim::Run> runs;
    for (uint64_t seed = 1; seed <= 5; ++seed)
        runs.push_back(sim::simulate(ctx, c.game(), f.book, f.book.scenarios[0], seed, 60));
    // A seed whose value is out of band does not fail its run.
    CHECK_FALSE(runs[4].objectives[0].pass);
    CHECK(runs[4].passed());
    const baker::Vec<sim::Median> medians = sim::judge_medians(ctx, f.book, runs);
    REQUIRE(medians.size() == 2);
    CHECK(medians[0].objective.str() == "counter.strict");
    CHECK(medians[0].value == doctest::Approx(3.0));
    CHECK(medians[0].seeds == 5);
    CHECK_FALSE(medians[0].pass);
    CHECK(medians[1].pass);  // counter.typical
    CHECK_FALSE(sim::all_passed(runs, medians));
    baker::Vec<sim::Median> typical;
    typical.push_back(medians[1]);
    CHECK(sim::all_passed(runs, typical));

    // The verdicts are data that reads back, and compare judges by them.
    const std::string file =
        f.dir.write("out/results.dta", std::string(sim::results_dta(runs, medians).view()));
    baker::Vec<sim::Run> back;
    baker::Vec<sim::Median> back_medians;
    REQUIRE(sim::read_results(file, back, back_medians));
    REQUIRE(back_medians.size() == 2);
    CHECK(back_medians[1].pass);
    CHECK(back[0].objectives[0].over_median);

    bool broke = false;
    // The strict objective's median fails on both sides: the same, whatever the seeds did.
    std::string json(sim::compare_json(back, back_medians, runs, medians, broke).view());
    CHECK_FALSE(broke);
    CHECK(json.find("\"schema\": \"baker.compare/1\"") != std::string::npos);
    // The typical objective's median passed, and now fails: broke.
    baker::Vec<sim::Median> worse = medians;
    worse[1].pass = false;
    json = sim::compare_json(runs, medians, runs, worse, broke).view();
    CHECK(broke);
    CHECK(json.find("\"over\": \"median\"") != std::string::npos);
    CHECK(json.find("\"line\": 2") != std::string::npos);  // counter.typical, located
}

TEST_CASE("sim: a directory the host lists is read in name order, through its reader") {
    std::map<std::string, std::string> files = {
        {"pak://sim/b.dta", "(scenario second (seconds 1))"},
        {"pak://sim/a.dta", "(scenario first (seconds 1))"},
        {"pak://sim/notes.txt", "not data"}};
    const baker::LoadSession::Reader reader = [&](std::string_view path, baker::TextSink out) {
        const auto it = files.find(std::string(path));
        if (it == files.end()) return false;
        out(it->second);
        return true;
    };
    const sim::Book::Lister list = [&](std::string_view dir,
                                       baker::Callback<void(std::string_view)> file) {
        if (dir != "pak://sim") return false;
        for (const auto& [name, text] : files) file(name);  // in any order: the kit sorts
        return true;
    };
    sim::Book book;
    REQUIRE(book.read("pak://sim", reader, list));
    REQUIRE(book.scenarios.size() == 2);
    CHECK(book.scenarios[0].name.str() == "first");
    CHECK(book.scenarios[1].name.str() == "second");
    sim::Book unlisted;  // without the lister, a directory only the host has reads nothing
    REQUIRE(unlisted.read("pak://sim", reader));
    CHECK(unlisted.scenarios.empty());
}

TEST_CASE("sim: a run ends on its game's condition, with its length as a ceiling") {
    Fixture f;
    // A turn-based game: one step a turn, so lengths and times are in steps.
    REQUIRE_MESSAGE(f.load(R"(
        (objective turns.taken (measure {+ $steps 0}) (pass {<= $value 12})))",
                           R"((scenario race (steps 40)
        (start {counter.rate 1})
        (at step 5 {counter.rate 2})
        (until {>= {counter.total} 0.25})))"),
                    f.message());
    Counter c;
    baker::Context ctx;
    c.bind(ctx);
    // At 60 Hz, a step is 1/60: rate 1 for 5 steps, then 2, reaching 0.25 at step 10.
    sim::Run r = sim::simulate(ctx, c.game(), f.book, f.book.scenarios[0], 1, 60);
    CHECK(r.ended == baker::Symbol("until"));
    CHECK(r.steps == 10);
    REQUIRE(r.objectives.size() == 1);
    CHECK(r.objectives[0].value == 10);  // $steps, for the measure
    CHECK(r.passed());
    // The same at another rate: (steps N) is a count, not a time.
    sim::Run slow = sim::simulate(ctx, c.game(), f.book, f.book.scenarios[0], 1, 1);
    CHECK(slow.steps == 1);  // a step of a whole second reaches 0.25 at once
    CHECK(slow.ended == baker::Symbol("until"));

    // A condition that never holds stops at the ceiling.
    Fixture never;
    REQUIRE(never.load("(objective n (measure {+ $steps 0}) (pass {1}))",
                       "(scenario s (steps 7) (until {< 1 0}))"));
    sim::Run ceiling = sim::simulate(ctx, c.game(), never.book, never.book.scenarios[0], 1, 60);
    CHECK(ceiling.ended == baker::Symbol("length"));
    CHECK(ceiling.steps == 7);

    // The results say how each run ended, and read back.
    baker::Vec<sim::Run> runs;
    runs.push_back(std::move(r));
    runs.push_back(std::move(ceiling));
    const std::string json(sim::results_json(runs, {}, baker::Symbol("turn")).view());
    CHECK(json.find("\"step_name\": \"turn\"") != std::string::npos);
    CHECK(json.find("\"ended\": \"until\"") != std::string::npos);
    CHECK(json.find("\"ended\": \"length\"") != std::string::npos);
    const std::string file =
        f.dir.write("out/results.dta", std::string(sim::results_dta(runs, {}).view()));
    baker::Vec<sim::Run> back;
    baker::Vec<sim::Median> medians;
    REQUIRE(sim::read_results(file, back, medians));
    CHECK(back[0].ended == baker::Symbol("until"));
    CHECK(back[1].ended == baker::Symbol("length"));
}

TEST_CASE("sim: (until ...) may name how a run ended, which $ended and the results show") {
    Fixture f;
    REQUIRE_MESSAGE(f.load(R"(
        (objective siege.won (measure {if {== $ended victory} 1 0}) (pass {== $value 1})))",
                           R"((scenario siege (steps 40)
        (start {counter.rate 1})
        (until {if {>= {counter.total} 0.25} victory 0})))"),
                    f.message());
    Counter c;
    baker::Context ctx;
    c.bind(ctx);
    sim::Run won = sim::simulate(ctx, c.game(), f.book, f.book.scenarios[0], 1, 60);
    CHECK(won.ended == baker::Symbol("victory"));
    CHECK(won.steps == 15);  // 15 steps at rate 1 reach 0.25
    REQUIRE(won.objectives.size() == 1);
    CHECK(won.objectives[0].pass);
    sim::Run again = sim::simulate(ctx, c.game(), f.book, f.book.scenarios[0], 2, 60);
    baker::Vec<sim::Run> runs;
    runs.push_back(std::move(won));
    runs.push_back(std::move(again));
    const std::string json(sim::results_json(runs, {}).view());
    CHECK(json.find("\"schema\": \"baker.sim/1.3\"") != std::string::npos);
    CHECK(json.find("\"ended\": \"victory\"") != std::string::npos);
    CHECK(json.find(R"j({"scenario": "siege", "endings": {"victory": 2}})j") != std::string::npos);
    const std::string file =
        f.dir.write("out/results.dta", std::string(sim::results_dta(runs, {}).view()));
    baker::Vec<sim::Run> back;
    baker::Vec<sim::Median> medians;
    REQUIRE(sim::read_results(file, back, medians));
    CHECK(back[0].ended == baker::Symbol("victory"));
}

TEST_CASE("sim: (every ...) repeats at each multiple of its period, after the (at ...)s due") {
    Fixture f;
    // $seen gains a digit each time the (every ...) runs: the (at step 5 ...) sets $mark 1
    // first, so the run at step 5 already sees it.
    REQUIRE_MESSAGE(f.load(R"(
        (objective seen (measure {+ $seen 0}) (pass {== $value 111}))
        (objective count (measure {+ $count 0}) (pass {== $value 4})))",
                           R"((scenario waves (steps 20)
        (start {set $seen 0} {set $mark 0} {set $count 0})
        (every step 5 {set $seen {+ {* $seen 10} $mark}} {set $count {+ $count 1}})
        (at step 5 {set $mark 1})))"),
                    f.message());
    Counter c;
    baker::Context ctx;
    c.bind(ctx);
    sim::Run r = sim::simulate(ctx, c.game(), f.book, f.book.scenarios[0], 1, 60);
    REQUIRE(r.objectives.size() == 2);
    CHECK(r.objectives[0].value == 111);  // steps 0, 5, 10 and 15; 20 is past the end
    CHECK(r.objectives[1].value == 4);
    CHECK(r.passed());

    // In seconds at a rate that does not divide them: each multiple on the half-step rule,
    // 0.25 s at 10 Hz is due at steps 0, 2, 5 and 7 of 10.
    Fixture s;
    REQUIRE_MESSAGE(s.load("(objective count (measure {+ $count 0}) (pass {== $value 4}))",
                           R"((scenario quarter (seconds 1)
        (start {set $count 0})
        (every 0.25 {set $count {+ $count 1}})))"),
                    s.message());
    sim::Run q = sim::simulate(ctx, c.game(), s.book, s.book.scenarios[0], 1, 10);
    REQUIRE(q.objectives.size() == 1);
    CHECK(q.objectives[0].value == 4);

    Fixture zero;
    CHECK_FALSE(zero.load(kMetrics, "(scenario z (steps 5) (every step 0 {counter.rate 1}))"));
    CHECK(zero.message().find("a period above 0") != std::string::npos);
    Fixture negative;
    CHECK_FALSE(negative.load(kMetrics, "(scenario z (steps 5) (every -1 {counter.rate 1}))"));
}

TEST_CASE("sim: a scenario gives one length, and (at step N) or (at SECONDS)") {
    Fixture both;
    CHECK_FALSE(both.load(kMetrics, "(scenario s (seconds 1) (steps 60))"));
    CHECK(both.message().find("one length") != std::string::npos);
    Fixture bad_until;
    CHECK_FALSE(bad_until.load(kMetrics, "(scenario s (steps 5) (until 1))"));
    CHECK(bad_until.message().find("(until {command})") != std::string::npos);
    Fixture bad_at;
    CHECK_FALSE(bad_at.load(kMetrics, "(scenario s (steps 5) (at step x {counter.rate 1}))"));
}
