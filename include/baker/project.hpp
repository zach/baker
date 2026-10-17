#pragma once

// A project file: what a Config is made of (its layers, pack levels, include directories,
// and defines), in one place that the game's setup and every tool read, so an editor or a
// checker sees the stack the game loads without a second copy of it.
//
//   ; baker-project.dta, at the root a tool opens
//   (project
//      (pack_levels engine game task)     ; hardest first
//      (include_dirs "data")              ; searched after an including file's directory
//      (defines PC)                       ; flags, as #ifdef sees them
//      (reference "build/reference.json") ; for tools: where the build writes the reference
//      (layers                            ; bottom to top
//         (default "data/default.dta")
//         (local "data/local.dta" optional)
//         (agent "data/agent.dta" optional)))
//
// Paths are relative to the project file's directory unless absolute. Every part but
// (layers ...) may be left out. A key the format does not know is an error, located, so a
// typo is found rather than ignored. Every entry read keeps where it is written, so a tool
// can point into the file.
//
//   auto project = baker::read_project("baker-project.dta");
//   if (!project) ...;
//   baker::Config cfg;
//   if (auto r = baker::open_project(cfg, *project, {.skip = {"local"}}); !r) ...;
//
// A tool opened somewhere inside a project finds its file with find_project.

#include <baker/config.hpp>
#include <baker/list.hpp>
#include <baker/parse.hpp>
#include <baker/symbol.hpp>

#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace baker {

// The name a project file has, which find_project looks for.
inline constexpr std::string_view kProjectFileName = "baker-project.dta";

// Where an entry is written in the project file, as Baker's errors give it: a 1-based line
// and column, the column in characters (code points). Zero: not read from a file (a
// Project built in code).
struct ProjectPosition {
    int32_t line = 0;
    int32_t column = 0;
};

// A pack level, include directory, or define, and where it is written.
struct ProjectEntry {
    Symbol value;  // an include directory joined with the project file's directory
    ProjectPosition at{};
};

struct ProjectLayer {
    Symbol name;
    Symbol path;            // joined with the project file's directory, unless absolute
    bool optional = false;  // a missing file is an empty layer (LayerOptions::optional)
    bool enabled = true;    // `disabled` in the file: in the stack, skipped by the merge
    ProjectPosition name_at{};
    ProjectPosition path_at{};
};

struct Project {
    Symbol file;                      // the project file, as it was named
    List<ProjectLayer> layers;        // bottom to top
    List<ProjectEntry> pack_levels;   // hardest first; empty: the session's stay
    List<ProjectEntry> include_dirs;  // joined with the project file's directory
    List<ProjectEntry> defines;       // flags
    // Where the game's build writes its reference (write_reference_json), joined with the
    // project file's directory; empty if the file names none. For tools that check data
    // without the game: a game ignores it, and nothing here reads it or asks that it exist.
    Symbol reference;
    ProjectPosition reference_at{};
};

// Reads a project file, through `reader` if given (an editor's unsaved buffer), else from
// the disk. It reaches no other file: an #include in it is refused.
[[nodiscard]] Expected<Project> read_project(std::string_view path,
                                             const LoadSession::Reader& reader = {});

// The project file that governs `path` (a file or a directory): kProjectFileName in `path`
// itself, else in the nearest directory above it, or empty if there is none. The search
// is lexical, up to the top of `path` as written: an absolute path searches to the root,
// a relative one to the directory it is relative to. Each candidate is tried through
// `reader` if given (its text is read and discarded), else looked for on the disk.
[[nodiscard]] Symbol find_project(std::string_view path, const LoadSession::Reader& reader = {});

// A layer open_project could not push, and why.
struct ProjectProblem {
    Symbol layer;
    Error error;
};

struct ProjectOptions {
    // Layers to leave out, by name: a simulation or a check leaves out one person's own
    // tuning (`local`), so its results do not depend on whose machine runs it.
    // Initialized from an empty list's value, not from {}: MSVC does not implement
    // list-initializing a default member initializer (C2797).
    std::initializer_list<std::string_view> skip = std::initializer_list<std::string_view>();
    // Go on past a layer that does not read, as a tool that shows every problem at once
    // does; a game stops at the first.
    bool go_on = false;
};

// Sets `cfg.session()`'s pack levels, include directories, and defines from `project`, then
// pushes its layers, bottom to top. A Config to open a project into has no layers yet; set
// its session's reader first if the layers' text comes from somewhere other than the disk.
// Stops at the first layer that does not read, with its error. With `go_on`, a layer that
// does not read is left out of the stack, as a failed push_layer leaves it, the layers above
// it read over the ones below, and each such layer's problem is returned, in stack order:
// none if every layer read. Only a problem allocates.
[[nodiscard]] Expected<List<ProjectProblem>> open_project(Config& cfg, const Project& project,
                                                          const ProjectOptions& options = {});

}  // namespace baker
