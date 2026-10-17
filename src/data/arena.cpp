#include <baker/allocator.hpp>
#include <baker/array.hpp>
#include <baker/node.hpp>

#include "arena.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <type_traits>

namespace baker::detail {

static_assert(sizeof(SoloBlock) == 16 && alignof(SoloBlock) <= alignof(Node));

namespace {
constexpr size_t kMinBlock = 512;
constexpr size_t kMaxBlock = size_t{1} << 20;
constexpr size_t kAlign = alignof(Node);
constexpr size_t round_up(size_t n) noexcept {
    return (n + kAlign - 1) & ~(kAlign - 1);
}
}  // namespace

Arena* Arena::create(const Allocator& alloc, size_t expected_bytes) {
    auto* a = ::new (allocate(alloc, sizeof(Arena), alignof(Arena))) Arena(alloc);
    a->next_ = std::clamp(round_up(expected_bytes), kMinBlock, kMaxBlock);
    return a;
}

Arena* Arena::create_holding(const Allocator& alloc, size_t bytes, char*& payload) {
    const size_t self = round_up(sizeof(Arena));
    const size_t whole = self + kBlockHeader + round_up(bytes);
    char* mem = static_cast<char*>(allocate(alloc, whole, kAlign));
    auto* a = ::new (mem) Arena(alloc);
    a->next_ = kMinBlock;
    a->whole_ = whole;
    ::new (mem + self) Block{&a->head_.owner, nullptr, whole - self};
    payload = mem + self + kBlockHeader;
    return a;
}

void* Arena::take(size_t bytes, uint32_t& offset) {
    bytes = round_up(bytes);
    constexpr size_t header = kBlockHeader;
    static_assert(round_up(sizeof(Block)) == kBlockHeader);
    if (bytes > left_) {
        // A payload larger than half a block gets a block of its own, which leaves the
        // current block open.
        const bool alone = bytes > next_ / 2;
        const size_t size = alone ? header + bytes : std::max(next_, header + bytes);
        auto* b = ::new (allocate(alloc_, size, kAlign)) Block{&head_.owner, blocks_, size};
        blocks_ = b;
        char* start = reinterpret_cast<char*>(b) + header;
        if (alone) {
            offset = static_cast<uint32_t>(header);
            return start;
        }
        current_ = b;
        cur_ = start;
        left_ = size - header;
        // The first block was sized for the whole load: one more of its size, then doubling.
        if (next_ < kMaxBlock && b->next) next_ *= 2;
    }
    char* p = cur_;
    offset = static_cast<uint32_t>(p - reinterpret_cast<char*>(current_));
    cur_ += bytes;
    left_ -= bytes;
    return p;
}

void Arena::adopt(Node* slot, Node&& n, bool building) {
    const void* payload =
        n.array() ? static_cast<const void*>(n.array()) : static_cast<const void*>(n.string());
    ::new (slot) Node(std::move(n));
    if (!payload) return;
    // `n` counted its target's owner. Keep that count only for a new dependency.
    Owner& target = owner_of(payload);
    if (&target == &head_.owner) {
        // Never the last: the builder's Handle, or the array being set, holds one.
        if (building)
            own_release();
        else
            (void)release(target.refs);
        return;
    }
    if (std::find(deps_, deps_ + dep_count_, &target) != deps_ + dep_count_) {
        (void)release(target.refs);  // never the last: a dependency holds it
        return;
    }
    if (dep_count_ == dep_cap_) {
        const uint32_t cap = dep_cap_ ? dep_cap_ * 2 : 4;
        auto** grown =
            static_cast<Owner**>(allocate(alloc_, cap * sizeof(Owner*), alignof(Owner*)));
        if (dep_count_) std::memcpy(grown, deps_, dep_count_ * sizeof(Owner*));
        deallocate(alloc_, deps_, dep_cap_ * sizeof(Owner*));
        deps_ = grown;
        dep_cap_ = cap;
    }
    deps_[dep_count_++] = &target;
}

Ref<Array> Arena::make_array(std::span<Node> nodes, Symbol file, int32_t line) {
    if (nodes.size() > UINT32_MAX) std::abort();
    const auto size = static_cast<uint32_t>(nodes.size());
    uint32_t offset = 0;
    void* mem = take(Array::bytes(size), offset);
    auto* a = ::new (mem) Array(offset, size, file, line);
    Node* d = a->data();
    if (size == 0) ::new (d) Node();
    for (uint32_t i = 0; i < size; ++i) adopt(d + i, std::move(nodes[i]));
    a->finish();
    own_retain();
    return Ref<Array>::adopt(a);
}

Ref<String> Arena::make_string(std::string_view text) {
    if (text.size() >= UINT32_MAX) std::abort();
    const auto size = static_cast<uint32_t>(text.size());
    uint32_t offset = 0;
    void* mem = take(sizeof(String) + size + 1, offset);
    auto* s = ::new (mem) String(offset, size);
    char* chars = reinterpret_cast<char*>(s + 1);
    if (size) std::memcpy(chars, text.data(), size);
    chars[size] = '\0';
    own_retain();
    return Ref<String>::adopt(s);
}

void Arena::own_retain() noexcept {
#if BAKER_ATOMIC_REFCOUNT_ENABLED
    head_.owner.refs.store(head_.owner.refs.load(std::memory_order_relaxed) + 1,
                           std::memory_order_relaxed);
#else
    ++head_.owner.refs;
#endif
}

void Arena::own_release() noexcept {
#if BAKER_ATOMIC_REFCOUNT_ENABLED
    head_.owner.refs.store(head_.owner.refs.load(std::memory_order_relaxed) - 1,
                           std::memory_order_relaxed);
#else
    --head_.owner.refs;
#endif
}

void Arena::free() noexcept {
    for (uint32_t i = 0; i < dep_count_; ++i)
        if (release(deps_[i]->refs)) free_owner(*deps_[i]);
    deallocate(alloc_, deps_, dep_cap_ * sizeof(Owner*));
    Block* b = blocks_;
    while (b) {
        Block* next = b->next;
        detail::deallocate(alloc_, b, b->bytes);
        b = next;
    }
    const Allocator alloc = alloc_;
    const size_t whole = whole_;
    this->~Arena();
    detail::deallocate(alloc, this, whole);
}

void* solo_payload(OwnerKind kind, size_t bytes) {
    void* mem = default_alloc(sizeof(SoloBlock) + bytes, alignof(SoloBlock));
    auto* block = ::new (mem) SoloBlock{nullptr, {}};
    block->owner = &block->self;
    block->self.kind = kind;
    return block + 1;
}

namespace {
SoloBlock* solo_block(Owner& owner) noexcept {
    return reinterpret_cast<SoloBlock*>(reinterpret_cast<char*>(&owner) -
                                        offsetof(SoloBlock, self));
}
}  // namespace

void free_owner(Owner& owner) noexcept {
    switch (owner.kind) {
        case kSoloArray: {
            SoloBlock* block = solo_block(owner);
            auto* a = reinterpret_cast<Array*>(block + 1);
            const uint32_t size = a->size_;
            Node* d = a->data();
            for (uint32_t i = 0; i < size; ++i) d[i].~Node();
            a->~Array();
            block->~SoloBlock();
            default_free(block, sizeof(SoloBlock) + Array::bytes(size));
            return;
        }
        case kSoloString: {
            SoloBlock* block = solo_block(owner);
            auto* s = reinterpret_cast<String*>(block + 1);
            const uint32_t size = s->size_;
            s->~String();
            block->~SoloBlock();
            default_free(block, sizeof(SoloBlock) + sizeof(String) + size + 1);
            return;
        }
        case kArena: Arena::of(owner).free(); return;
        default: return;
    }
}

}  // namespace baker::detail
