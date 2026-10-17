#!/usr/bin/env python3
"""Checks that Baker's public headers reach only the standard headers allowed.

Walks each header's #include closure among include/baker/ and reports every standard
header reached outside the allow-list, with the chain that reaches it. Opt-in interop
headers named std_*.hpp are exempt: a host includes them only when it wants the STL.
Exits 1 if anything is reported, so CTest can run it.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
INCLUDE = ROOT / "include" / "baker"
ALLOWED = {
    # language support and trivial C headers
    "cstddef", "cstdint", "climits", "cstdlib", "cstring", "cassert", "type_traits",
    "new", "bit", "atomic", "initializer_list", "compare", "concepts", "utility", "limits",
    "version", "coroutine",
    # header-only, non-allocating vocabulary types
    "string_view", "span", "optional", "expected",
}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*<([^>]+)>', re.M)


def includes(header):
    return INCLUDE_RE.findall(header.read_text())


def main():
    failures = 0
    for header in sorted(INCLUDE.glob("*.hpp")):
        if header.name.startswith("std_"):
            continue
        seen, stack, bad = set(), [(header, [header.name])], {}
        while stack:
            h, chain = stack.pop()
            if h in seen:
                continue
            seen.add(h)
            for inc in includes(h):
                if inc.startswith("baker/"):
                    stack.append((INCLUDE / inc[len("baker/"):], chain + [inc[len("baker/"):]]))
                elif inc not in ALLOWED:
                    bad.setdefault(inc, " -> ".join(chain))
        for inc, chain in sorted(bad.items()):
            print(f"{header.name}: <{inc}> via {chain}")
            failures += 1
    if failures:
        print(f"{failures} disallowed standard headers")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
