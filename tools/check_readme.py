#!/usr/bin/env python3
"""Checks that the docs' copies of example files match the files, line for line.

A fenced code block in README.md or docs/*.md preceded by the comment
`<!-- file: examples/quickstart/main.cpp -->` must equal that file. Reports each block
that differs, with the first differing line. Exits 1 if any does, so CTest can run it.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BLOCK_RE = re.compile(r"<!-- file: (\S+) -->\s*\n```[^\n]*\n(.*?)^```", re.M | re.S)


def main():
    failed = 0
    checked = 0
    for doc in [ROOT / "README.md", *sorted((ROOT / "docs").glob("*.md"))]:
        text = doc.read_text(encoding="utf-8")
        for m in BLOCK_RE.finditer(text):
            checked += 1
            target = ROOT / m.group(1)
            line = text.count("\n", 0, m.start(2)) + 1
            where = f"{doc.relative_to(ROOT)}:{line}"
            if not target.is_file():
                print(f"{where}: {m.group(1)} does not exist")
                failed += 1
                continue
            want = target.read_text(encoding="utf-8").splitlines()
            got = m.group(2).splitlines()
            if want == got:
                continue
            failed += 1
            for i in range(max(len(want), len(got))):
                w = want[i] if i < len(want) else "<end of file>"
                g = got[i] if i < len(got) else "<end of block>"
                if w != g:
                    print(f"{where}: differs from {m.group(1)} at its line {i + 1}:")
                    print(f"  file:  {w}")
                    print(f"  block: {g}")
                    break
    if checked == 0:
        print("no <!-- file: ... --> blocks found")
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
