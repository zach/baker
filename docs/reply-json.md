# Replies for tools: schema `baker.reply/1.1`

`baker::eval_request(ctx, line, sink)` evaluates one line from a tool (a console client, an
editor, an agent's bridge) and writes one line of JSON. A tool sends a request, reads one
line back, and never parses Baker text unless it wants to.

```cpp
std::string reply;
baker::eval_request(ctx, "(7 {config.source player.speed})", baker::string_out(reply));
// {"schema":"baker.reply/1.1","id":7,"ok":true,
//  "text":"(source (layer default) (file \"default.dta\") (line 3))",
//  "value":{"layer":"default","file":"default.dta","line":3}}
```

(The reply is one line; it is wrapped here to fit.)

## Requests

- **`(ID {command})`** evaluates the command. `ID` is an int, a symbol or a string, and the
  reply echoes it, so a client can match replies to requests.
- **Any other line** is evaluated whole, as `ctx.eval(line)` would evaluate it, and the
  reply's `id` is `null`.
- **A line that does not parse** gets an error reply with a `null` id. `eval_request` never
  fails and never throws.

Requests are Baker text, so a tool builds them as it would build any command. Baker reads
no JSON.

## The reply

| Field | Type | Contents |
|---|---|---|
| `schema` | string | `"baker.reply/1.1"` |
| `id` | number, string, or `null` | the request's ID |
| `ok` | bool | whether the command succeeded |
| `text` | string | on success: the result, printed as Baker text. **The authority**: it reads back as the same value. |
| `value` | any | on success: the result projected to JSON (below). Lossy by design. |
| `error` | object | on failure: `code`, `file`, `line`, `column` (1-based, in characters; 0 if unknown; 1.1), `path`, `message`, and `stack` |

`error.stack` lists the script calls the error unwound through, innermost first. Each one
has `function`, `on` (the object it was sent to, or `""`), `file` and `line`.

## The projection

`value` follows these rules, in order:

1. An int or a float is a number. A float that is not finite is `null`.
2. A symbol or a string is a string.
3. A **keyed array** is an object. That is an array whose children are all arrays headed by
   distinct symbols, after an optional leading symbol, which is dropped.
   - Each child becomes a member named by its tag.
   - A child with nothing after its tag is `null`, one value is that value, and several
     are an array.
   - A child that is itself keyed is an object.

   So `(source (layer default) (line 3))` is `{"layer":"default","line":3}`, and
   `(cfg (box (width 2) (height 1)))` is `{"box":{"width":2,"height":1}}`.
4. Any other array is an array of its children, its tag included:
   `(phase 10 20.5 x)` is `["phase",10,20.5,"x"]`. So is a list of entries,
   `(((layer a) (n 1)) ((layer b) (n 2)))`, which keeps its order:
   `[{"layer":"a","n":1},{"layer":"b","n":2}]`.
5. Anything else (a command, a variable, a function, an object) is its printed text as a
   string.

The projection cannot tell a symbol from a string, or `(a (b 1))` from `(b 1)`. When that
matters, read `text`.

## Stability

- One reply is one line, ending in a newline. Strings escape `"`, `\` and control
  characters (`\n`, `\t`, `\u00XX`); other characters are written as they are.
- Versioning follows `check`'s (`check-json.md`): a field is only added with a new minor
  version, and removed, renamed or changed only with a new major version.
