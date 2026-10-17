#pragma once

// Array: a refcounted, fixed-size sequence of Nodes stored in one allocation.
//
// The 24-byte header (refcount, size, source file, source line, height, flags) is
// followed directly by the nodes, so an array is one allocation and its children are
// contiguous. Every array remembers the file and line it was parsed from; that is what
// lets errors and provenance point at the source.
//
// Lookup. An array whose first node is a symbol is keyed by that symbol, its tag, and
// the nodes after the tag are its values:
//
//   (menu
//     ('bacon and eggs' (price 12.75) (calories 120)))
//
//   const baker::Array& menu = root->find("menu");          // child tagged menu
//   double price = menu.find("bacon and eggs").find_float("price");   // 12.75
//   float  same  = root->get<float>("menu.'bacon and eggs'.price");   // by path
//   int    cals  = root->get<int>("menu.'bacon and eggs'.fat", 0);    // absent: 0
//
// find() scans the direct children's tags, which each array keeps in a run of 4-byte keys
// beside its nodes, so a scan reads only the array's own memory. That is the right
// default for the small arrays config is made of; sort_for_lookup() switches an array to
// binary search. Only Symbol tags match. A child's tag is captured
// when the child is placed: after re-tagging a child in place (child.set(0, ...)), its
// parent's lookups may find it under neither tag until the parent sets it again.
//
// Raw versus typed: try_at(i), operator[], and try_find() return raw nodes and arrays and
// never evaluate anything. Typed reads (find_int, find<T>, get<T>, ...) convert values through
// Convert<T>. A Command or Var value needs baker::script to evaluate it; without that,
// reading one as a value is a clean error.
//
// Errors name the key, the expected and actual types, the source file and line, and the
// array path that led there:
//
//   baker: 'price' is float, not int  (menu.dta:3, at menu/'bacon and eggs')
//
// A lookup along a Path reports the whole path; a chain of finds reports the tag of the
// array the failing call was made on (an array does not know its parents).
//
// Typed reads return Expected<T>. The try_ forms are weak: try_find() and try_get()
// return the child array, or null when it is absent, and build no error. Forms taking a
// fallback return it only when the key is absent; a value that is present but the wrong
// type is an error, because silently ignoring a mistyped value is the failure located
// errors exist to prevent.

#include <baker/allocator.hpp>
#include <baker/node.hpp>
#include <baker/path.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace baker {

class Array;

// A run of nodes a lookup found, with the array they came from (for error locations):
// the values after a tag, or the contents of an array an index selected.
class Values {
public:
    constexpr Values() noexcept = default;
    constexpr Values(std::span<const Node> nodes, const Array* source) noexcept
        : nodes_(nodes), source_(source) {}

    [[nodiscard]] size_t size() const noexcept { return nodes_.size(); }
    [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }
    [[nodiscard]] const Node& operator[](size_t i) const noexcept { return nodes_[i]; }
    [[nodiscard]] const Node* begin() const noexcept { return nodes_.data(); }
    [[nodiscard]] const Node* end() const noexcept { return nodes_.data() + nodes_.size(); }
    [[nodiscard]] std::span<const Node> nodes() const noexcept { return nodes_; }
    [[nodiscard]] const Array* source() const noexcept { return source_; }

    // Reads node `i` as T into `out`. For a compound T (not a number, bool, symbol, or
    // string), a node that is an array reads as its contents, so a Convert<Vec2> can read
    // (1 2) whether it is the whole value or one element of a list. False if `i` is out of
    // range or the node does not convert.
    template <class T>
    [[nodiscard]] bool read(size_t i, T& out) const;

private:
    std::span<const Node> nodes_;
    const Array* source_ = nullptr;
};

// The write direction, for Config::set and write-back: the values a T becomes. The mirror
// of Values: a converter's to() appends its values with add(), and add() of a compound
// element nests it as an array, as Values::read descends into one.
namespace detail {
// A growable run of nodes: the first kInline are held inline, so the common short list
// never allocates.
class NodeBuffer {
public:
    static constexpr uint32_t kInline = 8;

