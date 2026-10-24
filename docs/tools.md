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
| **Always**, shipping builds included | Reading and provenance (`source`, `tools::explain`), `check` and its JSON, the reference JSON, the project file (`read_project`, `open_project`, `find_project`), `stack_files()`, `Context::declare`, `tools::script_functions`, `macro_definitions`, received layers checked under a `LayerPolicy` |
| **Development builds** | Live edits (`Config::set`, `Tweak<T>::set`), `reload()`, layer toggles, the scratch layer and its journal, `loop_stats()`, snapshots |
| **When you ask** (`tools::bind_config_tools(ctx, cfg, {.allow_write = true})`), development builds | Writing a live edit back to its file: `write_back`, `write_back_to`, `promote` |

`tools::bind_config_tools` makes the Config's tools script functions (`config.explain`,
`config.set`, ...), binding only those the build allows, and `eval_request` turns one line
of text into one line of JSON (see [the reply format](reply-json.md)). Together they let any
transport you choose carry Baker's tools: a socket, a pipe, an in-game console.

**Where they live.** What only a tool calls, or the host kit on a tool's behalf, is the
`baker::tools` library, in `<baker/tools/...>` and namespace `baker::tools`: the reference
and the check report as JSON, `explain`, `layer_effect` and `diff`, the console's
`config.*` commands, the vocabulary declared from a reference (`DescribedTweaks`,
`declare_type`, `declare_object`), `script_functions` and `read_structure`. A tool links it
directly; a game links it only through the host kit, which uses it. What a game runs on its
own data stays in the core: `check`, `source`, write-back, the tweaks' JSON (so a data-only
program can write it), `split_metadata` and `presented_kind`.

### Text as a person types it

An editor works on text that rarely parses, and checks it on every pause:

- **Its structure.** `tools::read_structure(text)` reads the text with the parser's own lexer and
  gives back every token (with its kind and byte span), every bracket (its parent and its
  tag) and every comment, even when the text does not parse. An unclosed bracket runs to the
  end of the text, an unterminated string or comment ends there, and the problems are listed
  beside the rest. It reads 2 MB in about 25 ms. The outline, folding, completion, hover, a
  rename and a highlighter can all work from it, with no lexer of the tool's own.
