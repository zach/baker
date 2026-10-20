#pragma once

// Live: the edit-to-see loop, as every host would write it. It owns a Config, opens the
// stack a project file describes, and each frame reloads what was saved, so a value edited
// in a text editor shows in the running game the next frame.
//
//   baker::Context ctx;              // bind the game's script functions first
//   baker::host::Live live(ctx);
//   if (auto r = live.open({.project = "data"}); !r) ...;   // the nearest baker-project.dta
//   each frame:
//     live.begin_frame();            // reloads what was saved; answers the console
//     ...read tweaks, simulate, draw, present...
//     live.end_frame();              // the frame that shows a reload: closes edit-to-see
//
// open():
//   - finds the project file from `project` (find_project: that path or the nearest
//     directory above it), or takes `built` when there is none, as a packaged game whose
//     data sits beside the executable does;
//   - opens it (open_project, with `open`'s options);
//   - adds the check validator, and the shape validator when baker::shape is built;
//   - binds the Config's tools (bind_config_tools), and those that write files only with
//     `console_write`;
//   - runs every enabled layer's #autorun commands, then calls `changed`;
//   - watches the stack's files and the project file, and serves the console on
//     `console_port` if one is given.
// After any change to the stack (a reload, a toggle from a panel, a layer pushed through
// the console) it runs #autorun again at the next begin_frame, then calls `changed`, where
// the game reads its structured data; after a live edit (epoch() moved) it calls `changed`
// alone.
//
// The Config's clock is the files' (file_clock_now_ns), so a reload's origin is when its
// file was saved, and loop_stats() measures from the save to the frame that showed it.
// A quiet frame allocates nothing. Under BAKER_SHIPPING the watcher and the console are
// inert; the stack still opens, and takes live-ops overlays as the Config allows.

#include <baker/callback.hpp>
#include <baker/config.hpp>
#include <baker/expected.hpp>
#include <baker/host/watcher.hpp>
#include <baker/parse.hpp>
#include <baker/project.hpp>
#include <baker/script.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace baker::host {

struct LiveOptions {
    // A path at or below the project's root: the nearest baker-project.dta from here
    // describes the stack. Empty: `built` alone.
    std::string_view project{};
    // The stack when no project file is found: a packaged game's, say. Null: none, and a
    // missing project file is an error.
    const Project* built = nullptr;
    // How the stack opens: layers to leave out (a simulation's `local`), and whether to go
    // on past a layer that does not read.
    ProjectOptions open = ProjectOptions();
    // Reads the project file and the layers, if not from the disk (an engine's virtual
    // roots); the watcher's hooks: where those paths are on the disk, and the stamps of
    // those that are not.
    LoadSession::Reader reader{};
    DiskPathHook disk_path{};
    StampHook stamp{};
    bool watch = true;           // false: reload only when asked (tests, a headless runner)
    uint16_t console_port = 0;   // 0: no console
    bool console_write = false;  // also bind config.write_back and config.promote
    // After the load, after each change to the stack once #autorun has run, and at the
    // next begin_frame after a live edit (a set(), from the game, a bound tweak, or the
    // console; a clear_scratch(), a write-back, a restore).
    Callback<void(const Config&)> changed{};
    // Warnings, validator findings, failed reloads and #autorun commands, as they happen.
    Callback<void(const Error&)> log{};
};

class Live {
public:
    explicit Live(Context& ctx, const ConfigOptions& config = {});
    ~Live();
    Live(const Live&) = delete;
    Live& operator=(const Live&) = delete;

    // Opens the stack, as above. An error if no project file was found and none was built,
    // if the project file does not read, or if a layer does not (unless `open.go_on`); the
    // problems are also in load_errors().
    [[nodiscard]] Expected<void> open(const LiveOptions& options);

    // Answers the console, reloads if a watched file was saved, and runs #autorun if the
    // stack changed. Call before the frame reads tweaks.
    void begin_frame();
    // The frame that shows the last reload is presented.
    void end_frame() noexcept;
    // Reloads now, whatever the watcher says; true if anything changed.
    bool reload_now();

    [[nodiscard]] Config& config() noexcept;
    [[nodiscard]] const Config& config() const noexcept;
    // The project file the stack was opened from; empty when built.
    [[nodiscard]] Symbol project_file() const noexcept;
    // The located problems of the last load: a layer that did not read, each #autorun
    // command that failed. What a check gate reports.
    [[nodiscard]] std::span<const Error> load_errors() const noexcept;
    // The console's port, once open() started one; 0 if none.
    [[nodiscard]] uint16_t console_port() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace baker::host
