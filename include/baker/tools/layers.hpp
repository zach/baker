#pragma once

// What each layer of a Config contributes, for tools: an editor's hover, an agent asking
// where a value comes from, an operator asking what a layer would change. Part of
// baker::tools, which a game links only with the host kit.

#include <baker/config.hpp>

namespace baker::tools {

struct DiffEntry {
    Symbol path;
    // Each layer's values, one node each, as LayerEffectEntry::values; empty: that layer
    // does not set this path.
    std::optional<RowValues> a, b;
    std::optional<Source> source_a, source_b;
    Symbol op_a, op_b;  // each row's verb, as LayerEffectEntry::op
    Symbol declares;    // a declaration's directive, as LayerEffectEntry::declares
    uint32_t folded_a = 0, folded_b = 0;  // each row's, as LayerEffectEntry::folded
};

// How one contributor to a path's value stands in the merged value (explain).
enum class Standing : uint8_t {
    Wins,      // supplies the value get() returns
    Shadowed,  // supplies a value that a higher contributor overrides or removes
    Removes,   // (key #remove) at the path or an ancestor: below it, the key is gone
    Disabled,  // a disabled layer (or scratch) that has the path: what enabling it would bring
    Composes,  // (key #append v...) or (key #drop v...): changes the value below it
};

struct ExplainEntry {
    Symbol layer{};      // a layer's name, "scratch", or "code" for a tweak's default
    RowValues values{};  // as LayerEffectEntry::values; the symbol #remove for Removes
    Source source{};
    Standing standing = Standing::Wins;
    Symbol op{};  // the row's verb, as LayerEffectEntry::op
    // The innermost #ifdef or #ifndef branch the value was read in, if any: the define that
    // chose it (with active_defines(), who made it).
    std::optional<ConditionalSection> section{};
    // The layer's condition (Config::layer_condition), if it has one: why a Disabled layer
    // is out of the merge, or what keeps an enabled one in.
    std::optional<Node> when{};
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
// A path `#shape.NAME` or `#shape.NAME.FIELD` lists each layer that declares that shape, or
// that field of it, with the declaration as the value: every enabled one Wins, since a
// shape's declarations combine, harder packs' holding.
[[nodiscard]] Expected<List<ExplainEntry>> explain(const Config& cfg, const Path& path);

}  // namespace baker::tools
