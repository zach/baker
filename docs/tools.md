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
| **Always**, shipping builds included | Reading, checking, the project file and the vocabulary: the list below |
| **Development builds** | Live edits (`Config::set`, `Tweak<T>::set`), `reload()`, layer toggles, the scratch layer and its journal, the change log (`set_change_history`, `undo`, `redo`, [`baker.changes/1`](changes-json.md)), `loop_stats()`, snapshots |
| **When you ask** (`tools::bind_config_tools(ctx, cfg, {.allow_write = true})`), development builds | Writing a live edit back to its file: `write_back`, `write_back_to`, `promote` |

Always available, shipping builds included:

- reading and provenance: `source`, `tools::explain`;
- `check` and its JSON, and the reference JSON;
- the project file: `read_project`, `open_project`, `find_project`;
- `stack_files()`;
- `Context::declare`, `tools::script_functions`, `macro_definitions`;
- received layers, checked under a `LayerPolicy`.

`tools::bind_config_tools` makes the Config's tools script functions (`config.explain`,
`config.set`, ...; the [script reference](script-reference.md) lists every one with its
arguments), binding only those the build allows, and `eval_request` turns one line
of text into one line of JSON (see [the reply format](reply-json.md)). Together they let any
transport you choose carry Baker's tools: a socket, a pipe, an in-game console.

**Where they live.** What only a tool calls, or the host kit on a tool's behalf, is the
`baker::tools` library, in `<baker/tools/...>` and namespace `baker::tools`: the reference
and the check report as JSON, `explain`, `layer_effect` and `diff`, the console's
`config.*` commands, the vocabulary declared from a reference (`DescribedTweaks`,
`declare_type`, `declare_object`), `script_functions`, `read_structure`, `format_text` and
`migrate_text`. A tool links it directly; a game links it only through the host kit, which
uses it. What a game runs on its own data stays in the core: `check`, `source`, write-back,
the tweaks' JSON (so a data-only program can write it), `metadata()` and `presented_kind`.

### Text as a person types it

An editor works on text that rarely parses, and checks it on every pause:

- **Its structure.** `tools::read_structure(text)` reads the text with the parser's own lexer and
  gives back every token (with its kind and byte span), every bracket (its parent and its
  tag) and every comment, even when the text does not parse. An unclosed bracket runs to the
  end of the text, an unterminated string or comment ends there, and the problems are listed
  beside the rest. It reads 2 MB in about 25 ms. The outline, folding, completion, hover, a
  rename and a highlighter can all work from it, with no lexer of the tool's own. A row's
  key, the `/a.b.c` of a top-level `(/a.b.c 1)`, is a token of the kind `PathKey`.
- **Laid out as Baker writes it.** `tools::format_text(text, sink, {.layout = ...})` lays
  out text as `write_dta` lays out a tree, with the project's `(format ...)`, keeping every
  comment, directive, macro and spelling. Only whitespace changes:
  - a form goes on one line if it fits, else one item a line;
  - a `;` comment stays on its line, and a blank line between forms stays (once);
  - a table or a column of trailing comments written aligned stays aligned, re-padded when
    a longer key is added;
  - text that does not parse comes back unchanged, with its errors.

  It returns the edits too, each a gap replaced by a number of line ends and spaces, so an
  editor applies them without moving the cursor through unchanged text; `.begin` and
  `.end` lay out only the forms a selection touches. Formatting twice changes nothing. 2 MB
  takes about 60 ms.
