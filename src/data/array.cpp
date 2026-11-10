#include <algorithm>
#include <baker/array.hpp>

#include "arena.hpp"
#include "array_fill.hpp"
#include "markers.hpp"
#include "text.hpp"

#include <bit>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
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
    if (nodes.size() > UINT32_MAX) detail::fatal("an array of more than 4,294,967,295 nodes");
    const auto size = static_cast<uint32_t>(nodes.size());
    Array* a = allocate(size, file, line);
    Node* d = a->data();
    for (uint32_t i = 0; i < size; ++i) new (d + i) Node(std::move(nodes[i]));
    a->finish();
    return Ref<Array>::adopt(a);
}

Ref<Array> Array::make_copy(std::span<const Node> nodes, Symbol file, int32_t line) {
    if (nodes.size() > UINT32_MAX) detail::fatal("an array of more than 4,294,967,295 nodes");
    const auto size = static_cast<uint32_t>(nodes.size());
    Array* a = allocate(size, file, line);
    Node* d = a->data();
    for (uint32_t i = 0; i < size; ++i) new (d + i) Node(nodes[i]);
    a->finish();
    return Ref<Array>::adopt(a);
}

namespace {
// Whether `n` is a metadata child, (@tag ...): an array whose first node is a symbol
// beginning with '@'.
bool is_meta_child(const Node& n) noexcept {
    const Array* a = n.try_array();
    return a && a->tag().is_meta();
}
}  // namespace

// A boundary the 13 bits cannot name: the first metadata child past them. Cold: an
// array of more than 8,190 values before its metadata.
uint32_t Array::meta_first_far() const noexcept {
    for (uint32_t i = kMetaFar; i < size_; ++i)
        if (is_meta_child(data()[i])) return i;
    return 0;
}

void Array::finish(int verb) noexcept {
    uint16_t h = 0;
    uint16_t below = 0;       // the children's flags, for kHasMarker
    uint32_t first_meta = 0;  // the metadata boundary, from the size word tag_key's test loads
    uint32_t* tags = tag_run();
    for (uint32_t i = 0; i < size_; ++i) {
        const Array* a = data()[i].array();
        if (a) {
            if (a->height_ > h) h = a->height_;
            below = static_cast<uint16_t>(below | a->flags_);
        }
        // tag_key, inline: only an Array is tagged, not a command or a property.
        const auto tag =
            a && data()[i].type() == Type::Array ? a->data()[0].try_sym() : std::nullopt;
        tags[i] = tag && !tag->empty() ? key_of(*tag) : kNoTag;
        if (tag && tag->is_meta() && !first_meta && i) first_meta = i;
    }
    height_ = above(h);
    // A verb stands in the first value slot, which the parser knows; a pin is metadata.
    if (verb < 0) verb = size_ >= 2 && data()[1].type() == Type::Symbol ? -1 : 0;
    if (verb || first_meta) below = static_cast<uint16_t>(below | marker_flag(verb, first_meta));
    // sort_for_lookup() runs this again, so the old boundary goes first; so does sorted
    // mode, whose run keys these are not (sort_for_lookup() sets it again after).
    flags_ = static_cast<uint16_t>((flags_ & kFlagBits & ~kSortedForLookup) | (below & kHasMarker) |
                                   (std::min(first_meta, kMetaFar) << kMetaShift));
}

void Array::unsort() noexcept {
    flags_ = static_cast<uint16_t>(flags_ & ~kSortedForLookup);
    uint32_t* tags = tag_run();
    for (uint32_t i = 0; i < size_; ++i) tags[i] = tag_key(data()[i]);
}

#if defined(_MSC_VER)
#define BAKER_COLD __declspec(noinline)
#else
#define BAKER_COLD [[gnu::noinline, gnu::cold]]
#endif

BAKER_COLD uint16_t Array::marker_flag(int verb, uint32_t first_meta) const noexcept {
    if (verb < 0)
        verb = size_ >= 2 && data()[1].type() == Type::Symbol &&
               detail::marker_of(data()[1]) != detail::Marker::None;
    const uint32_t* tags = tag_run();
    for (uint32_t i = first_meta; i && i < size_ && !verb; ++i)
        verb =
            tags[i] == key_of(detail::was_tag()) && data()[i].array()->tag() == detail::was_tag();
    return verb ? kHasMarker : 0;
}

