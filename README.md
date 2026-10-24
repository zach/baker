# Baker

Baker is a focused, MIT-licensed C++20 library for data and scripting, in the spirit of
Harmonix's DataArray (Eric Malafeew, *Data-Driven Programming Made Easy*, GDC 2005). It
reads a Lisp-like text format into trees, stacks files into layers that reload while the
program runs, and evaluates commands written in the same format.

Its first promise: turning a hardcoded constant into a configured, hot-reloadable,
designer-editable value costs one line.

```cpp
// Before:
const float kBoxWidth = 2.0f;

// After: read from data, reloaded when the file changes, tunable live and saved back.
static baker::Tweak<float> kBoxWidth{"box.width", 2.0f, {.range = {0, 10}, .desc = "Box width"}};

float w = kBoxWidth;  // one relaxed atomic load
```

## Five minutes to a working tweak

You need CMake 3.20, Ninja or Make, and a C++20 compiler (GCC 13, Clang 18, or MSVC 2022).

**1. Build Baker.** This builds the library, its tests, and the examples:

```sh
git clone https://github.com/zach/baker.git && cd baker
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**2. Write the data.** `examples/quickstart/default.dta` holds one value, with the metadata
a tuning panel would show. `;` starts a comment; an array is `(tag values...)`, and a path
like `player.speed` walks tags from the top.

<!-- file: examples/quickstart/default.dta -->
```dta
; The values the game reads, by path: player.speed is 4.5 here.
(player
   (speed 4.5 (range 0 10) (desc "Run speed, in m/s")))
```

**3. Replace the constant.** `examples/quickstart/main.cpp` is the whole program. A
`Tweak` registers itself when the program starts; the `Config` binds every tweak to the
value at its path, and binds them again on each reload. A missing path is not an error:
the default stands.

<!-- file: examples/quickstart/main.cpp -->
```cpp
// The five-minute path: a constant becomes a tweak, read from default.dta and reloaded.
#include <baker/config.hpp>

#include <cstdio>
#include <iostream>
#include <string>

// Before: const float kPlayerSpeed = 4.0f;
static baker::Tweak<float> kPlayerSpeed{"player.speed", 4.0f};

