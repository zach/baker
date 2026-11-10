# Changelog

Baker's releases, newest first. Versions follow [semantic versioning](https://semver.org);
before 1.0.0, a minor version may change the API.

## 0.9.0 - 2026-11-10

This release adds a change log with undo and redo, and lets a reload read and parse off the
game thread. Most of the rest is cost: a review of the whole library removed quadratic
paths, per-call allocations and work repeated on every change. A stack image must be baked
again, generated view headers regenerated, and a shipping `host::Live` no longer binds the
`config.*` commands (see **Breaking**).

- **Breaking: the stack image is version 3.** Its checksum and its table section changed; an
  older image is refused, naming both versions. Bake images again. A table section no longer
  carries a relocation list or the process's pointers, so two builds of the same data write
  the same bytes. On a test stack the image went from 9.6 to 7.8 MB and opens cold in less
  than half of 0.8.0's time.
- **Breaking: generated views order members by alignment,** largest first, and every view
  digest changed. Regenerate the headers (`baker_generate_views`, or `baker_bake --views`);
  `check` reports an old header as drift, and an image refuses its sections. A view
  initialized by position breaks; initialize it by member name.
- **Breaking: a baked table keeps record IDs only for `(@view by_id)`.** Mark a keyed
  shape `(@view by_id)` if the game calls `Table::index_of(RecordId)` or `id_of(i)`; without
  it they find nothing. The tree keeps every ID for `find_by_id`, `id_validator` and the
  tools.
- **Breaking: `LayerEffectEntry::path` is a `Ref<String>`,** the dotted path as text
  (`path->view()`), so checking candidate layers no longer adds a permanent symbol per path:
  1,000 refused candidates once added 999 symbols, now none.
- **Breaking: a shipping `host::Live` binds no `config.*` commands,** and links none of the
  layer tools: about 160 KB less code. A shipping `#autorun` that calls `config.get` fails
  unless the game binds `tools::bind_config_tools` itself. Its four validators run only when
  an overlay is checked, not after every merge.
- **Breaking: the console answers within a budget.** `host::ConsoleOptions` (also
  `LiveOptions::console`): `budget_ns`, 0.5 ms by default, after which `poll()` leaves
  the rest for the next frame, clients taking turns; and `max_bytes`, 1 MB, past which a
  client's unanswered input drops it. 0 turns either off. `Console(ctx, options)`.
- **Breaking: macros expand to at most `ParseLimits::max_macro_nodes`,** 2^20 nodes a use by
  default, shared bodies included; past it the read fails with `ErrorCode::Limit`, naming the
  macro. A 347-byte macro bomb took 1.6 s and 1.27 GB to be refused, and one written with
  shared lists was not refused at all; both are now refused in milliseconds.
- **Breaking: `shape::Field`'s members are reordered** (200 to 184 bytes). Only code that
  initializes a `Field` by position is affected.
- **New: the change log.** `set_change_history(n)` keeps the last `n` transactions, each
  a list of change records (set, add, remove, rename, reorder, append, drop) with whole
  values, the layer written and its source; `begin_transaction` and `end_transaction`
  group them. `undo()` and `redo()` move through them in memory; files a promotion wrote stay
  written. Off by default, it costs a test per edit. The console has
  `config.set_change_history`, `config.undo` and `config.redo`, each command being one
  transaction; `tools::write_changes_json` and the kit's `promote --baker-json` write
  `baker.changes/1`.
- **New: a layer names a record by its ID.** A layer's child tagged with a record's
  `(@id ...)` merges into that record, as an alias does; the record keeps its key.
- **New: reload off the game thread.** `Config::begin_reload()` takes what to check,
  `ReloadJob::run()` reads and parses on any thread, and `finish_reload(job)` merges on the
  game thread when the host chooses, refusing a job the stack changed under. In the host
  kit, `LiveOptions::run_async` hands the job to the game's job system: a 3 MB reload costs
  the game thread 1.7 ms in place of 54 ms. `reload()` still does it all at once.