    NodeBuffer() noexcept = default;
    NodeBuffer(const NodeBuffer& o) { assign(o.nodes()); }
    NodeBuffer& operator=(const NodeBuffer& o) {
        if (this != &o) assign(o.nodes());
        return *this;
    }
    NodeBuffer(NodeBuffer&& o) noexcept { take(o); }
    NodeBuffer& operator=(NodeBuffer&& o) noexcept {
        if (this != &o) {
            release();
            take(o);
        }
        return *this;
    }
    ~NodeBuffer() { release(); }

    void push(Node n) {
        if (size_ == cap_) grow();
        data_[size_++] = std::move(n);
    }
    void clear() noexcept {
        for (uint32_t i = 0; i < size_; ++i) data_[i] = Node();
        size_ = 0;
    }
    void assign(std::span<const Node> nodes) {
        clear();
        for (const Node& n : nodes) push(n);
    }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] std::span<const Node> nodes() const noexcept { return {data_, size_}; }
    [[nodiscard]] std::span<Node> nodes() noexcept { return {data_, size_}; }

private:
    void grow() {
        auto* bigger =
            static_cast<Node*>(default_alloc(sizeof(Node) * size_t{cap_} * 2, alignof(Node)));
        for (uint32_t i = 0; i < cap_ * 2; ++i) ::new (bigger + i) Node();
        for (uint32_t i = 0; i < size_; ++i) bigger[i] = std::move(data_[i]);
        if (data_ != inline_) free_heap();
        data_ = bigger;
        cap_ *= 2;
    }
    // The heap block holds cap_ constructed nodes; from the default allocator.
    void free_heap() noexcept {
        for (uint32_t i = 0; i < cap_; ++i) data_[i].~Node();
        default_free(data_, sizeof(Node) * size_t{cap_});
    }
    void release() noexcept {
        if (data_ != inline_)
            free_heap();
        else
            clear();
        data_ = inline_;
        size_ = 0;
        cap_ = kInline;
    }
    void take(NodeBuffer& o) noexcept {
        if (o.data_ == o.inline_) {
            for (uint32_t i = 0; i < o.size_; ++i) inline_[i] = std::move(o.inline_[i]);
            data_ = inline_;
        } else {
            data_ = o.data_;
            o.data_ = o.inline_;
        }
        size_ = o.size_;
        cap_ = o.cap_;
        o.size_ = 0;
        o.cap_ = kInline;
    }

    Node* data_ = inline_;
    uint32_t size_ = 0;
    uint32_t cap_ = kInline;
    Node inline_[kInline];
};
}  // namespace detail

class ValueList {
public:
    template <class T>
    void add(const T& v);
    void push(Node n) { nodes_.push(std::move(n)); }
    [[nodiscard]] std::span<const Node> nodes() const noexcept { return nodes_.nodes(); }
    [[nodiscard]] std::span<Node> nodes() noexcept { return nodes_.nodes(); }

private:
    detail::NodeBuffer nodes_;
};

// Convert<T>: how a lookup reads a T. Specialize it for your own types:
//
//   template <> struct baker::Convert<Vec2> {
//       static constexpr std::string_view name = "vec2";   // optional; used in errors
//       static bool from(const baker::Values& v, Vec2& out) {
//           return v.size() >= 2 && v.read(0, out.x) && v.read(1, out.y);
//       }
//       static void to(const Vec2& v, baker::ValueList& out) {   // optional; for set()
//           out.add(v.x);
//           out.add(v.y);
//       }
//   };
//
//   Vec2 size = cfg_root->get<Vec2>("renderer.screen_size");   // (screen_size 1920 1080)
//
// from() returns false if the values do not form a T; the lookup turns that into a
// located error. It sees raw nodes and should read them through Values::read. Built in:
// every integer and floating-point type (range-checked), bool (any integral number),
// Symbol, std::string_view and std::string (String nodes), Node (the first value, raw),
// std::vector<T>, and std::array<T, N>. Scalars read the first value and ignore the rest,
// so (width 2 (range 0 10)) reads as 2. T must be default-constructible. to() is needed
// only to write a T (Config::set, Tweak::set); every built-in converter has one, and a
// float is written as its shortest decimal (0.7f as 0.7, not 0.699999988).
//
// A class template rather than an ADL function, so a type you do not own (glm::vec3) can
// be given a converter without adding names to its namespace.
template <class T>
struct Convert;

