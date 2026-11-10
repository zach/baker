# The assets: schema `baker.assets/1`

The host kit's `assets` command writes every asset the merged stack names, one edge per
field value, so an asset pipeline can index what a record needs, or what a file change
touches, without reading Baker text. A field names an asset when its shape declares it
`(asset KIND)` or `(list (asset KIND))`, and the project file's `(assets ...)` declares the
kinds (see [the language](language.md#shapes)). Baker describes assets and reports edges; it
never imports, stores or cooks one.

```bash
build/game assets > assets.json      # exit 1 when a file is not there, 2 when it could not run
```

```json
{"schema": "baker.assets/1", "root": "/game/data", "assets": [
  {"from": {"file": "/game/data/bosses.dta", "line": 12, "path": "bosses.dragon.mesh",
            "id": "b3c9a7e5f214d68a1e2c3b4d5e6f7081"},
   "field": "mesh", "kind": "mesh", "target": "art/dragon.glb", "exists": true},
  {"from": {"file": "/game/data/bosses.dta", "line": 13, "path": "bosses.dragon.skins", "id": "b3c9a7e5f214d68a1e2c3b4d5e6f7081"},
   "field": "skins", "kind": "texture", "target": "art/red.png", "exists": false}
]}
```

| Field | Contents |
|---|---|
| `schema` | `"baker.assets/1"` |
| `root` | the project file's directory, which every `target` is relative to |
| `assets` | one edge per value, sorted by `from.file`, then `from.line`, then `from.path` |
| `from.file`, `from.line` | where the value is written: the layer that names it, so an overlay's own line |
| `from.path` | the field's path from the root, dotted: the record's path and the field's tag |
| `from.id` | the record's `(@id ...)`, 32 hex digits, or `""` when it carries none |
| `field` | the field's tag |
| `kind` | the field's `(asset KIND)` |
| `target` | the path as written |
| `exists` | whether the file is there, found by listing its directory through the host's lister (`CommandHooks::list`, else the disk); a directory that cannot be listed makes every file in it absent |

A list field gives one edge per element, each at the field's line. The merged stack is what
is listed: a layer that replaces a record's asset replaces its edge, and a disabled layer
contributes none. `check` reports an absent file as a shape error, so a gate catches it
before a pipeline does.

## Stability

- **The same data gives the same bytes,** as long as the same files are there: the edges are
  sorted by the data alone (above), and only `exists` reads the disk.
- **Formatting:** UTF-8, one edge per line, a newline at the end. Strings escape `"`, `\` and
  control characters (`\n`, `\t`, `\u00XX`); other characters are written as they are.
  Numbers are integers.
- **Versioning** follows [`check`'s](check-json.md#stability): a field is added only with a new
  minor version, and removed, renamed or changed only with a new major version.
