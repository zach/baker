// The errors a view's reader reports (<baker/view_read.hpp>): built here, out of
// line, so a read that succeeds carries none of their code.
#include <baker/view_read.hpp>

#include "common/containers.hpp"
#include "data/text.hpp"

namespace baker::view_detail {

namespace {

Str field_label(const Array& field, std::string_view shape) {
    return "field " + detail::quoted(field.tag()) + " of " + detail::quoted(Symbol(shape));
}

// The field's value as written: its one value, or the whole field when it has several.
Str value_text(const Array& field) {
    const Values v = field.values();
    if (v.size() == 1) return detail::preview(v[0]);
    return detail::preview(Node(Ref<Array>(const_cast<Array*>(&field))));
}

}  // namespace

Error wrong_type(const Array& field, std::string_view shape, std::string_view type) {
    const Str article = type == "int" ? "an " : "a ";
    return make_error(
        ErrorCode::Type,
        field_label(field, shape) + " is not " + article + Str(type) + ": " + value_text(field),
        field.file(), field.line());
}

Error out_of_range(const Array& field, std::string_view shape, std::string_view cxx) {
    return make_error(
        ErrorCode::Range,
        field_label(field, shape) + " is " + value_text(field) + ", outside " + Str(cxx),
        field.file(), field.line());
}

Error bad_count(const Array& field, std::string_view shape, uint32_t min, uint32_t max) {
    const auto n = field.values().size();
    Str want = max == UINT32_MAX ? "at least " + detail::decimal(min)
               : min == max      ? detail::decimal(min)
                                 : detail::decimal(min) + " to " + detail::decimal(max);
    return make_error(ErrorCode::Range,
                      field_label(field, shape) + " holds " +
                          detail::decimal(static_cast<int64_t>(n)) + " value" +
                          (n == 1 ? "" : "s") + ", not " + want,
                      field.file(), field.line());
}

Error missing(const Array& record, std::string_view shape, std::string_view field) {
    return make_error(ErrorCode::NotFound,
                      detail::quoted(Symbol(shape)) + " has no " + detail::quoted(Symbol(field)),
                      record.file(), record.line());
}

Error no_record(const Array& root, const Path& path) {
    Error e = make_error(ErrorCode::NotFound, "no record at " + detail::text_of(path), root.file(),
                         root.line());
    e.set_path(detail::text_of(path));
    return e;
}

Error past_packed(uint32_t i, uint32_t size) {
    return make_error(ErrorCode::Range, "element " + detail::decimal(i) +
                                            " is past the packed list's " + detail::decimal(size) +
                                            " elements");
}

Error bad_element(const Array& list, uint32_t i, std::string_view type) {
    const Values v = list.values();
    const Str article = type == "int" ? "an " : "a ";
    Str msg = "element " + detail::decimal(i) + " of " + detail::quoted(list.tag());
    if (i < v.size())
        msg += " is not " + article + Str(type) + ": " + detail::preview(v[i]);
    else
        msg += " is past its " + detail::decimal(static_cast<int64_t>(v.size())) + " values";
    return make_error(ErrorCode::Type, msg, list.file(), list.line());
}

void at_path(Error& err, const Path& path) {
    err.set_path(detail::text_of(path));
}

}  // namespace baker::view_detail
