// Config's write-back: planning an edit of a layer's file, writing it, and promoting
// scratch. The layer tools (explain, layer_effect, diff) are baker::tools'.

#include "arena.hpp"
#include "config_impl.hpp"

#include <baker/parse.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace baker {
namespace {

// Creates each missing parent directory of `path`. The library avoids std::filesystem,
// so this is the one place it creates directories, for an optional layer's file.
void make_parents(const Str& path) {
    for (size_t at = path.find_first_of("/\\", 1); at != Str::npos;
         at = path.find_first_of("/\\", at + 1)) {
        const Str dir = path.substr(0, at);
#if defined(_WIN32)
        (void)_mkdir(dir.c_str());
#else
        (void)mkdir(dir.c_str(), 0777);
#endif
    }
}

int32_t line_at(std::string_view text, uint32_t offset) {
    const auto end = text.begin() + std::min<size_t>(offset, text.size());
    return 1 + static_cast<int32_t>(std::count(text.begin(), end, '\n'));
}

// Every tagged child's leaves, a repeated tag's included, each path once: all a layer
// contributes, however its tags repeat. Their values are the caller's to fill.
// The paths a layer's tree holds values at, spelled as the merged tree spells them: a
// child keyed by a record's alias or ID (`merged` given) takes the record's
// key, so the write lands in the record wherever the target spells it.
void collect_paths(const Array& a, size_t first, const Str& prefix, HashMap<Str, char>& seen,
                   Vec<Leaf>& out, const Array* merged) {
    for (size_t i = first; i < a.size(); ++i) {
        if (!is_tagged(a[i])) continue;
        const Array& child = *a[i].try_array();
        if (child.tag().is_meta() && child.tag() == detail::was_tag()) continue;  // the layer's own
        Symbol name = child.tag();
        const Array* record = merged ? merged->try_find(name) : nullptr;
        if (merged && !record && !name.is_meta())
            for (size_t m = 1; m < merged->size() && !record; ++m) {
                const Array* c = (*merged)[m].try_array();
                if (!c || !c->meta_first()) continue;
                const Array* aka = c->meta(detail::aka_tag());
                const bool by_alias = aka && std::any_of(aka->values().begin(), aka->values().end(),
                                                         [&](const Node& n) { return n.is(name); });
                if (by_alias || detail::id_symbol(*c) == name) {
                    record = c;
                    name = c->tag();
                }
            }
        Str path = prefix;
        if (!path.empty()) path += '.';
        detail::append_path_name(path, name.str());
        if (has_values(child) && seen.emplace(path, 0).second) out.push_back({path, &child, {}});
        if (!detail::composes(child)) collect_paths(child, 1, path, seen, out, record);  // operands
    }
}

Str join(const Vec<Node>& values) {
    Str out;
    for (const Node& n : values) {
        if (!out.empty()) out += ' ';
        out += detail::text_of(n);
    }
    return out;
}

// path[from..] holding `values`, in the pretty-printer's form: (a (b value)). Lines after
// the first are indented by `indent`.
Str render_entry(const Path& path, size_t from, const Vec<Node>& values, const Str& indent,
                 const WriteOptions& layout) {
    Vec<Node> inner{Node(path[path.size() - 1].name)};
    inner.insert(inner.end(), values.begin(), values.end());
    Ref<Array> nest = Array::make_from(inner);
    for (size_t i = path.size() - 1; i-- > from;) nest = array(path[i].name, Node(nest));
    Str text = detail::dta_of(*array(Node(nest)), layout);
    while (!text.empty() && text.back() == '\n') text.pop_back();
    for (size_t at = text.find('\n'); at != Str::npos; at = text.find('\n', at + 1))
        text.insert(at + 1, indent);
    return text;
}

struct Edit {
    uint32_t begin, end;
    Str text;
};

// Whether `main`'s text holds a row at its top level, written "(/": then a key the layer
// lacks is added to it as a row.
bool holds_rows(const SourceMap& map, const Array& root, Symbol main, std::string_view text) {
    for (size_t i = 0; i < root.size(); ++i) {
        const SourceSpan* sp = map.span(root, i);
        if (sp && sp->literal() && sp->file == main && text.substr(sp->begin, 2) == "(/")
            return true;
    }
    return false;
}

// (/a.b.c values): empty when a segment cannot be written in a key (an element, a byte that
// ends an atom), or would open a comment.
Str render_row(const Path& path, const Vec<Node>& values) {
    for (size_t i = 0; i < path.size(); ++i)
        if (path[i].is_index) return {};
    const Str key = detail::text_of(path, SIZE_MAX, '.');
    for (const char c : key)
        if (detail::is_atom_end(static_cast<unsigned char>(c))) return {};
    return key[0] == '*' ? Str() : "(/" + key + (values.empty() ? "" : " ") + join(values) + ")";
}

// Deleting `path`'s array from `layer`'s file, with its line when it stands alone there: a
// removal promoted. Nothing to do when the file does not hold the key.
Expected<Plan> plan_removal(const LayerState& layer, const Path& path,
                            const LoadSession::Reader& read_file) {
    if (!layer.map || layer.text)
        return Unexpected(refused("layer '" + layer.name + "' has no file read in edit mode"));
    const Array* cur = layer.root.get();
    const SourceSpan* sp = nullptr;
    for (size_t i = 0; i < path.size() && cur; ++i) {
        const Array* parent = cur;
        cur = nullptr;
        for (size_t k = 0; k < parent->size() && !cur; ++k)
            if (!path[i].is_index && detail::tag_of((*parent)[k]) == path[i].name) {
                cur = (*parent)[k].try_array();
                sp = layer.map->span(*parent, k);
            }
        if (cur && (!sp || !sp->literal()))
            return Unexpected(refused("promote cannot remove " + quoted(path) +
                                          ", which is not written as itself in '" + layer.file +
                                          "'; remove it by hand",
                                      cur->file(), cur->line()));
    }
    if (!cur) return Plan{Symbol(layer.file), {}, false};
    // A key that is a segment of rows' keys is written in each of them.
    if (layer.map->row_made(*cur))
        return Unexpected(refused("promote cannot remove " + quoted(path) + ", which '" +
                                      layer.file +
                                      "' writes as part of rows' keys; remove those rows by hand",
                                  cur->file(), cur->line()));
    const auto text = layer.map->text(sp->file);
    Str out;
    if (!text || !detail::read_with(read_file, sp->file.str(), out) || out != *text)
        return Unexpected(refused("'" + Str(sp->file.str()) +
                                      "' has changed since it was read; reload before writing back",
                                  sp->file));
    size_t b = sp->begin, e = sp->end;
    while (b > 0 && (out[b - 1] == ' ' || out[b - 1] == '\t')) --b;
    while (e < out.size() && (out[e] == ' ' || out[e] == '\t')) ++e;
    const bool line_start = b == 0 || out[b - 1] == '\n';
    if (line_start && e < out.size() && out[e] == ';')  // its comment goes with it
        e = std::min(out.find('\n', e), out.size());
    if (line_start && (e == out.size() || out[e] == '\n' || out[e] == '\r'))
        e = std::min(out.find('\n', e), out.size() - 1) + 1;  // the line, which held it alone
    else if (line_start)  // closers follow on its line: they join the line before
        for (e = sp->end; b > 0 && (out[b - 1] == ' ' || out[b - 1] == '\t' || out[b - 1] == '\n' ||
                                    out[b - 1] == '\r');)
            --b;
    else
        e = sp->end;
    out.erase(b, e - b);
    return Plan{sp->file, std::move(out), true};
}

}  // namespace

