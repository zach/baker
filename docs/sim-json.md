# Scenarios, `sim` and `compare`: schemas `baker.sim/1.1` and `baker.compare/1`

A scenario runs your game headless for a set number of seconds, then judges the run by its
objectives. The host kit's `sim` command runs a project's scenarios and writes the results.
`compare` reads two sets of results and reports which objectives moved. A tool such as an
editor's test view, CI or an agent can find a game's scenarios, run one by name with a seed,
and read a located result, without knowing anything about the game.

## The project names them

```dta
(project
   (layers (default "data/default.dta"))
   (tool "build/bin/my_game")            ; the binary that answers the commands
   (scenarios "sim")                     ; a .dta file, or a directory of them
   (objectives "metrics"))               ; the same
```

Each path is relative to the project file. A path that does not exist reads nothing. A
directory is read file by file, in name order. An entry may name several files,
`(scenarios "sim/a.dta" "sim/b.dta")`. A game whose files are in an archive or a virtual
file system lists its own directories with `CommandHooks::list`, and reads them with
`CommandHooks::reader`.

## Scenarios and objectives

```dta
(objective cube.turns (desc "The cube turns at least twice")
   (measure {cube.turns})                ; a command that gives a number: $value
   (pass {>= $value 2}))                 ; nonzero passes
(objective duel.fair (over median)       ; judged once, on the median over the seeds run
   (measure {duel.p1_wins}) (pass {<= $value 0.6}))

(scenario turn (desc "Turn for twenty seconds")
   (seconds 20)
   (start {cube.reset})                  ; before the first step
   (at 5 {cube.push 1} {cube.push 2})    ; when 5 seconds have been stepped
   (objectives cube.turns))              ; leave it out to judge by every objective
```

A run resets your game with the seed (`CommandHooks::reset`; `$seed` in script) and runs the
start commands. It then steps the game at a fixed rate (`CommandHooks::step`, 60 a second by
default) for the scenario's seconds. Each step ticks the Context's scheduler and runs each
`(at ...)` when its time comes. Last, it evaluates each objective's measure, then its pass
with `$value` set. Steps are counted, never timed, so a run depends only on the data, the
scenario and the seed.

An objective `(over median)` does not fail a run. It is judged once per scenario, by its
pass on the median of its values over the seeds run.

`check` reads the scenarios and objectives with the data, so a call to an undefined
function or a wrong argument count in one is a check error.

## The commands

```
my_game sim [--scenario NAME]... [--seed N | --seeds A-B] [--step-hz N] [--json] [--results DIR]
my_game compare --before DIR --after DIR
```

- `--scenario` picks a scenario by its name, or by its file's name without `.dta`.
- `sim` prints a line per run, or, with `--json`, the `baker.sim/1.1` document.
- `--results DIR` also writes `results.json` (`baker.sim/1.1`) and `results.dta`, which is
  what `compare` reads.
- `compare` writes `baker.compare/1`.
- Both exit 0 when everything passed (`sim`) or nothing broke (`compare`), 1 when something
  failed or broke, and 2 when they could not run.

## A run your own loop advances

`sim` steps your game through `CommandHooks::step`, many steps inside one call. An engine
that owns its main loop advances its physics only inside that loop, a frame at a time, so
its game cannot step on demand. Such a game drives a `baker::host::SimRun` from its frames
instead, and the kit does the rest: it reads the scenarios, orders the runs, runs the
commands when they are due, judges the objectives and writes the same output, with the same
exit status, as `sim` through `run_command`.

```cpp
baker::host::SimRun run(line, hooks);           // line.command is "sim"
// once a frame, on the thread that owns run.context():
switch (run.wants()) {
case baker::host::SimRun::Wants::Reset:         // reset your world with run.seed(),
    if (world_ready()) run.ready();             // over as many frames as it takes
    break;
case baker::host::SimRun::Wants::Steps:         // your game took one fixed step of
    run.step();                                 // run.step_seconds()
    break;
case baker::host::SimRun::Wants::Done:          // run.status() is the exit status, and
    quit(run.status());                         // the output went where run_command sends it
}
```

