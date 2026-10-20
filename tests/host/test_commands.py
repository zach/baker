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
    expect(code == 0 and r["passed"] and r["schema"] == "baker.check/1.2", "clean data passes")
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
    code, out, _ = s.run("check", "--scenario", "scenario.dta", agent="")
    r = json.loads(out)
    expect(code == 1 and any("takes 2 arguments" in e["message"] for e in r["errors"]),
           "the game's extra trees are checked")
    code, out, _ = s.run("check", "--scenario", "missing.dta", agent="")
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
    expect(code == 0 and r["reference"]["schema"] == "baker.reference/1.1" and "spawn" in names,
           "reference --json: the game's functions")
    expect(r["shapes"]["schema"] == "baker.shapes/1" and r["tweaks"]["schema"] == "baker.tweaks/1",
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
    for case in (check, reference, eval_json, writes, command_line):
        case(Stack(sys.argv[1], sys.argv[2]))
    print(f"{len(FAILURES)} failures" if FAILURES else "all passed")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
