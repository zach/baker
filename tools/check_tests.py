#!/usr/bin/env python3
"""Fails when a plain TEST_CASE calls what a shipping build refuses, without handling shipping.

Shipping refuses live edits, reloads, toggling a layer, write-back and promotion, so a test of
them passes in development and fails only in the shipping build. A test of development alone
is a BAKER_DEV_TEST_CASE (tests/support/dev_only.hpp), which shipping skips. A plain TEST_CASE
may call them where it has considered shipping:

- inside an `if constexpr` on kShipping, of either sense, or its else;
- in a statement guarded by one;
- after an `if constexpr (kShipping) ...return...` in the same test;
- inside an `#if` on BAKER_SHIPPING, of either sense, or its #else.

The check is lexical. It reads the bodies of TEST_CASEs, and not helpers they call, nor script
text such as {config.set ...}. A line whose comment ends in `check_tests: ok` is passed over.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
REFUSED = re.compile(r"(?:\.|->)\s*(set|set_layer_enabled|toggle_layer|reload|clear_scratch|"
                     r"write_back|write_back_to|promote)\s*\(")
SHIPPING = re.compile(r"if\s+constexpr\s*\(\s*!?\s*(?:baker::)?kShipping\s*\)")
EARLY_OUT = re.compile(r"if\s+constexpr\s*\(\s*(?:baker::)?kShipping\s*\)\s*(?:\{[^{}]*)?return\b")
CASE = re.compile(r"\b(BAKER_DEV_TEST_CASE|TEST_CASE)\s*\(")
ALLOW = "check_tests: ok"


def blank(text):
    """The text with comments and literals replaced by spaces, newlines kept, so offsets and
    line numbers still match and braces inside literals do not count."""
    out, i, n = list(text), 0, len(text)

    def clear(a, b):
        for k in range(a, b):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            clear(i, j)
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            clear(i, j)
            i = j
        elif text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            m = re.match(r'R"([^(\s]*)\(', text[i:])
            delim = ")" + (m.group(1) if m else "") + '"'
            j = text.find(delim, i + 2)
            j = n if j < 0 else j + len(delim)
            clear(i + 1, j)
            i = j
        elif text[i] in "\"'" and not (text[i] == "'" and i > 0 and text[i - 1].isalnum()):
            q, j = text[i], i + 1
            while j < n and text[j] != q and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            clear(i + 1, min(j, n))
            i = j + 1
        else:
            i += 1
    return "".join(out)


def match_close(code, i, open_c, close_c):
    depth = 0
    for k in range(i, len(code)):
        if code[k] == open_c:
            depth += 1
        elif code[k] == close_c:
            depth -= 1
            if depth == 0:
                return k
    return len(code) - 1


def statement_start(code, i, floor):
    k = i
    while k > floor and code[k - 1] not in ";{}":
        k -= 1
    return k


def handled(code, body_start, pos):
    """Whether the call at pos sits where its test has considered shipping."""
    if EARLY_OUT.search(code, body_start, pos):
        return True
    if SHIPPING.search(code[statement_start(code, pos, body_start):pos]):
        return True
    # Each enclosing block, outward: its header, or for an else the if it belongs to.
    depth, k = 0, pos
    while k > body_start:
        k -= 1
        c = code[k]
        if c == "}":
            depth += 1
        elif c == "{":
            if depth:
                depth -= 1
                continue
            header = code[statement_start(code, k, body_start):k]
            if SHIPPING.search(header):
                return True
            if header.strip() == "else":
                before = code[:statement_start(code, k, body_start)].rstrip()
                if before.endswith("}"):
                    open_at = before.rfind("{", 0, len(before))
                    # The if's own header: back from its opening brace.
                    d, j = 0, len(before) - 1
                    while j > body_start:
                        if code[j] == "}":
                            d += 1
                        elif code[j] == "{":
                            d -= 1
                            if d == 0:
                                break
                        j -= 1
                    del open_at
                    if SHIPPING.search(code[statement_start(code, j, body_start):j]):
                        return True
    return False


def shipping_lines(code):
    """The lines inside an #if, #ifdef or #else whose condition names BAKER_SHIPPING."""
    inside, stack = set(), []
    for number, line in enumerate(code.split("\n"), 1):
        directive = line.strip()
        if re.match(r"#\s*if", directive):
            stack.append("BAKER_SHIPPING" in directive)
        elif re.match(r"#\s*endif", directive) and stack:
            stack.pop()
        elif any(stack):
            inside.add(number)
    return inside


def config_set(code, args):
    """Whether a set( call is a Config's: its path comes first, as a literal or a Path, where
    an Array's set takes an index."""
    first = code[args:].lstrip()
    return first.startswith('"') or re.match(r"(?:baker::)?Path\b|\w*path\b", first) is not None


def check(path):
    text = path.read_text(encoding="utf-8")
    code = blank(text)
    lines = text.split("\n")
    guarded = shipping_lines(code)
    problems = []
    for m in CASE.finditer(code):
        close = match_close(code, m.end() - 1, "(", ")")
        brace = code.find("{", close)
        if brace < 0:
            continue
        end = match_close(code, brace, "{", "}")
        if m.group(1) == "BAKER_DEV_TEST_CASE":
            continue
        for call in REFUSED.finditer(code, brace, end):
            line = code.count("\n", 0, call.start()) + 1
            if line in guarded or lines[line - 1].rstrip().endswith(ALLOW):
                continue
            if call.group(1) == "set" and not config_set(code, call.end()):
                continue
            if not handled(code, brace + 1, call.start()):
                problems.append(f"{path.relative_to(ROOT)}:{line}: {call.group(1)}() in a TEST_CASE "
                                "that does not handle shipping; make it a BAKER_DEV_TEST_CASE, or "
                                "guard the call with if constexpr (!kShipping)")
    return problems


def main():
    problems = []
    for path in sorted((ROOT / "tests").rglob("*.cpp")):
        problems += check(path)
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