namespace config_internal {
void leaf_values(const Array& a, Vec<Node>& out) {
    if (detail::composes(a)) {
        const Values v = a.values();
        for (size_t i = 1; i < v.size(); ++i) out.push_back(v[i]);
        return;
    }
    for (size_t i = 1; i < a.size(); ++i)
        if (!is_tagged(a[i])) out.push_back(a[i]);
}
Vec<Node> leaf_values(const Array& a) {
    Vec<Node> out;
    leaf_values(a, out);
    return out;
}

const Array* row_at(const Array& root, const Path& path, const Array* merged) {
    detail::Resolved r;
    const bool ok = detail::resolve_in_layer(root, last_is_index(path) ? parent_of(path) : path,
                                             merged, r) == detail::ResolveStatus::Ok;
    return ok ? r.array : nullptr;
}

bool stale(const Array& row, const Vec<Node>* below) {
    const Array* pin = row.has_marker() ? row.meta(detail::was_tag()) : nullptr;
    if (!pin) return false;
    const auto was = pin->values().nodes();
    return below ? !same_nodes(was, *below) : !was.empty();
}

void row_warnings(const Array& row, std::string_view path, const Vec<Node>* below,
                  const Array& stack, Vec<Error>& out) {
    if (!row.has_marker()) return;
    auto warn = [&](const Str& why) {
        out.push_back(
            make_error(ErrorCode::NotFound, "'" + Str(path) + "' " + why, row.file(), row.line()));
    };
    if (stale(row, below)) {
        const auto was = row.meta(detail::was_tag())->values().nodes();
        warn("was " + detail::text_of(pack(Vec<Node>(was.data(), was.data() + was.size()))) +
             " when this row was written; the stack has " +
             (below ? detail::text_of(pack(*below)) : Str("none")));
    }
    const detail::Marker verb = detail::verb_of(row);
    if (verb != detail::Marker::Append && verb != detail::Marker::Drop) return;
    // The run a verb applies to: every value below, a keyed tuple's included.
    const Array* run = row_at(stack, Path(path));
    const auto there = run ? run->values().nodes() : std::span<const Node>();
    const Values operands = row.values();
    // The run by hash, so each operand is found in it in log time, not by a scan.
    struct Held {
        uint64_t hash;
        const Node* node;
    };
    Vec<Held> held;
    held.reserve(there.size());
    for (const Node& n : there) held.push_back({detail::structural_hash(n), &n});
    std::sort(held.begin(), held.end(),
              [](const Held& a, const Held& b) { return a.hash < b.hash; });
    for (const Node& v : operands.nodes().subspan(1)) {
        const uint64_t h = detail::structural_hash(v);
        bool has = false;
        for (auto it = std::lower_bound(held.begin(), held.end(), h,
                                        [](const Held& x, uint64_t want) { return x.hash < want; });
             !has && it != held.end() && it->hash == h; ++it)
            has = structurally_equal(*it->node, v);
        if (verb == detail::Marker::Append && has)
            warn("already has " + detail::preview(v) + "; #append adds another");
        if (verb == detail::Marker::Drop && !has) warn("has no " + detail::preview(v) + " to drop");
    }
}

// Each tag's first child only, as lookup sees it: found by the tag run's scan in a small
// array, and by a set past kIndexed children, so a wide layer is linear.
// How many children of `a` from `first` have `tag`, a row that composes left out.
static uint32_t rows_tagged(const Array& a, size_t first, Symbol tag, bool marked) {
    uint32_t n = 0;
    for (size_t k = first; k < a.size(); ++k)
        if (const Array* s = a[k].try_array();
            s && (*s)[0].is(tag) && !(marked && detail::composes(*s)))
            ++n;
    return n;
}

static void collect_leaves(const Array& a, size_t first, const Str& prefix, Vec<Leaf>& out) {
    constexpr size_t kIndexed = 32;
    constexpr uint32_t kEmitted = 0x80000000U;
    const bool indexed = a.size() > kIndexed;
    const bool marked = a.has_marker();  // a verb or a pin below: one test for all children
    // A long array that repeats no tag, the usual case, has each child as its key's first
    // row: its tags sorted tell, which costs less than a hash of them. One that repeats gets
    // its rows by tag, counted in one pass: the first of each is the leaf, and the rest fold
    // into it. kEmitted marks a tag whose first row is out.
    HashMap<Symbol, uint32_t> rows;
    bool repeats = false;
    if (indexed) {
        Vec<const void*> ids;
        ids.reserve(a.size());
        for (size_t k = first; k < a.size(); ++k)
            if (is_tagged(a[k])) ids.push_back((*a[k].try_array())[0].try_sym()->id());
        std::sort(ids.begin(), ids.end());
        repeats = std::adjacent_find(ids.begin(), ids.end()) != ids.end();
    }
    if (repeats)
        for (size_t k = first; k < a.size(); ++k)
            if (is_tagged(a[k])) {
                const Array& s = *a[k].try_array();
                rows[*s[0].try_sym()] += marked && detail::composes(s) ? 0 : 1;
            }
    for (size_t i = first; i < a.size(); ++i) {
        if (!is_tagged(a[i])) continue;
        const Array& child = *a[i].try_array();
        const Symbol tag = *child[0].try_sym();
        if (marked && tag.is_meta() && tag == detail::was_tag()) continue;  // the layer's own pin
        // A key's later rows are leaves too when they compose: each applies, in order.
        uint32_t* count = repeats ? &rows[tag] : nullptr;
        const bool first_row = count     ? (*count & kEmitted) == 0
                               : indexed ? true
                                         : a.try_find(tag) == &child;
        if (count) *count |= kEmitted;
        const bool composes = marked && detail::composes(child);
        if (!first_row && !composes) continue;
        Str path = prefix;
        if (!path.empty()) path += '.';
        detail::append_path_name(path, tag.str());
        if (has_values(child)) {
            // Its later siblings of the same tag fold into it; tools only.
            uint32_t folded = 0;
            if constexpr (!kShipping)
                if (!composes)
                    folded = count     ? (*count & ~kEmitted) - 1
                             : indexed ? 0
                                       : rows_tagged(a, i, tag, marked) - 1;
            out.push_back({path, &child, leaf_values(child), folded, &a});
        }
        if (!composes) collect_leaves(child, 1, path, out);  // operands are not keys
    }
}

void declarations_of(const LoadSession& session, Symbol root, std::span<const Dep> files,
                     Vec<Declaration>& out) {
    auto mine = [&](Symbol file) {
        return file == root || std::any_of(files.begin(), files.end(), [&](const Dep& d) {
                   return d.path.view() == file.str();
               });
    };
    for (const ShapeDecl& d : session.shapes())
        if (d.root == root)
            out.push_back({Symbol("#shape"), d.form->tag(), Node(d.form), d.file, d.line});
    for (const Pack& p : session.packs())
        if (mine(p.file)) {
            Vec<Node> level{Node(Symbol("level")), Node(p.level)};
            Vec<Node> form{Node(p.name), Node(Array::make_from(level))};
            out.push_back({Symbol("#pack"), p.name, Node(Array::make_from(form)), p.file, 0});
        }
    for (const Origin& o : session.origins())
        if (mine(o.file)) {
            Vec<Node> form{Node(String::make(o.source.str()))};
            if (!o.tool.empty()) {
                Vec<Node> tool{Node(Symbol("tool")), Node(o.tool)};
                form.emplace_back(Array::make_from(tool));
            }
            out.push_back({Symbol("#origin"), o.file, Node(Array::make_from(form)), o.file, 0});
        }
}

Vec<Leaf> leaves_of(const Array& root) {
    Vec<Leaf> out;
    collect_leaves(root, 0, "", out);
    return out;
}

ReportTexts::ReportTexts(size_t count, size_t chars) noexcept
    : expected_(count * (sizeof(String) + 1 + alignof(Node)) + chars) {}

ReportTexts::~ReportTexts() {
    if (arena_) const detail::Arena::Handle creator(arena_);  // the strings keep it
}

Ref<String> ReportTexts::operator()(std::string_view text) {
    if (!arena_) arena_ = detail::Arena::create(default_allocator(MemoryKind::Config), expected_);
    return arena_->make_string(text);
}

bool write_disk_file(std::string_view where, std::string_view text) {
    const Str path(where);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        make_parents(path);
        f = std::fopen(path.c_str(), "wb");
    }
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

// The value to write over `old`: a whole float over an integer token stays an integer, as
// most hosts tune numbers in float fields: (height 96) tuned to 120.0 is written
// (height 120). A fraction, a NaN or infinity, or a whole value no int64 holds stays a
// float.
Node as_written(const Node& old, const Node& value) {
    if (old.type() != Type::Int || value.type() != Type::Float) return value;
    const double d = *value.try_float();
    if (!std::isfinite(d) || d != std::trunc(d) || d < -9223372036854775808.0 ||
        d >= 9223372036854775808.0)
        return value;
    return Node(static_cast<int64_t>(d));
}

// One node for a value: itself, or (v1 v2 ...) for several.
}  // namespace config_internal

