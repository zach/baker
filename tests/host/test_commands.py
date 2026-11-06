#!/usr/bin/env python3
"""Tests the host kit's commands through a game binary, on a copy of a small stack.

    python3 -I tests/host/test_commands.py build/tests/baker_host_test_game tests/host/stack [shipping]

CTest runs it. Each case works on a fresh copy, so the stack's own files are never touched.
Exits 1 on a failure.
"""
import json
import re
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
        p = subprocess.run([self.game, *args, "--baker-project", "data"], cwd=self.dir,
                           capture_output=True, text=True, timeout=120)
        return p.returncode, p.stdout, p.stderr


def check(s):
    code, out, _ = s.run("check")
    r = json.loads(out)
    expect(code == 0 and r["passed"] and r["schema"] == "baker.check/1.5", "clean data passes")
    code, out, err = s.run("check", agent="(waves {spawnn grunt 3})\n")
    r = json.loads(out)
    expect(code == 1 and [d["name"] for d in r["demand"]] == ["spawnn"] and "spawnn" in err,
           "a call to an undefined function fails the check, and is named")
    code, out, _ = s.run("check", agent="(player (speed 42))\n")
    r = json.loads(out)
    expect(code == 1 and len(r["shape_errors"]) == 1, "a value out of its shape's range fails")
    code, out, _ = s.run("check", agent="#shape (enemy (@at foes) (@keyed) (hp int))\n")
    r = json.loads(out)
    expect(code == 0 and any("shape 'enemy' governs nothing: no array is a child of foes"
                             in w["message"] for w in r["shape_warnings"]),
           "a shape that governs no array is a shape warning, and passes")
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


def views(s):
    """The views the game compiled, against the stack's shapes."""
    code, out, err = s.run("check", agent="")
    r = json.loads(out)
    expect(code == 0 and not any("regenerate the views" in w["message"] for w in r["shape_warnings"]),
           "a game whose views match its shapes passes check with no view warning")
    default = s.dir / "data" / "default.dta"
    text = default.read_text()
    default.write_text(text.replace("(lives int)", "(lives int (@cxx int64))"))
    code, out, err = s.run("check", agent="")
    r = json.loads(out)
    expect(code == 0 and any("regenerate the views" in w["message"] for w in r["shape_warnings"]),
           f"a shape whose layout moved is a shape warning: {err}")
    expect("view warning:" in err, "and is printed as one")
    code, out, err = s.run("check", "--baker-strict-views", agent="")
    r = json.loads(out)
    expect(code == 1 and any("regenerate the views" in e["message"] for e in r["shape_errors"]),
           f"--baker-strict-views makes it an error: {err}")
    default.write_text(text)


def aliases(s):
    """An (@aka old) nothing names is a finding; a layer naming it merges into the record."""
    code, out, err = s.run("check", agent="")
    r = json.loads(out)
    expect(code == 0 and any("alias 'hero' of player is named by no layer" in f["message"]
                             for f in r["findings"]), f"an unused alias is a finding: {err}")
    code, out, _ = s.run("check", agent="(hero (speed 4))\n")
    r = json.loads(out)
    expect(code == 0 and not any("alias 'hero'" in f["message"] for f in r["findings"]),
           "a layer that names the old key uses the alias")
    code, out, _ = s.run("eval", "{config.get player.speed}", agent="(hero (speed 4))\n")
    expect(code == 0 and out == "4\n", f"and merges into the renamed record: {out!r}")


def reference(s):
    code, out, _ = s.run("reference", "--baker-json", agent="")
    r = json.loads(out)
    names = [f["name"] for f in r["reference"]["functions"]]
    expect(code == 0 and r["reference"]["schema"] == "baker.reference/1.5" and "spawn" in names,
           "reference --baker-json: the game's functions")
    expect(r["shapes"]["schema"] == "baker.shapes/1.8" and r["tweaks"]["schema"] == "baker.tweaks/1.6",
           "reference --baker-json: tweaks and shapes")
    code, out, _ = s.run("reference")
    expect(code == 0 and "spawn" in out, "reference: Markdown")


