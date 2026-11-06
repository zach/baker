#pragma once

// Reading a view (<baker/view.hpp>): read_view<T> over a record, or over a Config at a
// path; what a generated reader (ViewReader<T>) calls for each field; and ListView's reads.
// This header reaches <baker/array.hpp> and <baker/config.hpp>, so it belongs in the files
// that read views, not in every file that names one.
//
//   auto jab = baker::read_view<Move>(cfg, "fighters.kestrel.moves.jab");
//   if (!jab) log(baker::to_string(jab.error()));   // "field 'startup' of 'move' is not an int
//   ..." int32_t frames = jab->startup;
//
// A view's strings and lists borrow from the tree it was read from: hold the Config's root()
// (a Ref) for as long as they are used; a reload never takes a held tree away. A Config read
// sees the tree as of its last apply_edits(), as every reader of views into the tree does.

#include <baker/array.hpp>
#include <baker/config.hpp>
#include <baker/curve.hpp>
#include <baker/view.hpp>

#include <cstdint>
#include <limits>
#include <string_view>

namespace baker {

// Reads `record` as a T. An error names the field, located where it is written.
template <class T>
[[nodiscard]] Expected<T> read_view(const Array& record) {
    T out{};
    Error err;
    if (ViewReader<T>::read(record, out, &err)) return out;
    return Unexpected(std::move(err));
}

namespace view_detail {

// The errors a reader reports, built out of line: nothing on the path that succeeds.
[[nodiscard]] Error wrong_type(const Array& field, std::string_view shape, std::string_view type);
[[nodiscard]] Error out_of_range(const Array& field, std::string_view shape, std::string_view cxx);
[[nodiscard]] Error bad_count(const Array& field, std::string_view shape, uint32_t min,
                              uint32_t max);
[[nodiscard]] Error missing(const Array& record, std::string_view shape, std::string_view field);
[[nodiscard]] Error no_record(const Array& root, const Path& path);
[[nodiscard]] Error bad_element(const Array& list, uint32_t i, std::string_view type);
// Element i of a packed list of `size` (a baked table's), which is past it.
[[nodiscard]] Error past_packed(uint32_t i, uint32_t size);
void at_path(Error& err, const Path& path);
// The first required field `seen` lacks a bit for, by its index in `names`.
[[nodiscard]] inline std::string_view first_missing(
    uint64_t seen, std::span<const std::string_view> names) noexcept {
    for (size_t i = 0; i < names.size(); ++i)
        if (!(seen & (uint64_t{1} << i))) return names[i];
    return {};
}

// The field's one value, or a type error.
[[nodiscard]] inline const Node* one(const Array& field, std::string_view shape,
                                     std::string_view type, Error* err) noexcept {
    const Values v = field.values();
    if (v.size() == 1) return &v[0];
    if (err) *err = wrong_type(field, shape, type);
    return nullptr;
}

template <class I>
[[nodiscard]] bool read_int(const Array& field, I& out, Error* err, std::string_view shape,
                            std::string_view cxx) noexcept {
    const Node* n = one(field, shape, "int", err);
    if (!n) return false;
    const auto i = n->type() == Type::Int ? n->try_int() : std::nullopt;
    if (!i) {
        if (err) *err = wrong_type(field, shape, "int");
        return false;
    }
    if (*i < std::numeric_limits<I>::min() || *i > std::numeric_limits<I>::max()) {
        if (err) *err = out_of_range(field, shape, cxx);
        return false;
    }
    out = static_cast<I>(*i);
    return true;
}

template <class F>
[[nodiscard]] bool read_float(const Array& field, F& out, Error* err, std::string_view shape,
                              std::string_view cxx) noexcept {
    const Node* n = one(field, shape, "float", err);
    if (!n) return false;
    const auto f = n->try_float();
    if (!f) {
        if (err) *err = wrong_type(field, shape, "float");
        return false;
    }
    if constexpr (sizeof(F) < sizeof(double)) {
        constexpr auto kMax = static_cast<double>(std::numeric_limits<F>::max());
        if (*f > kMax || *f < -kMax) {
            if (err) *err = out_of_range(field, shape, cxx);
            return false;
        }
    }
    out = static_cast<F>(*f);
    return true;
}

[[nodiscard]] inline bool read_bool(const Array& field, bool& out, Error* err,
                                    std::string_view shape) noexcept {
    const Node* n = one(field, shape, "bool", err);
    if (!n) return false;
    if (Values({n, 1}, &field).read(0, out)) return true;
    if (err) *err = wrong_type(field, shape, "bool");
    return false;
}

[[nodiscard]] inline bool read_symbol(const Array& field, Symbol& out, Error* err,
                                      std::string_view shape) noexcept {
    const Node* n = one(field, shape, "symbol", err);
    if (!n) return false;
    if (const auto s = n->try_sym()) {
        out = *s;
        return true;
    }
    if (err) *err = wrong_type(field, shape, "symbol");
    return false;
}

[[nodiscard]] inline bool read_string(const Array& field, std::string_view& out, Error* err,
                                      std::string_view shape) noexcept {
    const Node* n = one(field, shape, "string", err);
    if (!n) return false;
    if (const auto s = n->try_str()) {
        out = *s;
        return true;
    }
    if (err) *err = wrong_type(field, shape, "string");
    return false;
}

[[nodiscard]] inline bool read_curve(const Array& field, Curve& out, Error* err,
                                     std::string_view shape) noexcept {
    if (!baker::read_curve(field.values(), out)) return true;
    if (err) *err = wrong_type(field, shape, "curve");
    return false;
}

template <class V>
[[nodiscard]] bool read_list(const Array& field, ListView<V>& out, Error* err,
                             std::string_view shape, uint32_t min, uint32_t max) noexcept {
    const auto n = static_cast<uint32_t>(field.values().size());
    if (n < min || n > max) {
        if (err) *err = bad_count(field, shape, min, max);
        return false;
    }
    out = ListView<V>::over(&field, n);
    return true;
}

template <class T>
[[nodiscard]] bool read_nested(const Array& field, T& out, Error* err) noexcept {
    return ViewReader<T>::read(field, out, err);
}

// ViewReader<T>::read behind a ViewRead pointer, for a fix-up list.
template <class T>
[[nodiscard]] bool read_erased(const Array& record, void* out, Error* err) noexcept {
    return ViewReader<T>::read(record, *static_cast<T*>(out), err);
}
template <class T>
void init_erased(void* out) noexcept {
    new (out) T{};
}

}  // namespace view_detail

// Reads the record at `path` as a T, from the tree as of the last apply_edits(). An error
// names the field and the path.
template <class T>
[[nodiscard]] Expected<T> read_view(const Config& cfg, const Path& path) {
    const Ref<Array> root = cfg.root();
    const Array* record = root->try_get(path);
    if (!record) return Unexpected(view_detail::no_record(*root, path));
    Expected<T> r = read_view<T>(*record);
    if (!r) view_detail::at_path(r.error(), path);
    return r;
}

template <class V>
Expected<V> ListView<V>::at(uint32_t i) const noexcept {
    if (packed_) {
        if (i < size_) return static_cast<const V*>(data_)[i];
        return Unexpected(view_detail::past_packed(i, size_));
    }
    const auto* list = static_cast<const Array*>(data_);
    V out{};
    if (i < size_ && list->values().read(i, out)) return out;
    return Unexpected(view_detail::bad_element(*list, i, detail::convert_info<V>().name));
}

template <class V>
V ListView<V>::operator[](uint32_t i) const noexcept {
    if (packed_) return i < size_ ? static_cast<const V*>(data_)[i] : V{};
    const auto* list = static_cast<const Array*>(data_);
    V out{};
    if (i < size_) (void)list->values().read(i, out);
    return out;
}

}  // namespace baker
