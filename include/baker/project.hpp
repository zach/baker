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
//      (tool "build/bin/game")            ; for tools: the binary that runs the tool commands,
//                                         ; then any arguments of its own: (tool "engine" "--")
//      (console 4555)                     ; for tools: the port the game's console listens on
//      (scenarios "sim")                  ; for tools: scenario files, or directories of them
//      (objectives "metrics")             ; for tools: objective files, or directories
//      (layers                            ; bottom to top
//         (default "data/default.dta")
//         (mods (dir "mods") optional)    ; a layer for each mod in mods/ (push_layer_dir)
//         (local "data/local.dta" optional dev_only)
//         (agent "data/agent.dta" optional dev_only)))
//
// A layer marked dev_only is for development only (a person's tuning, an agent's proposal
// awaiting review): a shipping build never pushes it, even when its file is present, and a
// packaging step reads the project to leave its file out:
//
//   for (const baker::ProjectLayer& l : project->layers)
//       if (l.dev_only) exclude(l.path);   // as the project names it; not what it includes
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
#include <baker/write.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
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
    Symbol path;  // joined with the project file's directory, unless absolute
    // (name (dir "mods")): `path` is a directory of layers, pushed with push_layer_dir, and
    // `optional` lets it be absent. A packaging step that leaves out a dev_only layer
    // leaves out the whole directory.
    bool dir = false;
    bool optional = false;  // a missing file is an empty layer (LayerOptions::optional)
    bool enabled = true;    // `disabled` in the file: in the stack, skipped by the merge
    bool dev_only = false;  // `dev_only` in the file: never pushed under BAKER_SHIPPING
    ProjectPosition name_at{};
    ProjectPosition path_at{};
};

struct Project {
    Symbol file;                      // the project file, as it was named
    List<ProjectLayer> layers;        // bottom to top
    List<ProjectEntry> pack_levels;   // hardest first; empty: the session's stay
    List<ProjectEntry> include_dirs;  // joined with the project file's directory
    List<ProjectEntry> defines;       // flags
    // Where the game's build writes its reference (tools::write_reference_json), joined with the
    // project file's directory; empty if the file names none. For tools that check data
    // without the game: a game ignores it, and nothing here reads it or asks that it exist.
    Symbol reference;
    ProjectPosition reference_at{};
    // For tools that run the game's own commands (check, sim, ...): the binary that answers
    // them, and the files, or directories of .dta files, that hold its scenarios and its
    // objectives (the host kit's sim). Each joined with the project file's directory; empty
    // if the file names none. A game ignores them, and nothing here asks that they exist.
    //
    // A tool path may begin `$build`, the game's build directory, which the project file need
    // not know: (tool "$build/bin/game"). It stays so here; tool_path() resolves it.
    Symbol tool;
    ProjectPosition tool_at{};
    // (build "out"): where `$build` points, joined with the project file's directory; empty
    // if the file names none, and then it is "build" beside the project file.
    Symbol build;
    ProjectPosition build_at{};
    // (tool "engine" "--path" "." "--headless" "--"): the arguments after the binary, as
    // written and in order, for a game started through an engine or an interpreter. A tool
    // runs the binary with them, then the command, from the project file's directory, so a
    // relative path among them names what it names here.
    List<Symbol> tool_args;
    // (console 4555): the port the game's console listens on, for tools that talk to it, and
    // for the game as its default; 0 if the file names none. A game's own command line wins.
    uint16_t console_port = 0;
    ProjectPosition console_at{};
    List<ProjectEntry> scenarios;
    List<ProjectEntry> objectives;
    // (step_name turn): what one step of the game is called in sim's results, for a game
    // that steps in turns or events; empty: "step". Presentation only.
    Symbol step_name;
    // (format (indent 3) (width 100)): how the project's files are laid out, for a formatter
    // (tools::format_text) and the kit's format command; WriteOptions' defaults where the
    // file says nothing.
    WriteOptions format{};

    // The tool binary's path with `$build` resolved: `build_dir` if given (a tool's flag or
    // setting), else the project's (build ...), else "build" beside the project file. A
    // path without `$build` is returned as it is; empty if the file names no tool.
    [[nodiscard]] Symbol tool_path(std::string_view build_dir = {}) const;
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

// Up to kCapacity layer names, held in the value itself, so options built in one statement
// and passed in a later one (kept in a member, chosen by a condition, returned by a helper)
// stay valid for as long as the strings the names view do. Nothing allocates.
//   ProjectOptions o = skip_local ? ProjectOptions{.skip = {"local"}} : ProjectOptions{};
class LayerNames {
public:
    static constexpr size_t kCapacity = 8;

    LayerNames() noexcept = default;
    LayerNames(std::initializer_list<std::string_view> names) noexcept {
        for (const std::string_view n : names) add(n);
    }
    // Adds a name. Past kCapacity it is dropped, add returns false, and open_project
    // refuses the options rather than open a stack with a layer it was told to leave out.
    bool add(std::string_view name) noexcept {
        if (size_ == kCapacity) {
            overflow_ = true;
            return false;
        }
        names_[size_++] = name;
        return true;
    }
    [[nodiscard]] bool contains(std::string_view name) const noexcept {
        for (size_t i = 0; i < size_; ++i)
            if (names_[i] == name) return true;
        return false;
    }
    [[nodiscard]] const std::string_view* begin() const noexcept { return names_; }
    [[nodiscard]] const std::string_view* end() const noexcept { return names_ + size_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool overflowed() const noexcept { return overflow_; }

private:
    std::string_view names_[kCapacity]{};
    size_t size_ = 0;
    bool overflow_ = false;
};

struct ProjectOptions {
    // Layers to leave out, by name: a simulation or a check leaves out one person's own
    // tuning (`local`), so its results do not depend on whose machine runs it.
    LayerNames skip = LayerNames();
    // Leave out every layer marked dev_only, as a shipping build always does: a check or a
    // simulation that must not depend on anyone's development layers.
    bool skip_dev_only = false;
    // Go on past a layer that does not read, as a tool that shows every problem at once
    // does; a game stops at the first.
    bool go_on = false;
    // Flags the host defines beside the project's (defines ...), for #ifdef: the build's
    // features, the platform, a command-line switch. Each is listed by active_defines() as
    // the host's; Config::set_defines changes them later. The views need last only for the
    // call.
    //   static constexpr std::string_view kDefines[] = {"server", "steam"};
    //   open_project(cfg, project, {.defines = kDefines});
    std::span<const std::string_view> defines{};
};

// Sets `cfg.session()`'s pack levels, include directories, and defines from `project`, then
// pushes its layers, bottom to top. A Config to open a project into has no layers yet; set
// its session's reader first if the layers' text comes from somewhere other than the disk.
// Stops at the first layer that does not read, with its error; refuses options whose skip
// list overflowed, before changing anything. With `go_on`, a layer that
// does not read is left out of the stack, as a failed push_layer leaves it, the layers above
// it read over the ones below, and each such layer's problem is returned, in stack order:
// none if every layer read. Only a problem allocates.
[[nodiscard]] Expected<List<ProjectProblem>> open_project(Config& cfg, const Project& project,
                                                          const ProjectOptions& options = {});

}  // namespace baker