def eval_json(s):
    code, out, _ = s.run("eval", "--baker-seed", "4", "--baker-json", "{+ $score 1}", "{nosuch}", agent="")
    lines = [json.loads(l) for l in out.splitlines()]
    expect(code == 1 and len(lines) == 2 and lines[0]["value"] == 41 and lines[0]["id"] == 1,
           "eval --baker-json: on the game reset with the seed; one reply a command")
    expect(not lines[1]["ok"] and lines[1]["id"] == 2, "eval --baker-json: an error is a reply")
    code, out, _ = s.run("eval", "{config.get player.speed}", agent="(player (speed 4.5))\n")
    expect(code == 0 and out == "4.5\n", "eval: the stack with the agent layer")


def diff_and_promote(s):
    code, out, _ = s.run("diff", agent="(player (speed 4.5))\n")
    expect(code == 0 and "player.speed  3.5 -> 4.5" in out, "diff: what the agent layer changes")
    code, out, _ = s.run("diff", "--baker-rows", agent="(/player.speed 4.5)\n")
    expect(code == 0 and out == "; layer 'agent' changes:\n(/player.speed 4.5)  ; was 3.5\n",
           "diff --baker-rows: each change as a row a layer file holds")
    code, out, _ = s.run("promote", "--baker-rows", agent="(player (speed 4.5))\n")
    expect(code == 0 and "(/player.speed 4.5)  ; was 3.5\n" in out,
           "promote --baker-rows: what it would write, as rows")
    before = (s.dir / "data" / "default.dta").read_text()
    code, out, _ = s.run("promote", "--baker-patch", "change.patch")
    patch = (s.dir / "change.patch").read_text()
    expect(code == 0 and (s.dir / "data" / "default.dta").read_text() == before,
           "promote --baker-patch writes nothing in the stack")
    expect(patch.startswith("--- a/data/default.dta\n+++ b/data/default.dta\n@@ ") and
           "-   (speed 3.5)   ; how fast\n+   (speed 4.5)   ; how fast\n" in patch,
           "promote --baker-patch: one line, its comment kept, named from the project's directory")
    code, out, _ = s.run("promote", "--baker-write")
    expect(code == 0 and "(speed 4.5)   ; how fast" in (s.dir / "data" / "default.dta").read_text(),
           "promote --baker-write writes the line")
    # The layer verbs: diff names each, with a stale pin; the patch carries each row's note.
    code, out, _ = s.run("diff", agent="(loot #append ruby (@note \"a rarer drop\"))\n"
                                       "(player (lives 5 (@was 2)))\n")
    expect(code == 0 and "loot  (coin gem) -> #append ruby" in out and
           "player.lives  3 -> 5" in out and out.rstrip().endswith("stale"),
           "diff: a verb and a stale pin")
    code, out, _ = s.run("diff", "--baker-rows")
    expect(code == 0 and '(/loot #append ruby (@note "a rarer drop"))  ; was (coin gem)\n' in out and
           "(/player.lives 5 (@was 2))  ; was 3, stale\n" in out,
           "diff --baker-rows: a verb in its slot, with its metadata")
    code, out, _ = s.run("promote", "--baker-patch", "verbs.patch")
    patch = (s.dir / "verbs.patch").read_text()
    expect(code == 0 and patch.startswith("# loot: a rarer drop\n--- a/data/default.dta") and
           "+(loot coin gem ruby)\n" in patch,
           "promote --baker-patch: the note above the patch, the list resolved")


def format_files(s):
    messy = s.dir / "data" / "messy.dta"
    text = "(a   1) ; one\n\n\n(b\n 2)\n"
    messy.write_text(text)
    code, out, _ = s.run("format", "--baker-check", "data/messy.dta")
    expect(code == 1 and "messy.dta" in out and messy.read_text() == text,
           "format --baker-check lists an unformatted file and changes nothing")
    code, out, _ = s.run("format", "--baker-json", "data/messy.dta")
    r = json.loads(out)
    expect(code == 0 and r["schema"] == "baker.format/1.1" and r["files"][0]["changed"]
           and len(r["files"][0]["edits"]) >= 2 and messy.read_text() == text,
           "format --baker-json gives the edits and changes nothing")
    code, _, _ = s.run("format", "data/messy.dta")
    expect(code == 0 and messy.read_text() == "(a 1) ; one\n\n(b 2)\n", "format rewrites the file")
    code, out, _ = s.run("format", "--baker-check", "data/messy.dta")
    expect(code == 0 and out == "", "a formatted file passes --baker-check")
    messy.unlink()
    code, out, _ = s.run("format", "--baker-check")
    expect(code in (0, 1), "with no files, format takes the stack's own files")