namespace detail {

// How a lookup failure is described.
enum class ConvertKind : uint8_t {
    Int,       // integer target: a float value may be non-integral, a number out of range
    Float,     // floating-point target: a number may be out of range
    Scalar,    // decided by one node's type (symbol, string, bool, array)
    Compound,  // anything else: described by its values
};
struct ConvertInfo {
    std::string_view name;
    ConvertKind kind;
};

template <class T>
[[nodiscard]] constexpr std::string_view number_name() noexcept {
    constexpr bool s = std::is_signed_v<T>;
    if constexpr (std::is_floating_point_v<T>) {
        return sizeof(T) < sizeof(double) ? "float32" : "float";
    } else if constexpr (sizeof(T) == 1) {
        return s ? "int8" : "uint8";
    } else if constexpr (sizeof(T) == 2) {
        return s ? "int16" : "uint16";
    } else if constexpr (sizeof(T) == 4) {
        return s ? "int32" : "uint32";
    } else {
        return s ? "int" : "uint64";
    }
}

template <class T>
[[nodiscard]] constexpr ConvertInfo convert_info() noexcept {
    if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
        return {number_name<T>(),
                std::is_floating_point_v<T> ? ConvertKind::Float : ConvertKind::Int};
    } else if constexpr (requires { Convert<T>::scalar; }) {
        return {Convert<T>::name, ConvertKind::Scalar};
    } else if constexpr (requires { std::string_view(Convert<T>::name); }) {
        return {Convert<T>::name, ConvertKind::Compound};
    } else {
        return {"the requested type", ConvertKind::Compound};
    }
}

// The double nearest the shortest decimal that reads back as `f`: 0.7f gives 0.7.
[[nodiscard]] double float_to_decimal_double(float f) noexcept;

// Reads a number into any arithmetic type, range-checked (as Node::as<T>).
template <class T>
[[nodiscard]] bool read_number(const Node& n, T& out) noexcept {
    if constexpr (std::is_floating_point_v<T>) {
        const auto v = n.try_float();
        if (!v) return false;
        if constexpr (sizeof(T) < sizeof(double)) {
            constexpr auto kMax = static_cast<double>(std::numeric_limits<T>::max());
            if (*v > kMax || *v < -kMax) return false;
        }
        out = static_cast<T>(*v);
    } else {
        const auto v = n.try_int();
        if (!v || !std::in_range<T>(*v)) return false;
        out = static_cast<T>(*v);
    }
    return true;
}

// Out-of-line error builders (src/data/lookup.cpp); only reached when a lookup fails.

// find(tag) on `container` failed: `found` is null if no child has the tag, otherwise
// its values (or `values`, if given: the evaluated ones) did not convert as `info`
// describes (null `info`: no conversion was asked).
[[nodiscard]] Error find_error(const Array& container, Symbol tag, const Array* found,
                               const ConvertInfo* info, const Values* values = nullptr);

// Evaluates a Command or Var node in the calling thread's current Context. baker::script
// installs it; it is null in a program that does not use baker::script.
using Evaluator = Expected<Node> (*)(const Node&);
void set_evaluator(Evaluator evaluator) noexcept;
[[nodiscard]] Evaluator evaluator() noexcept;

struct Evaluated {
    NodeBuffer nodes;
    Values values;
};
// `in` with every Command and Var evaluated. False if there is none, or no evaluator.
[[nodiscard]] Expected<bool> evaluate_values(const Values& in, Evaluated& out);

