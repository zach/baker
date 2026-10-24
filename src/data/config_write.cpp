// Config's write-back: planning an edit of a layer's file, writing it, and promoting
// scratch. The layer tools (explain, layer_effect, diff) are baker::tools'.

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
void collect_paths(const Array& a, size_t first, const Str& prefix, HashMap<Str, char>& seen,
                   Vec<Leaf>& out) {
    for (size_t i = first; i < a.size(); ++i) {
        if (!is_tagged(a[i])) continue;
        const Array& child = *a[i].try_array();
        Str path = prefix;
        if (!path.empty()) path += '.';
        detail::append_path_name(path, child[0].try_sym()->str());
        if (has_values(child) && seen.emplace(path, 0).second) out.push_back({path, &child, {}});
        collect_paths(child, 1, path, seen, out);
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
Str render_entry(const Path& path, size_t from, const Vec<Node>& values, const Str& indent) {
    Vec<Node> inner{Node(path[path.size() - 1].name)};
    inner.insert(inner.end(), values.begin(), values.end());
    Ref<Array> nest = Array::make_from(inner);
    for (size_t i = path.size() - 1; i-- > from;) nest = array(path[i].name, Node(nest));
    Str text = detail::dta_of(*array(Node(nest)));
    while (!text.empty() && text.back() == '\n') text.pop_back();
    for (size_t at = text.find('\n'); at != Str::npos; at = text.find('\n', at + 1))
        text.insert(at + 1, indent);
    return text;
}

struct Edit {
    uint32_t begin, end;
    Str text;
};

}  // namespace

namespace config_internal {
Vec<Node> values_of(const Array& a) {
    Vec<Node> out;
    for (size_t i = 1; i < a.size(); ++i)
        if (!is_tagged(a[i])) out.push_back(a[i]);
    return out;
}

// Each tag's first child only, as lookup sees it: found by the tag run's scan in a small
// array, and by a set past kIndexed children, so a wide layer is linear.
static void collect_leaves(const Array& a, size_t first, const Str& prefix, Vec<Leaf>& out) {
    constexpr size_t kIndexed = 32;
    HashMap<Symbol, char> seen;
    const bool indexed = a.size() > kIndexed;
    for (size_t i = first; i < a.size(); ++i) {
        if (!is_tagged(a[i])) continue;
        const Array& child = *a[i].try_array();
        const Symbol tag = *child[0].try_sym();
        if (indexed ? !seen.emplace(tag, 0).second : a.try_find(tag) != &child) continue;
        Str path = prefix;
        if (!path.empty()) path += '.';
        detail::append_path_name(path, tag.str());
        if (has_values(child)) out.push_back({path, &child, values_of(child)});
        collect_leaves(child, 1, path, out);
    }
}

Vec<Leaf> leaves_of(const Array& root) {
    Vec<Leaf> out;
    collect_leaves(root, 0, "", out);
    return out;
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
Expected<Plan> Config::Impl::plan(const LayerState& layer, const Path& path,
                                  const Vec<Node>& values, bool insert,
                                  const LoadSession::Reader& read_file) const {
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
        return Plan{main, render_entry(path, 0, values, "") + "\n", true};
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
    size_t offset = 0;  // where index 0 is: 1 after a tag, 0 in the root or an element
    Vec<size_t> targets;
    size_t missing_from = path.size();
    for (size_t i = 0; i < path.size(); ++i) {
        const Path::Segment& seg = path[i];
        size_t k = cur->size();
        if (seg.is_index) {
            k = offset + seg.index < cur->size() ? offset + seg.index : cur->size();
        } else {
            for (size_t j = 0; j < cur->size() && k == cur->size(); ++j)
                if (is_tagged((*cur)[j]) && (*(*cur)[j].try_array())[0].is(seg.name)) k = j;
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
        if (last)
            for (size_t j = 1; j < cur->size(); ++j)
                if (!is_tagged((*cur)[j])) targets.push_back(j);
    }

    Vec<Edit> edits;
    Symbol file;
    if (missing_from < path.size()) {
        if (auto e = no_index(missing_from, cur->file(), cur->line())) return Unexpected(*e);
        if (!insert) return Unexpected(detail::get_error(*layer.root, path, nullptr));
        file = main;
        const auto text = map.text(main);
        if (!text) return Unexpected(refused("no source text for '" + layer.file + "'", main));
        if (cur == layer.root.get()) {
            // A new top-level form at the end of the file.
            const bool newline = !text->empty() && text->back() != '\n';
            const auto end = static_cast<uint32_t>(text->size());
            edits.push_back(
                {end, end,
                 (newline ? "\n" : "") + render_entry(path, missing_from, values, "") + "\n"});
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
                edits.push_back(
                    {last->end, last->end, " " + render_entry(path, missing_from, values, "")});
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
                    {at, at, "\n" + indent + render_entry(path, missing_from, values, indent)});
            }
        }
    } else {
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
                                               const Vec<Node>& values, bool insert) {
    auto p = plan(layers[li], path, values, insert, base.reader());
    if (!p) return Unexpected(std::move(p).error());
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
    }
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
    if (!at)
        return Unexpected(refused(
            quoted(path) + " is not in any layer's file; use write_back_to to add it to one"));
    return impl_->write_into(*this, *at, path, values, false);
}

Expected<WriteReport> Config::write_back_to(const Path& path, std::string_view layer) {
    if constexpr (kShipping) return Unexpected(shipping("write-back"));
    if (!path.valid()) return Unexpected(detail::get_error(*root_, path, nullptr));
    if (layer == "scratch") return Unexpected(refused("scratch is never written to disk"));
    Vec<Node> values;
    if (!impl_->current(*this, path, values))
        return Unexpected(detail::get_error(*root_, path, nullptr));
    const auto at = impl_->find_layer(layer);
    if (!at)
        return Unexpected(make_error(ErrorCode::NotFound, "no layer named '" + Str(layer) + "'"));
    return impl_->write_into(*this, *at, path, values, true);
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
    // Each path the source holds, valued as the Config reads it with the source on top:
    // the enabled layers below, then the source, merged, so a repeated tag folds as a
    // merge folds it rather than as find() would see it alone.
    Vec<Leaf> leaves;
    if (tree) {
        HashMap<Str, char> seen;
        collect_paths(*tree, 0, "", seen, leaves);
        Ref<Array> upto = Array::make(0);
        const size_t below = fi ? *fi : impl_->layers.size();
        for (size_t i = 0; i < below; ++i)
            if (impl_->layers[i].options.enabled)
                upto = detail::merge(*upto, *impl_->layers[i].root, MergePolicy::Overwrite,
                                     impl_->merge_scratch);
        upto = detail::merge(*upto, *tree, MergePolicy::Overwrite, impl_->merge_scratch);
        for (Leaf& leaf : leaves)
            if (!value_nodes(*upto, Path(leaf.path), leaf.values))
                leaf.values = values_of(*leaf.holder);
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
        if (leaf.values.size() == 1 && leaf.values[0].is(detail::remove_marker())) {
            // Writing (key #remove) beside the key's children would not parse.
            refusals.push_back(refused("promote cannot write the removal of " +
                                       quoted(Path(leaf.path)) +
                                       "; remove the key from the target by hand"));
            continue;
        }
        auto p = impl_->plan(target, Path(leaf.path), leaf.values, true, vread);
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
    impl_->commit(*this, rec, t);
    impl_->finish(*this, rec, t);
    Vec<WrittenLine> written;
    for (const Leaf& leaf : leaves) {
        const Source s = impl_->source_in(impl_->layers[*ti], Path(leaf.path));
        if (!virt.empty()) written.push_back({s.file, s.line});
        // Scratch's edits are in the file now.
        if (from == "scratch") {
            const Symbol key(detail::text_of(Path(leaf.path)));
            for (const ScratchEntry& e : impl_->scratch)
                if (e.key == key) impl_->settle(e, EditFate::Written, {s.file, s.line});
        }
    }
    report.written = List<WrittenLine>(written);
    return report;
}

}  // namespace baker
