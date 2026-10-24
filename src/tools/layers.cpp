// What each layer contributes, for tools (<baker/tools/layers.hpp>): explain, layer_effect
// and diff, over a Config's internals.

#include <baker/tools/layers.hpp>

#include "access.hpp"

#include <algorithm>

namespace baker::tools {

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
        e.value = pack(leaf.values);
        e.source = li ? impl.located(impl.layers[*li], *leaf.holder) : in_scratch;
        if (value_nodes(*without, path, below)) e.below = pack(below);
        // The topmost other layer that supplies the path: it shadows this one if above it.
        const auto s = impl.supplier(path, impl.layers.size(), li ? *li : SIZE_MAX);
        const bool scratch_has = !is_scratch && impl.scratch_entry(path);
        if (scratch_has)
            e.below_source = in_scratch;
        else if (s)
            e.below_source = impl.source_in(impl.layers[*s], path);
        e.shadowed = scratch_has || (li && s && *s > *li);
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
        if (y && same_nodes(x.values, y->values)) continue;
        DiffEntry d{Symbol(x.path), pack(x.values), std::nullopt, source(a, x), std::nullopt};
        if (y) {
            d.b = pack(y->values);
            d.source_b = source(b, *y);
        }
        out.push_back(std::move(d));
    }
    for (size_t i = 0; i < lb.size(); ++i)
        if (!matched[i])
            out.push_back({Symbol(lb[i].path), std::nullopt, pack(lb[i].values), std::nullopt,
                           source(b, lb[i])});
    return List<DiffEntry>(out);
}

Expected<List<ExplainEntry>> explain(const Config& cfg, const Path& path) {
    const auto& impl = detail::ToolsAccess::impl(cfg);
    const Array& root = *detail::ToolsAccess::root(cfg);
    if (!path.valid()) return Unexpected(detail::get_error(root, path, nullptr));
    if (path.empty())
        return Unexpected(refused("the whole config has no single value: explain a leaf"));

    // What one tree holds at `path`: a value, a removal of it or of an ancestor, an array
    // of arrays, or nothing.
    struct Held {
        Vec<Node> nodes;
        const Array* marker = nullptr;
        bool value = false, subtree = false;
    };
    auto held = [&](const Array& tree) {
        Held h;
        detail::Resolved r;
        for (size_t k = 1; k <= path.size(); ++k) {
            if (path[k - 1].is_index) continue;
            const Path prefix = k == path.size() ? path : Path(detail::text_of(path, k));
            if (detail::resolve(tree, prefix, r) != detail::ResolveStatus::Ok) return h;
            if (r.array && detail::is_removal(*r.array)) {
                h.marker = r.array;
                return h;
            }
        }
        if (detail::resolve(tree, path, r) != detail::ResolveStatus::Ok) return h;
        if (last_is_index(path) || (r.array && has_values(*r.array)))
            h.value = value_nodes(tree, path, h.nodes);
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
        ExplainEntry e{layer, h.marker ? Node(detail::remove_marker()) : pack(h.nodes), source};
        if (!enabled) {
            e.standing = Standing::Disabled;
        } else if (decided) {
            e.standing = Standing::Shadowed;
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
        const Source source = h.marker  ? impl.located(l, *h.marker)
                              : h.value ? impl.source_in(l, path)
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