// The slow path of a typed lookup whose values `v` did not convert: evaluates any Command
// or Var among them and converts again. `fail` builds the error from the values that did
// not convert. Only the values themselves are evaluated, not arrays nested in them.
template <class T, class Fail>
[[nodiscard]] Expected<T> convert_evaluated(const Values& v, Fail&& fail) {
    Evaluated ev;
    auto r = evaluate_values(v, ev);
    if (!r) return Unexpected(std::move(r).error());
    T out{};
    if (*r && Convert<T>::from(ev.values, out)) return out;
    return Unexpected(fail(*r ? ev.values : v));
}

struct Resolved {
    Values values;
    const Array* array = nullptr;  // the array the path ends on; null if it ends on a leaf
};
enum class ResolveStatus : uint8_t { Ok, Missing, Malformed };
[[nodiscard]] ResolveStatus resolve(const Array& root, const Path& path, Resolved& out) noexcept;
// get(path) on `root` failed; re-walks the path to say where and why. `values`, if given,
// are the evaluated values that did not convert.
[[nodiscard]] Error get_error(const Array& root, const Path& path, const ConvertInfo* info,
                              const Values* values = nullptr);

inline constexpr ConvertInfo kArrayInfo{"array", ConvertKind::Scalar};

}  // namespace detail

// Iterates the children of an array that are Arrays (not Commands or Properties).
class SubarrayIterator {
public:
    using value_type = Array;
    using reference = const Array&;
    using pointer = const Array*;
    using difference_type = std::ptrdiff_t;

    SubarrayIterator() = default;
    SubarrayIterator(const Node* p, const Node* end) noexcept : p_(p), end_(end) { skip(); }

    reference operator*() const noexcept { return *p_->try_array(); }
    pointer operator->() const noexcept { return p_->try_array(); }
    SubarrayIterator& operator++() noexcept {
        ++p_;
        skip();
        return *this;
    }
    SubarrayIterator operator++(int) noexcept {
        SubarrayIterator old = *this;
        ++*this;
        return old;
    }
    friend bool operator==(const SubarrayIterator& a, const SubarrayIterator& b) noexcept {
        return a.p_ == b.p_;
    }

private:
    void skip() noexcept {
        while (p_ != end_ && p_->type() != Type::Array) ++p_;
    }
    const Node* p_ = nullptr;
    const Node* end_ = nullptr;
};

// A forward range over an array's subarrays: for (const Array& item : menu.subarrays()).
class Subarrays {
public:
    Subarrays() = default;
    Subarrays(const Node* begin, const Node* end) noexcept : begin_(begin), end_(end) {}
    [[nodiscard]] SubarrayIterator begin() const noexcept { return {begin_, end_}; }
    [[nodiscard]] SubarrayIterator end() const noexcept { return {end_, end_}; }

private:
    const Node* begin_ = nullptr;
    const Node* end_ = nullptr;
};

class Array {
public:
    // An array of `size` nodes, each Int 0.
    [[nodiscard]] static Ref<Array> make(uint32_t size, Symbol file = {}, int32_t line = 0);
    // An array holding `nodes`, which are moved from.
    [[nodiscard]] static Ref<Array> make_from(std::span<Node> nodes, Symbol file = {},
                                              int32_t line = 0);
    // An array holding copies of `nodes`.
    [[nodiscard]] static Ref<Array> make_copy(std::span<const Node> nodes, Symbol file = {},
                                              int32_t line = 0);

    [[nodiscard]] uint32_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    // The raw node at `i`, or null if out of range.
    [[nodiscard]] const Node* try_at(size_t i) const noexcept {
        return i < size_ ? data() + i : nullptr;
    }
    // Unchecked (asserted) access.
    [[nodiscard]] const Node& operator[](size_t i) const noexcept {
        assert(i < size_);
        return data()[i];
    }

    [[nodiscard]] const Node* begin() const noexcept { return data(); }
    [[nodiscard]] const Node* end() const noexcept { return data() + size_; }
    [[nodiscard]] std::span<const Node> nodes() const noexcept { return {data(), size_}; }
    [[nodiscard]] Subarrays subarrays() const noexcept { return {begin(), end()}; }

