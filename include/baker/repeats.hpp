#pragma once

// The repetition report: subtrees that appear
// again and again across the trees given, which are candidates for a word of their own
// (a macro, a script function, a shape): the harness tree's "third time" rule, computed
// rather than noticed.
//
//   auto repeats = baker::find_repeats(trees);   // List<Repeat>
//
// Arrays, commands, and properties are compared structurally (node types and values,
// recursively), after macro expansion and ignoring file and line. Reported: every subtree
// of at least `min_nodes` nodes, counted recursively, that appears at least `min_repeats`
// times, with every location. A repeated subtree is not reported again inside a reported
// one at the same places. A macro's expansions share its arrays, and one array
// counts once wherever it is spliced: a macro is a word already made.
//
// A tool, not a runtime path: it hashes every subtree, and allocates freely.

#include <baker/array.hpp>
#include <baker/list.hpp>
#include <baker/node.hpp>
#include <baker/symbol.hpp>

#include <cstdint>
#include <span>

namespace baker {

struct RepeatOptions {
    uint32_t min_nodes = 6;    // the smallest subtree reported, in nodes: (price 12.75) is 2
    uint32_t min_repeats = 3;  // the fewest places it must appear; 0: no report
};

struct RepeatSite {
    Symbol file;
    int32_t line = 0;
};

struct Repeat {
    Node form;               // one occurrence: an Array, Command, or Property node
    uint32_t nodes = 0;      // its size, counted recursively
    List<RepeatSite> sites;  // every place it appears, by file, then line
};

// Ordered by first site (file, then line), then by size, largest first.
[[nodiscard]] List<Repeat> find_repeats(std::span<const Array* const> trees,
                                        const RepeatOptions& options = {});
[[nodiscard]] List<Repeat> find_repeats(const Array& tree, const RepeatOptions& options = {});

}  // namespace baker
