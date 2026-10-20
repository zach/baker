# Changelog

Baker's releases, newest first. Versions follow [semantic versioning](https://semver.org);
before 1.0.0, a minor version may change the API.

## 0.2.0 - 2026-10-20

- **New: the host kit, `baker::host`.** A separate library beside the core for the part of
  a game's tooling every game writes the same way. It is built with `BAKER_BUILD_HOST`, on
  by default whenever `baker::script` is built, and you link it on its own; its headers are
  in `<baker/host/...>`. [Baker and your tools](docs/tools.md#the-host-kit) describes each
  piece, and [`examples/host`](examples/host) is a game's whole tooling in one `main`.
  - **`Live`:** the edit-to-see loop. It opens the stack from the nearest
    `baker-project.dta` (or a `Project` built in code), adds the check validator and, when
    `baker::shape` is built, the shape validator, binds the Config's tools, and runs
    `#autorun`. Each frame, `begin_frame()` reloads what was saved, timed from the save, and
    answers the console. Its `changed` callback is where the game reads its structured data.
  - **`Watcher`:** exactly the files the stack was read from, through inotify or
    `ReadDirectoryChangesW`, or by their write time where there is no notification. Hooks
    map an engine's virtual paths to files on the disk, or answer for paths that have none.
  - **`Console`:** one-line requests on 127.0.0.1, polled once a frame. A request,
    `(ID {command})`, is answered with one line of reply JSON, and a frame's replies go out
    together, so a tool's batch of requests is answered within the frame.
  - **The commands:** `check`, `reference [--json]`, `eval`, `diff` and `promote [--patch]`,
    run from your game's own binary with `parse_command_line` and `run_command`, so they
    know its script functions.
  - **Shipping builds:** under `BAKER_SHIPPING` it opens no socket and sets no watch; the
    stack still opens, and `diff` and `promote` refuse.
- **New: `{config.clear PATH}`** drops one path's live edit, so the value is the files'
  again, and the journal records it as reverted. It returns 1 if there was an edit and 0 if
  not, and changes nothing when there was none. It is bound in development builds by
  `bind_config_tools`, next to `config.set`.
- **Changed: `Config::epoch()` moves on every live edit.** Before, no `set()` moved it:
  neither the first set of a path nor a repeated one. Now every `set()` does, along with
  `clear_scratch()`, write-back, `promote()` and `restore()`, as before. Compare it once a
  frame to tell that what you show or cache is stale, whoever made the edit. The fast path
  of `set()` costs one more increment and still allocates nothing.
- **Changed: `examples/remote-console`** uses the host kit's `Console` in place of its own
  server; the example's client is unchanged.

## 0.1.1 - 2026-10-20

- **Fixed: options built before `open_project` kept a layer they should leave out.**
  `ProjectOptions::skip` held its names in a list that ended with the statement that wrote
  it. Options chosen by a condition, kept in a variable, or returned by a helper, and
  passed later, read freed memory; an optimized build could then open a layer it was told
  to leave out.
  - **The new type.** `skip` is now a `LayerNames`: up to eight names held in the options
    themselves, so options stay valid for as long as the strings they name do.
  - **Unchanged:** `{.skip = {"local"}}` reads as before.
  - **New:** `skip.add(name)` adds a name to options you keep.
  - **More than eight names:** `open_project` refuses the options rather than open the
    wrong stack.
- **New guide:** [Baker and your tools](docs/tools.md). It covers what Baker gives
  development tools in each build, what it leaves to your game (watching files, carrying
  requests, the game's own commands), and how an external tool should talk to a game.

## 0.1.0 - 2026-10-17

The first release. Baker reads a Lisp-like text format into trees, stacks files into layers
that reload while the program runs, and scripts behavior in the same syntax.

- **Data.** Parsing with macros, conditionals, includes and merges; lookups by tag and by
  path; typed reads with defaults; a printer that round-trips.
- **Errors.** Every error names its file and line, and a parse error its column, counted
  in characters.
- **Config.** Layers of files and overlays that reload on change, with rollback, provenance
  (`source`, and `explain` for a path's whole layer chain), live edits through `Tweak<T>`,
  layer placement (`below`/`above`), removal with `(key #remove)`, and write-back of edits
  to the files they came from as one-line diffs. A whole number tuned from a float field
  keeps its integer form.
- **Received layers.** Text a host received (a live-ops overlay, a mod) is pushed as a
  layer from the text alone, checked first under a `LayerPolicy` that limits what it may
  change. A `LayerChecker` checks on another thread, and its report lets the push skip
  the check.
- **Project files.** `baker-project.dta` names a game's layers, pack levels, include
  directories, defines and reference, for the game and its tools alike. `read_project`
  keeps each entry's position, `open_project` can report every layer's problem at once,
  and `find_project` finds the file that governs a path. Paths with a scheme (`user://`)
  are kept as written.
- **Watching files.** `Config::stack_files()` lists every file the layers were read from,
  with an epoch that moves when the list does, for a host that watches files itself.
- **Binary cache.** Trees written as they lie in memory and loaded with one allocation,
  laid out so that an edit to the text changes few bytes of the cache, for delta patches.
- **Packs.** Levels, ranks and the one-way rule; versions and versioned requirements;
  mods read as text only.
- **Shapes.** Declared structure for data, checked with located errors and warnings; a
  softer pack may narrow a harder pack's shape, not loosen it.
- **Script.** Functions, classes and objects, typed messages, tasks, a checker, call
  statistics, and autorun that follows the layer stack.
- **Tools.** One line in, one line of JSON out (`eval_request`), the Config's tools as
  script functions (`bind_config_tools`), and the vocabulary as JSON: functions, types,
  named objects, tweaks and shapes. `check` writes JSON too. A tool that has a game's
  reference but not its code registers its functions from their descriptions
  (`Context::declare`) and checks data as the game would. A load lists the macros and
  script functions it defined, and where.
- **Builds.** C++20 on GCC 13, Clang 18 and MSVC 2022; a shipping mode that compiles out
  live editing; Baker's own allocators throughout, optionally EASTL containers.