    // The nodes after the first: a tagged array's values.
    [[nodiscard]] Values values() const noexcept {
        return {size_ ? nodes().subspan(1) : std::span<const Node>(), this};
    }

    // Replaces the node at `i` (must be in range). Arrays may be shared, e.g. by macro
    // expansion, so the change is visible through every reference. Clears sorted mode.
    void set(size_t i, Node node) noexcept;

    // This array's own tag: its first node, if a symbol; the empty Symbol if not.
    // Walking (enemies (grunt ...) (brute ...)), each child's tag is its name.
    // The tag: the first node if it is a symbol other than '', else the empty
    // Symbol, which no array is tagged with.
    [[nodiscard]] Symbol tag() const noexcept {
        if (size_ == 0) return {};
        const auto s = data()[0].try_sym();
        return s ? *s : Symbol();
    }

    // --- Lookup by tag: the first direct child that is an Array tagged `tag`. ---

    // The child array tagged `tag`, or null.
    [[nodiscard]] const Array* try_find(Symbol tag) const noexcept {
        if (flags_ & kSortedForLookup) return find_sorted(tag);
        // The tag run: the scan reads 4-byte keys from this array's own memory, and
        // loads only the child a key matches, which the caller reads next anyway. The child's
        // own tag confirms it: two symbols can share a key.
        const uint32_t want = key_of(tag);
        const uint32_t* tags = tag_run();
        for (uint32_t i = 0; i < size_; ++i)
            if (tags[i] == want) {
                const auto* a = static_cast<const Array*>(data()[i].array());
                if (a->data()[0].is(tag)) return a;
            }
        return nullptr;
    }
    // The values of the child tagged `tag`, read as T.
    template <class T>
    [[nodiscard]] Expected<T> find(Symbol tag) const;
    // As above, or `fallback` if no child has the tag.
    template <class T>
    [[nodiscard]] Expected<T> find(Symbol tag, T fallback) const;

    [[nodiscard]] Expected<int64_t> find_int(Symbol tag) const { return find<int64_t>(tag); }
    [[nodiscard]] Expected<double> find_float(Symbol tag) const { return find<double>(tag); }
    [[nodiscard]] Expected<Symbol> find_sym(Symbol tag) const { return find<Symbol>(tag); }
    [[nodiscard]] Expected<std::string_view> find_str(Symbol tag) const {
        return find<std::string_view>(tag);
    }
    // The array that is the first value: (colors (1 0 0)) gives (1 0 0).
    [[nodiscard]] Expected<const Array*> find_array(Symbol tag) const;
    [[nodiscard]] Expected<int64_t> find_int(Symbol tag, int64_t fallback) const {
        return find<int64_t>(tag, fallback);
    }
    [[nodiscard]] Expected<double> find_float(Symbol tag, double fallback) const {
        return find<double>(tag, fallback);
    }
    [[nodiscard]] Expected<Symbol> find_sym(Symbol tag, Symbol fallback) const {
        return find<Symbol>(tag, fallback);
    }
    [[nodiscard]] Expected<std::string_view> find_str(Symbol tag, std::string_view fallback) const {
        return find<std::string_view>(tag, fallback);
    }

    // --- Lookup by path (see path.hpp): root->get<float>("renderer.clear_color.0"). ---

    // The array the path ends on, or null.
    [[nodiscard]] const Array* try_get(const Path& path) const noexcept {
        detail::Resolved r;
        return detail::resolve(*this, path, r) == detail::ResolveStatus::Ok ? r.array : nullptr;
    }
    template <class T>
    [[nodiscard]] Expected<T> get(const Path& path) const;
    // As above, or `fallback` if the path leads nowhere. A malformed path is an error.
    template <class T>
    [[nodiscard]] Expected<T> get(const Path& path, T fallback) const;

    // --- Sorted mode. ---

