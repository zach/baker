#pragma once

// Node: the 16-byte tagged value every Baker tree is made of.
//
//   Type      Syntax              Payload
//   Int       42  -7  0x1F        int64_t
//   Float     12.75  1e-3         double
//   Symbol    price  'odd sym'    interned Symbol
//   String    "bacon"             Ref<String>
//   Array     ( ... )             Ref<Array>
//   Command   { ... }             Ref<Array>; evaluated when read as a value (baker::script)
//   Property  [ ... ]             Ref<Array>; property-path lookup (baker::script)
//   Var       $name               variable name; bound to a slot by baker::script
//   Func      -                   native function record (baker::script)
//   Object    -                   64-bit salted object handle (baker::script)
//
// Numbers are stored at 64 bits, which costs nothing: the payload is 8 bytes anyway
// because it holds pointers. as<int32_t>(), as<float>() and other
// narrower reads are range-checked.
//
// Typed accessors (as_int, as_float, ...) return Expected: a mismatch is an Error naming
// the value and both types. The try_ forms are weak: they return nothing and build no
// error. An int always reads as a float (rounded beyond 2^53); a float reads as an int
// only if it is integral and in range.

#include <baker/error.hpp>
#include <baker/ref.hpp>
#include <baker/symbol.hpp>

#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace baker {

class Array;

enum class Type : uint8_t {
    Int,
    Float,
    Symbol,
    String,
    Array,
    Command,
    Property,
    Var,
    Func,
    Object,
};

[[nodiscard]] std::string_view type_name(Type type) noexcept;

// Reference to an object owned by a baker::script Context: 32 bits of table index and 32
// bits of salt, the layout of Unreal's FWeakObjectPtr. Opaque to
// baker::data, which can store, copy, and compare it but never resolve it.
struct ObjectHandle {
    uint64_t bits = 0;

    [[nodiscard]] static constexpr ObjectHandle make(uint32_t index, uint32_t salt) noexcept {
        return {static_cast<uint64_t>(index) | (static_cast<uint64_t>(salt) << 32)};
    }
    [[nodiscard]] constexpr uint32_t index() const noexcept {
        return static_cast<uint32_t>(bits & 0xFFFFFFFFu);
    }
    [[nodiscard]] constexpr uint32_t salt() const noexcept {
        return static_cast<uint32_t>(bits >> 32);
    }
    friend constexpr bool operator==(ObjectHandle, ObjectHandle) noexcept = default;
};

// The part of a native function record baker::data needs: its name, so a Func node can be
// printed. baker::script's handler records extend this.
struct FuncRecord {
    Symbol name;
};

namespace detail {
[[nodiscard]] std::optional<int64_t> float_to_int(double f) noexcept;
}  // namespace detail

// An immutable string, stored in one piece. Counted through its owner (ref.hpp).
class String {
public:
    [[nodiscard]] static Ref<String> make(std::string_view text);

    [[nodiscard]] std::string_view view() const noexcept { return {data(), size_}; }
    [[nodiscard]] const char* data() const noexcept {
        return reinterpret_cast<const char*>(this + 1);
    }
    [[nodiscard]] uint32_t size() const noexcept { return size_; }

    void ref() const noexcept { detail::retain_payload(this); }
    void unref() const noexcept { detail::release_payload(this); }
    // The count of the string's owner: for a solo string, its own.
    [[nodiscard]] uint32_t ref_count() const noexcept {
        return detail::count(detail::owner_of(this).refs);
    }

    String(const String&) = delete;
    String& operator=(const String&) = delete;

private:
    String(uint32_t block, uint32_t size) noexcept : block_(block), size_(size) {}
    ~String() = default;
    friend void detail::free_owner(detail::Owner&) noexcept;
    friend class detail::Arena;
    friend struct detail::CacheLayout;

    uint32_t block_;  // the offset back to the block that holds it
    uint32_t size_;
    // char data[size_ + 1] follows.
};