- **Old spellings, with their fixes.** `tools::migrate_text(text, sink)` finds each form a
  language migration rewrites, writes the text with all of them rewritten to `sink`, and
  returns them as findings. Each finding gives its migration (`Migration::Metadata`, the `@`
  of metadata; `Migration::Declarations`, `#pack`, `#origin`, `#shape` and `[NAME ...]`
  macros), its byte span, a warning worded as a check words it, and its edits: byte spans
  of the text replaced, and nothing else changed. An editor shows the warnings in every file
  of a project and offers a finding's edits as a quick fix (`apply_findings` with that one
  finding), or all of them for the file; a CI step fails on any finding; an agent writing
  from older examples is told what to change. What a migration will not decide, an array
  whose children are all tagged and the last with a metadata word, is a finding with no
  edits that says why. Text that does not parse gets the findings that can be located.
  Migrating the result finds nothing to rewrite. 2 MB takes about 25 ms.

  The `baker_migrate` tool (in `tools/`) runs it over files: `--write` rewrites them,
  `--report` lists what would change, `--check` exits 1 if anything would, `--only metadata`
  or `--only declarations` picks one, and `--cpp` and `--md` migrate the Baker text in C++
  string literals and in Markdown.
- **The house layout.** Three spaces a level and a hundred columns is `WriteOptions`'
  default, and the layout of a project without `(format ...)`. A Config opened from a
  project (`open_project`) writes in the project's layout: the forms promote and write-back
  add to a layer's file, and `layer_text` (`Config::set_layout`).
- **Values as Baker presents them.** An array's `values()` end before its metadata,
  `(width 2 (@range 0 10))`, and `metadata()` lists it; `presented_kind(values)` gives the
  kind a value is shown as (its `(@kind ...)`, or "color" for 3 or 4 numbers ranged 0 to 1).
  These are the rules tweaks follow.
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
built, the shape, ID and rules validators, binds the Config's tools (`config.*`; not in a
shipping build, so a shipping game links none of their code), and runs every layer's
`#autorun`. A development build runs the validators after every change to the stack and
reports what they find; a shipping build adds them `checks_only`, so they judge every check
and policy push of an overlay but cost nothing at a change.
When the watcher sees a save, `begin_frame` reloads. Given `.run_async`, a callback that
hands work to the game's job system, it reads and parses the changed files there, and a
later `begin_frame` merges them, so the frame pays only the merge; without it the whole
reload runs in `begin_frame`. Either way the reload's scan opens only the files whose
stamp (size and write time) moved.
`changed` is called after the load, after each change to the stack once `#autorun` has
run again, and after a live edit, which it applies first (`Config::apply_edits()`), so a
game that reads whole records or tables from `root()` sees the edit. The Config's clock is
the files', so `loop_stats()` measures from the save to the frame that showed it. A quiet
frame allocates nothing. `.console_any_port = true` serves the console on a free port instead, which
`console_port()` reports: a tool that starts several instances of the game reads each one's port
from it rather than choosing one first.

### `Watcher`: the files the stack was read from

`Watcher` watches exactly `stack_files()` (and any paths you add) through the operating
system's notifications: inotify on Linux, `ReadDirectoryChangesW` on Windows, on each
file's directory. A file whose directory does not exist yet is checked by its write time
and size every 100 ms. For an engine's virtual paths, `set_disk_path_hook` says which file
on the disk a path is, so it is watched like any other and still reported by your name;
`set_stamp_hook` answers for a path with no file on the disk at all. `Live` runs one.

### `Console`: one-line requests on loopback

`Console(ctx).listen(port)` serves 127.0.0.1 only; `poll()` once a frame answers the
complete lines clients have sent. A request, `(ID {command})`, gets one line of
[reply JSON](reply-json.md); any other line is evaluated and answered as text. Every reply
is its lines, then an empty line. A frame's replies go out together, so a tool's batch of
requests is answered within the frame, up to the frame's budget. `Live` runs one when given
a port, with `LiveOptions::console`.

What a console may cost a frame is bounded by `ConsoleOptions`:

- **`budget_ns`, 0.5 ms by default.** A poll answers lines until that much of it is spent,
  read on a steady clock after each line, and leaves the rest, in order, for the next
  poll. It always answers one, so a backlog moves every frame, and the clients take turns
  at going first. A poll overruns its budget by at most the line it was answering. 0 answers
  every complete line in one poll, however long they take.
