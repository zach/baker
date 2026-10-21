#pragma once

// The tool commands, as a game's own binary runs them: an editor, a checker in CI, or an
// agent with a shell calls `mygame check`, and the game answers with Baker's JSON. They
// live in the game's binary because only it has its script functions.
//
//   check                          baker.check/1.3 JSON; exit 1 on an error, a load
//                                  problem, a shape error, or a call to an undefined
//                                  function
//   reference [--json]             the script vocabulary, as Markdown; --json:
//                                  {"reference", "tweaks", "shapes"}, Baker's three documents
//   diff [--layer L]               what layer L (default agent) changes, value by value
//   promote [--from L] [--to T] [--write | --patch FILE]
//                                  layer L into T (default default): what it would write;
//                                  --write writes; --patch writes the change as a unified
//                                  diff against the files, and nothing else
//   eval [--seed N] [--json] "{command}"...
//                                  each command's result on the game reset with seed N
//                                  (default 1); --json: one baker.reply/1.1 line each
//   sim [--scenario NAME]... [--seed N | --seeds A-B] [--step-hz N] [--json] [--results DIR]
//                                  runs the project's scenarios (all, or those named, by
//                                  name or file name) for each seed, stepping the game
//                                  (`step`) at N Hz (default 60), then judges them by its
//                                  objectives; --json: baker.sim/1.1; --results: DIR gets
//                                  results.json and results.dta, what compare reads. Exit 1
//                                  if an objective failed
//   compare --before DIR --after DIR
//                                  which objectives moved between two --results
//                                  directories: baker.compare/1; exit 1 if one broke
//   every command: --project PATH  the stack: the nearest baker-project.dta from PATH
//                  --output FILE   the output in FILE, not on stdout (a failure to write
//                                  it is exit 2); the host may route it instead (`output`)
//
// Exit status: 0, kCommandFailed (1) when something failed, kCommandError (2) when it
// could not run.
//
//   int main(int argc, char** argv) {
//       auto line = baker::host::parse_command_line(argc, argv);
//       if (!line) ...;
//       // the host's own flags are in line->rest; its own commands, if line->command is one
//       const int status = baker::host::run_command(*line, hooks);
//       if (status >= 0) return status;   // -1: not a kit command
//   }

#include <baker/callback.hpp>
#include <baker/config.hpp>
#include <baker/expected.hpp>
#include <baker/list.hpp>
#include <baker/parse.hpp>
#include <baker/project.hpp>
#include <baker/script.hpp>

#include <cstdint>
#include <string_view>

