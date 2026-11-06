#!/usr/bin/env python3
"""Tests baker_bake --views on a copy of tests/views/project.

    python3 -I tests/views/test_bake_views.py build/baker_bake tests/views/project

CTest runs it. Exits 1 on a failure.
"""
import os
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


def main(tool, source):
    tool = str(Path(tool).resolve())
    with tempfile.TemporaryDirectory() as tmp:
        project = Path(tmp) / "project"
        shutil.copytree(source, project)
        out = Path(tmp) / "views"

        def run(*extra, project_file="baker-project.dta"):
            # A path the parser normalizes differently from how it is given (`..`, and on
            # Windows the separators): the tool must select declarations by the parser's form.
            cmd = [tool, "--views", str(out), "--project", str(project / ".." / "project" / project_file),
                   "--depfile", str(out / "views.d"), "--stamp", str(out / "views.stamp"), *extra]
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
            return p.returncode, p.stdout, p.stderr

        rc, _, err = run()
        expect(rc == 0, f"a first run succeeds: {err}")
        for name in ["boss.hpp", "boss_read.hpp", "item.hpp", "item_read.hpp", "vec2.hpp",
                     "vec2_read.hpp", "views.hpp", "tables.hpp", "views.d", "views.stamp"]:
            expect((out / name).exists(), f"{name} is written")
        dep = (out / "views.d").read_text().replace("\\", "/")
        expect(dep.startswith(str(out / "views.stamp").replace("\\", "/") + ":"),
               "the depfile's target is the stamp")
        for name in ["baker-project.dta", "core.dta", "content.dta"]:
            expect(name in dep, f"the depfile names {name}")
        expect("extra.dta" not in dep, "a layer outside the contract is not read")
        boss = (out / "boss.hpp").read_text()
        expect("std::string_view hint;" in boss, "the content layer's field is in the struct")
        expect("armor" not in boss, "the mod's field is not")
        expect("loot" not in boss, "a (@view no) field is not")
        expect("namespace views_gen {" in boss, "the project's namespace")

        # A second run rewrites nothing but the stamp.
        before = {n: (out / n).stat().st_mtime_ns for n in ["boss.hpp", "views.hpp"]}
        os.utime(out / "views.stamp", (0, 0))
        rc, _, err = run()
        expect(rc == 0, f"a second run succeeds: {err}")
        after = {n: (out / n).stat().st_mtime_ns for n in before}
        expect(before == after, "unchanged headers keep their time stamps")
        expect((out / "views.stamp").stat().st_mtime_ns > 0, "the stamp is touched")

        # A tuning edit changes no header; a layout edit changes one.
        core = project / "core.dta"
        text = core.read_text()
        core.write_text(text.replace("(@range 1 100000)", "(@range 1 9)"))
        rc, _, _ = run()
        expect(rc == 0 and (out / "boss.hpp").stat().st_mtime_ns == after["boss.hpp"],
               "a tuning edit rewrites no header")
        core.write_text(text.replace("(hp   int", "(hp   int (@cxx int64)"))
        rc, _, _ = run()
        expect(rc == 0 and "int64_t hp" in (out / "boss.hpp").read_text(),
               "a layout edit rewrites the header")
        core.write_text(text)

        # Errors, each named: a shape inside #ifdef, an unknown listed shape, a dev_only
        # contract layer, and a build without the listed layer.
        core.write_text("#ifdef PC\n" + text + "#endif\n")
        rc, _, err = run()
        expect(rc == 1 and "inside #ifdef PC" in err, f"a #shape under #ifdef is refused: {err}")
        core.write_text("#ifdef HANDHELD\n#shape (x (y int))\n#endif\n" + text)
        rc, _, err = run()
        expect(rc == 1 and "branch this build skipped" in err, f"a skipped #shape is refused: {err}")
        core.write_text(text)
        pf = project / "baker-project.dta"
        ptext = pf.read_text()
        pf.write_text(ptext.replace("(shapes item)", "(shapes item nope)"))
        rc, _, err = run()
        expect(rc == 1 and "'nope', which no contract layer declares" in err, f"an unknown listed shape: {err}")
        pf.write_text(ptext.replace("(layers core content)", "(layers core local)"))
        rc, _, err = run()
        expect(rc == 1 and "dev_only or under (when COND)" in err, f"a dev_only contract layer: {err}")
        pf.write_text(ptext.replace("(layers core content)", "(layers core nowhere)"))
        rc, _, err = run()
        expect(rc == 1 and "which (layers ...) does not have" in err, f"an unknown contract layer: {err}")
        pf.write_text(ptext)

    if FAILURES:
        print(f"{len(FAILURES)} failures")
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