- **New: reloads check stamps.** `LoadSession::set_stamper` lets a reload open only the
  files whose stamp moved; `host::file_stamp` is one, and `Live` installs it. A quiet scan
  of 50 files: 155 to 55 µs.
- **New: `ValidatorOptions::checks_only`,** a validator that judges candidate layers but
  does not run after a merge; `ValidatorStack::changed`, the tree a check or push brought;
  `shape::Scope` for `shape::check`.
- **New: `FuncRef::usage`,** a special form's whole call, a line a form, in the Markdown and
  JSON references (`baker.reference/1.6`). The reference documents every `config.*`
  command.
- **New: `SchedulerStats::pool_bytes`.**
- **Changed: the shape check of a push checks what the layer changes,** not the whole tree,
  unless the layer declares a `#shape` or removes a key a table names; a candidate's report
  holds only its own findings, so an old warning in the base no longer refuses every
  overlay under `warnings_refuse`. With 100,000 records and one changed, the validator went
  from 13 ms to under 2 µs. The ID, rules and check validators still judge the whole tree.
- **Changed: a field a shape does not declare is one warning** for its shape, at its first
  record, saying how many more records have it.
- **Changed: task frames come in size classes,** from 128 bytes up to
  `SchedulerOptions::frame_bytes`: 10,000 live native tasks take 2.5 MB, not 42.5 MB.
- **Faster.**
  - `format` finds alignment in n log n: 2.8 MB in 148 ms, not 44 s.
  - A read above a pending live edit shares one merge until the next edit: 106 ns, not 40 µs.
  - A live edit with many pending edits, and a change with many tweaks, are linear: 2,000
    tweaks rebind in 51 µs, not 731.
  - An edit in one layer no longer rebuilds baked tables that an upper layer overrides
    elsewhere: 0.008 ms, not 1.45.
  - Sorted lookup beats linear from about 100 children cold and 8 warm.
  - `ctx.call` and script messages of 6 to 8 arguments allocate nothing.
  - A curve read from a load bakes at its first sample: 46 ns a read and sample, not 93 to 134.
  - `key_of` fields, wide shapes and data rules in shapes no longer cost the square of the
    data: 100,000 records with a `key_of` field check in 8 ms, not 3 s.
  - The symbol table's probe is seeded per process, so text a game does not control cannot
    pile its names up on purpose.
  - `host::Watcher` and `file_stamp` call the system directly and allocate nothing;
    `watch()` is linear. On Windows a stamp no longer follows symbolic links.
- **Fixed.**
  - A push whose check report was held reported its shape findings a second time after the
    merge.
  - A table whose records' parent was renamed kept the old parent tag.
  - A promotion of a change set spelled by alias or ID wrote a new record keyed by it.
  - A stack image's layers lost their aliases.
  - Padding bytes from the stack reached a baked table's curves.
  - A push checked by the shape validator was about 16% slower in wall time than 0.6.0 on
    shapes of several ranged fields; it is now within 2 to 4%.

## 0.8.0 - 2026-11-06

This release generates C++ from your shapes: structs and readers for your tables (views),
tables baked from the merged data and read by index (baked tables), and record IDs that
survive a rename. It also changes how the layer tools report a row's values, and replaces
0.7.0's two migration tools with one (see **Breaking**).

- **Breaking: `baker_migrate` replaces `baker_migrate_metadata` and
  `baker_migrate_declarations`.** One tool runs both of 0.7.0's migrations in one pass, so
  their order no longer matters: `baker_migrate --write $(git ls-files '*.dta')`. `--only
  metadata` or `--only declarations` runs one, `--check` exits 1 if anything would change
  (for CI or a pre-commit hook), and `--report`, `--cpp` and `--md` work as before. If you
  ran the two 0.7.0 tools declarations first, a shape's metadata was left unmigrated: run
  `baker_migrate` again.