namespace baker::host {

inline constexpr int kCommandFailed = 1;
inline constexpr int kCommandError = 2;

// A command line, as the kit reads it. Views point into argv.
struct CommandLine {
    std::string_view command;          // argv[1]
    std::string_view project;          // --project PATH; the host may set it
    std::string_view patch_root;       // a patch's names are relative to this directory;
                                       // empty: the project file's
    std::string_view layer = "agent";  // diff --layer, promote --from
    std::string_view to = "default";   // promote --to
    std::string_view patch;            // promote --patch FILE
    std::string_view output;           // --output FILE: the command's output there
    uint64_t seed = 1;                 // eval and sim --seed; sim --seeds A-B: A
    uint64_t seed_last = 0;            // sim --seeds A-B: B; 0: seed alone
    double step_hz = 60;               // sim --step-hz
    List<std::string_view> scenarios;  // sim --scenario NAME, each
    std::string_view results;          // sim --results DIR
    std::string_view before, after;    // compare --before DIR --after DIR
    bool json = false;                 // --json
    bool write = false;                // promote --write
    List<std::string_view> commands;   // eval's {commands}
    List<std::string_view> rest;       // every argument the kit does not read, in order
};

// Reads argv[1] as the command, the kit's flags and their values, and arguments that are
// commands ({...}); everything else goes to `rest`, in order, for the host. An error for a
// flag missing its value, or a seed that is not a number.
[[nodiscard]] Expected<CommandLine> parse_command_line(int argc, const char* const* argv);

// What the game gives the commands.
struct CommandHooks {
    // Binds the game's script functions to a fresh Context, before its data loads, so check
    // and reference know them; and unbinds them before the Context goes.
    Callback<void(Context&)> bind{};
    Callback<void(Context&)> unbind{};
    // Reads the game's structured data, as Live's `changed`.
    Callback<void(const Config&)> changed{};
    // Resets the game's state with a seed, before eval runs its commands and before each
    // run of a scenario.
    Callback<void(Context&, uint64_t seed)> reset{};
    // One fixed step of the game, `dt` seconds, for sim; the game's systems and nothing
    // drawn. A game without it cannot run scenarios.
    Callback<void(Context&, double dt)> step{};
    // Files the game wants checked beside its data, read; or the located problem that stops
    // them reading, which check reports as a load problem. The project's scenarios and
    // objectives are checked without it.
    Callback<Expected<List<Ref<Array>>>(const Config&)> extra_trees{};
    // The stack when there is no project file, and how it opens: a check or a simulation
    // leaves out one person's own layer (`.skip = {"local"}`).
    const Project* built = nullptr;
    ProjectOptions open = ProjectOptions();
    // Reads the project file, the layers and what a patch compares against, if not from the
    // disk (an engine's virtual roots): the same reader Live and the Config take.
    LoadSession::Reader reader{};
    // Lists a directory of the host's own files (an archive, a virtual root): calls `file`
    // with each entry's path and returns true, or returns false if `dir` is not one of its
    // directories. The kit keeps the .dta files, in name order, and reads each through
    // `reader`. Empty, or false: the disk's directory, as without it. For the project's
    // scenarios and objectives.
    Callback<bool(std::string_view dir, Callback<void(std::string_view path)> file)> list{};
    // Where the output goes when the command line names no --output file; empty: stdout.
    // An engine that writes its own lines to stdout keeps the result apart from them.
    Callback<void(std::string_view text)> output{};
};

// Runs `line.command` if it is one of the kit's and returns its exit status; -1 if it is
// not one, for the host's own commands. Its output goes to `line.output`'s file, else to
// `hooks.output`, else to stdout; messages for a person go to stderr.
[[nodiscard]] int run_command(const CommandLine& line, const CommandHooks& hooks);

// A `sim` the host advances from its own loop, one fixed step per frame: for an engine that
// steps its game only inside its main loop, where a call that runs one step and comes back
// (`CommandHooks::step`) cannot be written. The kit does the rest of `sim` as run_command
// does, and the runs, output and exit status are the same. Every call is made on the thread
// that owns context(), between frames.
//
//   baker::host::SimRun run(line, hooks);   // line.command is "sim"
//   // once a frame:
//   if (run.wants() == SimRun::Wants::Reset && world_reset_with(run.seed())) run.ready();
//   else if (run.wants() == SimRun::Wants::Steps) run.step();  // one step_seconds() passed
//   else if (run.wants() == SimRun::Wants::Done) return run.status();
class SimRun {
public:
    enum class Wants : uint8_t {
        Reset,  // the game reset with seed(), over as many frames as it takes; then ready()
        Steps,  // a fixed step of step_seconds() of the game, then step()
        Done,   // status() is the exit status; the output went where run_command sends it
    };
    // Loads the stack as run_command does, binding the game's functions to context(), and
    // reads the scenarios. Done at once if it cannot run (status() 2, the reason on stderr).
    // `hooks` must outlive the run; it does not use `reset` or `step`.
    SimRun(const CommandLine& line, const CommandHooks& hooks);
    ~SimRun();
    SimRun(const SimRun&) = delete;
    SimRun& operator=(const SimRun&) = delete;

    [[nodiscard]] Wants wants() const noexcept;
    [[nodiscard]] uint64_t seed() const noexcept;  // the run's seed; $seed is set
    // One step, as the run counts it: 1 / --step-hz, in whole nanoseconds. Set the game's
    // fixed rate to match.
    [[nodiscard]] double step_seconds() const noexcept;
    [[nodiscard]] Context& context() noexcept;

    // The game is reset: the start commands run, and what is due at 0.
    void ready();
    // One step passed: the scheduler ticks, what is due runs; at the scenario's end the run
    // is judged and the next one wants its reset. A step with nothing due allocates nothing.
    void step();
    // Ends now (a window closed, a timeout): the run in progress is kept, not finished and
    // not judged, and the output is written so far, with status 2.
    void stop();
    [[nodiscard]] int status() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

// The commands and flags above, for a host's usage text. It leaves out --project, which a
// host may read under a name of its own (a data directory, say) and set in CommandLine.
[[nodiscard]] std::string_view command_help() noexcept;

}  // namespace baker::host
