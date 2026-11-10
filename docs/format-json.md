# Format's edits: schema `baker.format/1.1`

The host kit's `format --baker-json` writes, for each file, the edits that lay it out as Baker
writes it (see [`format`](tools.md#format)), and changes no file. An editor applies the edits to
its buffer without moving the cursor through unchanged text, and a CI step reads which files
would change. The edits are those `tools::format_text` returns: each replaces a gap between
tokens with line ends and spaces, so every token, comment and directive stays as written.

```bash
build/game format --baker-json data/messy.dta data/bad.dta   # exit 1 when a file does not parse
```

`data/messy.dta` holds `(a   1) ; one`, two blank lines, and `(b`, `2)` on two lines;
`data/bad.dta` holds `(c (d`, which does not parse:

```json
{"schema": "baker.format/1.1", "files": [
  {"file": "data/messy.dta", "changed": true, "rewrites": 0, "line_end": "\n",
   "edits": [[2, 5, 0, 1], [13, 16, 2, 0], [18, 20, 0, 1]], "errors": 0},
  {"file": "data/bad.dta", "changed": false, "rewrites": 0, "line_end": "\n", "edits": [],
   "errors": 1}
]}
```

| Field | Contents |
|---|---|
| `schema` | `"baker.format/1.1"` |
| `files` | one object per file: those named on the command line, in that order, or with none named, the stack's own files under the project's directory, in the order the stack reads them |
| `file` | the file's path, as named or as the stack names it |
| `changed` | whether formatting would change the file: an edit, or a rewrite |
| `rewrites` | how many fields the file would spell by their shape's new name (`(@renamed_from ...)`), plus how many `(@id ...)` `--baker-mint-ids` would write into it |
| `line_end` | the file's line end: `"\r\n"` (written `"\u000d\n"`) if its first line ends so, else `"\n"` |
| `edits` | the layout's edits, in order and none overlapping, each `[begin, end, newlines, spaces]`: the bytes `[begin, end)` replaced by `newlines` line ends (`line_end`) and then `spaces` spaces; `[]` when the file is laid out already |
| `errors` | `1` when the file does not parse, else `0`; such a file has no edits, and its errors go to standard error |

The offsets are bytes of the text after the rewrites. With `rewrites` at 0 they apply to the
file as it is. Otherwise the text they apply to is not in the document: run `format` without
`--baker-json` to write the rewrites and the layout together.

`--baker-json` alone exits 0 when files would change, 1 when a file does not parse, and 2 when
it could not run (a file it cannot read, a `--baker-mint-ids` table no layer has, or a stack
that does not load when no file is named). With `--baker-check` as well, it also exits 1 when
a file would change.

## Stability

- **The same files give the same bytes,** in the order above. Paths are written as named: a
  path given relative to the working directory stays relative.
- **Formatting:** UTF-8, one file per line, two-space indentation, a newline at the end.
  Strings escape `"`, `\` and control characters (`\n`, `\t`, `\u00XX`); other characters are
  written as they are. Numbers are integers.
- **Versioning** follows [`check`'s](check-json.md#stability): a field is added only with a new
  minor version, and removed, renamed or changed only with a new major version. 1.1 adds
  `rewrites`.