// Plans writing `values` as `path`'s value into `layer`'s file: replacing only the tokens
// that hold the old value or, with `insert`, adding the entry as the last child of the
// deepest ancestor written in that file (or creating an optional layer's absent file).
// `read_file` gives the file as it is now; it must match what the layer was read from.
Expected<Plan> Config::Impl::plan(const LayerState& layer, const Path& path, const Vec<Node>& given,
                                  bool insert, const LoadSession::Reader& read_file,
                                  const Node* pin) const {
    Vec<Node> values = given;  // a new row's: the pin follows its values
    if (pin) values.push_back(*pin);
    if (layer.path.empty())
        return Unexpected(refused(quoted(path) + " comes from layer '" + layer.name +
                                  "', which has no source file"));
    if (layer.text)
        return Unexpected(refused(quoted(path) + " comes from layer '" + layer.name +
                                  "', which was pushed as text and has no file"));
    if (!layer.map)
        return Unexpected(refused("layer '" + layer.name +
                                  "' was not read in edit mode, so it cannot be written"));
    const SourceMap& map = *layer.map;
    const Symbol main(layer.file);
    auto unchanged = [&](Symbol file) -> std::optional<Error> {
        const auto orig = map.text(file);
        Str disk;
        if (!orig || !detail::read_with(read_file, file.str(), disk) || disk != *orig)
            return refused("'" + Str(file.str()) +
                               "' has changed since it was read; reload before writing back",
                           file);
        return std::nullopt;
    };
    auto no_index = [&](size_t from, Symbol file, int32_t line) -> std::optional<Error> {
        for (size_t i = from; i < path.size(); ++i)
            if (path[i].is_index)
                return refused(quoted(path) + " names element " + detail::decimal(path[i].index) +
                                   ", which layer '" + layer.name + "' does not have",
                               file, line);
        return std::nullopt;
    };

    if (layer.missing) {
        if (!insert) return Unexpected(detail::get_error(*layer.root, path, nullptr));
        if (auto e = no_index(0, main, 0)) return Unexpected(*e);
        Str disk;
        if (detail::read_with(read_file, layer.file, disk))
            return Unexpected(refused(
                "'" + layer.file + "' has appeared since it was read; reload before writing back",
                main));
        // A layer file a tool creates is rows: one line a value.
        if (const Str row = render_row(path, values); !row.empty())
            return Plan{main, row + "\n", true};
        return Plan{main, render_entry(path, 0, values, "", layout) + "\n", true};
    }

    // Walk the layer's own tree, checking that every step was written literally in a file.
    auto not_literal = [&](const Array& parent, size_t k) -> std::optional<Error> {
        const SourceSpan* sp = map.span(parent, k);
        if (!sp)
            return refused(quoted(path) +
                               " is in an array assembled by #merge or #include_dir; edit it where "
                               "it is defined",
                           parent.file(), parent.line());
        if (!sp->macro.empty()) {
            const auto text = map.text(sp->file);
            return refused(quoted(path) + " comes from macro '" + Str(sp->macro.str()) +
                               "'; edit the macro's definition instead",
                           sp->file, text ? line_at(*text, sp->begin) : 0);
        }
        if (sp->file.empty()) return refused(quoted(path) + " has no source text");
        return std::nullopt;
    };
    const Array* cur = layer.root.get();
    const Array* mer = alias_root();  // the merged record beside `cur`, for its aliases and ID
    size_t offset = 0;  // where index 0 is: 1 after a tag, 0 in the root or an element
    Vec<size_t> targets;
    size_t missing_from = path.size();
    const auto child_named = [&](Symbol name) {
        size_t k = cur->size();
        for (size_t j = 0; j < cur->size() && k == cur->size(); ++j)
            if (is_tagged((*cur)[j]) && (*(*cur)[j].try_array())[0].is(name)) k = j;
        return k;
    };
    for (size_t i = 0; i < path.size(); ++i) {
        const Path::Segment& seg = path[i];
        size_t k = cur->size();
        if (seg.is_index) {
            k = offset + seg.index < cur->size() ? offset + seg.index : cur->size();
            mer = nullptr;
        } else {
            k = child_named(seg.name);
            // The layer may spell the record by an alias or its ID.
            const Array* rec = mer ? mer->try_find(seg.name) : nullptr;
            if (k == cur->size() && rec && rec->meta_first()) {
                if (const Array* aka = rec->meta(detail::aka_tag()))
                    for (const Node& old : aka->values())
                        if (const auto o = old.try_sym(); o && k == cur->size())
                            k = child_named(*o);
                if (const Symbol id = detail::id_symbol(*rec); !id.empty() && k == cur->size())
                    k = child_named(id);
            }
            mer = rec;
        }
        if (k == cur->size()) {
            missing_from = i;
            break;
        }
        if (auto e = not_literal(*cur, k)) return Unexpected(*e);
        const bool last = i + 1 == path.size();
        if (last && seg.is_index) {
            targets.push_back(k);
            break;
        }
        const Array* next = (*cur)[k].try_array();
        if (!next) return Unexpected(detail::get_error(*layer.root, path, nullptr));
        cur = next;
        offset = seg.is_index ? 0 : 1;
        if (last && detail::composes(*cur))
            return Unexpected(refused(quoted(path) + " is composed in layer '" + layer.name +
                                          "', so it has no value there to write over",
                                      cur->file(), cur->line()));
        if (last)
            for (size_t j = 1; j < cur->size(); ++j)
                if (!is_tagged((*cur)[j])) targets.push_back(j);
    }

    Vec<Edit> edits;
    Symbol file;
    // A key a row's key made has no text of its own: values for it, or a key under it,
    // go in a row of their own.
    const bool row_made = map.row_made(*cur);
    const bool own_row = missing_from == path.size() && targets.empty() && row_made;
    if (own_row) missing_from = path.size() - 1;  // the key has values now: one more row
    if (missing_from < path.size()) {
        if (auto e = no_index(missing_from, cur->file(), cur->line())) return Unexpected(*e);
        if (!insert && !own_row) return Unexpected(detail::get_error(*layer.root, path, nullptr));
        file = main;
        const auto text = map.text(main);
        if (!text) return Unexpected(refused("no source text for '" + layer.file + "'", main));
        // In a file of rows, one more row: after the last row under the deepest key the file
        // already has, so it stands beside its siblings; at the end when it shares none.
        Str row;
        if (row_made || holds_rows(map, *layer.root, main, *text)) row = render_row(path, values);
        if (row.empty() && row_made)
            return Unexpected(refused("cannot add " + quoted(path) + " to '" + layer.file +
                                          "' as a row; write it nested by hand",
                                      cur->file(), cur->line()));
        if (!row.empty() && cur != layer.root.get()) {
            uint32_t after = 0;
            if (const auto spans = map.spans(*cur))
                for (const SourceSpan& sp : *spans)
                    if (sp.file == main && sp.end > after) after = sp.end;
            if (const SourceSpan* ext = map.extent(*cur); ext && ext->file == main)
                after = std::max(after, ext->end);
            const size_t line_end = text->find('\n', after);
            const auto at = static_cast<uint32_t>(line_end == Str::npos ? text->size() : line_end);
            edits.push_back({at, at, "\n" + row});
        } else if (!row.empty()) {
            const bool newline = !text->empty() && text->back() != '\n';
            const auto end = static_cast<uint32_t>(text->size());
            edits.push_back({end, end, (newline ? "\n" : "") + row + "\n"});
        } else if (cur == layer.root.get()) {
            // A new top-level form at the end of the file.
            const bool newline = !text->empty() && text->back() != '\n';
            const auto end = static_cast<uint32_t>(text->size());
            edits.push_back({end, end,
                             (newline ? "\n" : "") +
                                 render_entry(path, missing_from, values, "", layout) + "\n"});
        } else {
            // The last child of the deepest ancestor written in this file itself.
            const SourceSpan* ext = map.extent(*cur);
            if (!ext || ext->file != main)
                return Unexpected(
                    refused("cannot add " + quoted(path) + " to '" + layer.file +
                                "': the array it would go in comes from another file (#include, "
                                "#include_dir or #merge), not this one",
                            cur->file(), cur->line()));
            const SourceSpan* last = map.span(*cur, cur->size() - 1);
            if (!last || last->file != main || last->end > text->size() || ext->end == 0)
                return Unexpected(
                    refused("cannot find where to add " + quoted(path), cur->file(), cur->line()));
            const bool one_line = text->find('\n', ext->begin) == Str::npos ||
                                  text->find('\n', ext->begin) >= last->begin;
            if (one_line) {
                edits.push_back({last->end, last->end,
                                 " " + render_entry(path, missing_from, values, "", layout)});
            } else {
                // Indented like the previous sibling, after the rest of its line.
                const size_t line_start = text->rfind('\n', last->begin) + 1;
                size_t ws = line_start;
                while (ws < last->begin && ((*text)[ws] == ' ' || (*text)[ws] == '\t')) ++ws;
                const Str indent(text->substr(line_start, ws - line_start));
                const size_t line_end = text->find('\n', last->end);
                const auto at = static_cast<uint32_t>(
                    line_end != Str::npos && line_end < ext->end - 1 ? line_end : last->end);
                edits.push_back(
                    {at, at,
                     "\n" + indent + render_entry(path, missing_from, values, indent, layout)});
            }
        }
    } else {
        // The pin goes after the values, or in place of the one the row has; a row with no
        // values gets it with them.
        const Array* was = pin ? cur->meta(detail::was_tag()) : nullptr;
        if (pin && (was || !targets.empty())) {
            values.pop_back();
            size_t k = targets.empty() ? 0 : targets.back();
            for (size_t j = 0; was && j < cur->size(); ++j)
                if ((*cur)[j].try_array() == was) k = j;
            if (auto e = not_literal(*cur, k)) return Unexpected(*e);
            const SourceSpan& sp = *map.span(*cur, k);
            edits.push_back(
                {was ? sp.begin : sp.end, sp.end, Str(was ? "" : " ") + detail::text_of(*pin)});
        }
        Vec<Node> old;
        for (size_t k : targets) {
            if (auto e = not_literal(*cur, k)) return Unexpected(*e);  // e.g. (width kWidth)
            const SourceSpan& sp = *map.span(*cur, k);
            if (file.empty()) file = sp.file;
            if (sp.file != file)
                return Unexpected(refused(quoted(path) + " is written across several files",
                                          cur->file(), cur->line()));
            old.push_back((*cur)[k]);
        }
        if (targets.size() == values.size()) {
            for (size_t i = 0; i < targets.size(); ++i) {
                const Node value = as_written(old[i], values[i]);
                if (structurally_equal(old[i], value)) continue;
                const SourceSpan& sp = *map.span(*cur, targets[i]);
                edits.push_back({sp.begin, sp.end, detail::text_of(value)});
            }
        } else if (targets.empty()) {
            if (auto e = not_literal(*cur, 0)) return Unexpected(*e);
            const SourceSpan* tag = map.span(*cur, 0);
            file = tag->file;
            edits.push_back({tag->end, tag->end, " " + join(values)});
        } else {
            for (size_t i = 1; i < targets.size(); ++i)
                if (targets[i] != targets[i - 1] + 1)
                    return Unexpected(
                        refused("the values of " + quoted(path) +
                                    " are split by nested arrays, so their number cannot change",
                                cur->file(), cur->line()));
            edits.push_back({map.span(*cur, targets.front())->begin,
                             map.span(*cur, targets.back())->end, join(values)});
        }
    }
    if (edits.empty()) return Plan{file, {}, false};

    if (auto e = unchanged(file)) return Unexpected(*e);
    Str text(*map.text(file));
    std::sort(edits.begin(), edits.end(),
              [](const Edit& a, const Edit& b) { return a.begin > b.begin; });
    for (const Edit& e : edits) {
        if (e.begin > e.end || e.end > text.size())
            return Unexpected(refused("source spans do not match '" + Str(file.str()) + "'", file));
        text.replace(e.begin, e.end - e.begin, e.text);
    }
    return Plan{file, std::move(text), true};
}