- **Breaking: a layer row's values come one node each.** `LayerEffectEntry::values` and
  `below`, `ExplainEntry::values` and `DiffEntry::a` and `b` are a `RowValues` (`nodes()`,
  `size()`, `[i]`, iteration) in place of one packed node, so `(aim (0 0))`, one value, and
  `(aim 0 0)`, two, are told apart, and an appended record is one operand.
  `to_string(RowValues)` in `<baker/std_interop.hpp>` prints them as a row writes them. The
  console's `config.effect`, `config.explain` and `config.diff` write one child a value,
  `(value (0 0))` against `(value 0 0)`; `config.get` is unchanged.
- **New: views.** A shape marked `(@view)` generates a C++ struct, its reader and a digest
  of its layout, so a table's layout is written once, in the data.
  `baker_generate_views(target PROJECT baker-project.dta)` in CMake runs
  `baker_bake --views` whenever a contract file changes; `read_view<T>(cfg, path)` reads a
  record with located errors; and `check` reports a struct compiled against another
  layout (`--baker-strict-views` fails on it). `(@view no)` keeps a field in the tree,
  `(@cxx T)` picks a member's C++ type and `(@cxx_name N)` its name; the project file's
  `(views (layers ...) (shapes ...) (namespace ...))` names the contract.
- **New: baked tables.** `Table<T>` holds a viewed, keyed shape's records as plain structs,
  rebuilt from the merged data at each change and read by index, key, path, record ID or
  parent: a read by index is a load. `add_table<T>` registers one, or the generated
  `register_tables` registers every table. A stack image (`bake --baker-stack` in the host
  kit, `write_stack_image`, `ConfigOptions::stack_image`) ships the contract layers and the
  tables in one file. Fields hold literals; a field that is not a literal reads as its
  default, with an error.
- **New: record identity.** `(@id ...)` gives a record an ID of 32 hex digits, minted by
  `format --baker-mint-ids TABLE`; `Array::record_id()`, `find_by_id`, and `id_validator`
  (two records, one ID). A renamed record keeps answering to its old key with
  `(@aka old)`, in overlays and `key_of`, and `check` lists an alias nothing names. `rename
  OLD NEW [--baker-write]` renames a key, its alias, its ID and every `key_of` naming it. A
  shape field may say `(@renamed_from old)`, which `check` checks, views read and `format`
  rewrites.
- **New: assets.** An `(asset KIND)` shape field names a file; the project's `(assets
  (texture ".png") ...)` says which kinds exist, `check` fails on a missing file, and the
  host kit's `assets` command writes `baker.assets/1`.
- **New: migrations as a call.** `tools::migrate_text(text, sink)` in
  `<baker/tools/migrate.hpp>` returns each old form in Baker text with its location, a
  warning worded as `check` words it, and the edits that rewrite it, which never overlap:
  for an editor's quick fix, a CI step, or an agent writing from older examples.
  `apply_findings` applies one finding or several. 2 MB takes about 25 ms.
- **New: tweaks the data may leave out.** `TweakMeta::optional`: `check_tweaks` no longer
  warns that the data lacks the tweak, and still holds a value the data supplies to its
  type and range. For a host that binds its own content's values as tweaks so layers can
  retune them by name. `baker.tweaks/1.6` carries it.
- **New: a check of a layer that folds a list.** A layer over a list written as records
  that repeat a tag, `(opening (build infantry) (build infantry) (build tank))`, merges
  into the first record only, so the stack reads the old list. A layer check and `check`
  now warn at the layer's row, naming the path and tag, and say to write the list as plain
  values or extend it with `#append` and `#drop`. `LayerEffectEntry::folded` and
  `DiffEntry::folded_a`/`folded_b` count the rows that folded. Merging is unchanged; the
  check is compiled out of shipping builds.
