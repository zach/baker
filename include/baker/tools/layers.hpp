#pragma once

// What each layer of a Config contributes, for tools: an editor's hover, an agent asking
// where a value comes from, an operator asking what a layer would change. Part of
// baker::tools, which a game links only with the host kit.

#include <baker/config.hpp>

namespace baker::tools {

struct DiffEntry {
    Symbol path;
    std::optional<Node> a, b;  // empty: that layer does not set this path
    std::optional<Source> source_a, source_b;
};

// How one contributor to a path's value stands in the merged value (explain).
enum class Standing : uint8_t {
    Wins,      // supplies the value get() returns
    Shadowed,  // supplies a value that a higher contributor overrides or removes
    Removes,   // (key #remove) at the path or an ancestor: below it, the key is gone
    Disabled,  // a disabled layer (or scratch) that has the path: what enabling it would bring
};

struct ExplainEntry {
    Symbol layer{};  // a layer's name, "scratch", or "code" for a tweak's default
    Node value{};    // packed as LayerEffectEntry::value; the symbol #remove for Removes
    Source source{};
    Standing standing = Standing::Wins;
    // The innermost #ifdef or #ifndef branch the value was read in, if any: the define that
    // chose it (with active_defines(), who made it).
    std::optional<ConditionalSection> section{};
};

// What `layer` (a layer's name, or "scratch") sets: each value, the stack's value without the
// layer, where each comes from, and whether a layer above shadows it.
[[nodiscard]] Expected<List<LayerEffectEntry>> layer_effect(const Config& cfg,
                                                            std::string_view layer);
// The paths where layers `a` and `b` (or "scratch") differ, with each one's value.
[[nodiscard]] Expected<List<DiffEntry>> diff(const Config& cfg, std::string_view a,
                                             std::string_view b);
// Every contributor to `path`'s value, bottom to top: the code default of a tweak at the
// path, each layer that holds a value there or removes it, and scratch. The one that
// Wins is the one Config::source() names. Refuses a path that holds arrays rather than values.
[[nodiscard]] Expected<List<ExplainEntry>> explain(const Config& cfg, const Path& path);

}  // namespace baker::tools
