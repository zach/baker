#pragma once

// Views: a C++ struct generated from a shape marked
// (@view), read from the records the shape governs. The struct reads data and never defines
// it: a record is readable without the struct, a field the record lacks reads as the
// member's default, and a field the shape does not declare is left in the tree.
//
//   // generated from #shape (move (@at fighters * moves) (@keyed) (@view) ...)
//   struct Move {                                     // by alignment, largest first
//       baker::ListView<baker::Symbol> cancel_into;
//       int32_t startup = 0;
//       float damage = 0;
//       static constexpr std::string_view kShapeName = "move";
//       static constexpr uint64_t kShapeDigest = 0x...;   // shape::view_digest of the shape
//   };
//   auto jab = baker::read_view<Move>(cfg, "fighters.kestrel.moves.jab");   // view_read.hpp
//
// This header is what a struct header reaches: names and small types only, so a file that
// only names a view pays little. Reading one is <baker/view_read.hpp>.

#include <baker/error.hpp>
#include <baker/symbol.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace baker {

class Array;
class Node;

// A view's name and the digest its struct was generated from, for check_views
// (the generated kViews). The name is tool text: empty under BAKER_SHIPPING.
struct ViewInfo {
    std::string_view name;
    uint64_t digest = 0;
};

// A member of a view that points outside the struct: what a baked table relocates
// into its pool, and an image fixes up at load. The generated kFixups lists each, by its
// offset in the struct; a nested struct's members are listed through the nested type's own
// list, and a list's elements through the element kind.
enum class ViewFixupKind : uint8_t {
    Symbol,  // a baker::Symbol: fixed up from an image's symbol table
    String,  // a std::string_view: its bytes go to the pool
    Curve,   // a baker::Curve: its points go to the pool as CurvePoint[]
    List,    // a baker::ListView<V>: its elements go to the pool as V[], each fixed up as
             // `element` says, with `nested` for a V that is a view
    Struct,  // a nested view: `nested` is its kFixups
};
// Reads one record or list element into the view at `out`, as ViewReader<T>::read does:
// what a Struct fix-up carries, so a table builder reads nested views without knowing T.
using ViewRead = bool (*)(const Array& record, void* out, Error* err);
// Constructs a default view at `out`: new (out) T{}.
using ViewInit = void (*)(void* out);
struct ViewFixup {
    uint32_t offset = 0;
    ViewFixupKind kind = ViewFixupKind::Symbol;
    // List: the elements' type. Int32, Float and Bool are copied; the rest as above.
    enum class Element : uint8_t { Int32, Float, Bool, Symbol, String, Curve, Struct };
    Element element = Element::Int32;
    uint32_t element_size = 0;  // List: sizeof(V)
    // Struct, or a List of structs: that view's fixups(), as a pointer and a count (a span
    // of the type being defined is not complete enough for every standard library).
    const ViewFixup* nested = nullptr;
    uint32_t nested_count = 0;
    ViewRead read = nullptr;  // Struct, or a List of structs: that view's reader
    ViewInit init = nullptr;  // and its default constructor

    [[nodiscard]] std::span<const ViewFixup> nested_fixups() const noexcept {
        return {nested, nested_count};
    }
};

// Tool text a view carries: its name. Shipping keeps none of it.
[[nodiscard]] constexpr std::string_view view_text(std::string_view text) noexcept {
    return kShipping ? std::string_view() : text;
}

// A list field of a view, (attacks (list symbol)): a view over the list's values, converting
// each as it is read. It borrows: from the tree, while a Ref to it is held (a reload never
// takes a held tree away), or, in a baked table, from the table's packed V[], which
// it reads directly. 16 bytes.
template <class V>
class ListView {
public:
    constexpr ListView() noexcept = default;

    [[nodiscard]] uint32_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    // Element i as a V, or an error naming the element and where the list is written.
    // Defined in <baker/view_read.hpp>.
    [[nodiscard]] Expected<V> at(uint32_t i) const noexcept;
    // Element i as a V, or V{} where it is out of range or does not convert.
    [[nodiscard]] V operator[](uint32_t i) const noexcept;

    class Iterator {
    public:
        constexpr Iterator(const ListView* list, uint32_t i) noexcept : list_(list), i_(i) {}
        [[nodiscard]] V operator*() const noexcept { return (*list_)[i_]; }
        Iterator& operator++() noexcept {
            ++i_;
            return *this;
        }
        [[nodiscard]] bool operator==(const Iterator& o) const noexcept { return i_ == o.i_; }

    private:
        const ListView* list_;
        uint32_t i_;
    };
    [[nodiscard]] Iterator begin() const noexcept { return {this, 0}; }
    [[nodiscard]] Iterator end() const noexcept { return {this, size_}; }

    // For a generated reader: over the values of `list`, a field's array (view_read.hpp).
    [[nodiscard]] static ListView over(const Array* list, uint32_t size) noexcept {
        ListView v;
        v.data_ = list;
        v.size_ = size;
        return v;
    }
    // For a baked table: over `size` converted elements the table owns.
    [[nodiscard]] static ListView packed(const V* elements, uint32_t size) noexcept {
        ListView v;
        v.data_ = elements;
        v.size_ = size;
        v.packed_ = 1;
        return v;
    }
    [[nodiscard]] bool is_packed() const noexcept { return packed_ != 0; }
    // The packed elements, or an empty span over a list that reads from the tree.
    [[nodiscard]] std::span<const V> elements() const noexcept {
        return packed_ ? std::span<const V>(static_cast<const V*>(data_), size_)
                       : std::span<const V>();
    }

private:
    const void* data_ = nullptr;  // the field's Array, or, packed, the table's V[]
    uint32_t size_ = 0;
    uint32_t packed_ = 0;  // 1 when data_ is V[]
};
static_assert(sizeof(ListView<float>) == 16);

// What the generator specializes for each view: reads `record`'s fields into `out`, in one
// pass over its children. False with `*err` set (if given) when a field has the wrong type,
// a value is out of its member's range, a list has the wrong count, or a required field is
// missing: located at the field, or the record.
template <class T>
struct ViewReader;

}  // namespace baker