- **`max_bytes`, 1 MB by default.** A client holding more than this of unanswered input is
  dropped, and `log` (Live's `log`, unless you set one) is told. While more than this of a
  client's replies waits unsent, its further lines wait unanswered, so a client that sends
  without reading reaches the cap and is dropped; one reply, however large, is sent whole.
  0 is no cap.

```cpp
live.open({.project = "data", .console_port = 4555,
           .console = {.budget_ns = 250'000, .max_bytes = 256 * 1024}});
```

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
error, so a misspelling is caught. `--baker-project PATH` names the stack for any command:
the nearest `baker-project.dta` from PATH.

Each runs on a fresh Context with your functions bound (`CommandHooks::bind`), the stack
opened as the game opens it, and, for `eval`, your state reset (`CommandHooks::reset`).
`CommandHooks::tables` (the generated `register_tables`) registers your baked tables with
each command's Config, so `check` fails on a record a table cannot read whole;
`CommandHooks::config` is the `ConfigOptions` each command's Config is made with (its
caches, its stack image). `sim` steps your game with `CommandHooks::step`, and `check` reads
the project's scenarios and objectives with the data. A game that can only step inside its
engine's main loop drives a `SimRun` from its frames instead
([how](sim-json.md#a-run-your-own-loop-advances)). `extra_trees` adds other files of your
own to `check`. `CommandHooks::reader` reads the stack through your file access, as `Live`
does, for files under an engine's own roots.

The exit status is 0, 1 when something failed, 2 when it could not run. In a shipping build,
`diff`, `promote` and `rename` refuse. A tool reads the commands' standard output as the
document itself, so your functions and `#autorun` must print nothing there while a command
runs. Where that cannot be kept clean (an engine prints its own lines), `--baker-output FILE`
writes the output to a file instead, or `CommandHooks::output` routes it.

| Command | Purpose |
|---|---|
| [`check`](#check) | Checks the stack and writes check's report |
| [`assets`](#assets) | Lists every asset the merged stack names |
| [`bake`](#bake) | Writes the contract layers and the baked tables as one stack image |
| [`reference`](#reference) | Writes the script vocabulary |
| [`diff`](#diff) | Shows what a layer changes, value by value |
| [`promote`](#promote) | Shows or writes what promoting one layer into another writes |
| [`eval`](#eval) | Runs script commands on the game reset with a seed |
| [`sim`](#sim) | Runs the project's scenarios and judges them by its objectives |
| [`compare`](#compare) | Says which objectives broke or were fixed between two `sim` runs |
| [`rename`](#rename) | Keys a record anew and rewrites every value that names it |
| [`format`](#format) | Lays out files as Baker writes them |

#### `check`

`check [--baker-strict-views]` writes [check's report](check-json.md). It exits 1 on an error, a
load problem, a shape error (an asset that is not there is one), or a call to an undefined
function. A shape that governs no array is a shape warning, and so is a view the game compiled
against another layout of its shape (`CommandHooks::views`). `--baker-strict-views` makes that
view an error, for a farm.

#### `assets`

`assets` writes every asset the merged stack names, as [`baker.assets/1`](assets-json.md),
sorted by file, line and path. It exits 1 when a file is not there.

#### `bake`

`bake --baker-stack FILE` loads the contract layers (`(views (layers ...))`) as the game loads
them, with its tables registered, and writes them to FILE as one stack image
(`<baker/image.hpp>`). A shipped build opens the image through `ConfigOptions::stack_image` in
place of the layers' text. The game's own binary bakes, so the tables are laid out as its
compiler lays them out.

#### `reference`

`reference [--baker-json]` writes the script vocabulary as Markdown. With `--baker-json` it
writes one object, `{"reference", "tweaks", "shapes"}`, holding
[the three documents](reference-json.md).

#### `diff`

`diff [--baker-layer L] [--baker-rows]` writes what layer L (default `agent`) changes, value by
value. A verb shows as `-> #append v`, and `stale` marks a row whose `(@was ...)` pin no longer
names the value below. `--baker-rows` prints each change as a row, `(/a.b 1)`, as a layer file
holds it.

#### `promote`

`promote [--baker-from L] [--baker-to T] [--baker-write | --baker-patch FILE] [--baker-rows]
[--baker-json]` writes what promoting layer L (default `agent`) into T (default `default`)
would write:

- `--baker-rows` prints it as rows.
- `--baker-write` writes it.
- `--baker-patch FILE` writes a unified diff to FILE and nothing else, after a line
  `# path: note` for each row's `(@note ...)`, which `git apply` skips.
- `--baker-json` writes the promotion's transaction as [`baker.changes/1`](changes-json.md):
  each value it changes, each line it writes. The files are untouched unless `--baker-write`
  is also given.

#### `eval`

`eval [--baker-seed N] [--baker-json] "{command}"...` writes each command's result on the game
reset with seed N (default 1). With `--baker-json` it writes one [reply](reply-json.md) line for
each, as the console answers.

#### `sim`

`sim [--baker-scenario NAME]... [--baker-seed N | --baker-seeds A-B] [--baker-step-hz N]
[--baker-json] [--baker-results DIR]` runs the project's scenarios (all, or those named, by
scenario name or file name) for each seed, stepping the game at N Hz (default 60), and judges
them by its objectives. `--baker-json` writes the results as [`baker.sim/1.3`](sim-json.md);
`--baker-results DIR` writes `results.json` and `results.dta` into DIR, which `compare` reads.
It exits 1 when an objective fails.

#### `compare`

`compare --baker-before DIR --baker-after DIR` writes which objectives broke or were fixed
between two `sim --baker-results` directories, as
[`baker.compare/1`](sim-json.md#bakercompare1). It exits 1 when one broke.

#### `rename`

`rename OLD NEW [--baker-write]` keys the record at path OLD (`weapons.laser`) NEW in the layer
that declares it, keeps `(@aka OLD)`, mints an `(@id ...)` if it has none, and rewrites every
`(key_of ...)` value naming it in every enabled layer. Without `--baker-write`, it lists the
edits, one a line, and writes nothing.

It refuses, writing nothing, when NEW is a sibling's key or alias, or when an edit cannot be
placed in the text (a record a row's path made, a macro's expansion): edit those where they are
written. An overlay's own record keeps the old key, which the alias resolves.

#### `format`

`format [--baker-check] [--baker-json] [--baker-mint-ids TABLE] [FILE...]` lays out each file
with the project's `(format ...)`; with no FILE, it takes the stack's own files under the
project's directory. Before the layout it spells each field by its shape's name where a record
uses a name the shape lists in `(@renamed_from ...)`.

- `--baker-mint-ids TABLE` first writes an `(@id ...)` into each record of TABLE that has none,
  or repeats another's, in the layer that declares the record.
- `--baker-check` changes nothing and exits 1, listing each file that would change, for CI and
  a pre-commit hook.
- `--baker-json` changes nothing and writes each file's edits as
  [`baker.format/1.1`](format-json.md): `rewrites` counts the renames and IDs, and `edits` lays
  out the text after them.

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
  [the reply](reply-json.md), [sim's results](sim-json.md), [the assets](assets-json.md),
  [the changes](changes-json.md) and [format's edits](format-json.md) are versioned JSON. A
  field is added only with a new minor version.
- **The console.** One request per line, one JSON reply per line, to a running game.
  `{config.epoch}` says whether anything changed since a tool last asked.
- **Commands.** Your game binary's own commands, run by the tool.

A tool that checks data without the game reads the project file, then the reference, and
declares the game's functions from it (`Context::declare`). It then runs `check` over the
same stack the game loads.
