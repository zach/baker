// What each layer contributes, for tools (<baker/tools/layers.hpp>): explain, layer_effect
// and diff, over a Config's internals.

#include <baker/tools/layers.hpp>

#include "access.hpp"

#include <algorithm>

namespace baker::tools {
namespace {
RowValues list_of(const Vec<Node>& nodes) {
    return RowValues({nodes.data(), nodes.size()});
}
RowValues one(const Node& n) {
    return RowValues({&n, 1});
}
// Layer `l`'s declarations.
Vec<Declaration> declared(const LayerState& l) {
    Vec<Declaration> out;
    if (l.after && l.root)
        declarations_of(*l.after, l.root->file(), {l.deps.data(), l.deps.size()}, out);
    return out;
}
// The children after a leaf's holder in its array with its tag, which fold into it.
Vec<const Array*> folded_rows(const Leaf& l) {
    Vec<const Array*> out;
    if (l.folded == 0 || !l.parent) return out;
    bool after = false;
    for (const Node& n : *l.parent) {
        const Array* c = n.try_array();
        if (c == l.holder)
            after = true;
        else if (after && c && c->tag() == l.holder->tag() &&
                 !(c->has_marker() && detail::composes(*c)))
            out.push_back(c);
    }
    return out;
}
// Whether two leaves fold the same rows into their records.
bool same_folded(const Leaf& x, const Leaf& y) {
    if (x.folded != y.folded) return false;
    const Vec<const Array*> fx = folded_rows(x), fy = folded_rows(y);
    for (size_t i = 0; i < fx.size(); ++i)
        if (!structurally_equal(Node(Ref<Array>(const_cast<Array*>(fx[i]))),
                                Node(Ref<Array>(const_cast<Array*>(fy[i])))))
            return false;
    return true;
}
const Declaration* find_declared(const Vec<Declaration>& ds, const Declaration& like) {
    for (const Declaration& d : ds)
        if (d.directive == like.directive && d.name == like.name) return &d;
    return nullptr;
}
}  // namespace

Expected<List<LayerEffectEntry>> layer_effect(const Config& cfg, std::string_view layer) {
    const auto& impl = detail::ToolsAccess::impl(cfg);
    const bool is_scratch = layer == "scratch";
    const auto li = impl.find_layer(layer);
    if (!li && !is_scratch)
        return Unexpected(make_error(ErrorCode::NotFound, "no layer named '" + Str(layer) + "'"));
    const Ref<Array> tree = li ? impl.layers[*li].root : impl.scratch_root();
    Vec<LayerEffectEntry> out;
    if (!tree) return List<LayerEffectEntry>();
    // The stack without this layer.
    Ref<Array> without = Array::make(0);
    for (size_t i = 0; i < impl.layers.size(); ++i)
        if (impl.layers[i].options.enabled && (!li || i != *li))
            without = detail::merge(*without, *impl.layers[i].root, MergePolicy::Overwrite,
                                    impl.merge_scratch);
    const Ref<Array> scratch = impl.scratch_enabled ? impl.scratch_root() : nullptr;
    if (!is_scratch && scratch)
        without = detail::merge(*without, *scratch, MergePolicy::Overwrite, impl.merge_scratch);

    const Source in_scratch{scratch_name()};
    Vec<Node> below;
    for (const Leaf& leaf : leaves_of(*tree)) {
        LayerEffectEntry e{};
        const Path path(leaf.path);
        e.path = Symbol(leaf.path);
        e.values = list_of(leaf.values);
        e.folded = leaf.folded;
        e.op = op_of(*leaf.holder);
        e.source = li ? impl.located(impl.layers[*li], *leaf.holder) : in_scratch;
        const bool verb = detail::composes(*leaf.holder);
        const bool has = below_nodes(*without, path, *leaf.holder, below);
        if (has) e.below = list_of(below);
        e.stale = stale(*leaf.holder, has ? &below : nullptr);
        // The topmost other layer that supplies the path: it shadows this one if above it.
        // Under a verb, the one that wrote the list it changes.
        const auto s = impl.supplier(path, impl.layers.size(), li ? *li : SIZE_MAX, verb);
        const bool scratch_has = !is_scratch && impl.scratch_entry(path);
        if (scratch_has)
            e.below_source = in_scratch;
        else if (s)
            e.below_source = impl.source_in(impl.layers[*s], path);
        e.shadowed = scratch_has || (li && s && *s > *li);
        out.push_back(std::move(e));
    }
    // Its declarations, each over the same one in the nearest enabled layer below.
    if (li)
        for (const Declaration& d : declared(impl.layers[*li])) {
            LayerEffectEntry e{};
            e.path = d.name;
            e.values = one(d.form);
            e.declares = d.directive;
            e.source = Source{Symbol(layer), d.file, d.line};
            for (size_t i = *li; i-- > 0 && !e.below;) {
                if (!impl.layers[i].options.enabled) continue;
                const Vec<Declaration> under = declared(impl.layers[i]);
                if (const Declaration* b = find_declared(under, d)) {
                    e.below = one(b->form);
                    e.below_source = Source{Symbol(impl.layers[i].name), b->file, b->line};
                }
            }
            out.push_back(std::move(e));
        }
    return List<LayerEffectEntry>(out);
}

Expected<List<DiffEntry>> diff(const Config& cfg, std::string_view a, std::string_view b) {
    const auto& impl = detail::ToolsAccess::impl(cfg);
    auto tree_of = [&](std::string_view name) -> Expected<Ref<Array>> {
        if (name == "scratch") {
            Ref<Array> s = impl.scratch_root();
            return s ? s : Array::make(0);
        }
        if (auto i = impl.find_layer(name)) return impl.layers[*i].root;
        return Unexpected(make_error(ErrorCode::NotFound, "no layer named '" + Str(name) + "'"));
    };
    auto ta = tree_of(a), tb = tree_of(b);
    if (!ta) return Unexpected(std::move(ta).error());
    if (!tb) return Unexpected(std::move(tb).error());
    const Vec<Leaf> la = leaves_of(**ta), lb = leaves_of(**tb);
    auto source = [&](std::string_view layer, const Leaf& l) {
        return layer == "scratch" ? Source{scratch_name()} : impl.located(layer, *l.holder);
    };
    // b's leaves by path, each matched at most once.
    HashMap<Str, size_t> in_b;
    for (size_t i = 0; i < lb.size(); ++i) in_b.emplace(lb[i].path, i);
    Vec<bool> matched(lb.size(), false);
    Vec<DiffEntry> out;
    for (const Leaf& x : la) {
        const auto it = in_b.find(x.path);
        const Leaf* y = it != in_b.end() ? &lb[it->second] : nullptr;
        if (y) matched[it->second] = true;
        if (y && same_nodes(x.values, y->values) && op_of(*x.holder) == op_of(*y->holder) &&
            same_folded(x, *y))
            continue;
        DiffEntry d{Symbol(x.path), list_of(x.values), std::nullopt, source(a, x),
                    std::nullopt,   op_of(*x.holder),  Symbol(),     Symbol()};
        d.folded_a = x.folded;
        if (y) {
            d.b = list_of(y->values);
            d.source_b = source(b, *y);
            d.op_b = op_of(*y->holder);
            d.folded_b = y->folded;
        }
        out.push_back(std::move(d));
    }
    for (size_t i = 0; i < lb.size(); ++i)
        if (!matched[i])
            out.push_back({Symbol(lb[i].path), std::nullopt, list_of(lb[i].values), std::nullopt,
                           source(b, lb[i]), Symbol(), op_of(*lb[i].holder), Symbol(), 0,
                           lb[i].folded});
    // The two layers' declarations, by directive and name.
    auto decls_of = [&](std::string_view name) {
        const auto i = name == "scratch" ? std::nullopt : impl.find_layer(name);
        return i ? declared(impl.layers[*i]) : Vec<Declaration>();
    };
    const Vec<Declaration> da = decls_of(a), db = decls_of(b);
    for (const Declaration& x : da) {
        const Declaration* y = find_declared(db, x);
        if (y && structurally_equal(x.form, y->form)) continue;
        DiffEntry d{x.name,       one(x.form), std::nullopt, Source{Symbol(a), x.file, x.line},
                    std::nullopt, Symbol(),    Symbol(),     x.directive};
        if (y) {
            d.b = one(y->form);
            d.source_b = Source{Symbol(b), y->file, y->line};
        }
        out.push_back(std::move(d));
    }
    for (const Declaration& y : db)
        if (!find_declared(da, y)) {
            DiffEntry d{y.name,
                        std::nullopt,
                        one(y.form),
                        std::nullopt,
                        Source{Symbol(b), y.file, y.line},
                        Symbol(),
                        Symbol(),
                        y.directive};
            out.push_back(std::move(d));
        }
    return List<DiffEntry>(out);
}

Expected<List<ExplainEntry>> explain(const Config& cfg, const Path& path) {
    const auto& impl = detail::ToolsAccess::impl(cfg);
    const Array& root = *detail::ToolsAccess::root(cfg);
    if (!path.valid()) return Unexpected(detail::get_error(root, path, nullptr));
    if (path.empty())
        return Unexpected(refused("the whole config has no single value: explain a leaf"));
    // #shape.NAME[.FIELD]: each layer that declares it, bottom to top.
    if (path[0].name == Symbol("#shape")) {
        Vec<ExplainEntry> out;
        if (path.size() < 2 || path.size() > 3 || path[1].is_index)
            return Unexpected(refused("explain a shape as #shape.NAME or #shape.NAME.FIELD"));
        for (const LayerState& l : impl.layers)
            for (const Declaration& d : declared(l)) {
                if (d.directive != Symbol("#shape") || d.name != path[1].name) continue;
                const Array* form = d.form.try_array();
                const Array* field =
                    path.size() == 3 && form ? form->try_find(path[2].name) : nullptr;
                if (path.size() == 3 && !field) continue;
                ExplainEntry e{Symbol(l.name),
                               one(field ? Node(Ref<Array>(const_cast<Array*>(field))) : d.form),
                               Source{Symbol(l.name), field ? field->file() : d.file,
                                      field ? field->line() : d.line}};
                e.standing = l.options.enabled ? Standing::Wins : Standing::Disabled;
                e.when = cfg.layer_condition(l.name.view());
                out.push_back(std::move(e));
            }
        if (out.empty())
            return Unexpected(make_error(
                ErrorCode::NotFound, "no layer declares " + detail::text_of(path, SIZE_MAX, '.')));
        return List<ExplainEntry>(out);
    }

    // What one tree holds at `path`: a value, a removal of it or of an ancestor, an array
    // of arrays, or nothing.
    struct Held {
        Vec<Node> nodes;
        const Array* marker = nullptr;    // a removal of it or of an ancestor
        const Array* composes = nullptr;  // (key #append v...) or (key #drop v...)
        bool value = false, subtree = false;
    };
    // A renamed record is spelled by an alias in a layer that retunes it by the old key.
    const Ref<Array> merged_root = cfg.root();
    const Array* merged = merged_root.get();
    auto held = [&](const Array& tree) {
        Held h;
        detail::Resolved r;
        // A row that composes the value, or the list an element is in: its operands, as
        // layer_effect gives them.
        if (const Array* row = tree.has_marker() ? row_at(tree, path) : nullptr;
            row && detail::composes(*row)) {
            h.composes = row;
            h.nodes = leaf_values(*row);
            h.value = true;
            return h;
        }
        for (size_t k = 1; k <= path.size(); ++k) {
            if (path[k - 1].is_index) continue;
            const Path prefix = k == path.size() ? path : Path(detail::text_of(path, k));
            if (detail::resolve_in_layer(tree, prefix, merged, r) != detail::ResolveStatus::Ok)
                return h;
            if (r.array && detail::is_removal(*r.array)) {
                h.marker = r.array;
                return h;
            }
        }
        Path spelled;  // as this layer writes it: by an alias, for a renamed record
        if (detail::resolve_in_layer(tree, path, merged, r, &spelled) != detail::ResolveStatus::Ok)
            return h;
        if (last_is_index(path) || (r.array && has_values(*r.array)))
            h.value = value_nodes(tree, spelled, h.nodes);
        else
            h.subtree = true;
        return h;
    };

    // Top down: the first enabled contributor decides (it wins, or it removes); every
    // enabled one below it is shadowed. Reversed at the end.
    Vec<ExplainEntry> out;
    bool decided = false, subtree = false;
    auto add = [&](Symbol layer, const Held& h, const Source& source, bool enabled) {
        subtree = subtree || h.subtree;
        if (!h.value && !h.marker) return;
        const Array* row = h.marker ? h.marker : h.composes;
        ExplainEntry e{
            layer,
            h.marker ? one(Node(detail::marker_symbol(detail::Marker::Remove))) : list_of(h.nodes),
            source, Standing::Wins, row ? op_of(*row) : Symbol()};
        e.when = cfg.layer_condition(layer.str());
        if (!enabled) {
            e.standing = Standing::Disabled;
        } else if (decided) {
            e.standing = Standing::Shadowed;
        } else if (h.composes) {
            e.standing = Standing::Composes;  // the value below it still decides
        } else {
            e.standing = h.marker ? Standing::Removes : Standing::Wins;
            decided = true;
        }
        out.push_back(std::move(e));
    };

    if (const Ref<Array> scratch = impl.scratch_root())
        add(scratch_name(), held(*scratch), Source{scratch_name()}, impl.scratch_enabled);
    for (size_t i = impl.layers.size(); i-- > 0;) {
        const LayerState& l = impl.layers[i];
        const Held h = held(*l.root);
        const Source source = h.marker     ? impl.located(l, *h.marker)
                              : h.composes ? impl.located(l, *h.composes)
                              : h.value    ? impl.source_in(l, path)
                                           : Source{};
        add(Symbol(l.name), h, source, l.options.enabled);
        // The branch of an #ifdef its value was read in: the innermost around its line.
        if (!out.empty() && out.back().layer == Symbol(l.name) && l.after && !source.file.empty())
            for (const ConditionalSection& s : l.after->conditional_sections())
                if (s.file == source.file && s.begin < source.line && source.line < s.end &&
                    (!out.back().section || s.begin > out.back().section->begin))
                    out.back().section = s;
    }
    if (TweakRegistry* tweaks = impl.options.tweaks) {
        bool found = false;
        tweaks->each([&](TweakBase& t) {
            if (found || !(detail::ToolsAccess::parsed(t) == path)) return;
            found = true;
            ValueList v;
            t.default_values(v);
            Held h;
            const std::span<const Node> d = v.nodes();
            h.nodes.assign(d.data(), d.data() + d.size());
            h.value = !h.nodes.empty();
            const Symbol code("code");
            add(code, h, Source{code}, true);
        });
    }

    if (out.empty()) {
        if (subtree)
            return Unexpected(refused(quoted(path) + " holds arrays, not values: explain a leaf"));
        return Unexpected(detail::get_error(root, path, nullptr));
    }
    std::reverse(out.begin(), out.end());
    return List<ExplainEntry>(out);
}

}  // namespace baker::tools