    // Reorders the children so find() can binary-search: children that are not tagged
    // arrays move to the front in their original order (so a tag in slot 0 stays there),
    // and tagged arrays follow, sorted by tag hash and then text (an order that does not
    // depend on the run). Duplicate tags keep their order, so find() still returns the
    // first. Opt-in and never automatic: it changes at(i) and printing order, and every
    // array sharing this one (through a macro) sees the change. set() turns it off;
    // re-tagging a child in place is not detected, so sort again after doing that.
    void sort_for_lookup();
    [[nodiscard]] bool sorted_for_lookup() const noexcept {
        return (flags_ & kSortedForLookup) != 0;
    }
    // Whether this array, or one below it, is a `(key #remove)`: a Config layer's removal,
    // which a merge strips from what it adds to the stack.
    [[nodiscard]] bool has_removal() const noexcept { return (flags_ & kHasRemoval) != 0; }

    [[nodiscard]] Symbol file() const noexcept { return file_; }
    [[nodiscard]] int32_t line() const noexcept { return line_; }

    // 1 + the height of the tallest child array; 1 for an array with no array children.
    // Bounds every recursive walk over a parsed tree. Saturates at 65535.
    [[nodiscard]] uint16_t height() const noexcept { return height_; }

    // The bytes an array of `size` nodes takes: the header, the run of its children's tags,
    // padded to 8, and the nodes (one, Int 0, when empty).
    [[nodiscard]] static constexpr size_t bytes(uint32_t size) noexcept {
        return nodes_offset(size) + size_t{size ? size : 1u} * sizeof(Node);
    }
    // Where an array of `size` nodes keeps them, from its start: after the header and the
    // tag run. The binary cache lays arrays out alike.
    [[nodiscard]] static constexpr size_t nodes_offset(uint32_t size) noexcept {
        return sizeof(Array) + run_bytes(size);
    }

    void ref() const noexcept { detail::retain_payload(this); }
    void unref() const noexcept { detail::release_payload(this); }
    // The count of the array's owner: for a solo array, its own; for an array read
    // from a file, its load's.
    [[nodiscard]] uint32_t ref_count() const noexcept {
        return detail::count(detail::owner_of(this).refs);
    }

    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;

private:
    static constexpr uint16_t kSortedForLookup = 1;
    static constexpr uint16_t kHasRemoval = 2;

    Array(uint32_t block, uint32_t size, Symbol file, int32_t line) noexcept
        : block_(block), size_(size), file_(file), line_(line) {}
    ~Array() = default;

    // Storage for `size` nodes, not yet constructed. An empty array still gets one slot,
    // holding Int 0, so try_find() can read every child's first node without first
    // checking its size.
    [[nodiscard]] static Array* allocate(uint32_t size, Symbol file, int32_t line);
    friend void detail::free_owner(detail::Owner&) noexcept;
    friend class detail::Arena;
    friend struct detail::CacheLayout;
    // Sets the height and the tag run from the nodes: every way of building an array ends
    // here.
    void finish() noexcept;