// Writes one value into layer `li`, re-reads that layer and those above (trigger
// write_back), and drops the scratch entry the file now holds.
Expected<WriteReport> Config::Impl::write_into(Config& c, size_t li, const Path& path,
                                               const Vec<Node>& values, bool insert,
                                               const Node* pin) {
    if (auto e = composed(path, li)) return Unexpected(*e);
    auto p = plan(layers[li], path, values, insert, base.reader(), pin);
    if (!p) return Unexpected(std::move(p).error());
    const Ref<Array> target_before = layers[li].root;  // for the log's records
    ReloadRecord rec = begin(trigger_name("write_back"));
    uint64_t t = rec.start_ns;
    if (p->changed) {
        if (!writer(Str(p->file.str()), p->text))
            return Unexpected(
                make_error(ErrorCode::Io, "cannot write '" + Str(p->file.str()) + "'", p->file));
        rec.first_file = p->file;
        lap(rec.write_ns, t);
        if (auto r = reread_stages(rec, li, t); !r) {
            finish(c, rec, t);
            return Unexpected(std::move(r).error());
        }
    }
    const bool log = logging();  // the write as one transaction, with its line
    if (log) {
        begin_change(trigger_name("write_back"));
        walk_changes(c, *target_before, *layers[li].root, Symbol(layers[li].name.view()));
    }
    // The file holds the value now. Drop it from scratch, unless a layer above still hides it.
    const Path key = last_is_index(path) ? parent_of(path) : path;
    const auto it = std::find_if(scratch.begin(), scratch.end(),
                                 [&](const ScratchEntry& e) { return e.path == key; });
    ScratchEntry written_edit;  // its journal entry gets the line written
    if (it != scratch.end()) {
        written_edit.key = it->key;
        written_edit.edit = it->edit;
        // The layer that now supplies the value, without a merge: overwrite merging means
        // the highest enabled layer that has it won.
        const auto top = supplier(path, layers.size());
        Vec<Node> now_values;
        if (top && value_nodes(*layers[*top].root, path, now_values) &&
            same_nodes(now_values, values))
            scratch.erase(it);
    }
    commit(c, rec, t);
    finish(c, rec, t);
    WriteReport report;
    const Source s = source_in(layers[li], path);
    if (p->changed) {
        const WrittenLine line{s.file, s.line};
        report.written = List<WrittenLine>(std::span<const WrittenLine>(&line, 1));
        if (log) changes.pending_written.push_back(line);
    }
    if (log) close_change();
    settle(written_edit, EditFate::Written, {s.file, s.line});
    return report;
}