- **New: the list a verb changes.** Under `#append` or `#drop`, a layer row's `below` is
  the list the verb changes, records included, with the source of the row that wrote it,
  and a `(@was ...)` pin and the verbs' warnings are judged against it. It was empty for a
  list of records.
- **Changed:** `baker_bake` is built whenever the library is; `BAKER_BUILD_TOOLS` covers
  the rest. Schemas: `baker.shapes/1.8` (`view`, `cxx`, `cxx_name`, `renamed_from`,
  `asset`), `baker.format/1.1` (`rewrites`), `baker.tweaks/1.6` (`optional`), and the new
  `baker.assets/1`. `CommandHooks::tables` and `CommandHooks::config` in the host kit.
- **Faster.** A push checked by the shape validator costs what it did in 0.6.0 again (0.7.0
  added about 16%); a layer check in a shipping build takes about 4% fewer instructions.
- **Fixed.** On Windows, `baker_migrate` writes what it migrated byte for byte (a text-mode
  stdout doubled a CRLF file's line ends).

## 0.7.0 - 2026-10-31

This release changes how data spells metadata and declarations. Migrate your data with the
two tools it ships before you take it (see **Breaking**).

- **Breaking: metadata is spelled with `@`.** A child array whose tag starts with `@` is
  metadata, and nothing else is: `(speed 4.5 (@range 0 10) (@desc "Run speed"))`. The old
  words (`(range 0 10)`, `(desc ...)`) are fields now. Metadata comes after the value; a value
  after it is a located error. Run `baker_migrate_metadata --write` over your data
  (`--cpp` for text in C++ string literals, `--md` for Markdown, `--report` to list what it
  leaves for a person). In return, `get<std::array<float, 3>>` of
  `(tint 0.9 0.5 0.2 (@range 0 1))` reads three numbers, and a record may have a field named
  `kind`, `desc` or `step`.
- **Breaking: `#pack`, `#origin` and `#shape` declare a file's pack, origin and shapes;**
  `(pack ...)`, `(origin ...)` and `(shapes ...)` are data. Run
  `baker_migrate_declarations --write` over your data. A live-ops publisher switches its
  overlays to `#pack`/`#origin` before the game takes this release: an overlay still in the
  old form reads as data, and a game whose policy has `warnings_refuse` refuses it.
- **Breaking: the `Allocator` takes a `MemoryKind`.** `alloc(user, size, align, kind)` and
  `free(user, p, size, kind)`: each allocation says what it is for (symbols, trees, config,
  script, tasks, tweaks, errors, tools, other), and a free names the kind its allocation
  did, so a game can hold Baker to a budget per kind. `default_allocator(kind)` gives the
  default's copy for a kind; `memory_kind_name` names one for a report.
- **Breaking: a game must be built with the library's `BAKER_SHIPPING` and
  `BAKER_ATOMIC_REFCOUNT`.** CMake's targets pass them on. A game built another way and
  mismatched now fails to link (MSVC names both values) rather than crashing.
- **Breaking, smaller:** `TweakBase::code_meta()` is `dev_only()`; `WriteOptions` defaults to
  three spaces a level and a hundred columns (it was 2 and 80), and a Config opened from a
  project writes in the project's `(format ...)` (`Config::set_layout`).
- **New: rows.** `(/fighters.kestrel.health 1100)` at a file's top level is
  `(fighters (kestrel (health 1100)))`: a layer written as one key per line. A key written
  twice in a file of rows is an error.
- **New: layer verbs.** `(items #append rope)` adds to the list below, `(items #drop glider)`
  removes the first equal value, and `(reaction 26 (@was 30))` pins what a row was written
  over: a check warns when the stack below has moved. For lists that mods and packs extend.
- **New: braces written infix.** `{0.3 <= $value <= 0.7}`, one operator a brace; `+ - *` take
  two numbers or more, comparisons chain, and `+= -= *= /=` store. Keep data that older
  builds read, such as live-ops overlays, in the prefix spelling.
- **New: shapes count a list.** `(pos (list float 3))`, `(spares (list T 0 1))`;
  `shape::unused` names a shape that governs nothing; a received layer under `data_only` or
  `scalars_only` may not declare shapes. `#` opens a directive only before a letter, so `#1`
  and `#00aa00` are symbols.
- **New: a layer's `(when COND)`** (a define, `not`, `and`, `or`) on a project's layer: it is
  in the merge only while the defines say so, and `set_defines` applies it again;
  `LayerOptions::when`, `Config::layer_condition`, `ExplainEntry::when`.
- **New: stable IDs for names.** `symbol_hash(text)` and `"name"_hash` give a name's 64-bit
  FNV-1a hash at compile time, the same in every build, for switches and saves; `"name"_sym`
  interns with it. A build that is not shipping reports two names that share a hash
  (`set_symbol_collision_handler`).
- **New: limits for content you do not control.**
  - `Context::set_step_budget(n)`: every call spends a step, the same on every run; when it
    runs out the call fails with a Limit error, and a Scheduler's tick stops there, the
    tasks after it running at the next tick. `set_step_hook` may grant more (a time check,
    a profiler). `-DBAKER_STEP_BUDGET=OFF` compiles the check out.
  - `ParseLimits::max_new_symbols` caps the names one read adds to the program
    (1,048,576 by default); lower it for overlays and mods.
- **New: `set_fatal_handler`.** Every fatal error (an allocator that returned null, a size
  past 4 GB, reading an `Expected` that holds an error) goes to your handler with a message
  and the script stack, for your crash reporter.
- **New: `<baker/fwd.hpp>`** declares Baker's types and includes only `<cstdint>`, for a
  header that only names them: a few milliseconds a file, where any other header costs about
  half a second.
- **New: a shipped cache without metadata.** `baker_bake --drop-metadata`,
  `ConfigOptions::cache_drop_metadata`, `LoadSession::drop_metadata` and
  `without_metadata(root)`: the tree's `(@...)` and the shapes' notes are left out.
- **New: `BAKER_RTTI`** (on by default): `OFF` builds Baker without RTTI, for a game built
  without it or one that never derives from `ObjectBase` with RTTI on.
- **Shipping builds carry less.** A `Tweak` keeps no tool text (`desc`, `kind`, `group`,
  `step`) and is 128 bytes, not 184; doc text (`.doc()`, a property's, a global's, the
  built-ins') is dropped where it is written. Metadata in the data still applies.
- **Faster.** Binding a function costs about half the code and compile time (and a lambda
  that captures nothing is not stored); calls of nine arguments or more allocate nothing once
  warm; `#append` and `#drop` are linear (16,000 operands merge in milliseconds); a file of
  rows holds about what the nested file holds; a layer check takes fewer instructions.
- **Fixed.**
  - Curves sample to the same bits on every compiler: Baker's targets build with
    `-ffp-contract=off` (`/fp:precise` under MSVC).
  - Numbers written by the simulation tools no longer follow the C locale (`1,5`).
  - Metadata past the 8,190th child stays metadata, also after a merge grows the array.
  - A host Reader that switches fibers no longer disturbs the read's name count.
  - Static initialization: nine dynamic initializers are one.
- **Docs.** Read a tweak once into a local before a hot loop (the read is atomic, and blocks
  vectorization); do not switch fibers inside a Baker call; budgets per memory kind; the
  shipped link sizes, re-measured.

## 0.6.0 - 2026-10-23

- **New: content that is a directory, not a list of files.**
  - **A directory of layers** for mods, DLC and plugin packs: `Config::push_layer_dir("mods",
    "mods")`, or `(mods (dir "mods"))` in the project's layers. Each `*.dta` file in it is a
    layer, and so is each folder that holds a `layer.dta`, in the order `mods/order.dta`
    lists, `(order base_fixes hard_mode)`, then by name. Every `reload()` lists the directory
    again, so a mod dropped in, deleted or reordered takes effect, and `Live` watches the
    directory. A mod turned off with `set_layer_enabled` stays off across a rescan, and
    `set_layer_enabled("mods", false)` turns them all off. A mod that does not parse is an
    empty layer, its error located and passed to the warning handler, until it is fixed.
  - **`#include_dir "cards"`** makes each `.dta` file of a directory a record named by its
    file, `(fireball (cost 3) (damage 6))`, and each subfolder a nested table, in the same
    `order.dta` order. A layer retunes `cards.fireball.cost` by name, `source()` and
    write-back point into the card's own file, and a new card is a new file. A cache is
    stale once a file is added, removed or renamed.
  - **`LoadSession::set_lister`** lists directories through the host, as `set_reader` reads
    files, so both work over an archive or a virtual file system. The host kit passes
    `CommandHooks::list` to it, and `LiveOptions::lister` takes one.
- **New: curves.** A curve is a key's values: `(falloff (0 1) (5 3) (10 8))`, with an
  optional interpolation (`monotone`, the default, which never leaves the keys around it;
  `linear`; `catmull-rom`; `constant`) and extension (`clamp`, the default; `extrapolate`;
  `loop`), and a point may give its in and out slopes, `(5 3 0.2 0.4)`. `get<Curve>` and
  `Tweak<Curve>` read one, `sample(x)` gives its value, and `{curve.sample path x}` does in
  script. A shape field may have the type `curve`, its range holding every point's y, and
  a tweak's range does the same. The tweaks JSON names the type `"curve"`.
- **New: defines the host supplies, changed live.** `Config::set_defines` replaces the
  host's own flags for `#ifdef` on an open stack, such as a server switching mode: only the
  layers whose reads tested a changed name are read again, then one merge follows. It works
  in shipping builds. `ProjectOptions::defines` are the host's at open,
  `LoadSession::active_defines()` says who made each flag, and `config.explain` names the
  `#ifdef` branch a value was read in.
- **New: a formatter that keeps comments.** `tools::format_text` lays out text as Baker writes
  it, keeping comments and blank lines, and the kit's `format` command formats files
  (`--baker-check` to only check, `--baker-json` for `baker.format/1`). The project file
  sets the layout with `(format (indent 3) (width 100))`.
- **New: rules between values in shapes.** `(rule "starts at or below its maximum" {<=
  $start_hp $max_hp})` in a shape holds every record to it; `(warn)` makes a failure a
  warning. `{data tiers $tier ideal}` reads another table, and `{data.sum loot weight}` sums
  one. `tools::rules_validator` checks them at every merge, and a failure lists the values
  it read.
- **New: a shape field may name a key of a table,** `(weapon (key_of weapons))`, checked
  against the merged tree, with a suggestion for a typo.
- **New: tweaks a worker thread reads together.** `TweakRegistry::read_consistent(fn)` runs
  `fn` again if the tweaks changed while it ran, so several atomic tweaks are never seen
  half old and half new.
- **New in scenarios:** `(until {command})` may name how a run ended with a symbol, which
  `$ended` holds for the objectives and `sim` tallies per scenario. `(every SECONDS {...})`
  and `(every step N {...})` repeat a command. `(steps N)` gives a length in the game's own
  steps, and `(at step N {...})` runs at one.
- **New for peers and saves:**
  - `Config::digest()` and `stack_digest()` hash the data by value, the same on every
    platform, so lockstep peers, replays and saves can tell they agree; `{config.digest}`.
  - `Config::layer_text` writes a layer as text for a peer, and `set_layer_text` replaces a
    received layer's text in place.
  - `Config::apply_edits()` merges pending live edits into `root()`, and a read of a record
    or table above a live edit sees it.
- **New for tools:** a list tweak or parameter describes its elements (`element` and
  `length` in the JSON), so a tool checks a `std::vector<vec2>` without the game.
  `ValueCount` says whether a converter takes exactly its values, at least them, or any
  number. A project's tool path may begin `$build`.
- **Fixed:** a number is written as the same text on every platform, where a Mac wrote
  `1.2e+02` for 120. A tool that described a tweak whose default did not read lost every
  tweak it had described before it. Tweak findings come in one order.
- **What breaks:**
  - **What only tools call is the library `baker::tools`:** `tools::explain(cfg, path)`,
    `tools::layer_effect`, `tools::diff`, `tools::declare_type(ctx, ...)`,
    `tools::write_reference_json(...)`, `tools::DescribedTweaks`, `tools::read_structure` and
    `tools::bind_config_tools`, from `<baker/tools/...>`. Link `baker::tools`; the host kit
    brings it.
  - **The kit's flags begin `--baker-`:** `--baker-project`, `--baker-seed`, and so on,
    leaving the short names to the game.
  - The console's `{config.clear}` is `{config.clear_scratch}`.
  - `sim::Run::ended` (a symbol) replaces `until`.
  - A game's own converter that declares `values` and reads more than them says
    `ValueCount::AtLeast` or `Any`, or a tool reads its extra values wrong.
  - A list tweak's `default` and `value` are always written in parentheses, `((0.0 0.0))`
    for one `vec2`.
  - The JSON documents are `baker.reference/1.5`, `baker.tweaks/1.5`, `baker.shapes/1.4`,
    `baker.check/1.4` and `baker.sim/1.3`; each adds fields, and a 1.x reader keeps working
    unless it rejects a new shape field `type` (`"key"`, `"curve"`).

## 0.5.0 - 2026-10-21

- **New for tools that check without the game:**
  - **`read_structure(text)`** reads text with the parser's own lexer even when it does not
    parse. It gives every token, bracket and comment with byte spans, and lists the problems
    beside them, for an editor while a person types, a formatter or a highlighter.
  - **`split_metadata` and `presented_kind`** are the rules a tweak follows, callable on any
    value. The first separates a value from the metadata after it; the second gives the kind
    a value is shown as, "color" for 3 or 4 numbers ranged 0 to 1.
  - **`Context::has_type(name)`**, and `declare_type` and `declare_object` refuse a name
    already taken with `ErrorCode::Refused`.
  - **Checking again after an edit:** keep one Config, read through a reader that serves the
    open editors' text, and call `reload()`. It reads again only from the first layer whose
    text changed. `docs/tools.md` describes it.
  - **Fixed-size lists** (`std::array<float, 3>`) declare their value types, so a tool can
    describe such a tweak or parameter.
- **New for hosts with a scripting language or engine of their own:**
  - **`Context::declare(name, overloads, body)`** registers a function built at runtime with
    a described signature. `check` counts its arguments and checks their types, and a call
    holds each argument to its type before the body runs.
  - **`CommandHooks::list`** lists a directory of the host's own files, such as an archive's,
    so `sim` finds scenario directories wherever the stack's files live.
  - **`LiveOptions::console_any_port`** serves the console on a free port, which
    `console_port()` reports.
  - **`{config.epoch}`** says whether anything changed since a tool last asked.
  - **The project file** may name the console's port, `(console 4555)`, and the tool's
    arguments, `(tool "engine" "--path" "." "--")`.
- **New: allowed values for string and integer tweaks.** A string tweak takes `one_of`; an
  integer tweak takes named choices, `TweakMeta::choices` or `(one_of (0 easy) (1 normal))`
  in the data. `check` holds the data to both.
- **What breaks:**
  - `baker.tweaks/1.3` adds `choices`, and `one_of` also lists a string tweak's strings.
  - Each `check_tweaks` finding carries its tweak's path, or a stray key's own path, as its
    error path, so `to_string` of one now ends with that path.
  - `declare_type` and `declare_object` report a name already taken as `ErrorCode::Refused`,
    where it was `ErrorCode::Type`.
  - Code that reads `detail::ParamInfo` directly reads its type name with `type()`.

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
