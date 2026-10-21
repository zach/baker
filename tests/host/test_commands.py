#!/usr/bin/env python3
"""Tests the host kit's commands through a game binary, on a copy of a small stack.

    python3 -I tests/host/test_commands.py build/tests/baker_host_test_game tests/host/stack [shipping]

CTest runs it. Each case works on a fresh copy, so the stack's own files are never touched.
Exits 1 on a failure.
"""
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FAILURES = []


def expect(cond, what):
    if not cond:
        FAILURES.append(what)
        print(f"FAIL: {what}")


class Stack:
    def __init__(self, game, source):
        self.game = str(Path(game).resolve())
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name) / "stack"
        shutil.copytree(source, self.dir)

    def run(self, *args, agent=None):
        if agent is not None:
            (self.dir / "data" / "agent.dta").write_text(agent)
        p = subprocess.run([self.game, *args, "--project", "data"], cwd=self.dir,
                           capture_output=True, text=True, timeout=120)
        return p.returncode, p.stdout, p.stderr


def check(s):
    code, out, _ = s.run("check")
    r = json.loads(out)
    expect(code == 0 and r["passed"] and r["schema"] == "baker.check/1.3", "clean data passes")
    code, out, err = s.run("check", agent="(waves {spawnn grunt 3})\n")
    r = json.loads(out)
    expect(code == 1 and [d["name"] for d in r["demand"]] == ["spawnn"] and "spawnn" in err,
           "a call to an undefined function fails the check, and is named")
    code, out, _ = s.run("check", agent="(player (speed 42))\n")
    r = json.loads(out)
    expect(code == 1 and len(r["shape_errors"]) == 1, "a value out of its shape's range fails")
    code, out, _ = s.run("check", agent="(player (speed 3\n")
    r = json.loads(out)
    expect(code == 2 and len(r["load"]) == 1 and r["load"][0]["file"].endswith("agent.dta"),
           "data that does not load is a load problem, exit 2")
    # The game's own files, checked beside its data.
    (s.dir / "scenario.dta").write_text("(steps {spawn grunt})\n")
    code, out, _ = s.run("check", "--extra", "scenario.dta", agent="")
    r = json.loads(out)
    expect(code == 1 and any("takes 2 arguments" in e["message"] for e in r["errors"]),
           "the game's extra trees are checked")
    code, out, _ = s.run("check", "--extra", "missing.dta", agent="")
    r = json.loads(out)
    expect(code == 2 and len(r["load"]) == 1, "an extra tree that does not read: exit 2")
    # local.dta is left out, as the game asked.
    (s.dir / "data" / "local.dta").write_text("(player (speed 99))\n")
    code, out, _ = s.run("check", agent="")
    expect(code == 0, "the local layer is left out (its value would fail the shape)")


def reference(s):
    code, out, _ = s.run("reference", "--json", agent="")
    r = json.loads(out)
    names = [f["name"] for f in r["reference"]["functions"]]
    expect(code == 0 and r["reference"]["schema"] == "baker.reference/1.2" and "spawn" in names,
           "reference --json: the game's functions")
    expect(r["shapes"]["schema"] == "baker.shapes/1.1" and r["tweaks"]["schema"] == "baker.tweaks/1.3",
           "reference --json: tweaks and shapes")
    code, out, _ = s.run("reference")
    expect(code == 0 and "spawn" in out, "reference: Markdown")


def eval_json(s):
    code, out, _ = s.run("eval", "--seed", "4", "--json", "{+ $score 1}", "{nosuch}", agent="")
    lines = [json.loads(l) for l in out.splitlines()]
    expect(code == 1 and len(lines) == 2 and lines[0]["value"] == 41 and lines[0]["id"] == 1,
           "eval --json: on the game reset with the seed; one reply a command")
    expect(not lines[1]["ok"] and lines[1]["id"] == 2, "eval --json: an error is a reply")
    code, out, _ = s.run("eval", "{config.get player.speed}", agent="(player (speed 4.5))\n")
    expect(code == 0 and out == "4.5\n", "eval: the stack with the agent layer")


def diff_and_promote(s):
    code, out, _ = s.run("diff", agent="(player (speed 4.5))\n")
    expect(code == 0 and "player.speed  3.5 -> 4.5" in out, "diff: what the agent layer changes")
    before = (s.dir / "data" / "default.dta").read_text()
    code, out, _ = s.run("promote", "--patch", "change.patch")
    patch = (s.dir / "change.patch").read_text()
    expect(code == 0 and (s.dir / "data" / "default.dta").read_text() == before,
           "promote --patch writes nothing in the stack")
    expect(patch.startswith("--- a/data/default.dta\n+++ b/data/default.dta\n@@ ") and
           "-   (speed 3.5)   ; how fast\n+   (speed 4.5)   ; how fast\n" in patch,
           "promote --patch: one line, its comment kept, named from the project's directory")
    code, out, _ = s.run("promote", "--write")
    expect(code == 0 and "(speed 4.5)   ; how fast" in (s.dir / "data" / "default.dta").read_text(),
           "promote --write writes the line")


def shipping(s):
    """Under BAKER_SHIPPING, the commands that write refuse; the others run."""
    for command in ("diff", "promote"):
        code, _, err = s.run(command)
        expect(code == 2 and "shipping" in err, f"shipping: {command} refuses")