void Array::set(size_t i, Node node) noexcept {
    assert(i < size_);
    const uint16_t h = above(child_height(node));
    if (h > height_) height_ = h;
    if (flags_ & kSortedForLookup) [[unlikely]]
        unsort();
    // A verb or a pin made here is flagged as finish() flags one.
    if (const Array* a = node.array()) {
        const bool pin = i > 0 && node.type() == Type::Array && a->tag().is_meta() &&
                         a->tag() == detail::was_tag();
        flags_ = static_cast<uint16_t>(flags_ | (a->flags_ & kHasMarker) | (pin ? kHasMarker : 0));
    } else if (i == 1 && node.type() == Type::Symbol &&
               detail::marker_of(node) != detail::Marker::None) {
        flags_ = static_cast<uint16_t>(flags_ | kHasMarker);
    }
    tag_run()[i] = tag_key(node);
    // The boundary moves only if this slot is, or was, the first metadata child. The far
    // marker compares right against any slot below it, so only a slot past it scans.
    uint32_t m = flags_ >> kMetaShift;
    if (m == kMetaFar && i >= kMetaFar) [[unlikely]]
        m = meta_first_far();
    const bool meta = i > 0 && is_meta_child(node);
    if (meta ? (m == 0 || i < m) : (m != 0 && i == m)) {
        auto first = meta ? static_cast<uint32_t>(i) : 0;
        for (auto k = static_cast<uint32_t>(i) + 1; !first && k < size_; ++k)
            if (is_meta_child(data()[k])) first = k;
        flags_ =
            static_cast<uint16_t>((flags_ & kFlagBits) | (std::min(first, kMetaFar) << kMetaShift));
    }
    detail::Owner& owner = detail::owner_of(this);
    if (owner.kind != detail::kArena) {
        data()[i] = std::move(node);
        return;
    }
    // Arena nodes count nothing: the old node is dropped as it is, and the arena counts
    // the new one's owner if it is another. Curves baked from the load bake again.
    detail::Arena& arena = detail::Arena::of(owner);
    arena.changed();
    arena.adopt(data() + i, std::move(node), false);
}

void detail::ArrayFill::fill(Array& a, const Node& first, const Node& second,
                             std::span<const Node> rest) noexcept {
    assert(a.size_ == rest.size() + 2 && owner_of(&a).kind == kSoloArray);
    Node* d = a.data();
    d[0] = first;
    d[1] = second;
    for (size_t i = 0; i < rest.size(); ++i) d[i + 2] = rest[i];
    // finish()'s own test of the verb slot, without its out-of-line call for every symbol.
    a.finish(marker_of(second) != Marker::None ? 1 : 0);
}

void detail::ArrayFill::clear(Array& a) noexcept {
    assert(owner_of(&a).kind == kSoloArray);
    Node* d = a.data();
    uint32_t* tags = a.tag_run();
    for (uint32_t i = 0; i < a.size_; ++i) {
        d[i] = Node();
        tags[i] = Array::kNoTag;
    }
    a.height_ = 1;
    a.flags_ = 0;
}

namespace {
// `n` without the metadata of its data arrays: `n` itself when there is none to leave out.
Node without_metadata(const Node& n) {
    const Array* a = n.type() == Type::Array ? n.array() : nullptr;
    if (!a || a->height() < 2) return n;
    const uint32_t first = a->meta_first();
    Vec<Node> kept;
    kept.reserve(a->size());
    bool changed = false;
    for (uint32_t i = 0; i < a->size(); ++i) {
        const Node& c = (*a)[i];
        // From the boundary on, a field may follow metadata: only '@' tags go. A
        // record's identity stays: shipped overlays resolve by it.
        if (first && i >= first && c.type() == Type::Array && c.array()->tag().is_meta() &&
            c.array()->tag() != detail::id_tag() && c.array()->tag() != detail::aka_tag()) {
            changed = true;
            continue;
        }
        kept.push_back(without_metadata(c));
        changed = changed || kept.back().array() != c.array();
    }
    if (!changed) return n;
    return Node(Array::make_from(std::span<Node>(kept.data(), kept.size()), a->file(), a->line()));
}
}  // namespace

Ref<Array> without_metadata(const Array& root) {
    const Node kept = without_metadata(Node(Ref<Array>(const_cast<Array*>(&root))));
    return Ref<Array>(const_cast<Array*>(kept.array()));
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