// --- Write-back ---

Expected<WriteReport> Config::write_back(const Path& path) {
    if constexpr (kShipping) return Unexpected(shipping("write-back"));
    if (!path.valid()) return Unexpected(detail::get_error(*root_, path, nullptr));
    Vec<Node> values;
    if (!impl_->current(*this, path, values))
        return Unexpected(detail::get_error(*root_, path, nullptr));
    const auto at = impl_->supplier(path, impl_->layers.size());
    if (auto e = at ? std::nullopt : impl_->composed(path, 0)) return Unexpected(*e);
    if (!at)
        return Unexpected(refused(
            quoted(path) + " is not in any layer's file; use write_back_to to add it to one"));
    return impl_->write_into(*this, *at, path, values, false);
}

Expected<WriteReport> Config::write_back_to(const Path& path, std::string_view layer, bool pin) {
    if constexpr (kShipping) return Unexpected(shipping("write-back"));
    if (!path.valid()) return Unexpected(detail::get_error(*root_, path, nullptr));
    if (layer == "scratch") return Unexpected(refused("scratch is never written to disk"));
    Vec<Node> values;
    if (!impl_->current(*this, path, values))
        return Unexpected(detail::get_error(*root_, path, nullptr));
    const auto at = impl_->find_layer(layer);
    if (!at)
        return Unexpected(make_error(ErrorCode::NotFound, "no layer named '" + Str(layer) + "'"));
    if (!pin) return impl_->write_into(*this, *at, path, values, true);
    // The stack's value below the layer, which the row is written against.
    Ref<Array> below = Array::make(0);
    for (size_t i = 0; i < *at; ++i)
        if (impl_->layers[i].options.enabled)
            below = detail::merge(*below, *impl_->layers[i].root, MergePolicy::Overwrite,
                                  impl_->merge_scratch);
    Vec<Node> was{Node(detail::was_tag())};
    Vec<Node> v;
    if (value_nodes(*below, path, v)) was.insert(was.end(), v.begin(), v.end());
    const Node node(Array::make_from(was));
    return impl_->write_into(*this, *at, path, values, true, &node);
}