def renames(s):
    """A field under a name its shape renamed: check warns, format rewrites it."""
    shape = "#shape (player (lives int (@renamed_from hp)))\n"
    code, out, err = s.run("check", agent=shape + "(player (hp 5))\n")
    r = json.loads(out)
    expect(code == 1 and any("'hp' was renamed 'lives', and the record has both" in e["message"]
                             for e in r["shape_errors"]),
           f"the base's new name and an overlay's old one: an error naming both: {err}")
    default = s.dir / "data" / "default.dta"
    text = default.read_text()
    default.write_text(text.replace("(lives 3)", "(hp 3)"))
    code, out, err = s.run("check", agent=shape)
    r = json.loads(out)
    expect(code == 0 and any("'hp' is now 'lives' (format rewrites it)" in w["message"]
                             for w in r["shape_warnings"]), f"the old name alone is a warning: {err}")
    code, out, _ = s.run("format", "--baker-json", "data/default.dta", agent=shape)
    r = json.loads(out)
    expect(code == 0 and r["files"][0]["rewrites"] == 1 and "(hp 3)" in default.read_text(),
           "format --baker-json counts the rewrite and changes nothing")
    code, out, _ = s.run("format", "--baker-check", "data/default.dta", agent=shape)
    expect(code == 1 and "default.dta" in out, "--baker-check lists a file a rename would change")
    code, out, err = s.run("format", "data/default.dta", agent=shape)
    expect(code == 0 and "(lives 3)" in default.read_text() and "(hp 3)" not in default.read_text()
           and "; how fast" in default.read_text(), f"format rewrites the old name, keeping the rest: {err}")
    code, out, _ = s.run("check", agent=shape)
    r = json.loads(out)
    expect(code == 0 and not r["shape_warnings"], "after which check is quiet")
    default.write_text(text)


def tables(s):
    """The game's baked tables register with each command's Config; a record a table cannot read fails check."""
    code, out, err = s.run("check", agent="(pets (dog (hp {heal})))\n")
    r = json.loads(out)
    expect(code == 1 and any("'hp'" in e["message"] and e.get("path") == "pets.dog"
                             for e in r["shape_errors"]),
           f"a view field that is not a literal is a shape error from the table's build: {err} {out}")
    code, out, _ = s.run("check", agent="(pets (cat (hp 2)))\n")
    expect(code == 0, "a record a table reads whole passes")


def assets(s):
    """assets: every asset the stack names, baker.assets/1; check fails on a missing one."""
    code, out, err = s.run("assets", agent="")
    r = json.loads(out)
    expect(code == 0 and r["schema"] == "baker.assets/1" and len(r["assets"]) == 1
           and r["assets"][0]["from"]["path"] == "player.model" and r["assets"][0]["kind"] == "mesh"
           and r["assets"][0]["target"] == "assets/hero.glb" and r["assets"][0]["exists"]
           and r["assets"][0]["from"]["file"].endswith("default.dta") and r["assets"][0]["from"]["id"] == "",
           f"assets lists the edge, found by listing its directory: {out} {err}")
    code, out, err = s.run("assets", agent="(player (model \"assets/none.glb\"))\n")
    r = json.loads(out)
    expect(code == 1 and not r["assets"][0]["exists"] and r["assets"][0]["from"]["file"].endswith("agent.dta"),
           f"a file that is not there: exists false, exit 1, from the layer that names it: {out}")
    code, out, err = s.run("check", agent="(player (model \"assets/none.glb\"))\n")
    r = json.loads(out)
    expect(code == 1 and any('asset "assets/none.glb" of player.model is not there' in e["message"]
                             for e in r["shape_errors"]), f"check fails on it: {err}")
    code, out, err = s.run("check", agent="(player (model \"assets/hero.obj\"))\n")
    r = json.loads(out)
    expect(code == 1 and any('not a mesh (.glb)' in e["message"] for e in r["shape_errors"]),
           f"an extension the kind does not list: {err}")
    code, out, err = s.run("check", agent="#shape (player (icon (asset texture) (@optional)))\n"
                                           "(player (icon \"assets/hero.glb\"))\n")
    r = json.loads(out)
    expect(code == 1 and any("declares no kind 'texture'" in e["message"] for e in r["shape_errors"]),
           f"a kind the project does not declare: {err}")