- The run counts steps, never time. Set your game's fixed rate to `step_seconds()`, which
  is `--step-hz` in whole nanoseconds, and report one `step()` per fixed step.
- `SimRun` sets `$seed`, then waits. It never calls `CommandHooks::reset` or `step`, and
  frames spent waiting for a reset count no steps.
- `stop()` ends early (a window closed, a timeout). The run in progress is kept as not
  finished and not judged, runs not started are left out, the output is written so far, and
  the status is 2.
- A step with nothing due allocates nothing.

## `baker.sim/1.1`

```json
{
  "schema": "baker.sim/1.1",
  "status": "passed",
  "runs": [
    {"scenario": "walk", "file": "sim/walk.dta", "line": 2, "seed": 0, "steps": 120,
     "finished": true, "passed": true, "errors": [], "objectives": [
       {"name": "scored", "file": "metrics/objectives.dta", "line": 4, "value": 0,
        "pass": false, "over": "median", "error": null},
       {"name": "steps", "file": "metrics/objectives.dta", "line": 2, "value": 120,
        "pass": true, "over": "seed", "error": null}]}
  ],
  "medians": [
    {"scenario": "walk", "objective": "scored", "file": "metrics/objectives.dta", "line": 4,
     "value": 10, "seeds": 2, "pass": true, "error": null}
  ],
  "summary": [
    {"objective": "steps", "file": "metrics/objectives.dta", "line": 2,
     "tally": {"passed": 2, "failed": 0, "min": 120, "median": 120, "max": 120}}
  ]
}
```

- `status` is `"passed"` or `"failed"`, as the exit code is 0 or 1, or `"stopped"` when a
  `SimRun` was stopped before its last run ended (exit 2).
- `finished` is false for a run that was stopped. It has the steps it took, no objectives,
  and does not pass. (New in 1.1.)
- `runs` are sorted by scenario, then seed. Each objective within a run is sorted by name.
  A run's `file` and `line` say where its scenario is declared. An objective's `file` and
  `line` say where the objective is declared.
- A run's `errors` are commands that failed, and objectives the scenario names that nothing
  declares. Each is a problem as check writes one: `code`, `file`, `line`, `column`, `path`,
  `message`.
- An objective whose measure or pass gives no number has `"value": null`, fails, and carries
  the problem in `error`.
- `over` is `"seed"` (judged in each run) or `"median"`. A `"median"` objective's `pass` in a
  run is for information. Its entry in `medians` decides.
- `summary` tallies each objective over every run: the passes, the failures, and the
  smallest, median and largest value.

## `baker.compare/1`

```json
{
  "schema": "baker.compare/1",
  "broke": 1,
  "fixed": 0,
  "objectives": [
    {"name": "steps", "file": "metrics/objectives.dta", "line": 2, "moved": "broke",
     "before": {"passed": 2, "failed": 0, "min": 120, "median": 120, "max": 120},
     "after": {"passed": 0, "failed": 2, "min": 120, "median": 120, "max": 120},
     "median_change": 0,
     "changed": [{"scenario": "walk", "seed": 0, "before": true, "after": false}]}
  ]
}
```

- `moved` is one of these:
  - `"broke"`: some run, or the median, passed before and fails now;
  - `"fixed"`: the reverse, with nothing broken;
  - `"same"`;
  - `"new"`: only in the after results;
  - `"gone"`: only in the before results.
- `changed` lists each scenario and seed whose pass moved. For an objective judged by its
  median, it lists each scenario whose median verdict moved, with `"over": "median"` in
  place of the seed.
- `broke` and `fixed` count objectives. The exit code is 1 when `broke` is above 0.

## Stability

Versioning follows `check`'s (`check-json.md`): a field is added only with a new minor
version, and removed, renamed or changed only with a new major version.