- **Laid out as Baker writes it.** `tools::format_text(text, sink, {.layout = ...})` lays
  out text as `write_dta` lays out a tree, with the project's `(format (indent 3) (width
  100))`, keeping every comment, directive, macro and spelling. Only whitespace changes:
  - a form goes on one line if it fits, else one item a line;
  - a `;` comment stays on its line, and a blank line between forms stays (once);
  - a table or a column of trailing comments written aligned stays aligned, re-padded when
    a longer key is added;
  - text that does not parse comes back unchanged, with its errors.

  It returns the edits too, each a gap replaced by a number of line ends and spaces, so an
  editor applies them without moving the cursor through unchanged text; `.begin` and
  `.end` lay out only the forms a selection touches. Formatting twice changes nothing. 2 MB
  takes about 60 ms.
- **Values as Baker presents them.** `split_metadata(values)` separates a value from the
  metadata after it, `(width 2 (range 0 10))`, and `presented_kind(values)` gives the kind a
  value is shown as (its `(kind ...)`, or "color" for 3 or 4 numbers ranged 0 to 1). These
  are the rules tweaks follow.
- **Checking again after an edit.** Keep one Config, reading through a reader that serves
  the open editors' text, and call `reload()` after each edit. It compares each layer's text
  by hash and reads again only from the first layer whose text changed, upward, so an edit
  to a small top layer above a 2 MB one reloads in about 2 ms where a fresh load takes about
  57. The result, its errors included, is what a fresh load gives.

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
  reference (`tools::write_reference_json` after your setup), a headless run. Only your game
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
run again, and after a live edit, which it applies first (`Config::apply_edits()`), so a
game that reads whole records or tables from `root()` sees the edit. The Config's clock is the files', so `loop_stats()`
measures from the save to the frame that showed it. A quiet frame allocates nothing.
`.console_any_port = true` serves the console on a free port instead, which
`console_port()` reports: a tool that starts several instances of the game reads each one's
port from it rather than choosing one first.

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

The kit's flags all begin `--baker-`, so they never take a name your game's own flags use:
any other flag is yours, in `line->rest`, and a `--baker-` flag the kit does not know is an
error, so a misspelling is caught.

| Command | What it writes |
|---|---|
| `check` | [check's report](check-json.md); exit 1 on an error, a load problem, a shape error, or a call to an undefined function |
| `reference [--baker-json]` | the script vocabulary as Markdown, or `{"reference", "tweaks", "shapes"}` |
| `eval [--baker-seed N] [--baker-json] "{command}"...` | each command's result on the game reset with the seed; `--baker-json`: one reply line each |
| `diff [--baker-layer L]` | what layer L (default `agent`) changes, value by value |
| `promote [--baker-from L] [--baker-to T] [--baker-write \| --baker-patch FILE]` | what promoting L into T would write; `--baker-write` writes; `--baker-patch` writes a unified diff and nothing else |
| `sim [--baker-scenario NAME]... [--baker-seed N \| --baker-seeds A-B] [--baker-step-hz N] [--baker-json] [--baker-results DIR]` | the project's scenarios run and judged by its objectives ([`baker.sim/1.3`](sim-json.md)); exit 1 when one fails |
| `compare --baker-before DIR --baker-after DIR` | which objectives broke or were fixed between two `sim --baker-results` ([`baker.compare/1`](sim-json.md)) |
| `format [--baker-check] [--baker-json] [FILE...]` | lays out each file (the stack's own files under the project's directory if none is named) with the project's `(format ...)`; `--baker-check` changes nothing and exits 1, listing each file that would change, for CI and a pre-commit hook; `--baker-json`: `{"schema": "baker.format/1", "files": [{"file", "changed", "line_end", "edits": [[begin, end, newlines, spaces], ...], "errors"}]}` |

Each runs on a fresh Context with your functions bound (`CommandHooks::bind`), the stack
opened as the game opens it, and, for `eval`, your state reset (`CommandHooks::reset`).
`sim` steps your game with `CommandHooks::step`, and `check` reads the project's scenarios
and objectives with the data. A game that can only step inside its engine's main loop
drives a `SimRun` from its frames instead ([how](sim-json.md#a-run-your-own-loop-advances)). `extra_trees` adds other files of your own to `check`. The exit status is 0, 1
when something failed, 2 when it could not run. In a shipping build, `diff` and `promote`
refuse. A tool reads the commands' standard output as the document itself, so your
functions and `#autorun` must print nothing there while a command runs. Where that cannot
be kept clean (an engine prints its own lines), `--baker-output FILE` writes the output to a
file instead, or `CommandHooks::output` routes it. `CommandHooks::reader` reads the stack
through your file access, as `Live` does, for files under an engine's own roots.

## How an external tool talks to a game

- **Files.** The project file (`baker-project.dta`) gives the stack. It names the reference
  JSON your build writes (`(reference "build/reference.json")`), the game binary that answers
  the commands (`(tool ...)`, then any arguments the binary takes first, such as an engine's
  `(tool "engine" "--path" "." "--")`, which a tool runs from the project file's directory),
  where the build put it when the project file cannot know (`(tool "$build/bin/my_game")`,
  with `(build "../build")` as the default a tool may override; `Project::tool_path(dir)`
  resolves it), the port its console listens on (`(console 4555)`, which
  the game may use as its default while its own command line wins), and its scenarios and
  objectives (`(scenarios ...)`, `(objectives ...)`).
- **Documents.** [The reference](reference-json.md), [check's report](check-json.md),
  [the reply](reply-json.md) and [sim's results](sim-json.md) are versioned JSON. A field is added only with a new minor
  version.
- **The console.** One request per line, one JSON reply per line, to a running game.
  `{config.epoch}` says whether anything changed since a tool last asked.
- **Commands.** Your game binary's own commands, run by the tool.

A tool that checks data without the game reads the project file, then the reference, and
declares the game's functions from it (`Context::declare`). It then runs `check` over the
same stack the game loads.
