// The host kit's scenario runner, behind the sim and compare commands (host/commands.hpp):
// a game's scenarios and objectives, read with where each is declared; runs that are a
// function of the data, the scenario and the seed; results as JSON for tools and as data for
// compare.
//
//   (scenario turn (seconds 20) (start {cube.reset}) (at 5 {...}) (objectives cube.turns))
//   (objective cube.turns (measure {cube.turns}) (pass {>= $value 2}))
//   (objective duel.normal_vs_combo (over median) (measure ...) (pass ...))
//
// A run resets the game with the seed ($seed in script), runs the start commands, steps the
// game at a fixed rate for the scenario's seconds (ticking the Context's scheduler, running
// each (at ...) when its time comes), then evaluates each objective's measure ($value) and
// its pass. Steps are counted, never timed. An objective (over median) is judged once per
// scenario, by its pass on the median of its values over the seeds run. A Stepper is one run
// a step at a time, for a host that steps the game from its own loop (SimRun).
#pragma once

#include <baker/array.hpp>
#include <baker/callback.hpp>
#include <baker/error.hpp>
#include <baker/parse.hpp>
#include <baker/script.hpp>

#include "common/containers.hpp"

#include <cstdint>
#include <string_view>

namespace baker::host::sim {

struct Objective {
    Symbol name{};
    std::string_view desc{};  // in a tree the Book keeps
    Node measure{}, pass{};   // commands
    bool over_median = false;
    Symbol file{};
    int32_t line = 0;
};

struct Scenario {
    struct At {
        double seconds = 0;
        Vec<Node> commands{};
    };
    Symbol name{};
    std::string_view desc{};
    double seconds = 10;
    Vec<Node> start{};         // before the first step
    Vec<At> at{};              // in time order
    Vec<Symbol> objectives{};  // empty: every objective
    Symbol file{};
    int32_t line = 0;
};

// A game's scenarios and objectives, and the trees that keep their commands.
struct Book {
    Vec<Objective> objectives{};
    Vec<Scenario> scenarios{};
    Vec<Ref<Array>> trees{};

    using Lister = Callback<bool(std::string_view dir, Callback<void(std::string_view path)>)>;
    // Reads `path`: a file, or every .dta file of a directory in name order (a directory is
    // listed by `list` if it has it, else on the disk; a file is read through `reader` if
    // given). A path that does not exist reads nothing. An error for a file that does not
    // parse, or a malformed form.
    [[nodiscard]] Expected<void> read(std::string_view path, const LoadSession::Reader& reader,
                                      const Lister& list = {});
};

struct ObjectiveResult {
    Symbol name{}, file{};
    int32_t line = 0;
    double value = 0;
    bool pass = false;
    bool over_median = false;  // its pass here is for information; the median's decides
    bool failed = false;       // the measure or the pass did not give a number: it fails
    Error error{};
};

struct Run {
    Symbol scenario{}, file{};
    int32_t line = 0;
    uint64_t seed = 0, steps = 0;
    bool finished = true;  // false: stopped before its last step, and not judged
    Vec<ObjectiveResult> objectives{};
    Vec<Error> errors{};  // a command of the scenario failed, or it names an unknown objective

    // Finished, no error, and every objective passed but those judged by their median.
    [[nodiscard]] bool passed() const noexcept;
};

struct Median {
    Symbol scenario{}, objective{}, file{};
    int32_t line = 0;
    double value = 0;
    int seeds = 0;  // the values it is the median of
    bool pass = false;
    bool failed = false;  // no value to judge, or the pass did not give a number
    Error error{};
};

struct Game {
    Callback<void(Context&, uint64_t seed)> reset;
    Callback<void(Context&, double dt)> step;
};

// A number as the results write it: six significant digits.
[[nodiscard]] Str number_text(double v);

// One run, a step at a time. Constructing it sets $seed; the game then resets with the seed,
// begin() runs the start commands and what is due at 0, and advance() follows each fixed
// step of dt() until done(). judge() then evaluates the objectives; stop() ends it unjudged.
class Stepper {
public:
    Stepper(Context& ctx, const Book& book, const Scenario& scenario, uint64_t seed,
            double step_hz);
    void begin();
    void advance();
    [[nodiscard]] bool done() const noexcept { return run_.steps >= total_; }
    [[nodiscard]] double dt() const noexcept { return dt_; }
    [[nodiscard]] Run judge();
    [[nodiscard]] Run stop();

private:
    void run_due();
    Context* ctx_;
    const Book* book_;
    const Scenario* scenario_;
    Run run_;
    double dt_ = 0;
    uint64_t total_ = 0;
    size_t next_at_ = 0;
};

// A whole run, stepping the game through `game.step`.
[[nodiscard]] Run simulate(Context& ctx, const Game& game, const Book& book,
                           const Scenario& scenario, uint64_t seed, double step_hz);
[[nodiscard]] Vec<Median> judge_medians(Context& ctx, const Book& book, const Vec<Run>& runs);
[[nodiscard]] bool all_passed(const Vec<Run>& runs, const Vec<Median>& medians);

// baker.sim/1.1: every run, sorted by scenario then seed, each located; the medians' verdicts;
// a tally per objective; and the status the exit code means.
[[nodiscard]] Str results_json(Vec<Run> runs, const Vec<Median>& medians);
// The same runs and verdicts as data, which read_results reads back for compare.
[[nodiscard]] Str results_dta(const Vec<Run>& runs, const Vec<Median>& medians);
[[nodiscard]] Expected<void> read_results(std::string_view file, Vec<Run>& runs,
                                          Vec<Median>& medians);
// baker.compare/1: per objective, whether it broke, was fixed, stayed the same, or is new or
// gone, with each side's tally and what changed. `broke` is set when anything broke.
[[nodiscard]] Str compare_json(const Vec<Run>& before, const Vec<Median>& medians_before,
                               const Vec<Run>& after, const Vec<Median>& medians_after,
                               bool& broke);

}  // namespace baker::host::sim
