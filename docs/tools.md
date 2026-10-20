# Baker and your tools

Baker is a library. It knows what your data means: where each value came from, whether a
script call is valid, how to write a tuned value back to its line. It does not touch your
operating system. Your game (the *host*) decides when to reload, which tools to expose, and
how they travel, and external tools (an editor, an agent, a live-ops service) talk to your
game through what the two publish. This page says which part does what.

- [Three parts](#three-parts)
- [What Baker gives tools](#what-baker-gives-tools)
- [What the host does](#what-the-host-does)
- [How an external tool talks to a game](#how-an-external-tool-talks-to-a-game)

## Three parts

| | Part | Holds | Never |
|---|---|---|---|
| 1 | **Baker** | What needs Baker's rules: provenance, checking, write-back, the JSON documents, the project file | Opens a file or socket itself, starts a thread to watch, draws UI, decides policy |
| 2 | **Your game** | The operating system (files, sockets, windows, the command line), your own script functions, and policy | Reimplements Baker's rules |
| 3 | **External tools** | A user interface, a protocol, another product | Links your game's code, or reimplements Baker's rules |

To place a feature: if a second host would have to copy Baker's rules to build it, it
belongs in Baker. If it needs the operating system, your game's code, or a choice about
what is allowed, it belongs in your game. Anything else is the external tool's own.

## What Baker gives tools

Baker reads through your `LoadSession` reader and writes through your Config's writer, so
everything below works over a virtual file system, an editor's unsaved buffers, or a
network store. Nothing here costs anything until you call it.

| Available | What |
|---|---|
| **Always**, shipping builds included | Reading and provenance (`source`, `explain`), `check` and its JSON, the reference JSON, the project file (`read_project`, `open_project`, `find_project`), `stack_files()`, `Context::declare`, `script_functions`, `macro_definitions`, received layers checked under a `LayerPolicy` |
| **Development builds** | Live edits (`Config::set`, `Tweak<T>::set`), `reload()`, layer toggles, the scratch layer and its journal, `loop_stats()`, snapshots |
| **When you ask** (`bind_config_tools(ctx, cfg, {.allow_write = true})`), development builds | Writing a live edit back to its file: `write_back`, `write_back_to`, `promote` |

`bind_config_tools` makes the Config's tools script functions (`config.explain`,
`config.set`, ...), binding only those the build allows, and `eval_request` turns one line
of text into one line of JSON (see [the reply format](reply-json.md)). Together they let any
transport you choose carry Baker's tools: a socket, a pipe, an in-game console.

## What the host does

- **Watch files.** List what to watch with `cfg.stack_files()`. Check
  `cfg.stack_files_epoch()` once a frame, and re-read the list when it moves. When a file
  changes, call `cfg.reload({.origin_ns = save_time})`, so that `loop_stats()` measures
  edit-to-see from the save.
- **Carry requests.** A console server reads lines and answers with `eval_request`.
  `examples/remote-console` is one, on 127.0.0.1. Keep it on loopback: it runs any command
  it is sent.
- **Offer commands** that need your game's own script functions: a check of the data, the
  reference (`write_reference_json` after your setup), a headless run. Only your game
  binary knows its functions, so these commands live in it.
- **Decide policy:**
  - which tools to bind, and whether to allow writes;
  - when a layer may change (`freeze`, `live_layers`);
  - which layers a check or a simulation leaves out (`ProjectOptions::skip`).

## How an external tool talks to a game

- **Files.** The project file (`baker-project.dta`) gives the stack, and names the
  reference JSON your build writes (`(reference "build/reference.json")`).
- **Documents.** [The reference](reference-json.md), [check's report](check-json.md) and
  [the reply](reply-json.md) are versioned JSON. A field is added only with a new minor
  version.
- **The console.** One request per line, one JSON reply per line, to a running game.
- **Commands.** Your game binary's own commands, run by the tool.

A tool that checks data without the game reads the project file, then the reference, and
declares the game's functions from it (`Context::declare`). It then runs `check` over the
same stack the game loads.
