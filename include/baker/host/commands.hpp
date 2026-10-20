#pragma once

// The tool commands, as a game's own binary runs them: an editor, a checker in CI, or an
// agent with a shell calls `mygame check`, and the game answers with Baker's JSON. They
// live in the game's binary because only it has its script functions.
//
//   check                          baker.check/1.2 JSON; exit 1 on an error, a load
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
//   every command: --project PATH  the stack: the nearest baker-project.dta from PATH
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
    uint64_t seed = 1;                 // eval --seed
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
    // Resets the game's state with a seed, before eval runs its commands.
    Callback<void(Context&, uint64_t seed)> reset{};
    // Files the game wants checked beside its data (scenarios, objectives), read; or the
    // located problem that stops them reading, which check reports as a load problem.
    Callback<Expected<List<Ref<Array>>>(const Config&)> extra_trees{};
    // The stack when there is no project file, and how it opens: a check or a simulation
    // leaves out one person's own layer (`.skip = {"local"}`).
    const Project* built = nullptr;
    ProjectOptions open = ProjectOptions();
};

// Runs `line.command` if it is one of the kit's, printing to stdout and stderr, and returns
// its exit status; -1 if it is not one, for the host's own commands.
[[nodiscard]] int run_command(const CommandLine& line, const CommandHooks& hooks);

// The commands and flags above, for a host's usage text. It leaves out --project, which a
// host may read under a name of its own (a data directory, say) and set in CommandLine.
[[nodiscard]] std::string_view command_help() noexcept;

}  // namespace baker::host
