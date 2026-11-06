// Record IDs (<baker/record_id.hpp>): parsing and writing, a record's own, the
// record an ID names, and the validator that reports two records sharing one.
#include <baker/record_id.hpp>

#include <baker/array.hpp>
#include <baker/config.hpp>
#include <baker/path.hpp>

#include "common/containers.hpp"
#include "data/markers.hpp"
#include "data/text.hpp"

#include <algorithm>

namespace baker {

std::optional<RecordId> RecordId::parse(std::string_view text) noexcept {
    if (text.size() != kDigits || text[0] < 'a' || text[0] > 'f') return std::nullopt;
    RecordId id;
    for (size_t i = 0; i < kDigits; ++i) {
        const char c = text[i];
        const unsigned v = c >= '0' && c <= '9'   ? static_cast<unsigned>(c - '0')
                           : c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10)
                                                  : 16;
        if (v == 16) return std::nullopt;
        uint64_t& word = i < 16 ? id.hi : id.lo;
        word = (word << 4) | v;
    }
    return id;
}

void RecordId::write(char (&out)[kDigits]) const noexcept {
    static constexpr char digits[] = "0123456789abcdef";
    uint64_t h = hi, l = lo;
    for (int i = 15; i >= 0; --i, h >>= 4, l >>= 4) {
        out[i] = digits[h & 0xF];
        out[16 + i] = digits[l & 0xF];
    }
}

RecordId Array::record_id() const noexcept {
    const Array* m = meta(detail::id_tag());
    if (!m) return {};
    const Values v = m->values();
    const auto sym = v.size() == 1 ? v[0].try_sym() : std::nullopt;
    if (!sym) return {};
    return RecordId::parse(sym->str()).value_or(RecordId{});
}

namespace {

// Every array below `a` (and `a`), in reading order, with the path to it: what the
// validator and find_by_id walk. `path` holds the tags from the root; an untagged array
// contributes its index.
template <class Visit>
bool walk(const Array& a, Vec<Str>& path, Visit&& visit) {
    if (!visit(a, path)) return false;
    const uint32_t first = a.tag().empty() ? 0 : 1;  // a root has no tag in slot 0
    for (uint32_t i = first; i < a.size(); ++i) {
        const Array* c = a[i].try_array();
        if (!c) continue;
        if (c->tag().empty())
            path.push_back(detail::decimal(static_cast<int64_t>(i - first)));
        else
            path.emplace_back(c->tag().str());
        const bool go_on = walk(*c, path, visit);
        path.pop_back();
        if (!go_on) return false;
    }
    return true;
}

Str dotted(const Vec<Str>& path) {
    Str out;
    for (const Str& seg : path) {
        if (!out.empty()) out += '.';
        if (detail::is_bare_symbol(seg.view()))
            out += seg;
        else
            out += "'" + seg + "'";
    }
    return out;
}

Str quoted_key(Symbol s) {
    return "'" + Str(s.str()) + "'";
}

Str slashed(const Vec<Str>& path) {
    Str out;
    for (const Str& seg : path) {
        if (!out.empty()) out += '/';
        if (detail::is_bare_symbol(seg.view()))
            out += seg;
        else
            out += "'" + seg + "'";
    }
    return out;
}

}  // namespace

const Array* find_by_id(const Array& root, RecordId id, Path* path) {
    if (id.empty()) return nullptr;
    const Array* found = nullptr;
    Vec<Str> segments;
    walk(root, segments, [&](const Array& a, const Vec<Str>& p) {
        if (a.record_id() == id) {
            found = &a;
            if (path) *path = Path(dotted(p).view());
            return false;
        }
        return true;
    });
    return found;
}

Config::Validator id_validator() {
    return [](const ValidatorStack& stack, const Callback<void(const Error&)>& warn) {
        if (!warn) return;
        struct Seen {
            RecordId id;
            const Array* record;
            Str path;
        };
        Vec<Seen> seen;
        Vec<Str> segments;
        Vec<Symbol> tags;
        walk(stack.root, segments, [&](const Array& a, const Vec<Str>& p) {
            // A key that is also a sibling's alias: both records answer to it.
            tags.clear();
            const uint32_t first = a.tag().empty() ? 0 : 1;
            bool any_alias = false;
            for (uint32_t i = first; i < a.size(); ++i)
                if (const Array* c = a[i].try_array(); c && !c->tag().empty()) {
                    tags.push_back(c->tag());
                    any_alias = any_alias || (c->meta_first() && c->meta(detail::aka_tag()));
                }
            if (any_alias)
                for (uint32_t i = first; i < a.size(); ++i) {
                    const Array* c = a[i].try_array();
                    const Array* aka = c && c->meta_first() ? c->meta(detail::aka_tag()) : nullptr;
                    if (!aka) continue;
                    for (const Node& old : aka->values()) {
                        const auto s = old.try_sym();
                        if (!s || std::find(tags.begin(), tags.end(), *s) == tags.end()) continue;
                        const Array* other = a.try_find(*s);
                        Error e = make_error(
                            ErrorCode::Refused,
                            quoted_key(*s) + " is a key of this table and an alias of " +
                                quoted_key(c->tag()) + " (" + Str(c->file().str()) + ":" +
                                detail::decimal(c->line()) +
                                "): a record answers to it twice; drop the"
                                " (@aka ...) or rename the record",
                            other ? other->file() : a.file(), other ? other->line() : a.line());
                        e.set_path(slashed(p));
                        warn(e);
                    }
                }
            const Array* m = a.meta(detail::id_tag());
            if (!m) return true;
            const Values v = m->values();
            const auto sym = v.size() == 1 ? v[0].try_sym() : std::nullopt;
            const auto id = sym ? RecordId::parse(sym->str()) : std::nullopt;
            if (!id) {
                Error e = make_error(ErrorCode::Syntax,
                                     "(@id ...) holds 32 lowercase hex digits, the first a to f: " +
                                         detail::preview(Node(Ref<Array>(const_cast<Array*>(m)))),
                                     m->file(), m->line());
                e.set_path(slashed(p));
                warn(e);
                return true;
            }
            seen.push_back({*id, &a, slashed(p)});
            return true;
        });
        std::stable_sort(seen.begin(), seen.end(),
                         [](const Seen& x, const Seen& y) { return x.id < y.id; });
        for (size_t i = 1; i < seen.size(); ++i) {
            if (!(seen[i].id == seen[i - 1].id)) continue;
            const Seen& first = seen[i - 1];
            const Seen& again = seen[i];
            char text[RecordId::kDigits];
            again.id.write(text);
            Error e =
                make_error(ErrorCode::Refused,
                           "two records share the id " + Str(std::string_view(text, 32)) + ": " +
                               first.path + " (" + Str(first.record->file().str()) + ":" +
                               detail::decimal(first.record->line()) + ") and " + again.path +
                               "; a copied record needs a fresh one "
                               "(format --baker-mint-ids)",
                           again.record->file(), again.record->line());
            e.set_path(again.path);
            warn(e);
        }
    };
}

}  // namespace baker
