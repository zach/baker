# The vocabulary as JSON: `baker.reference/1.2`, `baker.tweaks/1.3`, `baker.shapes/1.1`

Three documents describe what a program's data and script may say, for editors, agents,
and other tools that should not scrape the Markdown reference:

| Document | Written by | Library |
|---|---|---|
| `baker.reference/1.2` | `baker::write_reference_json(ctx, sink, &session)` | `baker::script` |
| `baker.tweaks/1.3` | `baker::write_json(registry, sink)` | `baker::data` |
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

## `baker.reference/1.2`

| Field | Contents |
|---|---|
| `schema` | `"baker.reference/1.2"` |
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
  - `params`, each with `type`, `kind` (`"value"`, `"raw"`, `"optional"` or `"rest"`),
    `raw` (passed unevaluated) and `values`: Baker's type of each value the parameter reads,
    `["int"]` for an `int`, `["float32", "float32"]` for a `vec2` whose `Convert` declares
    `values`, `null` when the type does not say (added in 1.2);
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
- **A type of the game's own** as a parameter, such as a `vec2`, is checked by its
  `values`: give them in the `ParamDescription`, and `check` holds a literal argument to
  them as the game's converter reads it, so `{aim 1}` where a `vec2` is wanted is an error.
- **A function with a body.** A host whose functions come from a scripting language of its
  own, with no C++ signature, registers each with `Context::declare(name, overloads, body)`:
  `check` and the reference see the described parameters, and a call runs `body`. When no
  parameter is raw, each argument is evaluated once and held to its parameter's type
  first, as a native's is.
- **Types and objects.** `Context::declare_type(name, bases, methods)` declares a type from
  its entry in `types`: its bases (each a type the Context already knows, so declare bases
  first) and each method as a `MethodDescription`, a name and its overloads.
  `Context::declare_object(name, type)` declares an entry of `objects`. `check` then holds
  `{tank fire 3}` to the type's methods and its bases' as it would for the game: a wrong
  argument count or literal type is an error, and a method the type does not have is in
  `demand`. Neither runs: a call on a declared object fails as a call on an undefined name
  does. A description it cannot hold (an unknown base or type, a parameter type with no
  `values`, a name already taken) is an error naming the type, method or object.
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

## `baker.tweaks/1.3`

`tweaks` lists every tweak in the registry, by path. Each one has:

| Field | Contents |
|---|---|
| `path` | as the tweak was declared: `player.speed` |
| `type` | the value's type as errors name it: `"float32"`, `"int32"`, `"bool"`, or your own (`"vec2"`) |
| `values` | Baker's type of each value: `["float32"]`, or `["float32", "float32"]` for a `vec2` whose `Convert` declares `values`; `null` when the type does not say (added in 1.2) |
| `default` | the code's default, as Baker text |
| `value` | the current value, as Baker text |
| `bound` | whether the data supplies the value |
| `range` | `[min, max]`, or `null` |
| `step`, `desc`, `kind`, `group` | metadata; the data's wins over the code's |
| `dev_only` | the code's flag: a shipping build keeps the default |
| `one_of` | the symbols, or a string tweak's strings, the value may be, in order: the data's `(one_of ...)`, else the code's; `[]`: any (added in 1.1; strings in 1.3) |
| `choices` | the integers an integer tweak may be, each `{"value", "name"}`: the data's `(one_of (0 easy) (1 normal))`, else the code's `TweakMeta::choices`; `[]`: any (added in 1.3) |
| `source` | while bound: `{layer, file, line}`; otherwise `null` |

### Checking data against tweaks without the game

A tool that has the game's reference but not its code (a language server, a CI checker,
an agent's pre-flight check) can hold data to the game's tweaks as `check` does. It
registers each tweak from its description, then runs `check_tweaks`:

```cpp
baker::TweakRegistry registry;
baker::DescribedTweaks tweaks(registry);
for (const auto& t : reference_tweaks)   // read from the JSON by the tool
    if (auto added = tweaks.add({.path = t.path, .type = t.type, .values = t.values,
                                 .default_text = t.default_text, .range = t.range,
                                 .one_of = t.one_of});
        !added)
        report(added.error());           // names the tweak
const baker::TweakFindings found = baker::check_tweaks(*merged_tree, registry);
```

`add` is the same call for every type, Baker's or the game's. A type of the game's own needs
its `values`, which the game gives by declaring them in its converter:

```cpp
template <> struct baker::Convert<Vec2> {
    static constexpr std::string_view name = "vec2";
    static constexpr std::string_view values[] = {"float32", "float32"};
    // from, to ...
};
```

A described tweak reads each value as the game's converter would with `Values::read`, so
`(aim 1)` where a `vec2` is wanted fails in the tool as it fails in the game. A description
it cannot hold is an error naming the tweak: a type of the game's own with no `values`, a
value type that is not one of Baker's, or a default that does not read as its types. A type
whose count of values is the data's (a `std::vector`) has no `values` and cannot be
described yet. A fixed-size list of one of Baker's single-value types (`std::array<float, 3>`)
declares its own: three `float32`s.

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
