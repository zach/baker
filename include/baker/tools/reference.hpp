#pragma once

// What a game writes for its tools: the script reference, as Markdown and as JSON, the check
// report as JSON, and the data's own script functions. (The tweaks' JSON is baker::data's,
// write_json in <baker/tweak.hpp>, so a data-only program can write it.) Part of baker::tools,
// which a game links only with the host kit.

#include <baker/script.hpp>

namespace baker::tools {

// Writes the script reference: every function with its overloads, tier, status,
// and documentation, and every documented global, as Markdown. <baker/tools/std_tools.hpp>
// has reference(ctx) returning a string.
// Given the session that loaded the data, it adds the harness register: a Packs section
// (each pack's level, requires, owners, audience, stage, and file) and an Origins section
// (which tools generated which files).
void reference(const Context& ctx, TextSink out, const LoadSession* packs = nullptr);
[[nodiscard]] Expected<void> write_reference(const Context& ctx, std::string_view path,
                                             const LoadSession* packs = nullptr);
// The same as JSON (schema baker.reference/1.1, docs/reference-json.md), for tools and
// agents: every function with its overloads' parameter types, status, documentation, and,
// for a script function, its file and line; every type's methods, properties, and script
// handlers; script-only classes; documented globals; the objects added under a name, with
// their types, as they are when it is written; and, given the session, its packs and
// origins. Sorted by name, so the same Context gives the same bytes.
void write_reference_json(const Context& ctx, TextSink out, const LoadSession* packs = nullptr);

// Writes the report as one JSON object, schema "baker.check/1" (docs/check-json.md): the
// same data as write(), every list in an order that depends only on the data, so two runs
// over the same files give the same bytes. Given the session, it lists its packs and their
// harness metadata too. <baker/std_script.hpp> has to_json(report).
void write_json(const CheckReport& report, TextSink out, const LoadSession* packs = nullptr);

// The script functions that #autorun commands define with {func NAME ...}, each where it is
// written, in load order, found without running anything: the data's own vocabulary, for a
// tool that checks or completes data without the game. A {func} nested in another command,
// or made by a macro, counts; one defined twice is listed twice. Give it a session's
// commands, or a Config's, which keeps each enabled file layer's apart from its sessions:
//   script_functions(session.autorun());
//   script_functions(cfg.autorun().items());
[[nodiscard]] List<Definition> script_functions(std::span<const Node> autorun);

}  // namespace baker::tools
