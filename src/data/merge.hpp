#pragma once

// The one merge routine behind #merge (fill-missing) and Config layers (overwrite).

#include <baker/config.hpp>
#include <baker/source_map.hpp>

#include "common/containers.hpp"

namespace baker::detail {

class Arena;

// What a merge reuses from one call to the next: for each depth of the recursion, buffers
// for the arrays it rebuilds and an index of a wide array's tags. Once warm,
// a merge allocates only the arrays it keeps. An owner that merges often (a Config) keeps
// one on its allocator; it is not for two merges at once.
class MergeScratch {
public:
    explicit MergeScratch(const Allocator& alloc = default_allocator()) : alloc_(alloc) {}

private:
    friend bool merge_into(Vec<Node>&, Vec<SourceSpan>*, size_t, const Array&, size_t, MergePolicy,
                           SourceMap*, MergeScratch&, Arena*);
    struct Level {
        explicit Level(const Allocator& a) : nodes(a), spans(a), keys(a), slots(a) {}
        Vec<Node> nodes;        // the array being rebuilt, or tagged children set aside
        Vec<SourceSpan> spans;  // parallel to nodes, with a source map
        Vec<const void*> keys;  // the index: open addressing by symbol, power-of-two sized
        Vec<uint32_t> slots;    // where each key's array is in dst
    };
    Level& level(size_t depth);
    const Allocator& alloc_;
    Vec<Own<Level>> levels_{alloc_};  // owned one by one: a deeper level never moves one
    size_t depth_ = 0;
};

// The value of `(key #remove)`: in a Config layer, removes `key` from the layers below.
// The parser writes it only as a tagged array's one value.
[[nodiscard]] inline const Symbol& remove_marker() {
    static const Symbol s("#remove");
    return s;
}
[[nodiscard]] inline bool is_removal(const Array& a) noexcept {
    return a.size() == 2 && a[1].is(remove_marker());
}

// Merges the nodes of `src` from `src_first` into `dst` from `dst_first` under `policy`
// (see baker::merge). Arrays along the way are rebuilt, never mutated, because they may be
// shared. With a `map`, `spans` runs parallel to `dst` and is kept in step, and rebuilt
// arrays are recorded in the map. Rebuilt arrays go into `arena` if given: a #merge
// inside a load must not make solo arrays that point back into the arena being built,
// which would then count each other. Returns whether `dst` changed.
bool merge_into(Vec<Node>& dst, Vec<SourceSpan>* spans, size_t dst_first, const Array& src,
                size_t src_first, MergePolicy policy, SourceMap* map, MergeScratch& scratch,
                Arena* arena = nullptr);
// baker::merge with the caller's scratch.
[[nodiscard]] Ref<Array> merge(const Array& base, const Array& over, MergePolicy policy,
                               MergeScratch& scratch);

}  // namespace baker::detail
