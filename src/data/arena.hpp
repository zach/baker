#pragma once

// Owners of arrays and strings: solo blocks, and load arenas.

#include <baker/allocator.hpp>
#include <baker/array.hpp>
#include <baker/node.hpp>
#include <baker/ref.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace baker::detail {

// A block holding one array or string: the owner pointer every block starts with, the
// owner itself, then the payload.
struct SoloBlock {
    Owner* owner;
    Owner self;
};
inline constexpr uint32_t kSoloOffset = sizeof(SoloBlock);

// Storage for a solo payload of `bytes`, from the default allocator; the payload's first
// member must be the offset kSoloOffset.
[[nodiscard]] void* solo_payload(OwnerKind kind, size_t bytes);

// One load's arrays and strings: bump-allocated in blocks from an allocator and freed
// together. A node stored here does not count its own arena; one that points elsewhere
// counts that owner once, in `deps`. Freeing runs no node destructors.
class Arena {
public:
    // A new arena. Its count starts at 1, held by the Handle the caller wraps it in.
    [[nodiscard]] static Arena* create(const Allocator& alloc, size_t expected_bytes = 0);
    // A new arena whose first block shares its allocation: `bytes` of payload, at `payload`,
    // kBlockHeader bytes into the block (a cache's file). Later storage takes blocks
    // as usual.
    [[nodiscard]] static Arena* create_holding(const Allocator& alloc, size_t bytes,
                                               char*& payload);
    // From the start of a block to its first payload.
    static constexpr uint32_t kBlockHeader = 24;

    // An array of `nodes`, moved from; the Ref counts this arena.
    [[nodiscard]] Ref<Array> make_array(std::span<Node> nodes, Symbol file, int32_t line);
    [[nodiscard]] Ref<String> make_string(std::string_view text);
    // Stores `n`, which is moved from, at `slot` (raw storage, or a node it overwrites
    // without destroying). `building`: the load is not yet returned, so no other thread
    // can count this arena (Array::set passes false).
    void adopt(Node* slot, Node&& n, bool building = true);

    // The arena an owner of kind kArena is.
    [[nodiscard]] static Arena& of(Owner& owner) noexcept;

    // The creator's count, as a scoped handle.
    class Handle {
    public:
        explicit Handle(Arena* a) noexcept : a_(a) {}
        ~Handle() {
            if (release(a_->head_.owner.refs)) free_owner(a_->head_.owner);
        }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        Arena& operator*() const noexcept { return *a_; }
        Arena* operator->() const noexcept { return a_; }

    private:
        Arena* a_;
    };

private:
    friend void free_owner(Owner&) noexcept;
    struct Block {
        Owner* owner;  // first, as in every block
        Block* next;
        size_t bytes;
    };
    explicit Arena(const Allocator& a) : head_{{}, this}, alloc_(a) { head_.owner.kind = kArena; }
    // The arena's own count, changed without atomics: until the load is returned, only the
    // thread building it can see it.
    void own_retain() noexcept;
    void own_release() noexcept;
    // Storage for `bytes`, and its offset from the start of its block.
    [[nodiscard]] void* take(size_t bytes, uint32_t& offset);
    void free() noexcept;

    // The owner every block points to, and the way back from it to the arena.
    struct Head {
        Owner owner;
        Arena* arena;
    };
    static_assert(std::is_standard_layout_v<Head>, "an Owner& must convert to its Head");
    Head head_;
    const Allocator alloc_;     // a copy: the tree may outlive whoever read it
    Block* blocks_ = nullptr;   // newest first
    Block* current_ = nullptr;  // the block being filled
    char* cur_ = nullptr;
    size_t left_ = 0;
    size_t next_ = 0;  // the size of the next block
    // The owners this arena's nodes point into, counted once each. A raw array, grown from
    // alloc_ on the first dependency: an empty Vec allocates under MSVC's debug library,
    // and most loads have no dependency.
    Owner** deps_ = nullptr;
    uint32_t dep_count_ = 0;
    uint32_t dep_cap_ = 0;
    size_t whole_ = sizeof(Arena);  // this allocation's size: more if it holds a block
};

inline Arena& Arena::of(Owner& owner) noexcept {
    return *reinterpret_cast<Head&>(owner).arena;
}

}  // namespace baker::detail