class Node {
public:
    Node() noexcept : value_{.i = 0}, type_(Type::Int) {}
    // Any integer type. Unsigned values above INT64_MAX keep their bit pattern.
    template <class T>
        requires std::is_integral_v<T>
    Node(T v) noexcept : type_(Type::Int) {  // NOLINT
        value_.i = static_cast<int64_t>(v);
    }
    template <class T>
        requires std::is_floating_point_v<T>
    Node(T v) noexcept : type_(Type::Float) {  // NOLINT
        value_.f = static_cast<double>(v);
    }
    Node(Symbol s) noexcept : type_(Type::Symbol) { value_.p = s.id(); }  // NOLINT
    Node(const Ref<String>& s) noexcept : Node(s.get()) {}                // NOLINT
    Node(Ref<String>&& s) noexcept : type_(Type::String) {                // NOLINT
        assert(s && "Node from null Ref<String>");
        value_.p = s.release();
    }
    // `kind` is Type::Array, Type::Command, or Type::Property. `a` must not be null.
    Node(const Ref<Array>& a, Type kind = Type::Array) noexcept  // NOLINT
        : type_(array_kind(kind)) {
        assert(a && "Node from null Ref<Array>");
        value_.p = a.get();
        detail::retain_payload(value_.p);
    }
    Node(Ref<Array>&& a, Type kind = Type::Array) noexcept : type_(array_kind(kind)) {  // NOLINT
        assert(a && "Node from null Ref<Array>");
        value_.p = a.release();
    }

    [[nodiscard]] static Node var(Symbol name) noexcept {
        Node n;
        n.type_ = Type::Var;
        n.value_.p = name.id();
        return n;
    }
    [[nodiscard]] static Node object(ObjectHandle h) noexcept {
        Node n;
        n.type_ = Type::Object;
        n.value_.u = h.bits;
        return n;
    }
    // `f` must outlive every node referring to it (function records are never freed).
    [[nodiscard]] static Node func(const FuncRecord* f) noexcept {
        Node n;
        n.type_ = Type::Func;
        n.value_.p = f;
        return n;
    }

    Node(const Node& o) noexcept : value_(o.value_), type_(o.type_) {
        if (counted()) detail::retain_payload(value_.p);
    }
    Node(Node&& o) noexcept : value_(o.value_), type_(o.type_) {
        o.type_ = Type::Int;
        o.value_.i = 0;
    }
    Node& operator=(const Node& o) noexcept {
        Node(o).swap(*this);
        return *this;
    }
    Node& operator=(Node&& o) noexcept {
        Node(std::move(o)).swap(*this);
        return *this;
    }
    ~Node() {
        if (counted()) detail::release_payload(value_.p);
    }
    void swap(Node& o) noexcept {
        std::swap(value_, o.value_);
        std::swap(type_, o.type_);
    }

    [[nodiscard]] Type type() const noexcept { return type_; }
    // Array, Command, or Property.
    [[nodiscard]] bool is_array_like() const noexcept {
        return type_ == Type::Array || type_ == Type::Command || type_ == Type::Property;
    }
    [[nodiscard]] bool is_number() const noexcept {
        return type_ == Type::Int || type_ == Type::Float;
    }
    // True if this is the symbol `s`: one pointer comparison, as lookup by tag needs.
    [[nodiscard]] bool is(Symbol s) const noexcept {
        // '&', not '&&': both sides are cheap, and one branch scans faster than two. The
        // pointer is copied out as bytes, which is defined whatever the node's type.
        const void* p;
        std::memcpy(&p, &value_, sizeof p);
        return (type_ == Type::Symbol) & (p == s.id());
    }