    // The tag run: for each node, the key of its tag if it is an Array whose first node is
    // a Symbol, else kNoTag. A key is 32 bits of the symbol's identity, made without loading
    // anything, and never kNoTag. Kept by finish(), set(), and sort_for_lookup(); a child
    // re-tagged in place is not seen until it is set again.
    static constexpr uint32_t kNoTag = 0;
    [[nodiscard]] static uint32_t key_of(Symbol s) noexcept {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s.id()) >> 3) | 1u;
    }
    [[nodiscard]] static uint32_t tag_key(const Node& n) noexcept {
        // data()[0] is readable even in an empty array (see allocate()).
        const Array* a = n.try_array();
        const auto tag = a ? a->data()[0].try_sym() : std::nullopt;
        return tag && !tag->empty() ? key_of(*tag) : kNoTag;  // '' is no tag
    }
    // The run follows the header, so a scan needs no load to find it and shares the
    // header's cache line; the nodes follow the run.
    [[nodiscard]] const uint32_t* tag_run() const noexcept {
        return reinterpret_cast<const uint32_t*>(this + 1);
    }
    [[nodiscard]] uint32_t* tag_run() noexcept { return reinterpret_cast<uint32_t*>(this + 1); }
    [[nodiscard]] static constexpr size_t run_bytes(uint32_t size) noexcept {
        return (size_t{size} * sizeof(uint32_t) + 7) & ~size_t{7};
    }
    [[nodiscard]] const Array* find_sorted(Symbol tag) const noexcept;

    [[nodiscard]] Node* data() noexcept {
        return reinterpret_cast<Node*>(reinterpret_cast<char*>(this + 1) + run_bytes(size_));
    }
    [[nodiscard]] const Node* data() const noexcept {
        return reinterpret_cast<const Node*>(reinterpret_cast<const char*>(this + 1) +
                                             run_bytes(size_));
    }

    uint32_t block_;  // the offset back to the block that holds it
    uint32_t size_;
    Symbol file_;
    int32_t line_;
    uint16_t height_ = 1;
    uint16_t flags_ = 0;  // kSortedForLookup, kHasRemoval
    // uint32_t tags[size_], padded to 8, follows, then Node nodes[size_].
};

namespace detail {
// The tag of `n` if it is an array (Array::tag()), else the empty Symbol.
[[nodiscard]] inline Symbol tag_of(const Node& n) noexcept {
    const Array* a = n.try_array();
    return a ? a->tag() : Symbol();
}
}  // namespace detail

static_assert(sizeof(Array) == 24, "Array header must stay 24 bytes");

// --- Built-in converters. ---

template <class T>
    requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
struct Convert<T> {
    static bool from(const Values& v, T& out) noexcept {
        return !v.empty() && detail::read_number(v[0], out);
    }
    static void to(T v, ValueList& out) {
        if constexpr (std::is_same_v<T, float>)
            out.push(Node(detail::float_to_decimal_double(v)));
        else
            out.push(Node(v));
    }
};

template <>
struct Convert<bool> {
    static constexpr std::string_view name = "bool";
    static constexpr bool scalar = true;
    static bool from(const Values& v, bool& out) noexcept {
        if (v.empty()) return false;
        const auto i = v[0].try_int();
        if (i) out = *i != 0;
        return i.has_value();
    }
    static void to(bool v, ValueList& out) { out.push(Node(v ? 1 : 0)); }
};

template <>
struct Convert<Symbol> {
    static constexpr std::string_view name = "symbol";
    static constexpr bool scalar = true;
    static bool from(const Values& v, Symbol& out) noexcept {
        if (v.empty() || v[0].type() != Type::Symbol) return false;
        out = *v[0].try_sym();
        return true;
    }
    static void to(Symbol v, ValueList& out) { out.push(Node(v)); }
};

// Views into the String node, valid while the array holding it is alive.
template <>
struct Convert<std::string_view> {
    static constexpr std::string_view name = "string";
    static constexpr bool scalar = true;
    static bool from(const Values& v, std::string_view& out) noexcept {
        if (v.empty() || v[0].type() != Type::String) return false;
        out = v[0].string()->view();
        return true;
    }
    static void to(std::string_view v, ValueList& out) { out.push(Node(String::make(v))); }
};

// The first value, raw: never converted or evaluated.
template <>
struct Convert<Node> {
    static constexpr std::string_view name = "value";
    static constexpr bool scalar = true;
    static bool from(const Values& v, Node& out) noexcept {
        if (v.empty()) return false;
        out = v[0];
        return true;
    }
    static void to(const Node& v, ValueList& out) { out.push(v); }
};

// --- Template definitions. ---

template <class T>
bool Values::read(size_t i, T& out) const {
    if (i >= nodes_.size()) return false;
    if constexpr (detail::convert_info<T>().kind == detail::ConvertKind::Compound) {
        if (const Array* a = nodes_[i].try_array())
            return Convert<T>::from(Values(a->nodes(), a), out);
    }
    return Convert<T>::from(Values(nodes_.subspan(i, 1), source_), out);
}

