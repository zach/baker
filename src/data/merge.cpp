
#include "merge.hpp"
#include "arena.hpp"
#include "common/containers.hpp"

#include <algorithm>
#include <bit>

namespace baker {
namespace detail {

Marker verb_in(const Array& a) noexcept {
    if (a.size() < 2) return Marker::None;
    const Marker m = marker_of(a[1]);
    if (m == Marker::None) return m;
    const auto tag = a[0].try_sym();
    if (!tag || tag->empty()) return Marker::None;
    const size_t values = a.meta_first() ? a.meta_first() : a.size();  // the tag's too
    if (m == Marker::Remove ? values != 2 : values < 3) return Marker::None;
    return m;
}

// A hash that agrees with structurally_equal: equal nodes hash the same (floats by their bits).
uint64_t structural_hash(const Node& n, uint64_t h) noexcept {
    constexpr uint64_t k = 0x9E3779B97F4A7C15ull;
    const auto mix = [&](uint64_t v) { h = std::rotl((h ^ v) * k, 27); };
    mix(static_cast<uint64_t>(n.type()) + 1);
    switch (n.type()) {
        case Type::Int: mix(static_cast<uint64_t>(*n.try_int())); break;
        case Type::Float: mix(n.float_bits()); break;
        case Type::Symbol: mix(n.try_sym()->hash()); break;
        case Type::Var: mix(n.var_name().hash()); break;
        case Type::String: mix(symbol_hash(n.string()->view())); break;
        case Type::Array:
        case Type::Command:
        case Type::Property:
            mix(n.array()->size());
            for (const Node& c : *n.array()) h = structural_hash(c, h);
            break;
        default: break;  // a function or an object: the compare decides
    }
    return h;
}

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

// `a` without its verbs, at any depth: what a layer adds to the stack keeps none. A
// (key #remove) or (key #drop ...) is left out (null), and (key #append v...) becomes
// (key v...); pins stay, as metadata. `a` itself when nothing changes. Only an array with
// has_marker() needs it.
Ref<Array> without_markers(const Array& a, Arena* arena) {
    const Marker verb = verb_of(a);
    if (verb == Marker::Remove || verb == Marker::Drop) return Ref<Array>();
    Vec<Node> kept;
    kept.reserve(a.size());
    bool changed = verb == Marker::Append;
    for (size_t i = 0; i < a.size(); ++i) {
        if (i == 1 && verb == Marker::Append) continue;
        const Array* c = a[i].type() == Type::Array ? a[i].array() : nullptr;
        if (c && c->has_marker()) {
            Ref<Array> clean = without_markers(*c, arena);
            changed = changed || clean.get() != c;
            if (clean) kept.emplace_back(std::move(clean));
            continue;
        }
        kept.push_back(a[i]);
    }
    if (!changed) return Ref<Array>(const_cast<Array*>(&a));
    return arena ? arena->make_array(kept, a.file(), a.line())
                 : Array::make_from(kept, a.file(), a.line());
}

// Where `dst`'s metadata begins, from `first`: an appended value goes before it.
size_t meta_start(const Vec<Node>& dst, size_t first) noexcept {
    while (first < dst.size() && !tag_of(dst[first]).is_meta()) ++first;
    return first;
}

// (key #append v...) or (key #drop v...) applied to dst's value run from `dst_first`: the
// operands go after the run, before any metadata, or each removes the first node of the run
// structurally equal to it (a tuple is restated as written). Spans move with their nodes.
// Linear in the run and the operands: the run's end is found once, appends go in at once,
// and a drop matches each node of the run against the operands through their hashes, which
// removes the first c nodes equal to a value that c operands name, as dropping them one at
// a time would. One at a time was quadratic: 16,000 operands took 0.8 s.
bool apply_verb(Vec<Node>& dst, Vec<SourceSpan>* spans, size_t dst_first, const Array& src,
                Marker verb, SourceMap* map, Arena* arena) {
    const size_t end = src.meta_first() ? src.meta_first() : src.size();
    if (end <= 2) return false;
    const size_t run = meta_start(dst, dst_first);
    if (verb == Marker::Append) {
        // Usually no operand holds a verb: the operands go in as they are, in one insert.
        bool plain = true;
        for (size_t j = 2; j < end && plain; ++j)
            if (const Array* op = src[j].type() == Type::Array ? src[j].array() : nullptr)
                plain = !op->has_marker();
        if (plain && !spans) {
            const Node* from = src.nodes().data();
            dst.insert(dst.begin() + static_cast<ptrdiff_t>(run), from + 2, from + end);
            return true;
        }
        Vec<Node> adds;
        Vec<SourceSpan> add_spans;
        adds.reserve(end - 2);
        for (size_t j = 2; j < end; ++j) {
            Node n = src[j];
            if (const Array* op = src[j].type() == Type::Array ? src[j].array() : nullptr;
                op && op->has_marker()) {  // an operand holding a verb is resolved too
                Ref<Array> clean = without_markers(*op, arena);
                if (!clean) continue;
                n = Node(std::move(clean));
            }
            adds.push_back(std::move(n));
            if (spans) {
                const SourceSpan* sp = map->span(src, j);
                add_spans.push_back(sp ? *sp : SourceSpan{});
            }
        }
        if (adds.empty()) return false;
        // A pointer range, as EASTL takes (it refuses move iterators): the rare path, so a copy.
        dst.insert(dst.begin() + static_cast<ptrdiff_t>(run), adds.data(),
                   adds.data() + adds.size());
        if (spans)
            spans->insert(spans->begin() + static_cast<ptrdiff_t>(run), add_spans.data(),
                          add_spans.data() + add_spans.size());
        return true;
    }
    // The operands by hash, each used once.
    struct Operand {
        uint64_t hash;
        uint32_t index;
        bool used;
    };
    Vec<Operand> ops;
    ops.reserve(end - 2);
    for (size_t j = 2; j < end; ++j)
        ops.push_back({structural_hash(src[j]), static_cast<uint32_t>(j), false});
    std::sort(ops.begin(), ops.end(), [](const Operand& a, const Operand& b) {
        return a.hash != b.hash ? a.hash < b.hash : a.index < b.index;
    });
    size_t kept = dst_first;
    for (size_t i = dst_first; i < run; ++i) {
        const uint64_t h = structural_hash(dst[i]);
        auto it = std::lower_bound(ops.begin(), ops.end(), h,
                                   [](const Operand& o, uint64_t v) { return o.hash < v; });
        bool drop = false;
        for (; it != ops.end() && it->hash == h; ++it)
            if (!it->used && structurally_equal(dst[i], src[it->index])) {
                it->used = true;
                drop = true;
                break;
            }
        if (drop) continue;
        if (kept != i) {
            dst[kept] = std::move(dst[i]);
            if (spans) (*spans)[kept] = (*spans)[i];
        }
        ++kept;
    }
    if (kept == run) return false;
    dst.erase(dst.begin() + static_cast<ptrdiff_t>(kept),
              dst.begin() + static_cast<ptrdiff_t>(run));
    if (spans)
        spans->erase(spans->begin() + static_cast<ptrdiff_t>(kept),
                     spans->begin() + static_cast<ptrdiff_t>(run));
    return true;
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
    // A verb applies to the value run below in place of the Overwrite rule; its operands are
    // not keys, so only its metadata merges key by key after it.
    size_t from = src_first;
    const Marker verb = src_first == 1 ? verb_of(src) : Marker::None;
    if (policy == MergePolicy::Overwrite && (verb == Marker::Append || verb == Marker::Drop)) {
        changed = apply_verb(dst, spans, dst_first, src, verb, map, arena);
        from = src.meta_first() ? src.meta_first() : src.size();
    } else if (policy == MergePolicy::Overwrite && has_values(src, src_first)) {
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
    const bool indexed = src.size() - from > kIndexed;
    Index index{level.keys, level.slots};
    const bool aliased = scratch.aliases && policy == MergePolicy::Overwrite;
    if (indexed) {
        index.reset(dst.size() - dst_first + src.size() - from);
        for (size_t i = dst_first; i < dst.size(); ++i)
            if (const Array* da = dst[i].try_array())
                if (const Symbol t = da->tag(); !t.empty()) index.add(t, i);
        // The aliases too, after every key, so a key wins: one pass over the
        // existing children, where a scan per missed key would be quadratic in a layer
        // that adds many.
        if (aliased)
            for (size_t i = dst_first; i < dst.size(); ++i) {
                const Array* da = dst[i].try_array();
                const Array* aka = da && da->meta_first() ? da->meta(aka_tag()) : nullptr;
                if (!aka) continue;
                for (const Node& old : aka->values())
                    if (const auto o = old.try_sym(); o && !o->empty()) index.add(*o, i);
            }
    }
    bool removes = false;  // a (key #remove) child: applied after the merge, below
    for (size_t j = from; j < src.size(); ++j) {
        const Array* sa = src[j].try_array();
        if (!sa) continue;
        const Symbol tag = sa->tag();
        if (tag.empty() && policy == MergePolicy::Overwrite) continue;  // a value, handled above
        const Marker v = verb_of(*sa);
        if (v == Marker::Remove) {  // #merge fills in; only a layer removes or composes
            removes = removes || policy == MergePolicy::Overwrite;
            continue;
        }
        if (v != Marker::None && policy == MergePolicy::FillMissing) continue;
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
        // A key the stack lacks may be a renamed record's old one: it merges into the
        // record that still answers to it, (@aka old), which keeps its new key.
        bool via_alias = false;
        if (indexed && aliased && found != dst.size()) {
            via_alias = dst[found].try_array()->tag() != tag;
        } else if (found == dst.size() && !tag.empty() && aliased && !indexed) {
            for (size_t i = dst_first; i < dst.size() && !via_alias; ++i) {
                const Array* da = dst[i].try_array();
                const Array* aka = da && da->meta_first() ? da->meta(aka_tag()) : nullptr;
                if (!aka) continue;
                for (const Node& old : aka->values())
                    if (old.is(tag)) {
                        found = i;
                        via_alias = true;
                        break;
                    }
            }
        }
        if (found == dst.size()) {
            Node added = src[j];
            if (policy == MergePolicy::Overwrite && sa->has_marker()) {
                // A key new to the stack is resolved against nothing: #drop and #remove add
                // nothing, #append adds its operands, and no verb reaches the tree.
                Ref<Array> clean = without_markers(*sa, arena);
                if (!clean) continue;
                added = Node(std::move(clean));
            }
            if (!tag.empty() && indexed) index.add(tag, dst.size());
            dst.push_back(std::move(added));
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
        if (!map && !via_alias && same_nodes(merged, *sa)) {
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
                for (size_t j = from; j < src.size() && !gone; ++j) {
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
