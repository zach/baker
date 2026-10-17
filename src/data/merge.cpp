
#include "merge.hpp"
#include "arena.hpp"
#include "common/containers.hpp"

namespace baker {
namespace detail {
namespace {

bool is_tagged(const Node& n) noexcept {
    return !tag_of(n).empty();
}
// Whether `a` has values from `first` on: nodes other than tagged arrays.
bool has_values(const Array& a, size_t first) noexcept {
    for (size_t i = first; i < a.size(); ++i)
        if (!is_tagged(a[i])) return true;
    return false;
}

// Whether `merged`, a rebuilt array's nodes, is `src` node for node: then `src` itself is
// the result, and nothing need be built. Most rebuilds in a stack of layers are so: a
// layer that sets every key of a section.
bool same_nodes(const Vec<Node>& merged, const Array& src) noexcept {
    if (merged.size() != src.size()) return false;
    for (size_t i = 0; i < merged.size(); ++i) {
        const Node& x = merged[i];
        const Node& y = src[i];
        if (x.type() != y.type()) return false;
        if (const Array* a = x.array()) {
            if (a != y.array()) return false;
        } else if (const String* str = x.string()) {
            if (str != y.string()) return false;
        } else if (!structurally_equal(x, y)) {
            return false;
        }
    }
    return true;
}

// `a` without its (key #remove) children, at any depth: what a layer adds to the stack keeps
// none. Only an array with has_removal() needs it.
Ref<Array> without_removals(const Array& a, Arena* arena) {
    Vec<Node> kept;
    kept.reserve(a.size());
    for (const Node& n : a) {
        const Array* c = n.type() == Type::Array ? n.array() : nullptr;
        if (c && is_removal(*c)) continue;
        if (c && c->has_removal())
            kept.emplace_back(without_removals(*c, arena));
        else
            kept.push_back(n);
    }
    return arena ? arena->make_array(kept, a.file(), a.line())
                 : Array::make_from(kept, a.file(), a.line());
}

// The index of a wide array's tags, in a level's tables: the first child with each tag.
struct Index {
    Vec<const void*>& keys;
    Vec<uint32_t>& slots;

