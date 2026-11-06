# The Baker language

This guide is the language as the person writing `.dta` files meets it: data, the
preprocessor, packs and levels, shapes, script, and errors. The C++ side appears where
it explains the data. Every built-in function is listed, with its signature, in the
generated [script reference](script-reference.md); the patterns that put the pieces
together are in the [cookbook](cookbook.md).

- [Data](#data)
- [The preprocessor](#the-preprocessor)
- [Packs and levels](#packs-and-levels)
- [Shapes](#shapes)
- [Script](#script)
- [Errors](#errors)

## Data

A file is a sequence of nodes. Most of them are arrays:

```dta
; A comment runs to the end of the line. /* This kind */ may span lines.
(menu
   ('bacon and eggs'
      (price 12.75)
      (calories 120))
   ('fruit and cereal'
      (price 11.75)
      (calories 12)))
```

### Nodes

| Kind | Written | Notes |
|---|---|---|
| Int | `42`, `-7`, `0x1F` | 64-bit. A hex literal is its unsigned value. |
| Float | `12.75`, `1e-3` | A double; it must be finite. |
| Symbol | `price`, `'bacon and eggs'` | Interned: comparing two is comparing pointers. Quote one that has spaces or punctuation. |
| String | `"Over here!"` | Two escapes: `\n` for a newline, `\q` for a double quote. |
| Array | `(tag values...)` | Data. |
| Command | `{function args...}` | Script: runs when its value is read ([Script](#script)). |
| Property | `[name]`, `[pos x]` | A property of `$this`, in a method. |
| Variable | `$name` | A script variable's value. |

There is no boolean literal. `bool` reads an integer: `0` is false and any other true.
DTA's traditional `TRUE` and `FALSE` are macros for `1` and `0`, so define them if
your data uses them: `[TRUE 1] [FALSE 0]`.

### Tags and lookup

An array whose first node is a symbol is *tagged* by it, and the nodes after the tag are
its *values*. Lookup walks tags: in the file above, `menu` has two children, tagged
`'bacon and eggs'` and `'fruit and cereal'`, and each of those has `price` and
`calories`. A tagged array with one value is how a setting is written: `(price 12.75)`.
With several, `(clear_color 0.1 0.1 0.2)`, a reader takes them all (a `float[3]`, a
`std::vector`) or one by index.

A **path** names a value from the root, by tags and indexes, separated by `.` or `/`:

```
menu.'bacon and eggs'.price      12.75
renderer.clear_color.2           0.2: indexes count from 0 after the tag
menu/'bacon and eggs'/price      the form errors print, so a path can be pasted back
```

From C++, typed reads return `Expected<T>`: the value, or an error naming the key, the
types, the file, the line, and the path. A fallback is used only when the key is absent;
a value of the wrong type is still an error, because silently ignoring a mistyped value is
the failure located errors exist to prevent.

```cpp
auto root = baker::read_file("menu.dta");                     // Expected<Ref<Array>>
double price = *(*root)->get<double>("menu.'bacon and eggs'.price");
int fat = *(*root)->get<int>("menu.'bacon and eggs'.fat", 0);  // absent: 0
const baker::Array* eggs = (*root)->try_get("menu.'bacon and eggs'");   // null if absent
auto cals = eggs->find_int("calories");                       // one level, by tag
```

A key written as text (`find_int("calories")`) is interned on every call; in a loop,
write it as a literal, `find_int("calories"_sym)` (after `using namespace baker::literals;`),
which interns once, or hold a `Symbol`. A name's hash, `Symbol::hash()` or `"calories"_hash`,
is the same in every process and on every platform, for a save file or a `switch`
(docs/cookbook.md, "Stable IDs for names").
`Convert<T>` teaches lookups your own types; vectors, fixed arrays, and
`std::string` (with `<baker/std_interop.hpp>`) are built in. When an array has several
children with the same tag, lookup finds the first; `subarrays()` visits them all.

Every array remembers its file and line, so a value can always say where it came from.

### Metadata

A child tagged with `@` and a word is metadata: it says something about the value it
follows, and is never part of it.

```dta
(player
   (speed 4.5 (@range 0 10) (@step 0.5) (@desc "Run speed, in m/s"))
   (tint 1 0.5 0 (@range 0 1) (@kind color) (@note "try 0.8 on handheld")))
```

`player.speed` is `4.5` to every reader, and `player.tint` three numbers, so
`get<std::array<float, 3>>("player.tint")` reads them; `values()` ends at the first
metadata child, and `metadata()` lists the metadata. Baker's own readers use `@range`,
`@step`, `@desc`, `@note`, `@kind`, `@group` and `@one_of` (tweaks and tuning panels), `@was` (a layer's pin, Merging),
`@id` and `@aka` (a record's identity, Identity below) and the shape words below; `check` warns on any other `@` word, so a misspelt
`(@rnage 0 10)` is found. A path reaches metadata like any child: `player.speed.@desc`.

- **Metadata comes after the value.** `(x (@range 0 1) 5)` is an error at the array.
  Fields may come before, between and after metadata, so a record can carry its own
  description: `(cottage (desc "Two rooms") (size 14 10) (@note "why"))`.
- **A field may have any name.** Without the `@`, `(kind ball)` is a field, wherever it
  sits: `(ball (kind ball) (pos -6 6.5))` is two fields.
- **A lone `@`** is an ordinary symbol, so `('@' ...)` keys a record by the `@` key.
- **Before the sigil**, metadata was told apart by its words and its place. `check` warns
  where an array holds a value and a child tagged `range`, `step`, `desc`, `note`, `kind`,
  `group` or `one_of`, which now reads as a field; `baker_migrate` (in `tools/`) rewrites
  such files, inserting the `@` and changing nothing else, and `tools::migrate_text` gives
  each such place with its fix.

### Merging

Two trees merge by tag, which is how a Config stacks layers and how `#merge` works. For
each tagged array of the upper tree, the array with the same tag below is merged into
recursively; everything else (the values) replaces what was below. So
`(width 3)` over `(width 2 (@range 0 10))` gives `(width 3 (@range 0 10))`: a layer changes
a value without restating its metadata. A tag the lower tree lacks is added.

**A list of records that repeats a tag.** `(opening (build infantry) (build infantry) (build
tank))` reads whole, as written, in the layer that writes it. A layer over it merges by tag,
so its `(build ...)` merges into the first of the three, and a layer that restates the list,
`(opening (build tank) (build infantry) (build infantry))`, folds all its rows into that one
record: the stack reads the old list. A list a layer may change is written as plain values,
`(opening infantry infantry tank)`, which a layer restates whole or extends with `#append`
and `#drop`. A check of the layer warns at its row, naming the path and the tag,
`layer_effect` gives the row's `folded` count (its later rows of the same tag), and `diff`
tells two layers apart by their folded rows too. Quiet: a key new to the stack (added
whole), and a table split across rows, `(items (bow 2))` and `(items (axe 3))`, whose keys
merge one by one as written.

**Removing a key.** In a Config layer, `(key #remove)` removes `key`, and everything under it,
from the layers below:

```dta
(menu (debug_menu #remove))
(enemies (exploit_boss #remove))
```

The marker is the key's only value; metadata may follow it. Anything else, such as
`(a #remove 1)` or a bare `#remove`, is an error at load. A key the layers below lack
removes nothing, a layer above may set the key again, and removing the layer brings the
key back. The merged tree never holds a marker, and `#merge` ignores one (it only fills in
what is missing). `layer_effect` lists a removal with the value `#remove`, and what it
removed in `below`.

**Adding to and dropping from a list.** A layer that restates a list replaces it, so two
packages that each add an item leave only the top one's. `#append` and `#drop` change the
list below instead:

```dta
; dlc_a.dta and dlc_b.dta, over (items climbing_gloves glider lamp_oil spyglass)
(items #append rope)
(items #append torch)          ; merged: ... spyglass rope torch
; a patch: one tuple out, one in, the rest of the list untouched
(households (cottage (things #drop (phone -2 4.6 180))
                     (things #append (lamp -2 4 0))))
```

The verb is the key's first value, and what follows it, up to any metadata, is what it
adds or drops: numbers, symbols, strings, or whole arrays, which here are elements and not
keys (`(lamp -2 4 0)`). `#append` adds its operands after the values below, before their
metadata; layers that append to one list apply in stack order, so their placement orders
the list. `#drop` removes, for each operand, the first value below that is equal to it, a
tuple restated as written. A key with two verbs takes two rows, which apply in order. A
key the layers below lack is made from an `#append`'s operands, and a `#drop` of it does
nothing. As with `#remove`, the merged tree never holds a verb, `#merge` skips one, and a
verb anywhere but a key's first value is an error at load. A verb is never written inside
a `#shape`: a shape is declared whole.

A composing row supplies no value of its own: `source()` names the layer whose list it
changes, `explain` shows it as `Composes` with its verb, and `layer_effect`'s `op` names the
verb. Its `values` are the operands, one node each (a record is one), and its `below` is the
list the verb changes, every element of it, records too, with the source of the row that
wrote that list: `(moves (jab ...) (heavy ...))` below `(moves #append (jab2 ...))` is two
elements, and the merged list three. A `(@was ...)` pin on the row names that list. A check
warns at the row when an `#append` adds a value already there (a layer left behind after its
promotion would add it twice) or a `#drop` names one that is not. Writing a value back into
a layer at or below a row that composes it is refused, since the verb would apply again over
what is written; `promote` writes the list the verb makes.

**Pinning what a row was written against.** `(@was v...)` is metadata on a layer's row: the
value below the layer when the row was written.

```dta
; agent.dta
(cpu (easy (reaction 26 (@was 30) (@note "easy blocked the first low kick too often"))))
```

When the layers below move, a check of the layer says so at the row: `'cpu.easy.reaction'
was 30 when this row was written; the stack has 32`, which a policy with `warnings_refuse`
turns into a refusal, and pushing the layer sends the same warning to the warning handler.
`write_back_to(path, layer, true)` writes the pin, and `write_back` keeps the one a row
has. A pin is metadata, so every reader of the value reads past it.

### Identity

A record may carry an identity, `(@id ...)`: 32 lowercase hex digits, the first `a` to `f`,
minted by a tool when the record is made (the kit's `format --baker-mint-ids`) and written
inside the record, so it travels with the data and survives a rename of the key:

```dta
(weapons (laser (@id b3c9a7e5f214d68a1e2c3b4d5e6f7081) (damage 3)))
```

References keep naming keys (`key_of`); the ID is for tools, change records and renames.
`Array::record_id()` reads one; `find_by_id(root, id, &path)` finds the record that carries
it, by a walk of the tree. `baker::id_validator()`, added with `add_validator`, reports two
records that share an ID (a copied record) and a malformed one, as errors to the warning
handler at every merge; the host kit's loop and `check` add it. A shipped cache written
without metadata keeps `@id` and `@aka`: they are identity, not tool text.

A renamed record keeps answering to its old key with `(@aka old)`, which the kit's
`rename OLD NEW` writes, with the new key and every `key_of` reference: a layer
that retunes `(weapons (laser ...))` merges into the record now named `plasma`, and a
`(key_of weapons)` naming `laser` resolves to it (a key wins over an alias). A key that is also a sibling's alias is an error from `id_validator`. Aliases stay
until someone removes them, since a shipped overlay may still name one; `check` lists an
alias that no layer and no `key_of` names, as a finding. A stack with no alias pays
nothing for them.

A renamed field is the shape's to remember: `(hp int (@renamed_from health))` says `hp`
was `health`. A record spelling the old name is checked as the field, with a
warning from `check` saying so; a record with both names is an error, since the old one
reaches no reader. The generated view reads either spelling, and `format` rewrites the old
name to the new in every file of the stack, so a shipped overlay that still says `health`
keeps working until it is next formatted. Nothing is renamed at load: `get<T>` reads the
tree as written, and the migration happens once, in the text.

IDs are minted by `format --baker-mint-ids TABLE`: every record of the table without one,
or repeating another's (a copied record), gets a fresh one in the layer that declares it,
written after its key. The library has no random source and mints none.

### Rows: a key by its path

A layer usually changes a few values deep in the tree. A **row** names each by its path, as
`diff`, errors and the console print it: a parenthesized form at a file's top level whose
first atom is a `/`-led path.

```dta
; agent.dta: one line a value
(/fighters.kestrel.health 1100)
(/fighters.kestrel.walk_back 1.7)          ; a row's own comment
(/cpu.easy.reaction 26)
(/menu.debug_menu #remove)
(/shapes.boss_phase.enter_at float (@range 0 2))
```

reads as the tree `(fighters (kestrel (health 1100) (walk_back 1.7))) (cpu (easy (reaction
26))) ...`: the rows sharing a prefix fold into one array a key, and below the parser
nothing knows rows were written. The key is written as a path is (Tags and lookup):
segments separated by `.` or `/`, a `'quoted'` one for a name holding either,
`(/menu.'v1.2'.price 3)`, at most 16 segments. What follows the key is what follows a tag:
values, metadata, children, or a verb: `#remove`, `#append` or `#drop`. A key's verb rows
stay apart, in the order written, where its other rows fold.

- **A file with rows holds each key once.** A value written twice, by two rows or by a row
  and a nested array, is an error at the second, where a file without rows keeps the first
  (Merging). So a list of records that repeats a tag, a file of `(scenario ...)` forms, is
  written without rows.
- **Only at the top level.** `(a (/b 1))` is an error; a key is never a macro, and the
  body expands as any body does.
- **Not a row:** a quoted `('/x' 1)`, a lone `/`, and a `/`-led symbol anywhere but the
  head of a top-level array, such as `(path /usr/share)`. The printer quotes a `/`-led
  head, so its text reads back as the same tree.
- **Not an element.** `(/fighters.0.health 5)` is an error: rows name keys, and a list is
  written whole.
- **Writing.** `write_back` replaces a row's value in place, its comment kept.
  `write_back_to` adds a key to a file of rows as a row beside the rows of its prefix, and
  creates an absent layer file as rows; a nested file stays nested. `promote` writes a
  layer of rows into a nested base as it writes a nested layer, and carries a removal:
  the key's array is deleted from the target.
- **The kit:** `diff --baker-rows` prints a layer's changes as rows, which paste into a
  layer file.

Rows are for layers: a value changed is a line, its key named. A base file of records
stays nested, where a path repeated on every line would only add bytes.

### Curves

A curve is a function designers tune, such as reward by stage, difficulty by time, or
falloff by distance. It is a key's values: points `(x y)` in ascending x.

```dta
(falloff (0 1) (5 3) (10 8))                       ; monotone, clamped
(difficulty catmull-rom loop (0 1) (30 4) (60 1))
(reward linear (1 10) (2 25 0 15) (3 40))          ; a point's slopes: (x y in out)
```

- **Interpolation,** an optional symbol before the points, for the whole curve: `monotone`
  (the default: smooth, and never above or below the two keys around it), `linear`,
  `catmull-rom` (smooth, but it overshoots between keys that swing), or `constant` (each
  key's value held to the next key). The names are RenderMan's and Houdini's.
- **Beyond the points,** another optional symbol: `clamp` (the default: the end keys'
  values), `extrapolate` (on along the end slopes), or `loop` (repeating the span of x).
- **A point's slopes:** a third and fourth number give the slope (dy/dx) arriving at the
  point and leaving it, as glTF's Hermite tangents do. They replace the slopes the
  interpolation would compute there; a `linear` segment with one becomes a cubic, and a
  `constant` curve has none. An engine that stores tangents as angles takes their atan.
- **In C++,** `get<Curve>(path)` and `Tweak<Curve>` read one (`<baker/curve.hpp>`), and
  `sample(x)` gives its value. **In script,** `{curve.sample path x}`.
- **A layer replaces a whole curve,** points, interpolation and extension together, as it
  replaces any key's values.

## The preprocessor

Directives are handled while a file is read and never appear in the tree. Files read
through one *session* (`LoadSession`, or a Config's `session()`) share defines and macros,
in the order they are read; a Config's layers read in one session, bottom to top.

| Directive | Effect |
|---|---|
| `#include path` | Splice another file's top-level nodes here. |
| `#include_opt path` | The same; a missing file is not an error. |
| `#include_dir path` | A child for each `.dta` file of a directory, named by its file and holding its nodes, and for each subdirectory, named by it and holding its own. See below. |
| `#require path` | The same, once per session: later `#require`s of that file, from any file, are no-ops. |
| `#merge path` | Merge another file into the enclosing array, keeping existing keys and adding missing ones. |
| `#ifdef N`, `#ifndef N`, `#else`, `#endif` | Conditional sections. |
| `#define N` | Define a flag, for `#ifdef`. |
| `#define N (body)` | The older macro form: define macro N. The read warns; write `[N body]`. |
| `#undef N` | Remove a flag or macro. |
| `#autorun {command}` | Collect the command into the session's `autorun()` list, for the host to run after loading. A Config reads each layer with a session of its own: `cfg.autorun()` returns its enabled layers' commands, bottom to top. |
| `[N node ...]` | At the top level of a file: define macro N. |
| `#pack (NAME ...)` | Declare the file a pack. See Packs and levels. |
| `#origin ("source" ...)` | Declare the file generated. See Generated files. |
| `#shape (NAME ...)` | Declare a shape. See Shapes. |

Paths may be quoted (`#include "shared/common.dta"`) or bare, and resolve against the
including file's directory, then the session's include directories
(`add_include_dir`). Either `/` or `\` separates a path's parts.

Any other `#name` is an error, so a misspelled directive is caught. `#` opens a directive
only before a letter: a lone `#`, `#1` and `#00aa00` are symbols, as in a key binding
`(# {toggle_hud})` or a color, and the printer writes them bare.

### A directory as a table: `#include_dir`

A game with a file per record (a card, a unit, a quest) names the directory instead of
each file:

```dta
(cards
   #include_dir "cards"
   (starter_deck fireball fireball shield))   ; other children beside it
```

`cards/fireball.dta`, holding `(cost 3) (damage 6)`, becomes `(fireball (cost 3) (damage 6))`
in `cards`, so a layer retunes it as `cards.fireball.cost`, and `source()` names
`cards/fireball.dta` and its line. The record is named by its file, so the two cannot
disagree, and a new card is a new file.

- **A subdirectory is a nested table:** `cards/fire/fireball.dta` becomes
  `(fire (fireball ...))` in `cards`. A name is unique in its directory.
- **Order:** the names a directory's `order.dta` lists, `(order fireball shield)`, come
  first, in its order; the rest follow in name order. A name it lists that the directory
  lacks is a warning, located in it. Without one, the order is name order, the same on
  every platform. `order.dta` is not a record.
- **What is read:** `.dta` files and subdirectories (entries without an extension).
  Hidden entries, those whose names begin with `.`, are skipped. A name that is not a
  symbol Baker reads bare (`my card.dta`) is an error, as is a file that does not parse,
  or a directory that cannot be listed.
- **Where:** the path resolves as `#include`'s does, against the including file's
  directory, then the include directories. The session's lister lists it
  (`LoadSession::set_lister`, the disk by default), so a host's archive works as its
  reader does.
- **What notices a change:** a Config's reload, the host kit's watcher, and a cache, which
  is stale once a file is added, removed or renamed, or `order.dta` changes. Each file
  counts as an include toward the session's limits.

### Macros

A macro is a named sequence of nodes, spliced wherever its name appears as a bare symbol:

```dta
[RED 1 0 0]
[HANDLER (hit {fall_down})]
(barrel (color RED) HANDLER)
(crate  (color RED) HANDLER)
```

`barrel` reads as `(barrel (color 1 0 0) (hit {fall_down}))`. Splicing shares the macro's
arrays rather than copying them. A macro defined in one file serves every later file of the
session, which is what a file of shared definitions is for.

### Platforms and builds

Defines usually come from the host, so one file serves every target:

```dta
(renderer
#ifdef HANDHELD
   (shadows 0)
#else
   (shadows 1)
#endif
)
```

```cpp
// with a project file: the build's own defines, beside the project's (defines ...)
static constexpr std::string_view kDefines[] = {"HANDHELD", "steam"};
baker::open_project(cfg, project, {.defines = kDefines});
// or before pushing the layers by hand
cfg.session().define(baker::Symbol("HANDHELD"));
```

- **Who made each.** `session.active_defines()` lists the flags that stand, in the order
  they were made, each with its origin: the host's, the project's `(defines ...)`, or a
  file's `#define` with its file and line. `check`'s JSON and the reference list them, so a
  tool can show which sections a build reads; `explain` names the define that chose a
  value read inside an `#ifdef`.
- **Changing them on a live stack.** `cfg.set_defines(names)` replaces the host's flags: a
  server switching mode, a demo unlocked. Each layer that tested, defined or undefined a
  name that changed is read again, in place; every other layer keeps its tree. It is one
  reload, with the trigger `defines`, and works in a shipping build. If a layer does not
  read under the new set, nothing changes.

### `#require`

`#include` splices a file every time. `#require` splices it the first time the session sees
it; afterwards it is a no-op, from any file and any later read through that session.
Use it for a file several others depend on, such as a pack of shared macros: required from
three files, it is read once, and its macros serve all three.

## Packs and levels

A *pack* is a file that declares itself one with `#pack`, anywhere at its top level:

```dta
#pack (boss_tools (level game) (requires combat_core))
[boss.restart_phase {boss_phase_restart}]
```

The host names the levels, hardest first; a pack's level is its rank among them (the
spec calls this its layer; Baker says *level*, because a Config's layers are its stack of
files):

```cpp
cfg.session().set_pack_levels({"engine", "game", "task"});
```

The form is recorded (`session.packs()`, `session.pack_of(file)`) and never in the tree.
A file declares one pack, and the whole file is the pack's, above its `#pack` too. A pack
without a level, with a level not in the list, a second `#pack`, or one below the top level
is an error at load. A form `(pack ...)` without the `#` is ordinary data, and a layer
check warns of it.

**Requires and versions.** Once a pack's file is read, each pack it `requires` must be
loaded, and a required version met; otherwise the load warns, located, and goes on.
A version is dot-separated integers, compared part by part (`"1.10"` is after
`"1.2"`), written as a string, a symbol, or an integer, never a float:

```dta
#pack (frost_dlc (level dlc) (version "1.0") (requires (base_game "1.2") season_pass))
```

**Harness metadata.** A pack may also say who keeps it, whom it is for, and how far it has
evolved, from an idea to a commodity:

```dta
#pack (boss_tools (level task) (requires combat_core)
   (owners "td-encounter" "td-combat") (for designers) (stage custom))
```

`owners` and `for` list names (strings or symbols); `stage` is `genesis`, `custom`,
`product`, or `commodity`, and anything else is an error at load. They are kept in the
`Pack` the session records, for tools and reports; nothing else depends on them.

**The one-way rule.** A harder pack may not depend on a softer one: engine code does not
reach into one task's harness. Baker warns, never fails, on two kinds of reach:

- **Macros**, while reading: a pack's file expanding a macro that a softer pack defined
  is a warning in `session.warnings()`, once per file and macro, with file and line.
  A Config forwards it to its warning handler.
- **Calls**, in `check`: a call, in a pack's file, to a script function defined in a
  softer pack's file is a warning in `CheckReport::warnings`. Pass the session that
  read the files: through a Config, `read_session()`, which holds every layer's packs,
  not `session()`, which each layer reads a copy of:
  `baker::check(tree, ctx, report, {.packs = &cfg.read_session()})`.

Softer using harder is the point of the levels, and passes silently; so does everything
in files that declare no pack, so a small project pays nothing. By convention a pack's
names carry a dotted prefix (`boss.restart_phase`), which the reference and tuning panels
group by.

### Generated files: `#origin`

A file that a tool generates, such as an export from a spreadsheet, says so with `#origin`,
anywhere at its top level:

```dta
#origin ("economy.xlsx" (tool sheets-export))
(econ (price 5) (tax 2))
```

The form is recorded (`session.origin_of(file)`) and never in the tree, and a Config's
`source()` reports `origin` and `tool` for every value the file supplies, so a number can
be traced to the spreadsheet it came from. The first value must be a string, and a file
declares one origin. A form `(origin ...)` without the `#`, such as `(origin 0 0 0)`, is
ordinary data.

## Shapes

A shape says what an array should look like: its fields, their types, and their ranges.
Each shape is declared with `#shape` at a file's top level, a form tagged by the shape's
name. The session records it, and the tree never holds it:

```dta
#shape (boss_phase (@at bosses * phases)
   (enter_at float (@range 0 1) (@kind percent) (@desc "HP fraction that starts it"))
   (attacks  (list symbol))
   (cadence  float (@range 0.2 5) (@optional)))
```

- **Types:** `int`, `float`, `bool`, `symbol`, `string`, `any`, `curve`, `(list T)`,
  `(key_of TABLE)`, `(asset KIND)`, or another shape's name. A `curve` field's values are a curve (see
  Curves, above), and its `(@range lo hi)` holds every point's y. A list may give its count:
  `(position (list float 3))` holds exactly three values and `(tags (list symbol 1 4))` one to
  four, and any other count is an error at the field. A softer pack may narrow the count, never
  widen it.
- **Metadata:** `@range`, `@step`, `@kind`, `@desc`, `@note` (as a tweak's), `@optional`,
  `@renamed_from` (Identity, above), and, for a shape that generates a C++ view (Views,
  below), `@view`, `@cxx` and `@cxx_name`. A field without `@optional` is required. A word without the `@` after a
  field's type, or a `@` word the grammar lacks, is an error. A shape may describe itself
  with its own `(@desc ...)` and `(@note ...)`, and its fields may be named anything,
  `at` and `desc` included. A `symbol` or `(list symbol)` field may list the
  symbols it allows, `(button symbol (@one_of light heavy kick special))`, in the order a tool
  should offer them; a later layer declaring the field again may add to the list.
- **What a shape governs:** every array tagged with its name, anywhere in the tree. `(at
  ...)` narrows it to arrays whose ancestors' tags, from the root, are the ones listed;
  `*` matches any one tag. The example above governs each `(boss_phase ...)` in
  `(bosses (dragon (phases (boss_phase ...) (boss_phase ...))))`.
- **Keyed children** are tagged by their own names, as in `(bosses (dragon ...) (golem
  ...))`, so no shape is named after them. Declare the shape `(@keyed)`, with an `(@at ...)`
  naming their parent, and it governs every child there, whatever its tag:

  ```dta
  #shape (move (@at fighters * moves) (@keyed)
     (startup int (@range 1 60) (@kind frames)))
  (fighters (kestrel (moves (jab (startup 4)) (heavy (startup 9)))))
  ```

  Keyed data is what layers merge by name: a layer above can retune `heavy` alone. Data
  written `(boss (name dragon) ...)` is governed by name, but repeated tags do not merge
  one by one.
- **A shape that governs nothing** checks nothing, silently: `(@at fighters moves)` for
  `(@at fighters * moves)` is one level off. `shape::unused(tree, shapes, report)` warns of
  each such shape, located where it is declared ("shape 'move' governs nothing: no array is
  a child of fighters/moves"), and the kit's `check` runs it. A validator does not, since a
  stack may lack some content for now.
- **Keys of a table.** A field `(key_of TABLE)` holds the key of a record in TABLE, a
  dotted path such as `weapons` or `items.weapons`; `(list (key_of TABLE))` holds several.
  The table is the nearest one of that path that encloses the record, looked up outward, the
  root last, so a fighter's move names one of that fighter's moves:

  ```dta
  #shape (unit (@at units) (@keyed) (weapon (key_of weapons)))
  #shape (move (@at fighters * moves) (@keyed) (cancel_into (list (key_of moves)) (@optional)))
  (weapons (laser (damage 3)) (cannon (damage 9)))
  (units (scout (weapon laser)))
  (fighters (kestrel (moves (jab) (heavy (cancel_into jab)))))
  ```

  The check runs on the merged tree, so a key a layer adds may be named, and one a layer
  removes is an error at each record that still names it, with the key it most likely
  meant: `'lazer' is not a key of weapons (laser?)`. With packs, a harder pack naming a
  softer pack's key is a warning, as for a call.
- **Assets.** A field `(asset KIND)` holds the path of a file outside Baker, a string
  relative to the project file's directory, and `(list (asset KIND))` several. The
  project file declares the kinds and the extensions each takes:

  ```dta
  (assets (mesh ".glb" ".fbx") (texture ".png" ".tga") (sound))
  ```

  `check` reports a kind the project does not declare, a path whose extension its kind
  does not list (a kind listing none takes any file), and a file that is not there, found
  by listing the file's directory once per run through the host's lister. The kit's
  `assets` command writes every edge of the merged stack as `baker.assets/1` (see [the
  assets document](assets-json.md)), for a pipeline to index forward or in reverse. A view
  holds the path as a `std::string_view`. Baker never imports, stores or cooks an asset.
- **Layers combine shapes by name**: a later layer's `#shape` of the same name adds a
  field or retunes one field's range, and one of a new name adds a shape. Its form is
  written whole, never with a layer verb. A layer received under `data_only` or
  `scalars_only` may not declare one.

Checking reports a field of the wrong type, out of range or outside its `one_of`, a key
its table lacks, or a required field missing, as an error, located by file, line, and
path. A field the shape does not declare is a warning, so adding data never waits on its shape. Commands and variables are not checked:
their values are unknown until they run.

```cpp
// checked at every push, reload, and restore, and in each check of a layer
cfg.add_validator(baker::shape::validator(), {.any_thread = true});
```

Through a Config, violations go to the warning handler; the load itself never fails on a
shape. Elsewhere, `baker::shape::Set` reads the declarations and `baker::shape::check`
checks a tree; `set.shapes()` enumerates them, with their metadata, for a tool to build
panels from. Shapes are opt-in, and cost nothing unless checked.

### Rules between values

Some rules span several values: a unit starts with no more health than its maximum, loot
weights sum to 100, a card's power is near its tier's ideal. A shape states them as rules,
each a message and a script expression that must be true for every record the shape
governs:

```dta
#shape (unit (@at units) (@keyed)
   (max_hp int (@range 1 9999))
   (start_hp int)
   (@rule "starts at or below its maximum" {<= $start_hp $max_hp}))
#shape (card (@at cards) (@keyed)
   (tier (key_of tiers))
   (power float)
   (@rule "within 10% of its tier's ideal"
         {and {>= $power {* 0.9 {data tiers $tier ideal}}}
              {<= $power {* 1.1 {data tiers $tier ideal}}}}
         (@warn)))
#shape (loot (@rule "weights sum to 100" {== {data.sum loot weight} 100}))
```

- **What a rule reads.**
  - Its record's fields, each as a `$variable` named by the field: one value is itself,
    several an array. A field the record lacks leaves its variable unset, and reading it
    fails.
  - The rest of the tree under check: `{data SEGMENT ...}` gives the value at a path,
    each argument evaluated, so `{data tiers $tier ideal}` follows a key. `{data.sum PATH
    FIELD}` sums each child's FIELD, or each child's first number without one. Both read
    the tree being checked, never a live Config, so a check of a candidate layer judges the
    candidate.
  - The game's own functions, when the game checks.
- **What a failure says.** A rule whose expression is false is an error, or a warning with
  `(@warn)`, located at the record and carrying each value its expression read:
  `starts at or below its maximum ($start_hp 12, $max_hp 10)`. A rule that does not
  evaluate, or gives something other than true or false, is an error naming the rule.
- **Layers** replace a rule by its message, so a later layer may add a rule or soften one
  with `(@warn)`. With packs, a softer pack may make a harder pack's rule an error, but not
  soften or change it.
- **Where they run.** `baker::shape` reads rules; it cannot run script, so
  `baker::tools` evaluates them:

  ```cpp
  // after every push, reload and restore, on the game's thread: a live edit that breaks a
  // rule warns at once
  cfg.add_validator(baker::tools::rules_validator(ctx));
  // or over one tree
  baker::tools::check_rules(tree, shapes, ctx, report);
  ```

  The host kit adds the validator, and its `check` command fails on a broken rule. A build
  without `baker::script` reads rules and says once that it cannot check them.
- **Cost.** Rules run only in checks: one evaluation per rule and governed record, about
  half a microsecond for a two-value comparison, so 3,000 cards with two rules each is
  under 3 ms a reload. A shape set with no rules costs nothing more.

### Views: a C++ struct from a shape

A shape marked `(@view)` generates a C++ struct the game reads its records into, so the
layout is written once, in the data, and the code never restates it:

```dta
#shape (move (@at fighters * moves) (@keyed) (@view)
   (startup     int (@range 1 60) (@cxx uint16))
   (damage      float)
   (cancel_into (list (key_of moves)) (@optional))
   (on_hit      any (@optional) (@view no)))
```

```cpp
#include "move_read.hpp"   // generated: game::Move, its reader and Convert<Move>
auto jab = baker::read_view<game::Move>(cfg, "fighters.kestrel.moves.jab");
if (jab) frames = jab->startup;              // uint16_t; damage is a float
for (baker::Symbol into : jab->cancel_into) ...   // a ListView<Symbol> over the list
```

- **Members.** `int` is `int32_t`, `float` is `float`, `bool`, `symbol` and `(key_of T)` are
  `baker::Symbol`, `string` is a `std::string_view`, `curve` a `baker::Curve`, another
  shape its struct, nested, and `(list T)` a `baker::ListView<V>` over the list, which
  converts each element as it is read and checks a declared count. `(@cxx int64)` picks a
  member's C++ type for an `int` (`int8` to `uint64`) or a `float` (`float`, `double`);
  `(@cxx_name N)` names a member or the struct when the data's name is not a C++ name. A
  field marked `(@view no)` stays out of the struct and is read from the tree as before; an
  `any` field must be. An absent `@optional` field reads as zero, `false` or empty: views
  hold no `std::optional`. Beside the digest, a struct carries `kAt` (its shape's `(@at
  ...)`, as written), `kKeyed`, and `fixups()`, the members that point outside the struct,
  which a baked table relocates into its own pool (Tables, below).
- **Reading.** `read_view<T>(cfg, path)` and `read_view<T>(record)` return an `Expected<T>`:
  a value of the wrong type, outside its member's range, or a list of the wrong count is a
  located error naming the field; a required field missing names it too. The generated
  `Convert<T>` lets `get<T>` read a view as well. A view's strings and lists borrow from the
  tree: hold the Config's `root()` while they are used, and read after `apply_edits()`.
- **Generating.** The project file names the contract layers the views come from and the
  namespace, `(views (layers core content) (namespace game))`; a field a mod or an overlay
  adds is readable from the tree and never a member. `baker_generate_views(target PROJECT
  file)` in CMake runs `baker_bake --views` whenever a contract file changes, and a header
  is rewritten only when its text changed, so a tuning edit rebuilds nothing. A view's
  shape set is one per build: a `#shape` inside an `#ifdef`, or a `dev_only` or `(when ...)`
  contract layer, is refused (`docs/cookbook.md`, Views in the build).
- **Drift.** Each struct carries a digest of its shape's layout (names, types and order,
  never tuning metadata). `baker::shape::check_views` compares the compiled views with
  the shapes a running stack declares, and the kit's `check` reports a mismatch as a
  shape warning, or an error under `--baker-strict-views`.
- A softer pack may not leave a field out of a view, change its C++ type or rename its
  member (as it may not loosen a range). Views cost nothing unless generated and read.

### Tables: a keyed shape's records, packed

A viewed `(@keyed)` shape's records can be read as one packed array of its struct, built
from the merged tree once per change instead of walked per read:

```cpp
#include "move_read.hpp"
#include <baker/table.hpp>

baker::add_table<game::Move>(cfg);               // before the load; or register_tables(cfg)
cfg.push_layer(...);
baker::Table<game::Move> moves = baker::table<game::Move>(cfg);
if (auto i = moves.index_of("kestrel.jab")) {   // the path below the table
    const game::Move& jab = moves[*i];           // one indexed load; at(i) checks
    for (baker::Symbol into : jab.cancel_into) ...  // a packed list, the table's own
}
for (uint32_t i : moves.children_of("kestrel"_sym)) ...   // one parent's records
```

- **One block.** The rows, in the merged tree's order and grouped by parent, then a pool
  the rows' strings, lists and curves point into, then the indices: by key (an alias of a
  renamed record resolves too), by `(@id ...)`, and by parent. A `Table<T>` handle keeps
  the block it was given: a reload that rebuilds the table takes nothing from a reader
  holding the old one. The block comes from the Config's allocator.
- **Finding a row.** `index_of(key)` for a shape whose `(@at ...)` has no `*`;
  `index_of("kestrel.jab")` or a `Path` for the tags the `*` segments stand for and then
  the key; `index_of(id)`; `children_of(parent)` is a contiguous range. `key_of(i)` and
  `id_of(i)` read back. The text form parses and interns on every call: a read that runs
  every frame holds the `Path`, the `Symbol` or the `RecordId`, or better, the index. An index is stable across a reload while the table's set of
  records is unchanged; a record added, removed or reordered renumbers what follows it.
- **Rebuilt by identity.** Every merge, reload and `apply_edits()` rebuilds a table whose
  governing arrays the new tree does not share, and no other; a Config with no table
  registered builds none. Registering after the load builds at once.
- **Literals only.** A view field holding a `{command}`, a `$variable` or a value of the
  wrong type reads as the member's default, with a located error to the warning handler;
  the record's other fields still read, and the kit's `check` fails. A field marked
  `(@view no)` is never in the table.
- **Shipped as an image.** The kit's `bake --baker-stack game.datb`, run on the game's own
  binary, writes the contract layers' trees and every registered table as one file;
  a Config made with `ConfigOptions::stack_image` takes those layers from it in
  `open_project`, in place of their text, and the tables with them, their pointers fixed
  up and their symbols interned in one pass. A table the game no longer compiles alike
  (another digest, size or fix-up list) is skipped with a warning and built from the
  tree. Layers above the contract (mods, overlays) push as today and rebuild the tables
  they touch. Development builds never need the file.

## Script

Commands are arrays in braces: `{function args...}`, or `{object method args...}`. A
command in data is a value computed when it is read:

```dta
(enemy
   (grunt (hp {* 30 $difficulty}))
   (brute (damage {if {> $difficulty 2} 12 9})))
```

Reading `enemy.grunt.hp` as an int from C++ evaluates the command, in the thread's current
`Context`, and converts the result. Reading the node raw (`try_find`, `operator[]`) gives
the command itself, unevaluated: that is how handlers are stored.

### Evaluation is lazy

Running `{f a b}` finds `f` and hands it the command; nothing evaluates `a` or `b` until
`f` reads one, by type. So `if`, `set`, `do`, `and`, and `or` are ordinary functions, not
keywords: `if` reads its condition and evaluates only the branch it takes. There are no
special forms, which is why the whole language is the [reference](script-reference.md)'s
list of functions.

The built-ins are control (`if`, `do`, `set`, `func`, `override`, `class`, `new`,
`delete`), arithmetic (`+ - * / %`), assignment (`+= -= *= /=`), comparison (`== != < <= >
>=`), and logic (`and or not`). Ints stay ints (wrapping on overflow) until a float joins
them; `{/ 7 2}` is 3.5. Everything else is what the host binds:

```cpp
baker::Context ctx;
ctx.func("spawn", [](baker::Symbol kind, int64_t count) { /* ... */ return count; });
ctx.global("difficulty", 2, "Set by the game: 1 to 3.");
```

A native function's parameter and result types are deduced from its signature; it can
take `Raw<Node>` for an argument it evaluates itself, `std::optional<T>` for an optional
one, `Args` for the rest, and registering the name again with another signature makes an
overload set. A call with two scalar arguments takes about 31 ns and never allocates.
The Context keeps a copy of the callable (moved, when given a temporary) in its own
memory; a lambda that captures nothing is not kept at all. Registering costs about 460
bytes of code per binding with GCC at -O2, most of it the call itself, and the
registration's own work runs out of line.

### Operators, and braces written infix

The operators take more than two arguments where that reads as one idea. `+`, `-` and `*`
fold left over two numbers or more: `{+ 1 2 3}` is 6 and `{- 10 2 3}` is 5, and `{- 5}` is
-5. `<`, `<=`, `>` and `>=` test every adjacent pair, evaluating each argument once and
stopping at the first pair that fails, so `{<= 0.3 $value 0.7}` is a band. `==`, `!=`, `/`
and `%` take two. `{+= $hp 5}` adds, stores and returns the new value, and so do `-=`, `*=`
and `/=`; in a method, `{+= [hp] 5}` stores to the property. A place that is not set, or
holds no number, is an error, as reading it is.

A brace may be written with its operator between its operands:

```dta
(objective cube.turns
   (measure {cube.turns})
   (pass {2 <= $value <= 10}))        ; the command {<= 2 $value 10}
(unlock {{econ.total_earned} >= 10000})
{func average ($a $b) {{$a + $b} / 2}}
```

A brace of an odd number of nodes, three or more, whose odd slots hold one and the same
bare operator (a symbol of `+ - * / % < > = ! & | ^ ~` only), reads head-first: `{a op b op
c}` is `{op a b c}`. It does so only when one of its operands is a number, a variable, a
property or a command, so a brace of bare symbols, `{wheat -> flour}`, is a call of `wheat`
as it always was. The tree is the prefix command's, so evaluation, `check`, layers and
provenance see no difference; the array remembers it was written infix, and printing,
write-back and promote give that spelling back.

- **One operator a brace.** `{0 < $value <= 10}` is an error at the brace ("mixed
  operators in {...}: nest braces to say which applies first"). Nest instead, under `and`:
  `{and {0 < $value} {$value <= 10}}`. There is no precedence to learn.
- **Every operand needs its operator.** `{0.3 <= $value 0.7}` and `{$value <=}` are errors
  at the brace, which name the fix.
- **A call stays a call.** A brace whose first element is a function's name is a call
  whatever follows it (`{f + 1 2}`), and so is a prefix command (`{<= 0.3 $v 0.7}`). A
  quoted operator never reads infix: `{$obj '<=' 1 2}` calls the object's `<=` handler, and
  the printer quotes an operator where the reader would otherwise take a prefix command as
  infix (`{+ '-' 1}`). A macro used between operands, or one that expands to more than one
  node, leaves its brace as written.
- **Overlays for older builds.** A build of Baker before infix reads `{2 <= $value <= 10}` as
  a call of 2. Data that a build without it reads, such as a live-ops overlay for a game
  already shipped, stays in the prefix spelling, which every build reads.

### Variables

`$name` reads a variable. Variables are **dynamically scoped**: `{do (($a 1) $b) body...}`
binds `$a` to 1 and `$b` to unset for the body, and restores both afterwards, even on an
error. A function sees, and can change, the variables of whoever called it. That is
deliberate (it is the original design, and why reading `$a` is one load), so do not treat
it as lexical scope. `{set $a value}` stores; the host reads and writes the same cells with
`ctx.var("a")`.

### Functions

```dta
{func scaled ($base) (@doc "Scales by difficulty.") {* $base $difficulty}}
{func old_spawn ($kind) (@status deprecated spawn) {spawn $kind 1}}
```

A script function binds its arguments like `do`, runs its body, and returns the last
value. `(@status stub)`, `(@status provisional)`, `(@status deprecated replacement)`, and
`(@doc "...")` may follow the parameters (another `@` word there is an error, and a form
without the `@` is the body's first); status shows in the reference and in `check`'s
demand report. A script function may not replace a native one unless written
`{override name ...}`, which logs it.

### Objects and classes

A C++ type derived from `baker::Object<T>` binds methods and properties once, in a static
`bind`, and is added to a Context by name:

```dta
{grunt dash 3}          ; a C++ method
{grunt get hp}          ; a property; {grunt set hp 50} stores one
{grunt hit 20}          ; a typed message's script face
```

`{class T (name ($params) body...) ...}` attaches script handlers to a type, C++ or
script-only; handlers come before native methods, for script and C++ senders alike, so a
designer can change one type's response without touching its C++. In a handler, `$this` is
the object, and `[name]` reads its property or loose parameter (`{set [hp] 5}` stores
one). Loose parameters belong to the object's stay in its Context: removing the object
drops them. `{new T name (param value) ...}` makes an object of a script class or a bound
type, and `{delete obj}` removes it when the host's `eval` or `run` returns (during a
Scheduler's tick, at its end): until then the commands after it still reach the object,
and a call already running on it finishes safely.

`{class T ...}` again replaces every handler of `T`: that is what a reloaded script means.
A handler or `{func}` replaced while it runs finishes its old body; the next call runs the
new one.
To add handlers from another file (a task pack adding one action to a type), write
`{extend T (name ($params) body...) ...}`: it adds each handler, replacing only one of the
same name. `T` is a class already defined or a bound type. Run an extension after its
class, as the layers' `#autorun` commands are, bottom to top.

```dta
{class villager (eat ($obj) ...) (sleep ($obj) ...)}     ; data/default.dta
{extend villager (gossip ($obj) ...)}                     ; packs/task/gossip.dta
```

A command sent to a destroyed object is a clean error: Contexts refer to objects by salted
handles, never pointers.

### Tasks

The default scheduler is an object named `scheduler`:

```dta
{scheduler delay_task 2 {play_sfx "ding"}}           ; once, 2 units from now
{scheduler interp_task $fade 0 100 {set_alpha $fade}} ; $fade from 0 to 100 over 100 units
{scheduler thread_task {open_door} {wait {door_open}} {wait_for 1} {close_door}}
```

A task captures, when it is made, the value of every bound variable it names, so a task
made inside `{do (($n 5)) ...}` still sees `$n` after the `do` has returned. Each returns
a handle for `{scheduler cancel $h}`. A task made in a handler ends when its `$this` object
is removed. The host owns time: `ctx.scheduler().tick(dt)`, once a frame, in its own
units. C++ tasks are coroutines on the same scheduler (`co_await baker::delay(t)`).

### Checking before running

`baker::check(tree, ctx)` walks data without evaluating anything and reports every call
with the wrong number of arguments for every overload, or a literal argument of the wrong
type. Arguments that are commands or variables are skipped: their types are unknown until
run. Its **demand report** lists the calls to functions nobody has written yet, and to
stubs, with every site: the backlog the data is asking for.

```
baker: 'spawn' takes 2 arguments, given 1  (rules.dta:11)
demand: 'play_sfx' is not defined, called at rules.dta:11
```

It also reports **repetition**: subtrees of at least six nodes that appear three or more
times across the trees given to one call, with every place. They are candidates for a
word of their own (a macro, a `{func}`, a shape). A macro's expansions share its arrays and
count once, since a macro is a word already made, and a repetition inside a larger one is
reported only where it also appears on its own. Give one call every file, so it can compare
them; `baker::find_repeats(trees)` (`<baker/repeats.hpp>`) does the same without a Context.

```
repeat: 3 times, 10 nodes: (enemy (hp 30) (speed 2) (loot coin)) at a.dta:2, b.dta:2, c.dta:5
```

With the session (`{.packs = &cfg.read_session()}`), each pack that declares one owner is a
**finding**, informational, never an error: a harness one person keeps.

Run it in CI over every data file, and on every merge in a development build: a Config
runs its validators after each push, toggle, reload, and restore, and sends what they find
to its warning handler.

```cpp
cfg.add_validator(baker::check_validator(ctx, {.packs = &cfg.read_session()}));
```

The validator reports errors and one-way warnings; the demand report, repeats, and
findings are for tools. `ctx` must outlive the Config's use of it. For a build
artifact, `baker::tools::to_json(report, &cfg.read_session())` writes the whole report,
packs included, as versioned JSON that is the same bytes for the same data
([schema](check-json.md)); and `baker::tools::reference(ctx, &cfg.read_session())` adds the
packs and the files tools generated to the reference: the harness register.

## Errors

Baker never throws: every call that can fail returns `Expected<T>`, whose error is
a structured `Error`: a code, a message, and where it happened. Its one-line form is

```
baker: 'price' is float, not int  (menu.dta:3, at menu/'bacon and eggs')
```

read as: what went wrong; the file and line of the array involved; and the path from the
root to it, which can be pasted back into a lookup. A parse error also names the column,
counted in characters from the start of the line, and points where the trouble started, so
an unclosed parenthesis points at the parenthesis itself:

```
baker: '(' is never closed  (menu.dta:12:4)
```

`Error::column()` gives it, 0 when unknown. An error found on an array after the parse (a
lookup, a shape, a script call) names the array's line and no column.

An error raised in script also carries the script calls it unwound through, innermost
first. `describe(error)` prints them:

```
baker: '%' by zero  (boss.dta:9)
  in {%}  (boss.dta:9)
  in {bob attack}  (boss.dta:4)
```

Read it top down: the failing call, then each caller, each with the file and line where it
is written. `script_backtrace()` prints the same stack while a call is running, for an
assert handler or crash reporter to print beside the C++ stack.

### Fatal errors

A few failures leave Baker nothing to return: an allocator that returns null, an array or a
string past 4 GB, reading the value of an `Expected` that holds an error. Baker reports
each to the fatal handler and aborts. The message names what failed, and while a script
call is running it carries the script stack, innermost first:

```
baker: fatal: an allocator returned null, asked for 16 bytes
in {exhaust}  (level.dta:1)
in {spawn_wave}  (level.dta:1)
```

`baker::set_fatal_handler(fn)` routes it to an engine's crash reporter; the default writes
it to stderr. The handler must not return, and Baker aborts if it does. The message is built
without allocating, since the allocator may be what failed, and is cut at 4 KB.