template <class T>
void ValueList::add(const T& v) {
    if constexpr (detail::convert_info<T>().kind == detail::ConvertKind::Compound) {
        ValueList inner;
        Convert<T>::to(v, inner);
        nodes_.push(Node(Array::make_from(inner.nodes())));
    } else {
        Convert<T>::to(v, *this);
    }
}

template <class T>
Expected<T> Array::find(Symbol tag) const {
    static constexpr detail::ConvertInfo kInfo = detail::convert_info<T>();
    T out{};
    const Array* a = try_find(tag);
    if (a && Convert<T>::from(a->values(), out)) return out;
    if (!a) return Unexpected(detail::find_error(*this, tag, nullptr, &kInfo));
    return detail::convert_evaluated<T>(a->values(), [&](const Values& v) {
        return detail::find_error(*this, tag, a, &kInfo, &v);
    });
}

template <class T>
Expected<T> Array::find(Symbol tag, T fallback) const {
    static constexpr detail::ConvertInfo kInfo = detail::convert_info<T>();
    const Array* a = try_find(tag);
    if (!a) return fallback;
    T out{};
    if (Convert<T>::from(a->values(), out)) return out;
    return detail::convert_evaluated<T>(a->values(), [&](const Values& v) {
        return detail::find_error(*this, tag, a, &kInfo, &v);
    });
}

template <class T>
Expected<T> Array::get(const Path& path) const {
    static constexpr detail::ConvertInfo kInfo = detail::convert_info<T>();
    detail::Resolved r;
    T out{};
    if (detail::resolve(*this, path, r) != detail::ResolveStatus::Ok)
        return Unexpected(detail::get_error(*this, path, &kInfo));
    if (Convert<T>::from(r.values, out)) return out;
    return detail::convert_evaluated<T>(
        r.values, [&](const Values& v) { return detail::get_error(*this, path, &kInfo, &v); });
}

template <class T>
Expected<T> Array::get(const Path& path, T fallback) const {
    static constexpr detail::ConvertInfo kInfo = detail::convert_info<T>();
    detail::Resolved r;
    const auto status = detail::resolve(*this, path, r);
    if (status == detail::ResolveStatus::Missing) return fallback;
    if (status != detail::ResolveStatus::Ok)
        return Unexpected(detail::get_error(*this, path, &kInfo));
    T out{};
    if (Convert<T>::from(r.values, out)) return out;
    return detail::convert_evaluated<T>(
        r.values, [&](const Values& v) { return detail::get_error(*this, path, &kInfo, &v); });
}

// Deep comparison of type and value, ignoring source locations.
// Floats compare by bit pattern. Strings compare by content; arrays recursively.
[[nodiscard]] bool structurally_equal(const Node& a, const Node& b) noexcept;
[[nodiscard]] bool structurally_equal(const Array& a, const Array& b) noexcept;

// A refcounted string node payload: array("name", str("Bob")).
[[nodiscard]] inline Ref<String> str(std::string_view text) {
    return String::make(text);
}

namespace detail {
template <class T>
Node to_node(T&& v) {
    using D = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<D, Node>) {
        return std::forward<T>(v);
    } else if constexpr (std::is_convertible_v<const D&, std::string_view>) {
        return Node(Symbol(std::string_view(v)));
    } else {
        return Node(std::forward<T>(v));
    }
}
}  // namespace detail

// Builds an array from C++ values in one allocation sized exactly. Text becomes a
// Symbol; use str() for a String.
//
//   auto price = baker::array("price", 12.75f);   // (price 12.75)
//   auto eggs  = baker::array("eggs", price);     // (eggs (price 12.75))
template <class... Ts>
[[nodiscard]] Ref<Array> array(Ts&&... xs) {
    if constexpr (sizeof...(Ts) == 0) {
        return Array::make(0);
    } else {
        Node nodes[] = {detail::to_node(std::forward<Ts>(xs))...};
        return Array::make_from(nodes);
    }
}

}  // namespace baker
