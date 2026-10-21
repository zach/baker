# `check`'s JSON output: schema `baker.check/1`

`baker::write_json(report, sink, &session)` (or `to_json(report, &session)` with
`<baker/std_script.hpp>`) writes a `CheckReport` as one JSON object, so a studio can keep it
as a build artifact and diff it from one milestone to the next. The text form
(`write(report, sink)`) holds the same data for people.

```cpp
baker::CheckReport report;
const baker::Array* trees[] = {game.get(), level.get()};
baker::check(trees, ctx, report, {.packs = &cfg.read_session()});
std::ofstream("check.json") << baker::to_json(report, &cfg.read_session());
```

`tests/script/check_expected.json` is a complete example, checked byte for byte by the test
suite.

**A gate.** A host that gates edits on `check` (an agent's proposal, a pre-commit hook, CI)
reads `passed` and shows the lists:

- `check(cfg, ctx, report, {.unresolved_fails = true}, extra)` checks a Config as it loads.
  That covers the merged tree and the `#autorun` commands of its layers, which are not in
  the tree, with any extra trees beside them. A call to a function nothing defines then
  fails the check, unless the function is declared a stub.
- The host adds what it found to `report.load`, `report.shape_errors` and
  `report.shape_warnings` before writing the JSON: what its load reported, and the shape
  check's errors and warnings.

## Stability

- **The same data gives the same bytes.** Every list is sorted by its data alone (below);
  nothing depends on memory addresses, hash order, or time.
- **Files are named as the loads named them.** A path given relative to the working
  directory stays relative; Baker adds no absolute paths.
- **Formatting:** UTF-8, two-space indentation, one list entry per line, a newline at the
  end. Strings escape `"`, `\`, and control characters (`\n`, `\t`, `\u00XX`); other
  characters are written as they are. Numbers are integers.
- **Versioning.** `schema` is `baker.check/MAJOR` while the minor version is 0, then
  `baker.check/MAJOR.MINOR` (now `baker.check/1.3`). A field is only ever added with a new minor version, which old
  readers can ignore; a field is removed or renamed, or its meaning changed, only with a new
  major version.

## The object

| Field | Type | Contents |
|---|---|---|
| `schema` | string | `"baker.check/1.3"` |
| `passed` | boolean | `CheckReport::ok()`: no errors, no load problems, no shape or tweak errors, and, when unresolved calls fail, no unresolved demand (1.1; tweak errors 1.3) |
| `errors` | list of problems | calls with the wrong arguments for every overload |
| `warnings` | list of problems | the one-way rule: a harder pack calling a softer one's function |
| `demand` | list of demands | functions and methods called but not defined, or stubs |
| `repeats` | list of repeats | subtrees repeated across the trees of one `check` call |
| `findings` | list of problems | informational: packs that declare one owner |
| `load` | list of problems | what loading the data reported: a file that did not parse, an `#autorun` command that failed. Filled by the host (1.1) |
| `shape_errors` | list of problems | the data's shape check: values out of range or of the wrong type. Filled by the host (1.1) |
| `shape_warnings` | list of problems | fields no shape declares; never a failure. Filled by the host (1.1) |
| `tweak_errors` | list of problems | the data against the tweaks the Config binds: a value that does not convert to its tweak's type, a number outside its tweak's range, a symbol outside its `one_of`. Filled by `check` of a Config (1.3) |
| `tweak_warnings` | list of problems | a tweak whose path the data lacks, so it keeps its default; a key beside a group's tweaks that no tweak reads, as a misspelled key looks. Never a failure (1.3) |

Each tweak finding's `path` is its subject: the tweak's path, or for a key no tweak reads, the key's own path (`world.gravty`, the group then the key). A tool groups and locates findings by it rather than by their words.
| `packs` | list of packs | the session's packs and their harness metadata; empty without a session |

**A problem** (in every list of problems), sorted by `file`, then `line`, then `column`,
then `message`:

| Field | Type | Contents |
|---|---|---|
| `code` | string | the `ErrorCode`: `syntax`, `range`, `directive`, `io`, `limit`, `type`, `not found`, `refused` |
| `file` | string | where it was found; `""` if unknown |
| `line` | integer | the line; 0 if unknown (a pack's finding names only its file) |
| `column` | integer | the column, 1-based, in characters from the line's start; 0 if unknown. A parse error has one; a call or shape problem, located by its array's line, does not (1.2) |
| `path` | string | the array path that led there, as errors print it; `""` if none |
| `message` | string | what is wrong, as the text form says it |

**A demand**, sorted by `name`, then `on`:

| Field | Type | Contents |
|---|---|---|
| `name` | string | the function or method called |
| `on` | string | for a method, the class of the object it was sent to; else `""` |
| `stub` | boolean | true: the function exists with status `stub`; false: nothing defines it |
| `sites` | list of sites | every call, sorted by `file`, then `line` |

**A repeat**, in the order `find_repeats` gives (first site, then size, largest first, then
text):

| Field | Type | Contents |
|---|---|---|
| `form` | string | one occurrence, as DTA text |
| `nodes` | integer | its size, counted recursively |
| `sites` | list of sites | every place it appears, sorted by `file`, then `line` |

**A site:** `file` (string) and `line` (integer).

**A pack**, sorted by `name`, then `file`:

| Field | Type | Contents |
|---|---|---|
| `name` | string | the pack's name |
| `level` | string | its level among the session's pack levels |
| `file` | string | the file that declares it |
| `requires` | list of strings | `(requires ...)` |
| `owners` | list of strings | `(owners ...)` |
| `for` | list of strings | `(for ...)` |
| `stage` | string | `genesis`, `custom`, `product`, `commodity`, or `""` if not declared |
