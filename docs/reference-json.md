# The vocabulary as JSON: `baker.reference/1.1`, `baker.tweaks/1.1`, `baker.shapes/1.1`

Three documents describe what a program's data and script may say, for editors, agents,
and other tools that should not scrape the Markdown reference:

| Document | Written by | Library |
|---|---|---|
| `baker.reference/1.1` | `baker::write_reference_json(ctx, sink, &session)` | `baker::script` |
| `baker.tweaks/1.1` | `baker::write_json(registry, sink)` | `baker::data` |
| `baker.shapes/1.1` | `baker::shape::write_json(shapes, sink)` | `baker::shape` |

They are separate so that a data-only program can write its tweaks and the script library
does not depend on shapes. A tool that wants all three can wrap them in one object.

```cpp
std::string json;
baker::write_reference_json(ctx, baker::string_out(json), &cfg.read_session());
```

## Stability

They follow `check`'s rules (`check-json.md`):

- **Determinism.** Every list is sorted by name or path, so the same program gives the
  same bytes.
- **Formatting.** UTF-8, one entry per line, and a newline at the end. Strings escape
  `"`, `\` and control characters.
- **Versioning.** A field is only added with a new minor version. It is removed, renamed
  or changed only with a new major version.
- **Numbers.** Ranges and steps are written as Baker prints floats (`0.5`, `10.0`).

## `baker.reference/1.1`

| Field | Contents |
|---|---|
| `schema` | `"baker.reference/1.1"` |
| `functions` | every function with at least one overload |
| `types` | every native type made known to the Context |
| `script_classes` | classes only script defined (`{class Name ...}` with no native type) |
| `globals` | documented globals: `name`, `doc` |
| `objects` | the objects added to the Context under a name (`ctx.add(object, "clock")`, `{new Class name}`) when it is written: `name`, and `type` as `types` or `script_classes` names it (1.1) |
| `packs` | given a session: as in `check`'s JSON (`name`, `level`, `file`, `requires`, `owners`, `for`, `stage`) |
| `origins` | given a session: `tool`, `source`, `file` for each file a tool generated |

**A function** has these fields:
- `name`.
- `tier`: `"native"` or `"script"`.
- `status`: `"normal"`, `"stub"`, `"provisional"` or `"deprecated"`.
- `replacement`: a deprecated function's replacement.
- `doc`.
- `file` and `line`: where a script function is defined; `""` and `0` for a native one.
- `overloads`. Each overload has:
  - `params`, each with `type`, `kind` (`"value"`, `"raw"`, `"optional"` or `"rest"`) and
    `raw` (passed unevaluated);
  - `result`;
  - `min_args`;
  - `max_args`, which is `null` when the overload takes the rest of its arguments.

**Checking without the game.** A tool that has the game's reference but not its code (an
editor, a CI job) can register each function from its overloads with
`Context::declare(name, overloads)`, then run `check`.
- Each `OverloadDescription` holds its `params` and `result`, and each `ParamDescription`
  holds its `type`, `kind` and `raw`, written as the reference writes them.
- `check` then counts arguments and checks literal argument types as it would against the
  game's own functions. A stub stays a stub when its status is set on the returned `FuncRef`.
- A declared function has no body: calling it fails.
- The types `declare` can check are Baker's own (`int` and the other integer names, `float`,
  `float32`, `bool`, `symbol`, `string`, `name`, `array`, `value`, `unevaluated`,
  `$variable`, `values...`) and any type that a native function or method of the same
  Context takes. Every function Baker itself binds, the config tools included, can be
  declared from its own reference.
  Any other type name is an error naming the function. The tool decides what to do then,
  for example to declare that parameter as `value`.
- **The data's own vocabulary** comes from the load, not the reference:
  `session.macro_definitions()` lists every macro the session's reads defined, with its
  file and line, and `script_functions(session.autorun())` lists every `{func}` the load's
  `#autorun` commands define, without running them. For a Config, the macros are on
  `cfg.read_session()`, and the functions come from `script_functions(cfg.autorun().items())`,
  since a Config keeps its enabled file layers' commands apart from its sessions.

**A type** has `name`, `bases`, `methods`, `properties` and `handlers`.
- A method has `name`, `status`, `replacement`, `doc`, `message` (the typed message it
  answers, or `""`) and `overloads`.
- A property has `name`, `type` and `doc`.
- `handlers` names the script handlers that `{class}` or `{extend}` attached.

## `baker.tweaks/1.1`

`tweaks` lists every tweak in the registry, by path. Each one has:

| Field | Contents |
|---|---|
| `path` | as the tweak was declared: `player.speed` |
| `type` | the value's type as errors name it: `"float32"`, `"int32"`, `"bool"`... |
| `default` | the code's default, as Baker text |
| `value` | the current value, as Baker text |
| `bound` | whether the data supplies the value |
| `range` | `[min, max]`, or `null` |
| `step`, `desc`, `kind`, `group` | metadata; the data's wins over the code's |
| `dev_only` | the code's flag: a shipping build keeps the default |
| `one_of` | the symbols the value may be, in order: the data's `(one_of ...)`, else the code's; `[]`: any (added in 1.1) |
| `source` | while bound: `{layer, file, line}`; otherwise `null` |

## `baker.shapes/1.1`

`shapes` lists every shape in the set, by name. Each one has:

- `name`;
- `at`, the ancestors' tags (`*` for any);
- `keyed`;
- `file` and `line`;
- `fields`.

Each field has these entries:

| Field | Contents |
|---|---|
| `name` | the field's tag |
| `type` | `"int"`, `"float"`, `"bool"`, `"symbol"`, `"string"`, `"list"`, `"shape"` or `"any"` |
| `element` | a list's element type; `""` otherwise |
| `shape` | the shape a field, or a list's elements, must match; `""` otherwise |
| `optional` | whether it may be absent |
| `range` | `[min, max]`, or `null` |
| `step`, `kind`, `desc`, `note` | as declared |
| `one_of` | the symbols a symbol field's value may be, in the order written; `[]`: any (added in 1.1) |
| `file`, `line` | where the field is declared |
