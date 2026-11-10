# Baker from C++: views, tables and the stack image

This page is for the programmer who connects a game's C++ to its data. A shape marked
`(@view)` generates a C++ struct the game reads records into; a viewed keyed shape's records
can be read as one packed table of those structs; and a shipping build can load the contract
layers and their tables from one baked file, the stack image. The words a shape is written with
are in the language guide: [Shapes](language.md#shapes), and for views,
[Views and tables](language.md#views-and-tables). The build steps are in the cookbook's
[Views in the build](cookbook.md#views-in-the-build).

- [Views: a C++ struct from a shape](#views-a-c-struct-from-a-shape)
  - [Members](#members)
  - [Reading](#reading)
  - [Generating](#generating)
  - [Drift](#drift)
- [Tables: a keyed shape's records, packed](#tables-a-keyed-shapes-records-packed)
  - [One block](#one-block)
  - [Finding a row](#finding-a-row)
  - [Rebuilt by identity](#rebuilt-by-identity)
  - [Literals only](#literals-only)
- [The stack image](#the-stack-image)

## Views: a C++ struct from a shape

A shape marked `(@view)` generates a C++ struct the game reads its records into, so the
layout is written once, in the data, and the code never restates it. Views cost nothing
unless generated and read.

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

### Members

`int` is `int32_t`, `float` is `float`, `bool` is `bool`, `symbol` and `(key_of T)` are
`baker::Symbol`, `string` is a `std::string_view`, `curve` a `baker::Curve`, another shape its
struct, nested, and `(list T)` a `baker::ListView<V>` over the list, which converts each
element as it is read and checks a declared count. `(@cxx int64)` picks a member's C++ type for
an `int` (`int8` to `uint64`) or a `float` (`float`, `double`); `(@cxx_name N)` names a member
or the struct when the data's name is not a C++ name. A field marked `(@view no)` stays out of
the struct and is read from the tree as before; an `any` field must be. An absent `@optional`
field reads as zero, `false` or empty: views hold no `std::optional`. Beside the digest
([Drift](#drift)), a struct carries `kAt` (its shape's `(@at ...)`, as written), `kKeyed`,
`kById` (`(@view by_id)`, below), and `fixups()`, the members that point outside the struct,
which a baked table relocates into its own pool
([Tables](#tables-a-keyed-shapes-records-packed)).

The members are declared by alignment, largest first, and in the shape's order within one
alignment, so the struct has no padding that another order would avoid: pointers and what
holds them (`std::string_view`, `baker::Symbol`, `ListView`, `Curve`) first, then four-byte
members, then two- and one-byte ones; a nested struct counts as its largest member. The names
are the fields'. Initialize a view by member name, `Move m; m.damage = 3;`, rather than by
position, which follows this order and not the shape's.

### Reading

`read_view<T>(cfg, path)` and `read_view<T>(record)` return an `Expected<T>`: a value of the
wrong type, outside its member's range, or a list of the wrong count is a located error naming
the field; a required field missing names it too. The generated `Convert<T>` lets `get<T>` read
a view as well. A view's strings and lists borrow from the tree: hold the Config's `root()`
while they are used, and read after `apply_edits()`.

### Generating

The project file names the contract layers the views come from and the namespace,
`(views (layers core content) (namespace game))`; a field a mod or an overlay adds is readable
from the tree and never a member. `baker_generate_views(target PROJECT file)` in CMake runs
`baker_bake --views` whenever a contract file changes, and a header is rewritten only when its
text changed, so a tuning edit rebuilds nothing. A view's shape set is one per build: a
`#shape` inside an `#ifdef`, or a `dev_only` or `(when ...)` contract layer, is refused (see the
cookbook's [Views in the build](cookbook.md#views-in-the-build)).

### Drift

Each struct carries a digest of its shape's layout (names, types and order, never tuning
metadata). `baker::shape::check_views` compares the compiled views with the shapes a running
stack declares, and the kit's `check` reports a mismatch as a shape warning, or an error under
`--baker-strict-views`. What a softer pack may not change in a view is in the language guide's
[Views and tables](language.md#views-and-tables).

## Tables: a keyed shape's records, packed

A viewed `(@keyed)` shape's records can be read as one packed array of its struct, built from
the merged tree once per change instead of walked per read:

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

### One block

The rows, in the merged tree's order and grouped by parent, then a pool the rows' strings,
lists and curves point into, each distinct string stored once however many rows hold it,
then the indices: by key (an alias of a renamed record resolves too) and by parent, and, for
a view marked `(@view by_id)`, the records' `(@id ...)` and an index of them. A `Table<T>`
handle keeps the block it was given: a reload that rebuilds the table takes nothing from a
reader holding the old one. The block comes from the Config's allocator.

### Finding a row

`index_of(key)` for a shape whose `(@at ...)` has no `*`; `index_of("kestrel.jab")` or a `Path`
for the tags the `*` segments stand for and then the key; `index_of(id)`; `children_of(parent)`
is a contiguous range. `key_of(i)` and `id_of(i)` read back. The text form parses and interns
on every call: a read that runs every frame holds the `Path`, the `Symbol` or the `RecordId`,
or better, the index. An index is stable across a reload while the table's set of records is
unchanged; a record added, removed or reordered renumbers what follows it.

A table keeps the records' IDs only when its shape asks for lookup by ID, so a table that
never looks a row up by its `(@id ...)` does not carry the IDs:

```dta
#shape (move (@at fighters * moves) (@keyed) (@view by_id) ...)
```

Without `by_id`, `index_of(id)` finds nothing and `id_of(i)` is empty, whatever IDs the
records have; the tree still holds them, for `find_by_id` and the tools. `by_id` is part of
the view's digest, and is refused on a shape that is not `(@keyed)`.

### Rebuilt by identity

Every merge, reload and `apply_edits()` rebuilds a table whose governing arrays the new tree
does not share, and no other; a Config with no table registered builds none. Registering after
the load builds at once.

### Literals only

A view field holding a `{command}`, a `$variable` or a value of the wrong type reads as the
member's default, with a located error to the warning handler; the record's other fields still
read, and the kit's `check` fails. A field marked `(@view no)` is never in the table.

## The stack image

The kit's `bake --baker-stack game.datb`, run on the game's own binary, writes the contract
layers' trees and every registered table as one file; a Config made with
`ConfigOptions::stack_image` takes those layers from it in `open_project`, in place of their
text, and the tables with them, their pointers fixed up and their symbols interned in one pass.
A table the game no longer compiles alike (another digest, size or fix-up list) is skipped with
a warning and built from the tree. Layers above the contract (mods, overlays) push as today and
rebuild the tables they touch. Development builds never need the file. An image carries
the version of its format, and a Baker that reads another refuses it by name: bake it again
with the game built on the new Baker. A table's section is its block as built, with
each pointer written as an offset into the block and each symbol by name; the loader finds
those slots through the view's `fixups()` and fixes them up in one pass over the rows.