def bake(s):
    """bake --baker-stack writes the contract layers and the tables as one image a Config opens."""
    code, out, err = s.run("bake", "--baker-stack", "game.datb", agent="(player (speed 9))\n")
    image = s.dir / "game.datb"
    expect(code == 0 and "wrote game.datb: 1 layers, 1 tables" in out and image.stat().st_size > 64,
           f"bake writes the image of the contract layers alone: {out} {err}")
    expect(image.read_bytes()[:10] == b"MOIRA.IMG1", "a stack image")
    # The game opens it in place of the layers' text: the base layer's values come from the image.
    (s.dir / "data" / "default.dta").write_text("(player (speed 1) (lives 1) (model \"assets/hero.glb\"))\n")
    code, out, err = s.run("eval", "{config.get player.speed}", "--image", "game.datb", agent="")
    expect(code == 0 and out == "3.5\n", f"the image's layer, not the file's: {out!r} {err}")
    code, out, err = s.run("check", "--image", "game.datb", agent="")
    r = json.loads(out)
    expect(code == 0 and r["passed"], f"check with the image passes, its tables taken from it: {err}")
    code, _, err = s.run("bake", agent="")
    expect(code == 2 and "--baker-stack" in err, "bake needs the file to write")


def rename(s):
    """rename OLD NEW: the key in the declaring layer, (@aka OLD), an ID, and every key_of."""
    if sys.argv[3:] == ["shipping"]:
        return  # refuses there, as `shipping` checks
    default = s.dir / "data" / "default.dta"
    text = default.read_text()
    added = ("#shape (wave (@at waves2) (@keyed) (foe (key_of foes)) (also (list (key_of foes)) (@optional)))\n"
             "(foes (grunt (hp 1)) (brute (@id a0000000000000000000000000000002) (hp 2)))\n"
             "(waves2 (first (foe grunt)) (second (foe brute) (also grunt brute grunt)))\n")
    default.write_text(text + added)
    agent = "(waves2 (third (foe grunt)))\n(foes (grunt (hp 5)))\n"
    code, out, err = s.run("rename", "foes.grunt", "runt", agent=agent)
    expect(code == 0 and "rerun with --baker-write" in out and default.read_text() == text + added
           and "grunt -> runt" in out and "(@aka grunt)" in out and "(@id " in out
           and out.count("(foe ...): grunt -> runt") == 2 and "(also ...): grunt -> runt" in out,
           f"a dry run lists the edits and writes nothing: {out} {err}")
    code, out, err = s.run("rename", "foes.grunt", "runt", "--baker-write", agent=agent)
    after = default.read_text()
    expect(code == 0 and "(runt (@id " in after and "(@aka grunt)" in after and "(foe runt)" in after
           and "(also runt brute runt)" in after and "(brute (@id a0000000000000000000000000000002)" in after,
           f"--baker-write: the key, its alias and ID, and every reference: {after}")
    expect(re.search(r"\(runt \(@id [a-f][0-9a-f]{31}\) \(hp 1\) \(@aka grunt\)\)", after) is not None,
           f"the ID after the key, the alias after the last child: {after}")
    agent_text = (s.dir / "data" / "agent.dta").read_text()
    expect("(foe runt)" in agent_text and "(foes (grunt (hp 5)))" in agent_text,
           f"an overlay's reference is rewritten; its record keeps the old key, which the alias resolves: {agent_text}")
    code, out, _ = s.run("check", agent=agent)
    r = json.loads(out)
    expect(code == 0 and not r["shape_errors"], f"after which the stack checks clean: {out}")
    code, out, _ = s.run("eval", "{config.get foes.runt.hp}", agent=agent)
    expect(code == 0 and out == "5\n", f"and the overlay merges into the renamed record: {out!r}")
    # Refusals: a key taken, an alias taken, a record that is not there, a bad key.
    code, _, err = s.run("rename", "foes.runt", "brute", agent=agent)
    expect(code == 2 and "'brute' is already a key of foes" in err, "a key a sibling has")
    code, _, err = s.run("rename", "foes.brute", "grunt", agent=agent)
    expect(code == 2 and "'grunt' is an alias of 'runt'" in err, "a key that is a sibling's alias")
    code, _, err = s.run("rename", "foes.nosuch", "x", agent=agent)
    expect(code == 2 and "no record 'foes.nosuch'" in err, "a record the stack lacks")
    code, _, err = s.run("rename", "foes.runt", "two words", agent=agent)
    expect(code == 2 and "is not a key" in err, "a key that is not one symbol")
    code, _, err = s.run("rename", "foes.runt", agent=agent)
    expect(code == 2 and "rename takes OLD" in err, "the arguments")
    default.write_text(text)