Expected<WriteReport> Config::promote(std::string_view from, std::string_view to) {
    if constexpr (kShipping) return Unexpected(shipping("promote()"));
    if (auto ok = impl_->may_change(*this); !ok) return Unexpected(std::move(ok).error());
    if (to == "scratch") return Unexpected(refused("scratch is never written to disk"));
    const auto ti = impl_->find_layer(to);
    const auto fi = impl_->find_layer(from);
    if (!ti || (!fi && from != "scratch"))
        return Unexpected(
            make_error(ErrorCode::NotFound, "no layer named '" + Str(ti ? from : to) + "'"));
    if (fi == ti) return Unexpected(refused("cannot promote a layer into itself"));
    const Ref<Array> tree = fi ? impl_->layers[*fi].root : impl_->scratch_root();
    const Ref<Array> target_before = impl_->layers[*ti].root;  // for the log's records
    // Each path the source holds, valued as the Config reads it with the source on top:
    // the enabled layers below, then the source, merged, so a repeated tag folds as a
    // merge folds it rather than as find() would see it alone.
    Vec<Leaf> leaves;
    Ref<Array> upto = Array::make(0);
    if (tree) {
        HashMap<Str, char> seen;
        collect_paths(*tree, 0, "", seen, leaves, impl_->alias_root());
        const size_t below = fi ? *fi : impl_->layers.size();
        for (size_t i = 0; i < below; ++i)
            if (impl_->layers[i].options.enabled)
                upto = detail::merge(*upto, *impl_->layers[i].root, MergePolicy::Overwrite,
                                     impl_->merge_scratch);
        upto = detail::merge(*upto, *tree, MergePolicy::Overwrite, impl_->merge_scratch);
        for (Leaf& leaf : leaves)
            if (!value_nodes(*upto, Path(leaf.path), leaf.values))
                leaf.values = leaf_values(*leaf.holder);
    }

    // Plan every entry against the target as each earlier one leaves it, in memory.
    HashMap<Str, Str> virt;
    const LoadSession::Reader disk = impl_->base.reader();
    const LoadSession::Reader vread = [&](std::string_view p, TextSink out) {
        if (auto it = virt.find(Str(p)); it != virt.end()) {
            out(it->second);
            return true;
        }
        return disk(p, out);
    };
    LayerState target = impl_->layers[*ti];
    // Each re-read starts from the same session below the target: one copy, not one a leaf.
    // read() copies what it is given, so this stays as it is.
    LoadSession before = *impl_->session_below(impl_->layers, *ti);
    before.set_reader(vread);
    Vec<Error> refusals;
    for (const Leaf& leaf : leaves) {
        // A row a layer above the target composes would apply a second time over what is
        // written. A composing row writes the value it makes: none, when it drops from a key
        // the stack lacks; a keyed operand cannot be written as a value.
        const Path path(leaf.path);
        if (auto e = impl_->composed(path, *ti + 1, fi ? *fi : SIZE_MAX)) {
            refusals.push_back(std::move(*e));
            continue;
        }
        const bool composes = detail::composes(*leaf.holder);
        if (composes && !row_at(*upto, path)) continue;
        if (composes &&
            std::any_of(leaf.holder->begin() + 2, leaf.holder->end(), [](const Node& n) {
                return is_tagged(n) && !detail::tag_of(n).is_meta();
            })) {
            refusals.push_back(refused("promote cannot write " + quoted(path) +
                                           ": its verb moves keyed tuples; write the list by hand",
                                       leaf.holder->file(), leaf.holder->line()));
            continue;
        }
        // A removal deletes the key from the target: its array, with its line if alone there.
        auto p = detail::is_removal(*leaf.holder)
                     ? plan_removal(target, path, vread)
                     : impl_->plan(target, path, leaf.values, true, vread);
        if (!p) {
            refusals.push_back(std::move(p).error());
            continue;
        }
        if (!p->changed) continue;
        virt[Str(p->file.str())] = std::move(p->text);
        if (auto r = impl_->read(target, before); !r) refusals.push_back(std::move(r).error());
    }
    WriteReport report;
    if (!refusals.empty()) {  // nothing written
        report.refused = List<Error>(refusals);
        return report;
    }
    ReloadRecord rec = impl_->begin(trigger_name("write_back"));
    uint64_t t = rec.start_ns;
    for (const auto& [file, text] : virt)
        if (!impl_->writer(file, text))
            return Unexpected(
                make_error(ErrorCode::Io, "cannot write '" + file + "'", Symbol(file)));
    if (!virt.empty()) {
        impl_->lap(rec.write_ns, t);
        if (auto r = impl_->reread_stages(rec, *ti, t); !r) {
            impl_->finish(*this, rec, t);
            return Unexpected(std::move(r).error());
        }
    }
    const bool log = impl_->logging();  // the promotion as one transaction
    if (log) {  // `target` is the layer as planned: the same tree whether or not written
        impl_->begin_change(trigger_name("promote"));
        impl_->walk_changes(*this, *target_before, *target.root, Symbol(to));
    }
    impl_->commit(*this, rec, t);
    impl_->finish(*this, rec, t);
    Vec<WrittenLine> written;
    for (const Leaf& leaf : leaves) {
        // Located in the target as planned: the same text the file now holds, or, with a
        // writer that only captures it (a dry run), the text it would hold.
        const Source s = impl_->source_in(target, Path(leaf.path));
        if (!virt.empty()) written.push_back({s.file, s.line});
        // Scratch's edits are in the file now.
        if (from == "scratch") {
            const Symbol key(detail::text_of(Path(leaf.path)));
            for (const ScratchEntry& e : impl_->scratch)
                if (e.key == key) impl_->settle(e, EditFate::Written, {s.file, s.line});
        }
    }
    report.written = List<WrittenLine>(written);
    if (log) {
        impl_->changes.pending_written.assign(written.begin(), written.end());
        impl_->close_change();
    }
    return report;
}

}  // namespace baker