    // --- Checked typed accessors. ---
    [[nodiscard]] Expected<int64_t> as_int() const {
        if (auto v = try_int()) return *v;
        return Unexpected(type_error(Type::Int));
    }
    [[nodiscard]] Expected<double> as_float() const {
        if (auto v = try_float()) return *v;
        return Unexpected(type_error(Type::Float));
    }
    [[nodiscard]] Expected<Symbol> as_sym() const {
        if (type_ == Type::Symbol) return Symbol::from_id(value_.p);
        return Unexpected(type_error(Type::Symbol));
    }
    [[nodiscard]] Expected<std::string_view> as_str() const {
        if (type_ == Type::String) return string_payload()->view();
        return Unexpected(type_error(Type::String));
    }
    [[nodiscard]] Expected<const Array*> as_array() const {
        if (type_ == Type::Array) return static_cast<const Array*>(value_.p);
        return Unexpected(type_error(Type::Array));
    }
    // A number read as T, range-checked: as<int32_t>(), as<float>(), ...
    template <class T>
    [[nodiscard]] Expected<T> as() const {
        static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>,
                      "Node::as<T> supports integer and floating-point types");
        if constexpr (std::is_floating_point_v<T>) {
            auto v = as_float();
            if (!v) return Unexpected(std::move(v).error());
            if constexpr (sizeof(T) < sizeof(double)) {
                if (*v > static_cast<double>(std::numeric_limits<T>::max()) ||
                    *v < -static_cast<double>(std::numeric_limits<T>::max()))
                    return Unexpected(range_error());
            }
            return static_cast<T>(*v);
        } else {
            auto v = as_int();
            if (!v) return Unexpected(std::move(v).error());
            if (!std::in_range<T>(*v)) return Unexpected(range_error());
            return static_cast<T>(*v);
        }
    }

    // --- Weak accessors: nothing on a mismatch. ---
    [[nodiscard]] std::optional<int64_t> try_int() const noexcept {
        if (type_ == Type::Int) return value_.i;
        if (type_ == Type::Float) return detail::float_to_int(value_.f);
        return std::nullopt;
    }
    [[nodiscard]] std::optional<double> try_float() const noexcept {
        if (type_ == Type::Float) return value_.f;
        if (type_ == Type::Int) return static_cast<double>(value_.i);
        return std::nullopt;
    }
    [[nodiscard]] std::optional<Symbol> try_sym() const noexcept {
        if (type_ == Type::Symbol) return Symbol::from_id(value_.p);
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::string_view> try_str() const noexcept {
        if (type_ == Type::String) return string_payload()->view();
        return std::nullopt;
    }
    [[nodiscard]] const Array* try_array() const noexcept {
        return type_ == Type::Array ? static_cast<const Array*>(value_.p) : nullptr;
    }

    // --- Raw payload access: no coercion, no evaluation. Wrong type gives null/empty. ---
    // The array behind an Array, Command, or Property node.
    [[nodiscard]] const Array* array() const noexcept {
        return is_array_like() ? static_cast<const Array*>(value_.p) : nullptr;
    }
    [[nodiscard]] Array* mutable_array() const noexcept {
        return is_array_like() ? static_cast<Array*>(const_cast<void*>(value_.p)) : nullptr;
    }
    [[nodiscard]] const String* string() const noexcept {
        return type_ == Type::String ? string_payload() : nullptr;
    }
    [[nodiscard]] Symbol var_name() const noexcept {
        return type_ == Type::Var ? Symbol::from_id(value_.p) : Symbol();
    }
    [[nodiscard]] ObjectHandle object_handle() const noexcept {
        return type_ == Type::Object ? ObjectHandle{value_.u} : ObjectHandle{};
    }
    [[nodiscard]] const FuncRecord* func_record() const noexcept {
        return type_ == Type::Func ? static_cast<const FuncRecord*>(value_.p) : nullptr;
    }
    // Bit pattern of a Float node (0 otherwise); for exact comparisons.
    [[nodiscard]] uint64_t float_bits() const noexcept {
        return type_ == Type::Float ? std::bit_cast<uint64_t>(value_.f) : 0;
    }

private:
    [[nodiscard]] static constexpr Type array_kind(Type k) noexcept {
        return k == Type::Command || k == Type::Property ? k : Type::Array;
    }
    [[nodiscard]] bool counted() const noexcept {
        constexpr uint32_t kMask =
            (1u << static_cast<int>(Type::String)) | (1u << static_cast<int>(Type::Array)) |
            (1u << static_cast<int>(Type::Command)) | (1u << static_cast<int>(Type::Property));
        return ((1u << static_cast<int>(type_)) & kMask) != 0;
    }
    [[nodiscard]] Error type_error(Type expected) const;
    [[nodiscard]] Error range_error() const;
    [[nodiscard]] const String* string_payload() const noexcept {
        return static_cast<const String*>(value_.p);
    }
    explicit Node(const String* s) noexcept : value_{.p = s}, type_(Type::String) {
        assert(s && "Node from null Ref<String>");
        detail::retain_payload(value_.p);
    }

    friend struct detail::CacheLayout;
    union Value {
        int64_t i;
        double f;
        uint64_t u;
        const void* p;
    } value_;
    Type type_;
    // 7 bytes of padding remain; do not spend them without a reason.
};

static_assert(sizeof(Node) == 16, "Node must stay 16 bytes");
static_assert(alignof(Node) == 8, "Node must be 8-byte aligned");

}  // namespace baker