def host_io(s):
    """The stack read through the host's reader; the output where the host or the line says."""
    code, out, _ = s.run("check", "--root", str(s.dir), agent="(player (speed 42))\n")
    r = json.loads(out)
    expect(code == 1 and len(r["shape_errors"]) == 1 and
           r["shape_errors"][0]["file"] == "game://data/agent.dta",
           "check reads the stack through the host's reader, named as the host names it")
    code, out, _ = s.run("eval", "{config.get player.speed}", "--root", str(s.dir),
                         agent="(player (speed 4.5))\n")
    expect(code == 0 and out == "4.5\n", "eval reads through the host's reader")
    if sys.argv[3:] != ["shipping"]:
        code, _, _ = s.run("promote", "--patch", "p.diff", "--root", str(s.dir))
        patch = (s.dir / "p.diff").read_text()
        expect(code == 0 and "-   (speed 3.5)   ; how fast\n+   (speed 4.5)   ; how fast\n" in patch,
               "promote --patch compares against what the reader reads")

    code, out, _ = s.run("check", "--output", "report.json", agent="")
    expect(code == 0 and out == "" and
           json.loads((s.dir / "report.json").read_text())["passed"],
           "--output FILE: the output in the file, nothing on stdout")
    code, out, err = s.run("check", "--output", "no/such/dir/report.json")
    expect(code == 2 and out == "" and "cannot write" in err,
           "--output to a file that cannot be written: exit 2")
    code, out, err = s.run("eval", "{+ 1 2}", "--to-stderr")
    expect(code == 0 and out == "" and err == "3\n", "the host routes the output")


def scenarios(s):
    if sys.argv[3:] == ["shipping"]:
        return
    code, out, err = s.run("sim", "--seed", "1")
    expect(code == 0 and "walk seed 1: passed" in out, "sim runs the project's scenarios")
    code, out, _ = s.run("sim", "--seeds", "0-2", "--json")
    r = json.loads(out)
    expect(code == 0 and r["schema"] == "baker.sim/1.1" and len(r["runs"]) == 3 and
           r["runs"][0]["file"].endswith("walk.dta") and r["runs"][0]["line"] == 2,
           "sim --json: every run, located")
    # The median of seeds 0-2 is 10: it passes, though seed 0 alone does not.
    code, out, _ = s.run("sim", "--seed", "0", "--json")
    expect(code == 1 and json.loads(out)["status"] == "failed",
           "an objective that fails its median: exit 1")
    code, _, err = s.run("sim", "--scenario", "nosuch")
    expect(code == 2 and "no scenario" in err, "a scenario that is not there: exit 2")
    code, out, _ = s.run("sim", "--scenario", "walk", "--step-hz", "30", "--json")
    expect(code == 1 and json.loads(out)["runs"][0]["steps"] == 60,
           "--step-hz sets the step rate (the steps objective wants 120)")
    # Through the host's reader and lister: game://sim is a directory only the host knows.
    code, out, _ = s.run("sim", "--seed", "1", "--json", "--root", str(s.dir))
    r = json.loads(out)
    expect(code == 0 and len(r["runs"]) == 1 and r["runs"][0]["file"] == "game://sim/walk.dta",
           "sim lists a scenario directory through the host, and reads it through its reader")
    # compare: the same results, then an objective that broke.
    expect(s.run("sim", "--seeds", "1-2", "--results", "before")[0] == 0,
           "--results DIR writes the results")
    expect((s.dir / "before" / "results.json").exists() and
           (s.dir / "before" / "results.dta").exists(), "results.json and results.dta")
    (s.dir / "data" / "agent.dta").write_text("")
    (s.dir / "metrics" / "objectives.dta").write_text(
        (s.dir / "metrics" / "objectives.dta").read_text().replace("120", "121"))
    s.run("sim", "--seeds", "1-2", "--results", "after")
    code, out, _ = s.run("compare", "--before", "before", "--after", "after")
    r = json.loads(out)
    steps = [o for o in r["objectives"] if o["name"] == "steps"]
    expect(code == 1 and r["schema"] == "baker.compare/1" and r["broke"] == 1 and
           steps and steps[0]["moved"] == "broke" and steps[0]["line"] == 2,
           "compare: an objective that broke, located: exit 1")
    code, out, _ = s.run("compare", "--before", "before", "--after", "before")
    expect(code == 0 and json.loads(out)["broke"] == 0, "compare: nothing moved: exit 0")
    code, _, _ = s.run("compare", "--before", "before", "--after", "nosuch")
    expect(code == 2, "compare: results that are not there: exit 2")
    # check reads the scenarios and objectives with the data.
    (s.dir / "sim" / "walk.dta").write_text("(scenario walk (seconds 1) (start {spawn grunt}))\n")
    code, out, _ = s.run("check")
    expect(code == 1 and any("takes 2 arguments" in e["message"] for e in json.loads(out)["errors"]),
           "check checks the scenarios' commands")


def command_line(s):
    code, out, _ = s.run("hello")
    expect(code == 0 and out == "hello\n", "a command of the game's own")
    code, _, err = s.run("nosuch")
    expect(code == 2 and "check" in err, "an unknown command: exit 2, with the commands")
    code, _, err = s.run("eval", "--seed", "x", "{+ 1 1}")
    expect(code == 2 and "--seed" in err, "a bad flag: exit 2")


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        return 2
    writes = shipping if sys.argv[3:] == ["shipping"] else diff_and_promote
    for case in (check, reference, eval_json, writes, host_io, scenarios, command_line):
        case(Stack(sys.argv[1], sys.argv[2]))
    print(f"{len(FAILURES)} failures" if FAILURES else "all passed")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
