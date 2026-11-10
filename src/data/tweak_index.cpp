#include "tweak_index.hpp"

#include <algorithm>
#include <atomic>
#include <functional>

namespace baker::detail {

namespace {

uint32_t leading_names(const Path& p) noexcept {
    uint32_t n = 0;
    while (n < p.size() && !p[n].is_index) ++n;
    return n;
}

// The place of `child` among `arr`'s nodes: `hint` if it is there.
uint32_t place_of(const Array& arr, const Array* child, uint32_t hint) noexcept {
    if (hint < arr.size() && arr[hint].try_array() == child) return hint;
    for (uint32_t i = 0; i < arr.size(); ++i)
        if (arr[i].try_array() == child) return i;
    return 0;
}

}  // namespace

TweakIndex::TweakIndex(const Allocator& alloc)
    : entries_(alloc),
      nodes_(alloc),
      binds_(alloc),
      loud_(alloc),
      table_keys_(alloc),
      table_slots_(alloc) {
    static std::atomic<uint64_t> ids{0};
    id_ = ids.fetch_add(1, std::memory_order_relaxed) + 1;
}

void TweakIndex::rebind(TweakRegistry& reg, const Ref<Array>& root,
                        const Callback<void(const Error&)>& warn, Vec<TweakBase*>& moved) {
    reg.lock();
    if (reg.binder_ != id_ || !root_) {
        index(reg, *root, warn, moved);
        reg.binder_ = id_;
    } else {
        binds_.clear();
        visit(0, root.get());
        for (const uint32_t e : loud_) queue(e);
        std::sort(binds_.begin(), binds_.end(),
                  [this](uint32_t a, uint32_t b) { return entries_[a].order < entries_[b].order; });
        loud_.clear();
        for (const uint32_t i : binds_) {
            Entry& e = entries_[i];
            e.queued = false;
            bool warned = false;
            if (e.tweak->bind(*root, warn, &warned)) moved.push_back(e.tweak);
            e.loud = warned;
            if (warned) loud_.push_back(i);
        }
    }
    root_ = root;
    reg.unlock();
}

void TweakIndex::touched(TweakRegistry& reg, const TweakBase& t) noexcept {
    if (reg.binder_ != id_) {
        reg.binder_ = 0;  // another index's tweaks: it indexes them again
        return;
    }
    if (t.slot_ >= entries_.size() || entries_[t.slot_].tweak != &t) {
        reg.binder_ = 0;
        return;
    }
    for (uint32_t n = entries_[t.slot_].node; n != kNone && !nodes_[n].stale; n = nodes_[n].parent)
        nodes_[n].stale = true;
}

// Every tweak bound in the registry's order, as a full rebind does, then indexed.
void TweakIndex::index(TweakRegistry& reg, const Array& root,
                       const Callback<void(const Error&)>& warn, Vec<TweakBase*>& moved) {
    entries_.clear();
    nodes_.clear();
    uint32_t order = 0;
    for (TweakBase* t = reg.head_; t; t = t->next_) {
        bool warned = false;
        if (t->bind(root, warn, &warned)) moved.push_back(t);
        entries_.push_back({t, order++, 0, leading_names(t->parsed()), warned, false});
    }
    // By path, a parent's tweaks before its children's: each node's tweaks, and each node's
    // subtree, are then one run.
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        const Path& x = a.tweak->parsed();
        const Path& y = b.tweak->parsed();
        for (uint32_t i = 0; i < a.names && i < b.names; ++i)
            if (x[i].name != y[i].name) return std::less<>()(x[i].name.id(), y[i].name.id());
        return a.names != b.names ? a.names < b.names : a.order < b.order;
    });
    nodes_.push_back({Symbol(), kNone, 0, 0, 0, 0, nullptr, 0, false});
    build(0, 0, static_cast<uint32_t>(entries_.size()), 0);
    loud_.clear();
    for (uint32_t i = 0; i < entries_.size(); ++i) {
        entries_[i].tweak->slot_ = i;
        if (entries_[i].loud) loud_.push_back(i);
    }
    binds_.clear();
    binds_.reserve(entries_.size());
    loud_.reserve(entries_.size());
    widest_ = 0;
    find_all(0, &root);
    // Room for the widest array found now, with a quarter to spare as it grows.
    size_t cap = 16;
    while (cap < (widest_ + widest_ / 4) * 2) cap *= 2;
    table_keys_.reserve(cap);
    table_slots_.reserve(cap);
}

// The node for entries_[lo, hi), which share `depth` leading names, and the nodes below.
void TweakIndex::build(uint32_t node, uint32_t lo, uint32_t hi, uint32_t depth) {
    uint32_t i = lo;
    while (i < hi && entries_[i].names == depth) entries_[i++].node = node;
    nodes_[node].tweaks_lo = lo;
    nodes_[node].tweaks_hi = i;
    const auto kids = static_cast<uint32_t>(nodes_.size());
    while (i < hi) {
        const Symbol name = entries_[i].tweak->parsed()[depth].name;
        uint32_t end = i;
        while (end < hi && entries_[end].tweak->parsed()[depth].name == name) ++end;
        // The run, kept in the tweak fields until the child is built.
        nodes_.push_back({name, node, 0, 0, i, end, nullptr, 0, false});
        i = end;
    }
    nodes_[node].kids_lo = kids;
    nodes_[node].kids_hi = static_cast<uint32_t>(nodes_.size());
    for (uint32_t k = kids; k < nodes_[node].kids_hi; ++k)
        build(k, nodes_[k].tweaks_lo, nodes_[k].tweaks_hi, depth + 1);
}

