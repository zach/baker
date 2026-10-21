# Baker and your tools

Baker is a library. It knows what your data means: where each value came from, whether a
script call is valid, how to write a tuned value back to its line. It does not touch your
operating system. Your game (the *host*) decides when to reload, which tools to expose, and
how they travel, and external tools (an editor, an agent, a live-ops service) talk to your
game through what the two publish. This page says which part does what.

- [Three parts](#three-parts)
- [What Baker gives tools](#what-baker-gives-tools)
- [What the host does](#what-the-host-does)
- [The host kit](#the-host-kit)
- [How an external tool talks to a game](#how-an-external-tool-talks-to-a-game)

## Three parts

| | Part | Holds | Never |
|---|---|---|---|
| 1 | **Baker** | What needs Baker's rules: provenance, checking, write-back, the JSON documents, the project file | Opens a file or socket itself, starts a thread to watch, draws UI, decides policy |
| 2 | **Your game** | The operating system (files, sockets, windows, the command line), your own script functions, and policy | Reimplements Baker's rules |
| 3 | **External tools** | A user interface, a protocol, another product | Links your game's code, or reimplements Baker's rules |

To place a feature: if a second host would have to copy Baker's rules to build it, it
belongs in Baker. If it needs the operating system, your game's code, or a choice about
what is allowed, it belongs in your game. Anything else is the external tool's own.

## What Baker gives tools

Baker reads through your `LoadSession` reader and writes through your Config's writer, so
everything below works over a virtual file system, an editor's unsaved buffers, or a
network store. Nothing here costs anything until you call it.

| Available | What |
|---|---|
| **Always**, shipping builds included | Reading and provenance (`source`, `explain`), `check` and its JSON, the reference JSON, the project file (`read_project`, `open_project`, `find_project`), `stack_files()`, `Context::declare`, `script_functions`, `macro_definitions`, received layers checked under a `LayerPolicy` |
| **Development builds** | Live edits (`Config::set`, `Tweak<T>::set`), `reload()`, layer toggles, the scratch layer and its journal, `loop_stats()`, snapshots |
| **When you ask** (`bind_config_tools(ctx, cfg, {.allow_write = true})`), development builds | Writing a live edit back to its file: `write_back`, `write_back_to`, `promote` |

`bind_config_tools` makes the Config's tools script functions (`config.explain`,
`config.set`, ...), binding only those the build allows, and `eval_request` turns one line
of text into one line of JSON (see [the reply format](reply-json.md)). Together they let any
transport you choose carry Baker's tools: a socket, a pipe, an in-game console.

## What the host does

Each of these has a ready-made form in [the host kit](#the-host-kit); a host with its own
file system, console or command line does them with Baker's calls directly.

- **Watch files.** List what to watch with `cfg.stack_files()`. Check
  `cfg.stack_files_epoch()` once a frame, and re-read the list when it moves. When a file
  changes, call `cfg.reload({.origin_ns = save_time})`, so that `loop_stats()` measures
  edit-to-see from the save.
- **Carry requests.** A console server reads lines and answers with `eval_request`. Keep
  any console on loopback: it runs any command it is sent.
- **Offer commands** that need your game's own script functions: a check of the data, the
  reference (`write_reference_json` after your setup), a headless run. Only your game
  binary knows its functions, so these commands live in it.
- **Decide policy:**
  - which tools to bind, and whether to allow writes;
  - when a layer may change (`freeze`, `live_layers`);
  - which layers a check or a simulation leaves out (`ProjectOptions::skip`).

## The host kit

`baker::host` is a library beside Baker's core, built with `BAKER_BUILD_HOST` (on with
`BAKER_BUILD_SCRIPT`) and linked on its own: it is the part of a host that every game
would write the same way, on the operating system Baker itself never touches. Its headers
are in `<baker/host/...>`. Under `BAKER_SHIPPING` it opens no socket and sets no watch;
the stack still opens. [`examples/host`](../examples/host) is a game's whole tooling in one
`main`.

### `Live`: the edit-to-see loop

```cpp
baker::Context ctx;                // bind the game's script functions first
baker::host::Live live(ctx);
live.open({.project = "data", .console_port = 4555,
           .changed = [](const baker::Config& cfg) { /* read structured data */ }});
// each frame
live.begin_frame();                // reloads what was saved; answers the console
// ...simulate and draw...
live.end_frame();                  // the frame that shows a reload
```

`open` finds the nearest `baker-project.dta` (or takes a `Project` built in code, for a
packaged game), opens the stack, adds the check validator and, when `baker::shape` is
built, the shape validator, binds the Config's tools, and runs every layer's `#autorun`.
`changed` is called after the load, after each change to the stack once `#autorun` has
run again, and after a live edit. The Config's clock is the files', so `loop_stats()`
measures from the save to the frame that showed it. A quiet frame allocates nothing.

### `Watcher`: the files the stack was read from

`Watcher` watches exactly `stack_files()` (and any paths you add) through the operating
system's notifications: inotify on Linux, `ReadDirectoryChangesW` on Windows, on each
file's directory. A file whose directory does not exist yet is checked by its write time
and size every 100 ms. For an engine's virtual paths, `set_disk_path_hook` says which file
on the disk a path is, so it is watched like any other and still reported by your name;
`set_stamp_hook` answers for a path with no file on the disk at all. `Live` runs one.

### `Console`: one-line requests on loopback

`Console(ctx).listen(port)` serves 127.0.0.1 only; `poll()` once a frame answers each
complete line. A request, `(ID {command})`, gets one line of
[reply JSON](reply-json.md); any other line is evaluated and answered as text. Every reply
is its lines, then an empty line. A frame's replies go out together, so a tool's batch of
requests is answered within the frame. `Live` runs one when given a port.

### The commands

```cpp
int main(int argc, char** argv) {
    auto line = baker::host::parse_command_line(argc, argv);
    // your own flags are in line->rest; set line->project if you name the stack otherwise
    const int status = baker::host::run_command(*line, {.bind = bind_my_functions});
    if (status >= 0) return status;  // -1: not one of the kit's commands
    // ...your own commands, or play...
}
```

| Command | What it writes |
|---|---|
| `check` | [check's report](check-json.md); exit 1 on an error, a load problem, a shape error, or a call to an undefined function |
| `reference [--json]` | the script vocabulary as Markdown, or `{"reference", "tweaks", "shapes"}` |
| `eval [--seed N] [--json] "{command}"...` | each command's result on the game reset with the seed; `--json`: one reply line each |
| `diff [--layer L]` | what layer L (default `agent`) changes, value by value |
| `promote [--from L] [--to T] [--write \| --patch FILE]` | what promoting L into T would write; `--write` writes; `--patch` writes a unified diff and nothing else |
| `sim [--scenario NAME]... [--seed N \| --seeds A-B] [--step-hz N] [--json] [--results DIR]` | the project's scenarios run and judged by its objectives ([`baker.sim/1`](sim-json.md)); exit 1 when one fails |
| `compare --before DIR --after DIR` | which objectives broke or were fixed between two `sim --results` ([`baker.compare/1`](sim-json.md)) |

Each runs on a fresh Context with your functions bound (`CommandHooks::bind`), the stack
opened as the game opens it, and, for `eval`, your state reset (`CommandHooks::reset`).
`sim` steps your game with `CommandHooks::step`, and `check` reads the project's scenarios
and objectives with the data. `extra_trees` adds other files of your own to `check`. The exit status is 0, 1
when something failed, 2 when it could not run. In a shipping build, `diff` and `promote`
refuse. A tool reads the commands' standard output as the document itself, so your
functions and `#autorun` must print nothing there while a command runs. Where that cannot
be kept clean (an engine prints its own lines), `--output FILE` writes the output to a
file instead, or `CommandHooks::output` routes it. `CommandHooks::reader` reads the stack
through your file access, as `Live` does, for files under an engine's own roots.

## How an external tool talks to a game

- **Files.** The project file (`baker-project.dta`) gives the stack. It names the reference
  JSON your build writes (`(reference "build/reference.json")`), the game binary that answers
  the commands (`(tool ...)`), and its scenarios and objectives (`(scenarios ...)`,
  `(objectives ...)`).
- **Documents.** [The reference](reference-json.md), [check's report](check-json.md),
  [the reply](reply-json.md) and [sim's results](sim-json.md) are versioned JSON. A field is added only with a new minor
  version.
- **The console.** One request per line, one JSON reply per line, to a running game.
- **Commands.** Your game binary's own commands, run by the tool.

A tool that checks data without the game reads the project file, then the reference, and
declares the game's functions from it (`Context::declare`). It then runs `check` over the
same stack the game loads.