def mint_ids(s):
    """format --baker-mint-ids TABLE: an ID for each record without one, in its declaring layer."""
    default = s.dir / "data" / "default.dta"
    text = default.read_text()
    default.write_text(text + "(foes (grunt (hp 1)) (brute (@id a0000000000000000000000000000001) (hp 2))"
                       " (copy (@id a0000000000000000000000000000001) (hp 2)) (@desc \"the foes\"))\n")
    code, out, err = s.run("format", "--baker-mint-ids", "foes", "--baker-json", agent="(foes (grunt (hp 9)))\n")
    r = json.loads(out)
    by_file = {f["file"].rsplit("/", 1)[-1]: f for f in r["files"]}
    expect(code == 0 and by_file["default.dta"]["rewrites"] == 2 and by_file["agent.dta"]["rewrites"] == 0,
           f"two records need an ID: the one without, and the copy; the overlay none: {err} {out}")
    code, out, err = s.run("format", "--baker-mint-ids", "foes", agent="(foes (grunt (hp 9)))\n")
    after = default.read_text()
    ids = re.findall(r"\(@id ([0-9a-f]{32})\)", after)
    expect(code == 0 and len(ids) == 3 and len(set(ids)) == 3 and ids[1] == "a0000000000000000000000000000001"
           and all(i[0] in "abcdef" for i in ids), f"each record has its own ID, the brute keeps its: {after}")
    expect("(grunt (@id " in after and "(@desc \"the foes\")" in after, "written after the key; metadata is no record")
    expect("(@id" not in (s.dir / "data" / "agent.dta").read_text(), "the overlay's record is not the declaring one")
    code, out, err = s.run("format", "--baker-mint-ids", "foes", agent="")
    expect(code == 0 and default.read_text() == after, "minted once, a second run changes nothing")
    code, _, err = s.run("format", "--baker-mint-ids", "nosuch", agent="")
    expect(code == 2 and "no layer of the stack has a table 'nosuch'" in err, "a table no layer has")
    default.write_text(text)


def shipping(s):
    """Under BAKER_SHIPPING, the commands that write refuse; the others run."""
    for command in ("diff", "promote", "rename"):
        code, _, err = s.run(command, "a.b", "c")
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
        code, _, _ = s.run("promote", "--baker-patch", "p.diff", "--root", str(s.dir))
        patch = (s.dir / "p.diff").read_text()
        expect(code == 0 and "-   (speed 3.5)   ; how fast\n+   (speed 4.5)   ; how fast\n" in patch,
               "promote --baker-patch compares against what the reader reads")

    code, out, _ = s.run("check", "--baker-output", "report.json", agent="")
    expect(code == 0 and out == "" and
           json.loads((s.dir / "report.json").read_text())["passed"],
           "--baker-output FILE: the output in the file, nothing on stdout")
    code, out, err = s.run("check", "--baker-output", "no/such/dir/report.json")
    expect(code == 2 and out == "" and "cannot write" in err,
           "--baker-output to a file that cannot be written: exit 2")
    code, out, err = s.run("eval", "{+ 1 2}", "--to-stderr")
    expect(code == 0 and out == "" and err == "3\n", "the host routes the output")


