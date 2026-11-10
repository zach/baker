# The change log: schema `baker.changes/1`

`baker::tools::write_changes_json(cfg, sink)` writes a Config's change log
(`cfg.set_change_history(n)`, see [the cookbook](cookbook.md#personal-layers-local)) as one
JSON object: every kept transaction, oldest first, each with its change records. The host
kit's `promote --baker-json` writes the promotion's transaction this way, so an agent or an
editor that promoted a change set reads what it changed and where it was written without
parsing Baker text.

```bash
build/game promote --baker-json                 # the transaction it would make; nothing written
build/game promote --baker-write --baker-json   # written, and the transaction
```

```json
{"schema": "baker.changes/1", "transactions": [
  {"id": 1, "name": "promote", "time_ns": 1730900000000000000, "undone": false,
   "written": [{"file": "/game/data/default.dta", "line": 12}],
   "records": [
    {"kind": "set", "path": "player.speed", "id": "", "before": "(3.5)", "after": "(4.5)",
     "layer": "default", "source": {"layer": "default", "file": "", "line": 0}, "time_ns": 1730900000000000000}
  ]}
]}
```

| Field | Contents |
|---|---|
| `schema` | `"baker.changes/1"` |
| `transactions` | the kept transactions, oldest first; the log keeps the last `n` |
| `id` | the transaction's sequence number in the session, from 1 |
| `name` | what made it: `set`, `clear_scratch`, a commit's trigger (`layer`, `reload`, `restore`, `write_back`, `promote`), or the name `begin_transaction` or a console command gave |
| `time_ns` | when it began, on the Config's clock |
| `undone` | whether `undo()` returned to before it (`redo()` applies it again) |
| `written` | the file and line of each line a promotion or a write-back wrote |
| `records` | its change records, in the order found |

A record:

| Field | Contents |
|---|---|
| `kind` | `set`, `add`, `remove`, `rename`, `reorder`, `append` or `drop` |
| `path` | the path from the root, dotted: a value's for `set`, `append` and `drop`; a record's for `add`, `remove` and `rename`; the parent's for `reorder` |
| `id` | the record's `(@id ...)`, 32 hex digits, or `""` |
| `renamed_from` | `rename` only: the old key, which the new record keeps as an alias |
| `before`, `after` | the value as Baker text, or `null` where there was none: a `set`'s values in a list, `(3.5)`; a record's whole tree for `add`, `remove` and `rename`, `(armor 1)`; the parent's whole tree for `reorder`; for `append` and `drop`, the list before and the row's operand |
| `layer` | the layer the new value was written to: `scratch`, the layer a commit's record found supplying it, or the layer a promotion or a write-back wrote into |
| `source` | where the old value came from, when the log knows: a live edit's, or the layer written into; else its fields are empty (`""`, `0`) |
| `time_ns` | when the change was recorded |

A live edit (`set`, a tweak) is one `set` in a transaction of its own, or in the open one.
A commit (a layer pushed, removed or toggled, a reload, a restore) records what changed in
the merged tree, walking only the subtrees that did; a write-back and a promotion record
what changed in the layer they wrote into, since the merged tree need not change when the
layer above supplied the values already. The walk finds:
a `set` at each value that differs, `add` and `remove` for keyed records, `rename` where a
removed and an added record carry one ID or the added one's `(@aka ...)` names the removed
key, `reorder` where a parent's common keys come in another order, and `append` or `drop`
where the layer's row composes a list with a verb.

## Stability

- **Order.** Transactions come oldest first, `written` in the order the lines were written, and
  records in the order found. `time_ns` is read from the Config's clock, so two runs of the
  same edits differ in it.
- **Optional field.** `renamed_from` is present on a `rename` record only.
- **Formatting:** UTF-8, one transaction per line and each record on a line of its own, a
  newline at the end. Strings escape `"`, `\` and control characters (`\n`, `\t`, `\u00XX`);
  other characters are written as they are. Numbers are integers.
- **Versioning** follows [`check`'s](check-json.md#stability): a field is added only with a new
  minor version, and removed, renamed or changed only with a new major version.
