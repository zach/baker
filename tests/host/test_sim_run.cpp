// A sim the host advances from its own loop (SimRun): the same runs and output as sim
// through run_command, a reset that takes frames, stopping, and a quiet step.
#include <baker/host/commands.hpp>

#include <doctest.h>

#include "alloc_counter.hpp"
#include "temp_dir.hpp"

#include <string>

namespace host = baker::host;
using Wants = host::SimRun::Wants;

namespace {

// A stack with one scenario and one objective: a counter that counts up at the rate the
// scenario sets from the seed, and an (at ...) that doubles it halfway.
struct Stack {
    baker::test::TempDir dir;
    Stack() {
        dir.write("baker-project.dta", R"((project
   (layers (default "data/default.dta"))
   (scenarios "sim")
   (objectives "metrics")))");
        dir.write("data/default.dta", "(rate 1)\n");
        dir.write("sim/count.dta", R"((scenario count (seconds 2)
   (start {counter.rate $seed})
   (at 1 {counter.rate {* 2 $seed}})
   (every 0.5 {counter.total})))");
        dir.write("metrics/m.dta", R"((objective counter.total
   (measure {counter.total}) (pass {>= $value 2.9})))");
    }
};

// The game: a counter the host steps.
struct Counter {
    double total = 0, rate = 1;
    void bind(baker::Context& ctx) {
        (void)ctx.func("counter.total", [this] { return total; });
        (void)ctx.func("counter.rate", [this](double r) { rate = r; });
    }
    void reset() { total = rate = 0; }
    void step(double dt) { total += rate * dt; }
};

struct Host {
    Counter game;
    std::string output;
    host::CommandHooks hooks;
    Host() {
        hooks.bind = [this](baker::Context& ctx) { game.bind(ctx); };
        hooks.reset = [this](baker::Context&, uint64_t) { game.reset(); };
        hooks.step = [this](baker::Context&, double dt) { game.step(dt); };
        hooks.output = [this](std::string_view text) { output.append(text); };
    }
};

host::CommandLine sim_line(const Stack& st, std::initializer_list<const char*> flags) {
    static std::string project;
    project = st.dir.path("data");
    static std::vector<const char*> argv;
    argv = {"game", "sim", "--baker-project", project.c_str()};
    argv.insert(argv.end(), flags.begin(), flags.end());
    auto line = host::parse_command_line(static_cast<int>(argv.size()), argv.data());
    REQUIRE(line);
    return *line;
}

}  // namespace

TEST_CASE("SimRun: the host advances each run; the output is sim's") {
    Stack st;
    const host::CommandLine line = sim_line(st, {"--baker-seeds", "1-2", "--baker-json"});

    Host sync;
    CHECK(host::run_command(line, sync.hooks) == 0);

    Host frames;
    host::SimRun run(line, frames.hooks);
    CHECK(run.step_seconds() == doctest::Approx(1.0 / 60));
    int resets = 0, steps = 0, reset_frames = 0;
    for (int frame = 0; run.wants() != Wants::Done && frame < 10000; ++frame) {
        if (run.wants() == Wants::Reset) {
            // A reset that takes three frames, as a world rebuilt by an engine would.
            if (++reset_frames < 3) continue;
            reset_frames = 0;
            CHECK(run.seed() == static_cast<uint64_t>(resets + 1));
            frames.game.reset();
            ++resets;
            run.ready();
        } else {
            frames.game.step(run.step_seconds());
            ++steps;
            run.step();
        }
    }
    REQUIRE(run.wants() == Wants::Done);
    CHECK(run.status() == 0);
    CHECK(resets == 2);
    CHECK(steps == 240);  // two seconds at 60 Hz, twice: waiting on a reset counts nothing
    CHECK(frames.output == sync.output);
    CHECK(frames.output.find("\"schema\": \"baker.sim/1.3\"") != std::string::npos);
}

TEST_CASE("SimRun: stopping keeps the run so far, not finished, and exits 2") {
    Stack st;
    const std::string out = st.dir.path("out");
    const host::CommandLine line =
        sim_line(st, {"--baker-seeds", "1-3", "--baker-json", "--baker-results", out.c_str()});
    Host h;
    host::SimRun run(line, h.hooks);
    run.ready();
    for (int i = 0; i < 10; ++i) run.step();
    run.stop();
    REQUIRE(run.wants() == Wants::Done);
    CHECK(run.status() == host::kCommandError);
    CHECK(h.output.find("\"status\": \"stopped\"") != std::string::npos);
    CHECK(h.output.find("\"steps\": 10, \"finished\": false") != std::string::npos);
    CHECK(h.output.find("\"seed\": 2") == std::string::npos);  // runs not started: left out
    CHECK(st.dir.read("out/results.dta").find("(finished 0)") != std::string::npos);
    run.step();  // after the end, a step does nothing
    CHECK(run.wants() == Wants::Done);
}

TEST_CASE("SimRun: a command that is not sim, or a stack that cannot run, ends at once") {
    Stack st;
    Host h;
    host::CommandLine line = sim_line(st, {});
    line.command = "check";
    const host::SimRun not_sim(line, h.hooks);
    CHECK(not_sim.wants() == Wants::Done);
    CHECK(not_sim.status() == host::kCommandError);

    const host::SimRun no_scenario(sim_line(st, {"--baker-scenario", "nosuch"}), h.hooks);
    CHECK(no_scenario.wants() == Wants::Done);
    CHECK(no_scenario.status() == host::kCommandError);
}

TEST_CASE("SimRun: a step with nothing due allocates nothing") {
    Stack st;
    Host h;
    host::SimRun run(sim_line(st, {}), h.hooks);
    run.ready();
    for (int i = 0; i < 3; ++i) run.step();
    const baker::test::AllocationScope scope;
    for (int i = 0; i < 10; ++i) run.step();  // before the (every 0.5 ...) at step 30
    CHECK(scope.count() == 0);
}
