# Changelog

Baker's releases, newest first. Versions follow [semantic versioning](https://semver.org);
before 1.0.0, a minor version may change the API.

## 0.4.0 - 2026-10-21

- **New in the host kit: a scenario run your own loop advances.** `sim` steps your game
  through `CommandHooks::step`, many steps inside one call. An engine that owns its main loop
  advances its physics only inside that loop, so its game cannot step on demand.
  `baker::host::SimRun` lets such a game run the kit's scenarios from its frames.
  - **How it works.** It asks for a reset with each run's seed, which your world may take
    several frames to do; call `ready()` when it is done. Then call `step()` once per fixed
    step of `step_seconds()`.
  - **What the kit still does.** It runs the scenario's commands when they are due, judges
    the objectives, and writes the same output, with the same exit status, as `sim`.
  - **Stopping.** `stop()` ends early: the run in progress is kept as not finished, and the
    exit status is 2.
  - **Cost.** A step with nothing due allocates nothing.
- **New: check data against the game's tweaks without the game.** A tool that has the game's
  reference but not its code can now hold data to every tweak, including those of the game's
  own types.
  - **`DescribedTweaks`** (`<baker/tweak.hpp>`) registers a tweak from its description, as
    `baker.tweaks` writes it, with one call for every type. `check_tweaks` then finds what
    the game's own `check` finds: `(aim 1)` where a `vec2` is wanted fails in the tool too.
  - **A type of your own** describes its values by declaring them in its converter:
    `static constexpr std::string_view values[] = {"float32", "float32"};`.
  - **`baker.tweaks/1.2`** adds `values`, Baker's type of each of a tweak's values, and
    `TweakBase::value_types()` gives the same in code.
- **New: check calls on objects, and values of game types, without the game.** A tool can
  now declare the rest of the reference, as it declares functions with `Context::declare`.
  - `Context::declare_type` declares a type with its bases and methods, and
    `Context::declare_object` a named object of it. `check` then holds
    `{tank fire 3}` to the type's methods as the game's `check` does.
  - A parameter of a type of the game's own, such as a `vec2`, is checked by the value
    types its converter declares, so `{aim 1}` where a `vec2` is wanted is an error.
  - **`baker.reference/1.2`** adds `values` to each parameter. `ParamDescription` takes it.
  - **What breaks:** `detail::ParamInfo` keeps its type name behind `text`; read it with
    `type()`. Only code that reads the parameter tables directly is affected.
- **Changed: `sim`'s JSON is `baker.sim/1.1`.** Each run gains `finished`, and the status may
  be `"stopped"`. Readers of 1.0 keep working, since nothing was removed or renamed.

## 0.3.0 - 2026-10-21

- **New: `check` holds the data to the tweaks your game declares.** A value a tweak cannot
  take was invisible to `check`: `(height "tall")` for a float tweak, a number outside its
  range, or a symbol outside its `one_of`. The game then ran on a default without a word.
  - `check_tweaks(root, registry)` (`<baker/tweak.hpp>`) checks a merged tree against the
    registered tweaks without changing any of them. The data's own `(range ...)` and
    `(one_of ...)` win over the code's, as they do for panels.
  - `check()` of a Config fills the new `CheckReport::tweak_errors` and `tweak_warnings`
    from the registry the Config binds (`Config::tweak_registry()`, new). Warnings are a
    tweak the data gives no value, and a key beside a group's tweaks that no tweak reads, as
    a misspelled key looks.
  - **What breaks:** a tweak error fails the check, so data that passed before may fail now.
    The JSON is `baker.check/1.3`, with the two new lists.
- **New in the host kit: scenarios a tool can find and run.** A project file may name the
  game binary that answers the commands, and the game's scenarios and objectives:
  `(tool "build/bin/my_game") (scenarios "sim") (objectives "metrics")`.
  - **Scenarios and objectives.** A `(scenario ...)` runs the game for set seconds from a
    seed, with commands at its start and at set times. An `(objective ...)` measures the
    run and judges the value, in each run or by its median over the seeds.
  - **`sim`** steps your game through the new `CommandHooks::step` and judges each run. It
    writes `baker.sim/1`, with every run, objective and failed command located by file and
    line.
  - **`compare --before DIR --after DIR`** reads two `sim --results DIR` directories and
    writes `baker.compare/1`, naming each objective that broke or was fixed.
  - **`check`** reads the scenarios and objectives with the data.
  - `docs/sim-json.md` describes the forms, the commands and both schemas.
  - **What breaks:** the kit now reads `--scenario`, `--seeds`, `--step-hz`, `--results`,
    `--before` and `--after` itself. A game whose own commands used one of those names should
    rename its flag.

## 0.2.1 - 2026-10-20

- **New: a field's allowed symbols.** A `symbol` or `(list symbol)` shape field may list
  what it allows, `(button symbol (one_of light heavy kick special))`, and `check` reports
  a value outside the list, located, as it reports one outside a range. A later layer
  declaring the field again adds to the list, in the order written. A tweak carries the
  same list as metadata for panels and editors, from `TweakMeta::one_of` or from
  `(one_of ...)` after its value in the data. `baker.shapes/1.1` and `baker.tweaks/1.1`
  write each list.
- **New: development-only layers.** A project file may mark a layer `dev_only`, such as one
  person's tuning or an agent's proposal awaiting review:
  `(agent "data/agent.dta" optional dev_only)`.
  - **Shipping builds:** under `BAKER_SHIPPING`, `open_project` never pushes such a layer,
    even when its file is present.
  - **Packaging:** a packaging step reads `ProjectLayer::dev_only` from `read_project` to
    leave those files out.
  - **Tools:** `ProjectOptions::skip_dev_only` leaves every marked layer out for a check or
    a simulation.
- **New in the host kit's commands:**
  - `--output FILE` writes a command's output to a file, and `CommandHooks::output` routes
    it to a sink of yours, for an engine that prints its own lines on stdout.
  - `CommandHooks::reader` reads the stack through your file access, as `Live` does.
- **Fixed: the watcher lost an edit when its list of files changed.** An edit saved between
  the last poll and a new `watch()` list (a new include, a file that appeared) counted as
  already seen, and never reloaded until the next save. A file that stays in the list now
  keeps what was last seen of it, and the edit is reported at the next poll.
- **Fixed: tweak callbacks that change tweaks.** A callback that added a callback to the
  same tweak changed the list being walked, and two tweaks whose callbacks set each other
  recursed until the stack ran out. A callback added from inside one now runs from the next
  change, and a tweak being notified is not notified again until its callbacks return.
- **Faster:**
  - `"key"_sym` interns once per distinct literal: a literal in a loop costs about what a
    held `Symbol` does. Write `using namespace baker::literals;` to use it.
  - Making a `Symbol` takes no lock when the name is already interned: about three times
    faster on one thread, and no longer serialized across threads.
  - The lexer finds the end of each atom with a table: about 3.6% fewer instructions per
    parse.
- **Docs:** a [Threads](docs/cookbook.md#threads) section in the cookbook covers what other
  threads may read and when, what a Node copy costs across threads, a reload as the frame's
  swap point, and the script stack budget on a small thread.

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