    void reset(size_t n) {
        size_t cap = 64;
        while (cap < n * 2) cap *= 2;
        keys.assign(cap, nullptr);
        slots.resize(cap);
    }
    [[nodiscard]] size_t probe(Symbol tag) const noexcept {
        const size_t mask = keys.size() - 1;
        size_t i = static_cast<size_t>(tag.hash()) & mask;
        while (keys[i] && keys[i] != tag.id()) i = (i + 1) & mask;
        return i;
    }
    void add(Symbol tag, size_t at) {  // keeps the first
        if ((count + 1) * 2 > keys.size()) grow();
        const size_t i = probe(tag);
        if (keys[i]) return;
        keys[i] = tag.id();
        slots[i] = static_cast<uint32_t>(at);
        ++count;
    }
    [[nodiscard]] size_t find(Symbol tag, size_t none) const noexcept {
        const size_t i = probe(tag);
        return keys[i] ? slots[i] : none;
    }
    void grow() {
        Vec<const void*> old_keys(keys);
        Vec<uint32_t> old_slots(slots);
        keys.assign(keys.size() * 2, nullptr);
        slots.resize(keys.size());
        for (size_t i = 0; i < old_keys.size(); ++i)
            if (old_keys[i]) {
                const size_t j = probe(Symbol::from_id(old_keys[i]));
                keys[j] = old_keys[i];
                slots[j] = old_slots[i];
            }
    }
    size_t count = 0;
};

}  // namespace

MergeScratch::Level& MergeScratch::level(size_t depth) {
    while (levels_.size() <= depth) levels_.push_back(Own<Level>::make(alloc_, alloc_));
    return *levels_[depth];
}

// The GDC 2005 slide shows (a (b 1)) merged with (a (b 2) (c 2)) giving (a (b 1) (c 2)).
// The spec took that for a typo; for #merge it is not, and for Config layers the leaves
// win. Both policies are here.
bool merge_into(Vec<Node>& dst, Vec<SourceSpan>* spans, size_t dst_first, const Array& src,
                size_t src_first, MergePolicy policy, SourceMap* map, MergeScratch& scratch,
                Arena* arena) {
    struct Depth {
        MergeScratch& s;
        explicit Depth(MergeScratch& m) : s(m) { ++s.depth_; }
        ~Depth() { --s.depth_; }
    } depth(scratch);
    MergeScratch::Level& level = scratch.level(scratch.depth_);
    bool changed = false;
    if (policy == MergePolicy::Overwrite && has_values(src, src_first)) {
        // The incoming values replace the existing ones; tagged children stay to merge. In
        // place: the tagged children are set aside in the level, which `dst` may not share
        // an allocator with. Spans move with their nodes.
        Vec<Node>& tagged = level.nodes;
        Vec<SourceSpan>& tagged_spans = level.spans;
        tagged.clear();
        tagged_spans.clear();
        for (size_t i = dst_first; i < dst.size(); ++i)
            if (is_tagged(dst[i])) {
                tagged.push_back(std::move(dst[i]));
                if (spans) tagged_spans.push_back((*spans)[i]);
            }
        dst.resize(dst_first);
        if (spans) spans->resize(dst_first);
        for (size_t j = src_first; j < src.size(); ++j)
            if (!is_tagged(src[j])) {
                dst.push_back(src[j]);
                if (spans) {
                    const SourceSpan* sp = map->span(src, j);
                    spans->push_back(sp ? *sp : SourceSpan{});
                }
            }
        for (size_t k = 0; k < tagged.size(); ++k) {
            dst.push_back(std::move(tagged[k]));
            if (spans) spans->push_back(tagged_spans[k]);
        }
        tagged.clear();
        tagged_spans.clear();
        changed = true;
    }
    // Each incoming key is found by scanning the existing ones, which is fastest for the
    // small arrays config is made of; past kIndexed incoming keys, an index of the first
    // child with each tag keeps a large flat layer linear.
    constexpr size_t kIndexed = 32;
    const bool indexed = src.size() - src_first > kIndexed;
    Index index{level.keys, level.slots};
    if (indexed) {
        index.reset(dst.size() - dst_first + src.size() - src_first);
        for (size_t i = dst_first; i < dst.size(); ++i)
            if (const Array* da = dst[i].try_array())
                if (const Symbol t = da->tag(); !t.empty()) index.add(t, i);
    }
    bool removes = false;  // a (key #remove) child: applied after the merge, below
    for (size_t j = src_first; j < src.size(); ++j) {
        const Array* sa = src[j].try_array();
        if (!sa) continue;
        const Symbol tag = sa->tag();
        if (tag.empty() && policy == MergePolicy::Overwrite) continue;  // a value, handled above
        if (is_removal(*sa)) {  // #merge fills in; only a layer removes
            removes = removes || policy == MergePolicy::Overwrite;
            continue;
        }
        size_t found = dst.size();
        if (!tag.empty() && indexed) {
            found = index.find(tag, dst.size());
        } else if (!tag.empty()) {
            for (size_t i = dst_first; i < dst.size(); ++i) {
                const Array* da = dst[i].try_array();
                if (da && da->tag() == tag) {
                    found = i;
                    break;
                }
            }
        }
        if (found == dst.size()) {
            if (!tag.empty() && indexed) index.add(tag, dst.size());
            if (policy == MergePolicy::Overwrite && sa->has_removal())
                dst.emplace_back(without_removals(*sa, arena));
            else
                dst.push_back(src[j]);
            if (spans) {
                const SourceSpan* sp = map->span(src, j);
                spans->push_back(sp ? *sp : SourceSpan{});
            }
            changed = true;
            continue;
        }
        const Array& da = *dst[found].try_array();
        // This level's buffers are free: the Overwrite branch above has given its back.
        Vec<Node>& merged = level.nodes;
        merged.assign(da.begin(), da.end());
        Vec<SourceSpan>& merged_spans = level.spans;
        if (map) {
            const auto known = map->spans(da);
            if (known)
                merged_spans.assign(known->data(), known->data() + known->size());
            else
                merged_spans.assign(da.size(), SourceSpan{});
        }
        if (!merge_into(merged, map ? &merged_spans : nullptr, 1, *sa, 1, policy, map, scratch,
                        arena)) {
            merged.clear();
            continue;
        }
        if (!map && same_nodes(merged, *sa)) {
            dst[found] = src[j];  // the incoming array is the result: share it
        } else {
            // A rebuilt array is located where its values came from.
            const bool from_src = policy == MergePolicy::Overwrite && has_values(*sa, 1);
            const Array& at = from_src ? *sa : da;
            Ref<Array> rebuilt = arena ? arena->make_array(merged, at.file(), at.line())
                                       : Array::make_from(merged, at.file(), at.line());
            if (map) {
                const SourceSpan* ext = map->extent(da);
                map->record(rebuilt, ext ? *ext : SourceSpan{}, merged_spans);
            }
            dst[found] = Node(std::move(rebuilt));
        }
        merged.clear();
        changed = true;
    }
    if (removes) {
        // Each removed key's children leave dst, with their spans; after the loop, so the
        // index above stayed valid. A key the layers below lack removes nothing.
        size_t out = dst_first;
        for (size_t i = dst_first; i < dst.size(); ++i) {
            const Array* da = dst[i].try_array();
            const Symbol t = da ? da->tag() : Symbol();
            bool gone = false;
            if (!t.empty())
                for (size_t j = src_first; j < src.size() && !gone; ++j) {
                    const Array* sa = src[j].try_array();
                    gone = sa && is_removal(*sa) && sa->tag() == t;
                }
            if (gone) {
                changed = true;
                continue;
            }
            if (out != i) {
                dst[out] = std::move(dst[i]);
                if (spans) (*spans)[out] = (*spans)[i];
            }
            ++out;
        }
        dst.resize(out);
        if (spans) spans->resize(out);
    }
    return changed;
}

Ref<Array> merge(const Array& base, const Array& over, MergePolicy policy, MergeScratch& scratch) {
    Vec<Node> nodes;
    nodes.reserve(base.size() + over.size());  // at most: one allocation, not log n
    nodes.assign(base.begin(), base.end());
    if (!merge_into(nodes, nullptr, 0, over, 0, policy, nullptr, scratch))
        return Ref<Array>(const_cast<Array*>(&base));
    return Array::make_from(nodes, base.file(), base.line());
}

}  // namespace detail

Ref<Array> merge(const Array& base, const Array& over, MergePolicy policy) {
    detail::MergeScratch scratch;
    return detail::merge(base, over, policy, scratch);
}

}  // namespace baker
