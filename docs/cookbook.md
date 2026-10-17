# Baker cookbook

Patterns for using Baker well. The [language guide](language.md) says what each piece is;
this says how they fit a studio's day. Each pattern links the example that runs it.

- [Group parameters under the code that owns them](#group-parameters-under-the-code-that-owns-them)
- [Tweaks in practice](#tweaks-in-practice)
- [Reload callbacks: migrate or punt](#reload-callbacks-migrate-or-punt)
- [One project file for the game and its tools](#one-project-file-for-the-game-and-its-tools)
- [Personal layers: `local`](#personal-layers-local)
- [Experiments and A/B comparison](#experiments-and-ab-comparison)
- [Live ops](#live-ops)
- [The binary cache in a build step](#the-binary-cache-in-a-build-step)
- [Linking a shipped game](#linking-a-shipped-game)
- [Harness packs](#harness-packs)
- [A developer console over `eval`](#a-developer-console-over-eval)
- [Repro scripts](#repro-scripts)

## Group parameters under the code that owns them

Put a subsystem's parameters under one tag (`(renderer ...)`, `(audio ...)`) and have only
that subsystem read them. Reading another subsystem's parameters is bad form even though a
global database makes it possible. Baker enforces nothing, but hot reload depends on it:
`cfg.on_reload("renderer", rebuild)` fires when the `renderer` subtree changes, so a group
is the unit that gets refreshed and notified together.

## Tweaks in practice

A tweak replaces a constant where the constant was:

```cpp
static baker::Tweak<float> kJumpHeight{"player.jump.height", 1.2f,
                                       {.range = {0.5, 3}, .step = 0.1, .desc = "Peak, in m"}};
```

- **Declare tweaks at namespace scope.** A function-local static adds a guard check to
  every read. At namespace scope, reading a `Tweak<float>` is one relaxed atomic load,
  checked in the generated assembly by a test.
- **The default is the old constant.** A path the data does not set keeps it, and that
  is never an error: a tweak can go in before anyone writes its data.
- **Metadata belongs in the data once someone tunes it.** The code's `TweakMeta` is a
  starting point; `(height 1.4 (range 0.5 4) (note "4 breaks the level 3 ceiling"))` in
  the data wins, and `note` exists only there: a remark from whoever tuned the value for
  whoever tunes it next. A value of several numbers takes metadata the same way:
  `(tint 0.9 0.5 0.2 (range 0 1))` is three numbers, which a tool shows as a color.
- **Types.** Any `T` with a `Convert<T>` works. One whose `std::atomic` is lock-free reads
  with a load; another (`std::string`, `std::vector<float>`) reads through `get()`, whose
  reference stays valid until the next reload.
- **Paths made at runtime.** A scripting host or an editor that makes tunables from its own
  scripts passes the path as a `Symbol`: `Tweak<float>(Symbol(path), 1.0f, {}, registry)`.
  A symbol lives as long as the program, so the path outlives the tweak; a `const char*`
  must, which is why code writes literals. Each new path stays interned, so make such
  tweaks once, at startup or at a load. `TweakMeta`'s strings must outlive the tweak too;
  a symbol's `str()` does.
- **`on_change`** runs after the value changes, on the thread that changed the Config,
  for the few values whose change must do something (resize a buffer).
- **`dev_only`** marks a tweak a shipped build must not take from data: under
  `BAKER_SHIPPING` it keeps its default, and `set()` is refused. Use it for cheats and
  debug switches.
- **Tests** give a tweak its own `TweakRegistry` and Config, so they do not share the
  global one.

`examples/one_line_tweak` and the [README's five-minute path](../README.md) run them.

## Reload callbacks: migrate or punt

When a group's data changes, its subsystem either rebuilds from scratch (*punts*) or moves
its runtime state onto the new values (*migrates*). Punting is the default and needs no
code beyond the rebuild:

```cpp
cfg.on_reload("renderer", [&] { renderer.rebuild(); });   // punt
cfg.on_reload(
    "audio", [&] { audio.restart(); },                     // the punt, if migration declines
    [&] {                                                  // migrate first
        if (!audio.can_retune(cfg)) return baker::Reload::Punt;
        audio.retune(cfg);
        return baker::Reload::Migrated;
    });
```

- A callback fires only when its group's subtree changed, after every tweak has its new
  value, for a reload, a layer pushed, removed, or toggled, and a restore alike.
- Punt first. Migrate only where a rebuild loses state someone is looking at (a playing
  sound, a camera mid-move), and let migration decline when the change is too large.
- A reload that fails to parse changes nothing: no callback fires, and the old tree stays.
- `cfg.on_reload_complete(...)` sees each change's `ReloadRecord`: six timed stages (scan,
  read, parse, merge, bind, notify), the files reloaded, the tweaks changed, and how many
  callbacks migrated or punted. Give it `ReloadRequest{.origin_ns = saved_at}` from a file
  watcher, and the record measures edit to effect.
- Call `cfg.mark_visible()` once a frame, when it presents: it costs one branch when
  nothing is waiting. `cfg.loop_stats()` then gives, per file and per layer, the median and
  worst time from a saved edit to the frame that showed it, over the last 32 edits, with
  failed reloads counted. Polling for changes does not dilute it.
- **Watching files yourself.** `cfg.stack_files()` lists every file the layers were read
  from: each layer's file, the files its `#include`s reached, and those they looked for and
  missed, with the layer each belongs to and the path as the reader was given it. An engine
  or editor that already watches files calls `reload()` only when one of these changes,
  with the save's time as `origin_ns`. `cfg.stack_files_epoch()` moves whenever the list
  does, so checking it costs one comparison a frame.

`examples/config` runs a reload that rebuilds the audio group and leaves the renderer alone.

## One project file for the game and its tools

A game's layer stack, pack levels, include directories, and defines are usually set up in
its C++ `main`. A tool that opens the data (an editor checking as you type, a CI check, an
agent) then needs a second copy of that setup, which drifts. A project file holds it once,
and the game and every tool read it.

```dta
; baker-project.dta, at the root a tool opens
(project
   (pack_levels engine game task)     ; hardest first
   (include_dirs "data")              ; searched after an including file's directory
   (defines PC)                       ; flags, as #ifdef sees them
   (reference "build/reference.json") ; for tools: where the build writes the reference
   (layers                            ; bottom to top
      (default "data/default.dta")
      (local "data/local.dta" optional)
      (agent "data/agent.dta" optional)))
```

```cpp
auto project = baker::read_project("baker-project.dta");
if (!project) return report(project.error());
baker::Config cfg;
if (auto r = baker::open_project(cfg, *project); !r) return report(r.error());
```

- **Paths** are relative to the project file's directory unless absolute. A path that
  begins with a scheme, as an engine names its virtual roots (`"user://baker/local.dta"`),
  is absolute and kept exactly as written, through to the reader, the writer and
  `source()`. Serve it from your reader. A reader that cannot open it reads an optional
  layer as empty.
- **Layer flags:** `optional` makes a missing file an empty layer; `disabled` keeps a layer
  in the stack, skipped by the merge, for a panel to toggle.
- **Leaving layers out:** `open_project(cfg, project, {.skip = {"local"}})` leaves out one
  person's own tuning, so a simulation or a check gives the same results on every machine.
- **Mistakes are located.** A key the format does not know is an error with its file,
  line and column, so a typo is found rather than ignored.
- **Every entry keeps where it is written:** each layer's name and path (`name_at`,
  `path_at`), each pack level, include directory and define (`at`), and the reference, as
  a line and a column in characters. A tool can underline a layer whose file is missing.
- **Every problem at once:** `open_project(cfg, project, {.go_on = true})` pushes every
  layer that reads and returns a `ProjectProblem` (the layer's name and its error) for each
  one that does not. A layer that does not read is left out, and the layers above it read
  over the ones below. Without `go_on`, as a game opens it, the first error stops it.
- **The reference** names where the game's build writes `write_reference_json`, for tools
  that check data without the game. `read_project` returns the path in `Project::reference`
  and does not ask that it exist; a game ignores it.
- **Finding the project:** `find_project(path)` returns the nearest `baker-project.dta`
  in `path` or a directory above it, or nothing, so an editor opened on a folder holding
  several games finds each file's project. Give it a reader to look through the editor's
  files instead of the disk.
- **The project file reaches no other file:** an `#include` in it is refused.
- **An editor's unsaved buffer** reaches `read_project` through a reader, as it reaches the
  layers through `cfg.session().set_reader(...)`; set the reader before `open_project`.
- **A packaged game** may ship without the file and build the same `Project` in code,
  then call `open_project` with it, so both paths open one stack.

## Personal layers: `local`

Each developer gets a layer of their own, for tuning without touching shared files or
blocking anyone. It is a convention, not a mechanism: Baker never looks up user names.

```cpp
cfg.push_layer("default", "config/default.dta");
cfg.push_layer("platform", "config/ps5.dta");
cfg.push_layer("local", "config/local/" + user + ".dta", {.optional = true});
cfg.push_layer("patch-2026-10-11", overlay);   // live patches go above local
```

- Push it above the shared layers and below live patches.
- `{.optional = true}` makes a missing file an empty layer. The next `reload()` picks the
  file up once it exists, and `write_back_to(path, "local")` creates it.
- Keep the directory out of version control:

  ```gitignore
  config/local/
  ```

- To keep a value for yourself, tune it, then `write_back_to(path, "local")`. To share an
  experiment, `promote("local", "default")` writes its entries into the shared file as one
  reviewable diff, or nothing if any entry cannot be written.
- Before quitting, check what was tuned and not kept: `cfg.unsaved()` lists the paths, and
  `cfg.journal(fn)` every live edit of the session (up to the last 256) with its old and
  new values, where the old one came from, and whether it was written, reverted, or
  superseded. A harness can warn at exit, and the journal's counts say how much
  tuning was kept.

`examples/config` writes a value into a `local` layer that did not exist;
`examples/harness-imgui` pushes `local/<user>.dta` this way.

## Experiments and A/B comparison

Put a variant in its own layer and flip it:

```cpp
cfg.push_layer("boss-faster", "config/experiments/boss-faster.dta");
cfg.toggle_layer("boss-faster");               // A
cfg.toggle_layer("boss-faster");               // B
auto changes = cfg.layer_effect("boss-faster"); // what it actually changes
auto ab = cfg.diff("boss-slower", "boss-faster");
```

A disabled layer keeps its place and is skipped by merging, provenance, and write-back.
Live tuning goes to the `scratch` layer, so "my tuning versus the files" is also a toggle:
`cfg.toggle_layer("scratch")`.

`source()` names the layer a value comes from; `cfg.explain(path)` names every layer that
has a say, bottom to top. Each entry gives the layer, its value, its file and line, and how
it stands:

- `Wins`: it supplies the value `get` returns.
- `Shadowed`: a layer above overrides it.
- `Removes`: it holds `(key #remove)` for the key or an ancestor.
- `Disabled`: a toggled-off layer that would bring the value back.

A tweak at the path adds a first entry, `code`, holding its default.

```
default      shadowed  2    default.dta:2
boss-faster  disabled  3    boss-faster.dta:1
scratch      wins      2.5
```

`cfg.snapshot()` and `cfg.restore(s)` capture and return to an exact state (the layer
stack, each layer's trees, and scratch) without reading files. Two snapshots make A/B
comparison of two tuned states one call each.

## Live ops

A shipped game changes its data without a new build by taking **overlays**: small files of
only what changes, each pushed as a layer of its own above everything shipped. A layer is
the whole mechanism: merging applies it, provenance names it, `layer_effect` says what it
changes, and removing it takes it back. `examples/live-ops` runs each step below.

### Environment boundaries

Development, QA, and live are separate layer stacks, built from separate directories, and
the difference between them is what each is allowed to change after it starts.

```
config/default.dta           shared, checked in, shipped
config/platform/ps5.dta      shipped
config/local/<user>.dta      development only, never checked in
overlays/qa/...              pushed to QA builds
overlays/live/...            pushed to players, after QA
```

- A development build reloads files, takes live edits, and writes back.
- A shipped build loads its data, then calls `freeze()`. After that, reloads, layer
  changes, and live edits are refused, unless `ConfigOptions::live_layers` allows layer
  changes: `Config({.live_layers = true})` is the one door overlays come through. Under
  `BAKER_SHIPPING`, reload, toggles, `set()`, write-back, and `promote()` are inert
  whatever the options say, and `dev_only` tweaks keep their defaults.
- An overlay arrives as text from the studio's server; read it with
  `read_string(text, "patch-2026-10-17.dta")`, whose name labels its errors and its
  provenance, and push the tree: `cfg.push_layer("patch-2026-10-17", tree)`.
- Do not push a QA overlay to live by copying it into the live directory by hand; promote
  it through the same review a code change gets (below).

### Version history

- **Overlays are files in version control**, one per change, named by the date it ships
  and what it does: `overlays/live/2026-10-17-double-xp.dta`. A comment at the top says
  when it goes out and when it comes back. The history of live data is then `git log
  overlays/live`, and each change has its review.
- **The layer's name is the file's name**, so `source()` on any value in a player's game
  names the overlay that set it, and a bug report can carry it.
- **`snapshot()` beside a repro.** When QA captures a bug, capture the Config's state with
  it; restoring the snapshot gives back the exact values, overlays included, without
  reading files that may have changed since.

### Rollback

```cpp
cfg.remove_layer("2026-10-17-double-xp");
```

The values below the overlay return, the tweaks rebind, and reload callbacks fire, as for
any layer change. Rollback leaves no trace because an overlay never modified anything: the
shipped trees are immutable, and the merged tree is recomputed without it. That is why
overlays are layers and not edits to a shipped file. Keep overlays independent (each
setting only its own values) so that removing one never needs another removed first.

### Tests before an overlay ships

- **Shapes.** Declare, in the shipped data, the shapes of everything an overlay may set,
  with ranges: `(event (drop_rate float (range 0 1)))`. With
  `cfg.add_validator(baker::shape::validator(), {.any_thread = true})`, the shapes judge
  every push and every check of an overlay before it is pushed: `examples/live-ops` refuses
  an overlay that sets a 15% drop rate as `15`.
- **`check`.** An overlay that carries script (`(on_event {grant_xp 2})`) gets
  `baker::check` against a Context with the game's functions bound: a wrong call is an
  error before a player runs it. `cfg.add_validator(baker::check_validator(ctx))` checks
  every push in a development build.
- **In CI**, run both over every file under `overlays/`, stacked on the shipped data, on
  every change. An overlay that fails never reaches the server.

### Bounding what an overlay may change

A shipped game should take an overlay only after checking it, and only within bounds it
declares.

**The policy.** A `LayerPolicy` states the bounds:

| Rule | Refuses |
|---|---|
| `data_only` | commands, properties, variables, `#autorun` |
| `scalars_only` | a value that is not a number, symbol or string, and a removal |
| `existing_only` | a new key, and a removal |
| `allow` | anything outside the listed paths |
| `warnings_refuse` | anything a validator, or the read, warns about |

**The check.** `cfg.check_layer_text(name, text, policy)` checks text received from
outside, changing nothing:
- it reads the text as text only, never a cache, and refuses an `#include` or `#merge`
  in it, so the overlay reaches no file of the game's;
- it reads it against the stack's packs, so an overlay whose `requires` names a newer base
  version is warned about;
- it runs the validators over the stack with the overlay;
- it reports the overlay's effect, refusals and warnings.

**The push.** `push_layer_text(name, text, {.policy = &policy})` reads the text as the
check does, runs the same check, and refuses, changing nothing, if the policy refuses. The
layer is read as a file layer is, so its pack and `origin` are recorded, and the Config
keeps the text to read again when a layer below it changes. `push_layer` takes a policy too,
by path or with a tree.

```cpp
const baker::Path tunable[] = {baker::Path("economy"), baker::Path("events")};
const baker::LayerPolicy hotfix{.data_only = true, .scalars_only = true,
                                .existing_only = true, .warnings_refuse = true,
                                .allow = tunable};
auto report = cfg.check_layer_text("hotfix_0412", downloaded, hotfix);  // show it, log it
cfg.push_layer_text("hotfix_0412", downloaded, {.policy = &hotfix});  // refused: nothing changed
```

### Published overlays

An overlay that leaves the studio is a file with a name, a version, and the base it was made
for. `examples/live-ops` runs each step below.

**The overlay is a pack.** The shipped data declares itself one too, so an overlay can say
which base it needs:

```dta
; data/default.dta
(pack game_data (level game) (version "1.0"))

; overlays/2026-10-17-double-xp.dta
(pack double_xp_1017 (level live) (version 1) (requires (game_data "1.0")))
(origin "liveops console, ticket 4412" (tool baker-publish))
(event (xp_multiplier 2) (banner "Double XP all weekend!"))
```

- With the pack levels set (`cfg.session().set_pack_levels({"game", "live"})`), an overlay
  made for game data `"1.1"` is warned about against `"1.0"`. Under `warnings_refuse` it is
  refused.
- This guards rollbacks too. An overlay checked against the base it lands on cannot meet
  data whose shape has moved since it was made.
- `(origin ...)` carries into provenance. `source("event.xp_multiplier")` names the
  overlay's layer, the ticket, and the publishing tool, so an operator can see what is live
  and why.

**Its life in the game:**

1. **Fetch and verify.** The host's transport fetches the overlay's text and verifies it,
   with a content hash and a signature checked before Baker sees any bytes. Baker's
   `Cipher` hides a cache's contents; it is not a signature.
2. **Stage.** `cfg.check_layer_text(name, text, policy)` checks it at once, changing
   nothing; log or show the report, and keep it.
   - To keep the check off the frame, take `cfg.checker()` on the game's thread, which
     costs well under a microsecond, and call its `check_text` on a worker.
   - The checker shares the stack's trees and never touches the Config again.
   - It runs the validators added with `{.any_thread = true}`, such as
     `shape::validator()`. The push runs the rest on the game's thread.
3. **Apply at a boundary.** Push it at a moment the game chooses: a session's start, a
   lobby, never mid-match. Push it with `push_layer_text`, the same policy, and the
   staged report: `{.policy = &policy, .checked = &report}`.
   - If nothing in the Config changed since the stage, the push takes the report's verdict
     and costs what an unchecked push costs.
   - If the stack moved, the push checks again, and refuses, changing nothing, if the
     policy does. A stale report is never used.
   - The pack's `requires` and its `origin` are read with the text. A tree read separately
     and pushed directly carries neither.
   - Dedicated servers apply the same overlay at the same boundary, so a client and its
     server never disagree.
4. **Keep the last good one.** Store each applied overlay's text, and re-stage it at the
   next start. If the store is unreachable, the game starts on what it last ran. If the
   stored overlay no longer passes against a patched base, it starts on the base.
5. **Roll back** with `remove_layer`, as above.

**Retire it into source.** An overlay that sticks belongs in the shipped data. Load the
base checkout with the overlay as a layer and `promote` it into `default`: a
reviewed change, one line per value. Then stop publishing the overlay for builds that
contain it. Otherwise the next patch silently drops values players have been playing
with.

### Personal and experiment layers

Developers tune in their own [`local` layer](#personal-layers-local), and variants live in
[experiment layers](#experiments-and-ab-comparison). Both sit below the live overlays, so
a developer reproducing a live problem sees what players see, with their own tuning on
top only if they enable it.

## The binary cache in a build step

Text is for people; a shipped game can load the same trees from binary caches, one read
and one allocation each: a 6.2 MB tree in 2.3–2.5 ms.

- **A file at a time:** `baker_bake [-D NAME]... [-I DIR]... in.dta out.datb` in the build,
  and `baker::read_cached(text_path, cache_path, session)` in the game, which uses the
  cache when it is fresh and the text otherwise. Give `baker_bake` the same defines as the
  game, or the cache is stale for it.
- **A cache is a memory image**, for one byte order and pointer size: bake it with the
  target's layout. Today's consoles and PCs are all little-endian and 64-bit, so a bake
  on a development PC serves them. A cache from another layout is refused with an error
  naming it; `read_cached` then reads the text, if the game shipped it.
- **A Config's layers:** `ConfigOptions::cache_dir` keeps one cache per file layer, used
  outside edit mode. Each layer is read with the macros and defines of the layers below
  it, so bake the layers the way the game stacks them: a build step runs a development
  build's Config with the same layer paths and `cache_dir`, then the directory ships.

  ```cpp
  baker::Config bake({.edit_mode = false, .tweaks = nullptr, .cache_dir = "build/datb"});
  bake.session().define("PS5");
  for (auto& [name, path] : layers) bake.push_layer(name, path);   // writes the caches
  ```

  The shipped game builds the same Config with the same `cache_dir`. Under
  `BAKER_SHIPPING` it loads each layer from its cache without re-reading the files. It
  does check that the session begins as the bake did, in memory: the same macros, pack
  levels, and required files. A cache baked for another session is set aside for
  the text, with a warning, or refused if there is no text. Caches are named by the
  normalized layer path, so the bake and the game may spell paths differently.
- A cache holds the expanded tree and replays the defines, macros, and `#autorun` commands
  its load made. It is build output: it detects damage, not tampering, so text from a
  source you do not trust goes through the parser. A `Cipher` (`LoadSession::cipher`)
  enciphers caches, if a game wants players not to edit them.

## Content packages: patches, DLC, and mods

How a game delivers content after it ships:

- **Each package is a layer:** the base game, then each DLC, then title updates, then live
  ops. A package installed later still goes in its place: `push_layer("pack3", path,
  {.below = "tu1"})` puts a new DLC under the title updates, so their fixes to it win.
  A DLC that may be absent is pushed with `LayerOptions::optional`. Code that reads
  its content takes a fallback (`get(path, fallback)`), so a save that names an
  uninstalled DLC's content still loads.
- **Anything packages extend is a keyed table.** Two layers that set one value list leave
  the top one's list, because Overwrite replaces values. Keyed children merge, so every
  package's entries survive. This is how Rock Band's `songs.dta` scaled to thousands of
  downloadable songs:

  ```dta
  ; base: (songs (song_a (bpm 120)) (song_b (bpm 90)))
  ; dlc_pack1.dta:
  (songs (song_c (bpm 140)))
  ; merged: (songs (song_a ...) (song_b ...) (song_c ...))
  ```

  A value list such as `(setlist song_a song_b)` is right for something one package owns.
  `layer_effect` shows a layer that replaces another's list.
- **A title update removes with `(key #remove)`**, a debug menu or an exploit's entry, and
  the key stays gone until the layer is removed.
- **A title update changes values, not the macros below it.** A layer reads with the macros
  of the layers below it. A macro a title update redefines reaches only the layers above
  it, never the base that already expanded it.
- **Run autorun with `Context::run_autorun(cfg.autorun().items())`** after every stack
  change. Script an uninstalled DLC defined goes with it, and what it replaced comes back.
- **Declare what a package requires**, and at which version: `(pack expansion (level dlc)
  (version "1.0") (requires (base_game "1.2") season_pass))`. A requirement that is not loaded once the pack's file is read is a
  located warning, forwarded to the Config's warning handler.
- **A cache patches about as small as its text**. It is laid out in source order,
  with no absolute offsets, so an edit changes the cache's bytes near the edit and a binary
  diff or a chunked store sends little more than the edit: inserting a section into a
  10 MB cache is a 352-byte xdelta patch. Bake an update under the same path as the base
  bake, since every array names its file.
- **Mods and user files are text, never caches.** A cache is a memory image that a forged
  file can point anywhere. A loader of content from outside the game's own packages sets
  `LoadSession::text_only`, which refuses every cache and parses the text, which no input
  can make crash the reader. The game's own packages are signed by their platform.
  Content that arrives as text, such as a live overlay, also sets
  `LoadSession::refuse_includes`, so it cannot `#include` the game's files;
  `Config::push_layer_text` sets both.

## Linking a shipped game

Baker's libraries are static, and the tools share them with the runtime. A game
pays only for what it links:

- **Tools in their own files cost nothing.** `check`, its JSON, the reference, and
  `find_repeats` are in object files a game never references, so the linker leaves them
  out: 0 bytes.
- **`Config`'s tools come with `Config`.** Write-back, `promote`, `diff`, and
  `layer_effect` share an object file with the loader. With GCC or Clang, compile with
  `-ffunction-sections -fdata-sections` and link with `-Wl,--gc-sections`, and the linker
  drops the ones the game never calls. MSVC's release defaults (`/O2` implies `/Gy`, and
  the linker uses `/OPT:REF` unless given `/DEBUG`) already do.

  ```cmake
  target_compile_options(baker_data PRIVATE -ffunction-sections -fdata-sections)
  target_compile_options(baker_script PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(game PRIVATE -Wl,--gc-sections)
  ```

A minimal game (load, read, a tweak, a function, an object, `eval`) built with
`BAKER_SHIPPING` links 580 KB of Baker code, 41.5 KB of it tooling; with sections, 407
KB, 4.8 KB of it tooling (the source maps that loading reaches).

## Harness packs

A harness is the set of data, macros, and functions a team builds to make one kind of work
fast: a boss-tuning harness, an economy harness. Packs give harnesses a structure:

```dta
; harness/combat_core.dta
(pack combat_core (level game))
[combat.hit_pause 0.08]
{func combat.knockback ($force) {* $force 1.5}}
```

```dta
; harness/boss_tools.dta
(pack boss_tools (level task) (requires combat_core))
#require "combat_core.dta"
[boss.restart_phase {boss_phase_restart}]
```

- **`#require`** a pack from every file that uses it: it is read once per session however
  many files require it, and its macros serve them all.
- **Levels** are the session's: `cfg.session().set_pack_levels({"engine", "game", "task"})`,
  before pushing layers. A game-level pack that expands a task pack's macro, or calls its
  function, is warned about, located: engine and game harnesses must not depend on one
  task's tools. Run `check` with the session to see calls:
  `baker::check(*cfg.root(), ctx, report, {.packs = &cfg.read_session()})`.
- **Prefix** a pack's names with its subject (`boss.`, `combat.`); the reference and
  tuning panels group by prefix.

## A developer console over `eval`

`ctx.eval(text)` parses and runs a command and returns its value, or a located error with
the script stack. A console is that, fed from a text box or a socket:

- Poll the socket once a frame, on the thread that owns the Context, so commands run
  between frames like any other script. `examples/remote-console` does it in about a
  hundred lines on BSD sockets and Winsock, listening on 127.0.0.1 only: a console runs any
  command it is sent, so it is a development tool, not a shipped port.
- Name the input (`ctx.eval(line, "<console>")`) so errors say where they came from, and
  print `describe(error)` for the stack.
- Bind what designers ask for as they ask for it; `check`'s demand report over the data
  lists what they have already written calls to.
- `examples/harness-imgui` has the same console as a panel.

### Consoles for tools and agents

A tool can drive the same console, but it should not have to parse text written for
people. Two calls give it what it needs:

- **`baker::eval_request(ctx, line, sink)`** answers a request, `(ID {command})`, with one
  line of JSON. The JSON holds the ID, the result's text, a JSON projection of the result,
  or the error with its script stack (`docs/reply-json.md`). `examples/remote-console`
  answers lines that begin with `(` this way and every other line as before.
- **`baker::bind_config_tools(ctx, cfg)`** registers the Config's tooling as script
  functions, so a console, an editor or an agent can ask the running game directly:
  - `{config.explain player.speed}`: every layer that sets the value, and which one wins;
  - `{config.set player.speed 6}`, `{config.toggle experiment}` and
    `{config.snapshot a}`: live edits and A/B;
  - `{config.journal}` and `{config.unsaved}`: what was tuned, and what was not kept.

  They are documented, so `reference` lists them and `check` checks calls to them. A
  shipping build binds only the reading tools. `config.write_back` and `config.promote`
  write files, so they are bound only with `{.allow_write = true}`.

```
> (1 {config.explain player.speed})
{"schema":"baker.reply/1.1","id":1,"ok":true,"text":"...","value":[
  {"layer":"default","value":4.5,"standing":"shadowed","file":"default.dta","line":3},
  {"layer":"local","value":6,"standing":"wins","file":"local/me.dta","line":1}]}
```

## Repro scripts

A command is already a serialized, parseable, evaluable value, so a sequence of console
commands is a script that reproduces what was done:

- Log each command a console or tool runs, one per line, to a `.dta` file. Running it with
  `ctx.run((*baker::read_file("repro.dta"))->nodes())` replays it.
- Begin it with the state: the Config snapshot QA captured, or the overlays that were live
  (`cfg.layers()`). A repro of data that has since changed reproduces nothing.
- Drive time through the scheduler the script uses (`{wait_for 2}` in a `thread_task`), and
  tick it from the test with fixed steps, so a replay does not depend on frame timing.
- A repro that runs in CI against the fixed build is a regression test, written in the
  language the bug was found in.