def scenarios(s):
    if sys.argv[3:] == ["shipping"]:
        return
    code, out, err = s.run("sim", "--baker-seed", "1")
    expect(code == 0 and "walk seed 1: passed" in out, "sim runs the project's scenarios")
    code, out, _ = s.run("sim", "--baker-seeds", "0-2", "--baker-json")
    r = json.loads(out)
    expect(code == 0 and r["schema"] == "baker.sim/1.3" and len(r["runs"]) == 3 and
           r["runs"][0]["file"].endswith("walk.dta") and r["runs"][0]["line"] == 2,
           "sim --baker-json: every run, located")
    # The median of seeds 0-2 is 10: it passes, though seed 0 alone does not.
    code, out, _ = s.run("sim", "--baker-seed", "0", "--baker-json")
    expect(code == 1 and json.loads(out)["status"] == "failed",
           "an objective that fails its median: exit 1")
    code, _, err = s.run("sim", "--baker-scenario", "nosuch")
    expect(code == 2 and "no scenario" in err, "a scenario that is not there: exit 2")
    code, out, _ = s.run("sim", "--baker-scenario", "walk", "--baker-step-hz", "30", "--baker-json")
    expect(code == 1 and json.loads(out)["runs"][0]["steps"] == 60,
           "--baker-step-hz sets the step rate (the steps objective wants 120)")
    # Through the host's reader and lister: game://sim is a directory only the host knows.
    code, out, _ = s.run("sim", "--baker-seed", "1", "--baker-json", "--root", str(s.dir))
    r = json.loads(out)
    expect(code == 0 and len(r["runs"]) == 1 and r["runs"][0]["file"] == "game://sim/walk.dta",
           "sim lists a scenario directory through the host, and reads it through its reader")
    # compare: the same results, then an objective that broke.
    expect(s.run("sim", "--baker-seeds", "1-2", "--baker-results", "before")[0] == 0,
           "--baker-results DIR writes the results")
    expect((s.dir / "before" / "results.json").exists() and
           (s.dir / "before" / "results.dta").exists(), "results.json and results.dta")
    (s.dir / "data" / "agent.dta").write_text("")
    (s.dir / "metrics" / "objectives.dta").write_text(
        (s.dir / "metrics" / "objectives.dta").read_text().replace("120", "121"))
    s.run("sim", "--baker-seeds", "1-2", "--baker-results", "after")
    code, out, _ = s.run("compare", "--baker-before", "before", "--baker-after", "after")
    r = json.loads(out)
    steps = [o for o in r["objectives"] if o["name"] == "steps"]
    expect(code == 1 and r["schema"] == "baker.compare/1" and r["broke"] == 1 and
           steps and steps[0]["moved"] == "broke" and steps[0]["line"] == 2,
           "compare: an objective that broke, located: exit 1")
    code, out, _ = s.run("compare", "--baker-before", "before", "--baker-after", "before")
    expect(code == 0 and json.loads(out)["broke"] == 0, "compare: nothing moved: exit 0")
    code, _, _ = s.run("compare", "--baker-before", "before", "--baker-after", "nosuch")
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
    code, _, err = s.run("eval", "--baker-seed", "x", "{+ 1 1}")
    expect(code == 2 and "--baker-seed" in err, "a bad flag: exit 2")


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        return 2
    writes = shipping if sys.argv[3:] == ["shipping"] else diff_and_promote
    for case in (check, views, aliases, renames, mint_ids, rename, assets, tables, bake, reference, eval_json, writes, host_io, scenarios, command_line,
                 format_files):
        case(Stack(sys.argv[1], sys.argv[2]))
    print(f"{len(FAILURES)} failures" if FAILURES else "all passed")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