void TweakIndex::find_all(uint32_t node, const Array* arr) {
    nodes_[node].seen = arr;
    if (arr && nodes_[node].kids_hi - nodes_[node].kids_lo > 8)
        widest_ = std::max<size_t>(widest_, arr->size());
    for (uint32_t k = nodes_[node].kids_lo; k < nodes_[node].kids_hi; ++k) {
        const Array* child = arr ? arr->try_find(nodes_[k].name) : nullptr;
        nodes_[k].at = child ? place_of(*arr, child, 0) : 0;
        find_all(k, child);
    }
}

void TweakIndex::queue(uint32_t entry) {
    if (entries_[entry].queued) return;
    entries_[entry].queued = true;
    binds_.push_back(entry);
}

// `node`'s array is now `arr`, which differs from the one it saw, or a tweak below it was
// bound outside a pass: its tweaks bind, and its children are looked at.
void TweakIndex::visit(uint32_t node, const Array* arr) {
    PathNode& n = nodes_[node];
    const Array* old = n.seen;
    n.seen = arr;
    n.stale = false;
    for (uint32_t i = n.tweaks_lo; i < n.tweaks_hi; ++i) queue(i);
    if (n.kids_lo == n.kids_hi) return;
    // The tags of the arrays that differ from the old array's in the same place. A few
    // let each child be found by its place; past those, each is looked up.
    constexpr size_t kFew = 8;
    Symbol changed[kFew];
    size_t count = 0;
    bool by_place = arr && old;
    if (by_place) {
        const uint32_t both = std::min(arr->size(), old->size());
        for (uint32_t j = 0; j < both && by_place; ++j) {
            const Array* a = (*arr)[j].try_array();
            if (!a || a == (*old)[j].try_array()) continue;
            if (count == kFew)
                by_place = false;
            else
                changed[count++] = a->tag();
        }
    }
    // Past those, a table of the new array's tags: each child is found in it, not by a
    // scan of the array per child.
    const bool table = arr && !by_place && n.kids_hi - n.kids_lo > kFew;
    if (table) tabulate(*arr);
    // Every child's array first, as the table serves this node alone; then the children
    // whose array changed are visited.
    for (uint32_t k = n.kids_lo; k < n.kids_hi; ++k) {
        PathNode& kid = nodes_[k];
        const Array* found = nullptr;
        uint32_t at = 0;
        if (table) {
            at = look_up(kid.name);
            found = at == kNone ? nullptr : (*arr)[at].try_array();
            at = found ? at : 0;
        } else if (arr) {
            bool shadowed = !by_place;
            for (size_t c = 0; c < count && !shadowed; ++c) shadowed = changed[c] == kid.name;
            if (!shadowed && kid.seen && kid.at < arr->size() && kid.at < old->size() &&
                (*arr)[kid.at].try_array() == kid.seen && (*old)[kid.at].try_array() == kid.seen) {
                // In the old array it was the first with its tag; the arrays before it are
                // the old ones, or changed ones with other tags: it is the first still.
                found = kid.seen;
                at = kid.at;
            } else if (!shadowed && !kid.seen && arr->size() <= old->size()) {
                // None had its tag; nor does any array in the new one, the same or changed.
            } else {
                found = arr->try_find(kid.name);
                at = found ? place_of(*arr, found, kid.at) : 0;
            }
        }
        kid.at = at;
        kid.next = found;
    }
    for (uint32_t k = n.kids_lo; k < n.kids_hi; ++k)
        if (nodes_[k].stale || nodes_[k].next != nodes_[k].seen) visit(k, nodes_[k].next);
}

// The first place of each tag among `arr`'s arrays, as try_find finds them. The table is
// sized at index() for the widest array then, and grows only for a wider one: a change
// allocates once for each new width, as a merge's scratch does.
void TweakIndex::tabulate(const Array& arr) {
    size_t cap = 16;
    while (cap < size_t{arr.size()} * 2) cap *= 2;
    table_keys_.assign(cap, nullptr);
    table_slots_.assign(cap, 0);
    for (uint32_t i = 0; i < arr.size(); ++i) {
        const Array* a = arr[i].try_array();
        const Symbol tag = a ? a->tag() : Symbol();
        if (tag.empty()) continue;
        size_t h = static_cast<size_t>(tag.hash()) & (cap - 1);
        while (table_keys_[h] && table_keys_[h] != tag.id()) h = (h + 1) & (cap - 1);
        if (table_keys_[h]) continue;  // the first keeps it
        table_keys_[h] = tag.id();
        table_slots_[h] = i;
    }
}

uint32_t TweakIndex::look_up(Symbol tag) const noexcept {
    const size_t mask = table_keys_.size() - 1;
    size_t h = static_cast<size_t>(tag.hash()) & mask;
    while (table_keys_[h] && table_keys_[h] != tag.id()) h = (h + 1) & mask;
    return table_keys_[h] ? table_slots_[h] : kNone;
}

}  // namespace baker::detail
