#pragma once

// A Config's tweaks by path, so that a change rebinds only those below what it changed.
//
// The tweaks' paths form a tree of their leading names; each node keeps the array the last
// pass found at its path, in the root the index holds (so no array it names is freed and
// its address taken again). The merge shares every array it did not rebuild, so where the
// new root has the same array, every tweak below binds as it did, and the pass skips it. A
// changed array's children are found by place: a child at the place it had, with no
// changed child before it that has its tag, is the same one. The tweaks under a changed
// array, and those whose last bind warned (which warn at every change, as before), bind
// in the registry's order, as the full pass binds them.

#include <baker/array.hpp>
#include <baker/callback.hpp>
#include <baker/tweak.hpp>

#include "common/containers.hpp"

namespace baker::detail {

class TweakIndex {
public:
    explicit TweakIndex(const Allocator& alloc);

    // Rebinds `reg`'s tweaks against `root`, appending those whose value changed to
    // `moved`, in the registry's order. Indexes them again first when the registry has
    // changed, or another index bound them, since this one's last pass.
    void rebind(TweakRegistry& reg, const Ref<Array>& root,
                const Callback<void(const Error&)>& warn, Vec<TweakBase*>& moved);

    // `t` was bound outside a pass (a live edit's retune): the next pass binds it again.
    // Under `reg`'s lock.
    void touched(TweakRegistry& reg, const TweakBase& t) noexcept;

private:
    static constexpr uint32_t kNone = UINT32_MAX;
    struct Entry {
        TweakBase* tweak = nullptr;
        uint32_t order = 0;  // in the registry
        uint32_t node = 0;
        uint32_t names = 0;  // its path's leading name segments: its node's depth
        bool loud = false;   // its last bind warned
        bool queued = false;
    };
    struct PathNode {
        Symbol name;
        uint32_t parent = kNone;
        uint32_t kids_lo = 0, kids_hi = 0;      // its children, in nodes_
        uint32_t tweaks_lo = 0, tweaks_hi = 0;  // its tweaks, in entries_
        const Array* seen = nullptr;            // its array in root_, or null
        uint32_t at = 0;                        // that array's place in its parent's
        bool stale = false;                     // a tweak at or below it bound outside a pass
        const Array* next = nullptr;            // during a visit of its parent: its new array
    };

    void index(TweakRegistry& reg, const Array& root, const Callback<void(const Error&)>& warn,
               Vec<TweakBase*>& moved);
    void build(uint32_t node, uint32_t lo, uint32_t hi, uint32_t depth);
    void find_all(uint32_t node, const Array* arr);
    void visit(uint32_t node, const Array* arr);
    void queue(uint32_t entry);
    void tabulate(const Array& arr);
    [[nodiscard]] uint32_t look_up(Symbol tag) const noexcept;

    uint64_t id_;
    Ref<Array> root_;
    Vec<Entry> entries_;
    Vec<PathNode> nodes_;
    Vec<uint32_t> binds_;  // this pass's, reserved for every tweak: a pass allocates nothing
    Vec<uint32_t> loud_;
    // A changed array's tags, for a visit that cannot find its children by place.
    Vec<const void*> table_keys_;
    Vec<uint32_t> table_slots_;
    size_t widest_ = 0;
};

}  // namespace baker::detail
