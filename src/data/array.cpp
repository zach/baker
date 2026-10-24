#include <baker/array.hpp>

#include "arena.hpp"
#include "text.hpp"

#include <bit>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>

namespace baker {

static_assert(std::is_standard_layout_v<Array>);
static_assert(sizeof(Array) % alignof(Node) == 0, "nodes must follow the header aligned");

namespace {
uint16_t child_height(const Node& n) noexcept {
    const Array* a = n.array();
    return a ? a->height() : uint16_t{0};
}
uint16_t above(uint16_t h) noexcept {
    return h == UINT16_MAX ? h : static_cast<uint16_t>(h + 1);
}
}  // namespace

Array* Array::allocate(uint32_t size, Symbol file, int32_t line) {
    // A solo block, from the default allocator.
    void* mem = detail::solo_payload(detail::kSoloArray, bytes(size));
    auto* a = new (mem) Array(detail::kSoloOffset, size, file, line);
    if (size == 0) new (a->data()) Node();  // Int 0 owns nothing, so it is never destroyed
    return a;
}

Ref<Array> Array::make(uint32_t size, Symbol file, int32_t line) {
    Array* a = allocate(size, file, line);
    Node* d = a->data();
    for (uint32_t i = 0; i < size; ++i) new (d + i) Node();
    a->finish();
    return Ref<Array>::adopt(a);
}

Ref<Array> Array::make_from(std::span<Node> nodes, Symbol file, int32_t line) {
    if (nodes.size() > UINT32_MAX) std::abort();
    const auto size = static_cast<uint32_t>(nodes.size());
    Array* a = allocate(size, file, line);
    Node* d = a->data();
    for (uint32_t i = 0; i < size; ++i) new (d + i) Node(std::move(nodes[i]));
    a->finish();
    return Ref<Array>::adopt(a);
}

Ref<Array> Array::make_copy(std::span<const Node> nodes, Symbol file, int32_t line) {
    if (nodes.size() > UINT32_MAX) std::abort();
    const auto size = static_cast<uint32_t>(nodes.size());
    Array* a = allocate(size, file, line);
    Node* d = a->data();
    for (uint32_t i = 0; i < size; ++i) new (d + i) Node(nodes[i]);
    a->finish();
    return Ref<Array>::adopt(a);
}

void Array::finish() noexcept {
    uint16_t h = 0;
    uint16_t below = 0;  // the children's flags, for kHasRemoval
    uint32_t* tags = tag_run();
    for (uint32_t i = 0; i < size_; ++i) {
        if (const Array* a = data()[i].array()) {
            if (a->height_ > h) h = a->height_;
            below = static_cast<uint16_t>(below | a->flags_);
        }
        tags[i] = tag_key(data()[i]);
    }
    height_ = above(h);
    if (size_ == 2 && data()[1].type() == Type::Symbol) {
        static const Symbol marker("#remove");
        if (data()[1].is(marker)) below = kHasRemoval;
    }
    flags_ = static_cast<uint16_t>(flags_ | (below & kHasRemoval));
}

void Array::set(size_t i, Node node) noexcept {
    assert(i < size_);
    const uint16_t h = above(child_height(node));
    if (h > height_) height_ = h;
    flags_ = static_cast<uint16_t>(flags_ & ~kSortedForLookup);
    if (const Array* a = node.array())
        flags_ = static_cast<uint16_t>(flags_ | (a->flags_ & kHasRemoval));
    tag_run()[i] = tag_key(node);
    detail::Owner& owner = detail::owner_of(this);
    if (owner.kind != detail::kArena) {
        data()[i] = std::move(node);
        return;
    }
    // Arena nodes count nothing: the old node is dropped as it is, and the arena counts
    // the new one's owner if it is another.
    detail::Arena::of(owner).adopt(data() + i, std::move(node), false);
}

bool structurally_equal(const Node& a, const Node& b) noexcept {
    if (a.type() != b.type()) return false;
    switch (a.type()) {
        case Type::Int: return *a.try_int() == *b.try_int();
        case Type::Float: return a.float_bits() == b.float_bits();
        case Type::Symbol: return *a.try_sym() == *b.try_sym();
        case Type::String: return a.string()->view() == b.string()->view();
        case Type::Array:
        case Type::Command:
        case Type::Property: return structurally_equal(*a.array(), *b.array());
        case Type::Var: return a.var_name() == b.var_name();
        case Type::Func: return a.func_record() == b.func_record();
        case Type::Object: return a.object_handle() == b.object_handle();
    }
    return false;
}

bool structurally_equal(const Array& a, const Array& b) noexcept {
    if (&a == &b) return true;
    if (a.size() != b.size()) return false;
    for (uint32_t i = 0; i < a.size(); ++i)
        if (!structurally_equal(a[i], b[i])) return false;
    return true;
}

// --- Digest ---

namespace {
struct Digest {
    uint64_t state;
    void word(uint64_t w) noexcept {  // splitmix64's finalizer over state ^ w
        uint64_t z = (state ^ w) + 0x9e3779b97f4a7c15ULL;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        state = z ^ (z >> 31);
    }
    void text(std::string_view s) noexcept {
        word(s.size());
        for (size_t i = 0; i < s.size(); i += 8) {
            uint64_t w = 0;
            for (size_t k = 0; k < 8 && i + k < s.size(); ++k)
                w |= uint64_t{static_cast<unsigned char>(s[i + k])} << (8 * k);
            word(w);
        }
    }
    void node(const Node& n) noexcept {
        word(static_cast<uint64_t>(n.type()) + 1);
        switch (n.type()) {
            case Type::Int: word(static_cast<uint64_t>(*n.try_int())); break;
            case Type::Float: {
                double d = *n.try_float();
                if (d == 0) d = 0;  // -0.0 is 0.0
                word(d != d ? 0x7ff8000000000000ULL : std::bit_cast<uint64_t>(d));
                break;
            }
            case Type::Symbol: text(n.try_sym()->str()); break;
            case Type::String: text(*n.try_str()); break;
            case Type::Array:
            case Type::Command:
            case Type::Property: array(*n.array()); break;
            default: text(detail::text_of(n)); break;  // a variable, a function: as written
        }
    }
    void array(const Array& a) noexcept {
        word(a.size());
        for (const Node& n : a) node(n);
    }
};
}  // namespace

uint64_t digest(const Array& tree, uint64_t seed) noexcept {
    Digest d{seed};
    d.array(tree);
    return d.state;
}

}  // namespace baker