int main() {
    baker::Config cfg;  // binds every Tweak in the program
    if (auto r = cfg.push_layer("default", "default.dta"); !r) {
        std::printf("%s\n", baker::to_string(r.error()).data());
        return 1;
    }
    std::printf("player speed %g\n", double(kPlayerSpeed));

    // Edit default.dta, save, and press Enter. A game calls reload() once a frame instead.
    for (std::string line; std::getline(std::cin, line);) {
        if (auto r = cfg.reload(); !r) std::printf("%s\n", baker::to_string(r.error()).data());
        std::printf("player speed %g\n", double(kPlayerSpeed));
    }
    return 0;
}
```

Every call that can fail returns `Expected<T>`, which holds the value or a located
`Error`; Baker never throws.

**4. Run it, then change the data.**

```sh
cd examples/quickstart
../../build/examples/baker_example_quickstart
```

It prints `player speed 4.5`. Change `4.5` to `6` in `default.dta`, save, and press
Enter: it prints `player speed 6`. Break the file (delete a parenthesis) and press Enter:
the reload reports the file and line, and the old value stays. Your program never
restarted.

**5. Use it in your own project.**

```cmake
include(FetchContent)
FetchContent_Declare(baker GIT_REPOSITORY https://github.com/zach/baker.git GIT_TAG v0.6.0)
FetchContent_MakeAvailable(baker)
target_link_libraries(game PRIVATE baker::baker)
```

Call `cfg.reload()` once a frame, or when a file watcher fires. That is the loop: edit,
save, see it.

## What else is there

- **Lookup.** `find_float("price")`, `get<int>("menu.eggs.price")`, and `Convert<T>` for
  your own types; every failure says what, where, and how it got there:
  `baker: 'price' is float, not int  (menu.dta:3, at menu/eggs)`.
  [Language guide](docs/language.md#data).
- **Layers and write-back.** Stack `default`, `platform`, a per-user `local` layer, and
  live-ops overlays; ask any value where it came from (`source()`); tune it live and write
  it back to its file as a one-line diff; flip experiment layers A/B; snapshot and restore.
  [Cookbook](docs/cookbook.md), [`examples/config`](examples/config).
- **Live ops.** Overlays pushed into a shipped build, frozen except for what
  `live_layers` allows, rolled back with one call. [Cookbook: live ops](docs/cookbook.md#live-ops),
  [`examples/live-ops`](examples/live-ops).
- **Script.** Commands like `{* $x 2}` evaluate lazily, so `if` is an ordinary function.
  Native functions bind in one line with types deduced; `{func}` defines script ones;
  `check` finds bad calls before anything runs; `eval` serves consoles.
  [Language guide](docs/language.md#script), [reference](docs/script-reference.md),
  [`examples/scripting`](examples/scripting), [`examples/remote-console`](examples/remote-console).
- **Objects and messages.** `Object<T>` makes a C++ type callable from script, with
  methods, properties, and script handlers attached by `{class}`; typed messages go C++ to
  C++ without building a node. [`examples/objects`](examples/objects).
- **Tasks.** C++20 coroutines (`co_await delay(t)`) whose frames come from a pool, and
  script tasks (`delay_task`, `interp_task`, `thread_task`); the host calls `tick(dt)`.
  [`examples/tasks`](examples/tasks).
- **The binary cache.** A tree written as it lies in memory and loaded in one read; a
  Config with `cache_dir` keeps one per layer, and `baker_bake` makes them in a build step.
  [Cookbook](docs/cookbook.md#the-binary-cache-in-a-build-step).
- **Packs and levels.** `#require` reads a shared file once per session; a pack declares
  its level (`engine`, `game`, `task`), and a harder pack using a softer one's macro or
  function is warned about. [Language guide](docs/language.md#packs-and-levels).
- **Shapes.** `(shapes (boss_phase (enter_at float (range 0 1)) ...))` declares what data
  should look like; a Config validator reports violations at load, located.
  [Language guide](docs/language.md#shapes).
- **The host kit.** `baker::host` gives a game the loop every host writes the same way:
  it watches the stack's files and reloads on a save, serves a remote console on loopback,
  and runs `check`, `reference`, `eval`, `diff` and `promote` from the game's own binary.
  [Baker and your tools](docs/tools.md#the-host-kit), [`examples/host`](examples/host).
- **A tuning harness.** Dear ImGui panels generated from the tweaks and shapes, an `eval`
  console, provenance on hover, write-back, and reload telemetry.
  [`examples/harness-imgui`](examples/harness-imgui).
- **Harness instrumentation.** Edit-to-see time per file and per layer (`loop_stats`), a
  journal of every live edit and whether it was kept, pack owners and stages, `origin` for
  generated files, a repetition report, and `check` output as versioned JSON.
  [Cookbook](docs/cookbook.md#reload-callbacks-migrate-or-punt),
  [language guide](docs/language.md#checking-before-running).

Public headers include no standard containers or strings (an engine that bans the STL can
use them); text comes out through a `TextSink`. `<baker/std_interop.hpp>`,
`<baker/std_script.hpp>` and `<baker/tools/std_tools.hpp>` add `std::string` forms for
hosts that want them.

## Measured

Release builds, GCC 13 on the development container (Xeon, 2.8 GHz); the benchmarks are
in `bench/` (`-DBAKER_BUILD_BENCH=ON`).

| | | |
|---|---:|---|
| Reading a `Tweak<float>` | one load | checked in the generated assembly |
| `find_float` on a 20-child array, warm | ~9 ns | no allocation |
| A native call, `{add 1 2}` | ~31 ns | no allocation |
| A typed message, `bob.handle(HitMsg{1})` | ~9 ns | no allocation |
| `tick` with 10,000 tasks asleep | 5–7 ns | |
| Parsing text | 121–131 MB/s | |
| A 6.2 MB tree from the binary cache | 2.3–2.5 ms | one allocation |
| An 18 MB layer from text into a Config | 45 ms | |
| Checking 50,000 shaped instances | 6.5 ms | |

## Building

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Targets: `baker::data` (data, lookup, config, tweaks, the cache; usable alone),
`baker::script` (evaluation, objects, tasks), `baker::shape` (shapes), and `baker::baker`
(all of them). `baker::tools` holds what only tools call (the reference, the layer tools,
the console's commands, checking without the game), and `baker::host`, the host kit, is a
separate library over them: the file watcher, the remote console, the edit-to-see loop,
and the tool commands a game runs from its `main`
([Baker and your tools](docs/tools.md#the-host-kit)).

| Option | Default | Effect |
|--------|---------|--------|
| `BAKER_BUILD_SCRIPT` | `ON` | Build `baker::script` |
| `BAKER_BUILD_SHAPE` | `ON` | Build `baker::shape` |
| `BAKER_BUILD_HOST` | `ON` with `BAKER_BUILD_SCRIPT` | Build `baker::host`, the host kit |
| `BAKER_BUILD_TESTS` | `ON` when top-level | The test suite |
| `BAKER_BUILD_EXAMPLES` | `ON` when top-level | `examples/`, each run by CTest |
| `BAKER_BUILD_HARNESS` | `OFF` | `examples/harness-imgui`; fetches Dear ImGui and GLFW |
| `BAKER_BUILD_TOOLS` | `ON` when top-level | `baker_parse_corpus`, `baker_bake` |
| `BAKER_BUILD_BENCH` | `OFF` | `bench/` |
| `BAKER_BUILD_FUZZERS` | `OFF` | libFuzzer targets (Clang) |
| `BAKER_ATOMIC_REFCOUNT` | `ON` | `OFF` for single-threaded programs |
| `BAKER_SHIPPING` | `OFF` | Reload and live edits inert; `dev_only` tweaks keep defaults |
| `BAKER_USE_EASTL` | `OFF` | Internal containers from EASTL (fetched) |
| `BAKER_SANITIZE` | empty | e.g. `address,undefined` |

Android: CMake builds get C++20 from the targets; with `ndk-build`, add
`APP_CPPFLAGS += -std=c++20`.

## Documentation

- [`docs/language.md`](docs/language.md): the language, from data to script.
- [`docs/cookbook.md`](docs/cookbook.md): patterns, live ops among them.
- [`docs/tools.md`](docs/tools.md): what Baker gives tools, and what it leaves to your game.
- [`docs/script-reference.md`](docs/script-reference.md): every built-in, generated.
- [`docs/reply-json.md`](docs/reply-json.md): one-line JSON replies for tools and consoles.
- [`docs/reference-json.md`](docs/reference-json.md): the vocabulary, tweaks and shapes as JSON.
- [`docs/sim-json.md`](docs/sim-json.md): scenarios, objectives, and their results as JSON.
- [`CHANGELOG.md`](CHANGELOG.md): what each release changed.

## License

MIT. See `LICENSE` and `THIRD_PARTY.md`.
