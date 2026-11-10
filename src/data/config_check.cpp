// Config::check_layer: a candidate layer's effect, the policy's refusals, and the
// validators' warnings, before it takes effect.

#include "config_impl.hpp"

#include <algorithm>

namespace baker {
namespace {

// Tag lookups for one check, through an index of each wide array met, built when it is
// first searched. A check resolves a few paths per changed value; scanning a wide table
// for each was most of its cost. It finds what Array::try_find finds: the
// first child array with the tag.
class Finder {
public:
    explicit Finder(const Allocator& a) : wide_(a), keys_(a), found_(a), counts_(a), sorted_(a) {}

    [[nodiscard]] const Array* find(const Array& parent, Symbol tag) {
        if (parent.size() < kWide || tag.empty()) return parent.try_find(tag);
        const Wide& w = index(parent);
        for (uint32_t i = static_cast<uint32_t>(tag.hash()) & w.mask;; i = (i + 1) & w.mask) {
            const void* k = keys_[w.first + i];
            if (!k) return nullptr;
            if (k == tag.id()) return found_[w.first + i];
        }
    }
    // Whether two children of `parent` share a tag: the index's note past a short array, a
    // scan of the tags of a short one. Most arrays repeat none, and need no count.
    [[nodiscard]] bool repeats(const Array& parent) {
        if (parent.size() >= kWide) {
            for (const Wide& w : wide_)
                if (w.array == &parent) return w.repeats;
            // Not indexed yet, as a layer's own array is not: its tags sorted, which costs a
            // fraction of an index it may never need.
            sorted_.clear();
            for (const Node& c : parent.nodes())
                if (const Array* a = c.try_array(); a && !a->empty())
                    if (const auto t = (*a)[0].try_sym()) sorted_.push_back(t->id());
            std::sort(sorted_.begin(), sorted_.end());
            return std::adjacent_find(sorted_.begin(), sorted_.end()) != sorted_.end();
        }
        Symbol seen[kWide];
        size_t n = 0;
        for (const Node& c : parent.nodes()) {
            const Array* a = c.try_array();
            if (!a || a->empty() || !(*a)[0].try_sym()) continue;
            const Symbol t = *(*a)[0].try_sym();
            for (size_t k = 0; k < n; ++k)
                if (seen[k] == t) return true;
            seen[n++] = t;
        }
        return false;
    }
    // How many children of `parent` are tagged `tag`, a row that composes or removes left
    // out: through the index past a short array, which counts as it is built.
    [[nodiscard]] uint32_t count(const Array& parent, Symbol tag) {
        if (parent.size() < kWide || tag.empty()) {
            uint32_t n = 0;
            for (const Node& c : parent.nodes()) n += counts_as_row(c, tag) ? 1 : 0;
            return n;
        }
        const Wide& w = index(parent);
        for (uint32_t i = static_cast<uint32_t>(tag.hash()) & w.mask;; i = (i + 1) & w.mask) {
            const void* k = keys_[w.first + i];
            if (!k) return 0;
            if (k == tag.id()) return counts_[w.first + i];
        }
    }
    // detail::resolve, through find().
    const Array* merged = nullptr;  // the stack, for a renamed record's aliases
    [[nodiscard]] bool resolve(const Array& root, const Path& path, detail::Resolved& out) {
        if (resolve_plain(root, path, out)) return true;
        return merged &&
               detail::resolve_in_layer(root, path, merged, out) == detail::ResolveStatus::Ok;
    }
    [[nodiscard]] bool resolve_plain(const Array& root, const Path& path, detail::Resolved& out) {
        if (!path.valid()) return false;
        Values v(root.nodes(), &root);
        const Array* arr = &root;
        for (size_t i = 0; i < path.size(); ++i) {
            const Path::Segment& seg = path[i];
            if (seg.is_index) {
                if (seg.index >= v.size()) return false;
                if (const Array* a = v[seg.index].try_array()) {
                    v = Values(a->nodes(), a);
                    arr = a;
                } else {
                    v = Values(v.nodes().subspan(seg.index, 1), v.source());
                    arr = nullptr;
                }
                continue;
            }
            const Array* a = arr ? find(*arr, seg.name) : nullptr;
            if (!a) return false;
            v = a->values();
            arr = a;
        }
        out = {v, arr};
        return true;
    }
    // value_nodes, through find().
    bool values(const Array& root, const Path& path, Vec<Node>& out) {
        out.clear();
        detail::Resolved r;
        if (last_is_index(path)) {
            if (!resolve(root, parent_of(path), r)) return false;
            const uint32_t i = path[path.size() - 1].index;
            if (i >= r.values.size()) return false;
            out.push_back(r.values[i]);
            return true;
        }
        if (!resolve(root, path, r) || !r.array) return false;
        for (size_t i = path.empty() ? 0 : 1; i < r.array->size(); ++i)
            if (!is_tagged((*r.array)[i])) out.push_back((*r.array)[i]);
        return true;
    }
    // list_nodes, through find(): the list a verb changes, records too.
    bool list(const Array& root, const Path& path, Vec<Node>& out) {
        out.clear();
        detail::Resolved r;
        if (path.empty() || last_is_index(path) || !resolve(root, path, r) || !r.array)
            return false;
        for (const Node& n : r.array->values()) out.push_back(n);
        return true;
    }

private:
    static constexpr size_t kWide = 32;  // below this, the tag run's scan is as quick
    struct Wide {
        const Array* array;
        uint32_t first, mask;  // its slots in keys_ and found_
        bool repeats;          // two children share a tag
    };

    const Wide& index(const Array& parent) {
        for (const Wide& w : wide_)
            if (w.array == &parent) return w;
        uint32_t cap = 64;
        while (cap < parent.size() * 2) cap *= 2;
        Wide w{&parent, static_cast<uint32_t>(keys_.size()), cap - 1, false};
        keys_.resize(keys_.size() + cap, nullptr);
        found_.resize(found_.size() + cap, nullptr);
        counts_.resize(counts_.size() + cap, 0);
        for (const Node& n : parent.nodes()) {
            const Array* child = n.try_array();
            const Symbol tag = child ? child->tag() : Symbol();
            if (tag.empty()) continue;
            uint32_t i = static_cast<uint32_t>(tag.hash()) & w.mask;
            while (keys_[w.first + i] && keys_[w.first + i] != tag.id()) i = (i + 1) & w.mask;
            counts_[w.first + i] += counts_as_row(n, tag) ? 1 : 0;
            w.repeats = w.repeats || keys_[w.first + i];
            if (keys_[w.first + i]) continue;  // the first child with the tag wins
            keys_[w.first + i] = tag.id();
            found_[w.first + i] = child;
        }
        wide_.push_back(w);
        return wide_.back();
    }

    // A child tagged `tag` that merges by it: not metadata, not a row that composes or
    // removes.
    static bool counts_as_row(const Node& n, Symbol tag) {
        const Array* c = n.try_array();
        if (!c || !(*c)[0].is(tag) || tag.is_meta()) return false;
        return !(c->has_marker() && (detail::composes(*c) || detail::is_removal(*c)));
    }

    Vec<Wide> wide_;
    Vec<const void*> keys_;
    Vec<const Array*> found_;
    Vec<uint32_t> counts_;
    Vec<const void*> sorted_;  // repeats(): a scratch of tag ids
};

bool is_code(const Node& n) noexcept {
    return n.type() == Type::Command || n.type() == Type::Property || n.type() == Type::Var;
}

bool is_scalar(const Node& n) noexcept {
    const Type t = n.type();
    return t == Type::Int || t == Type::Float || t == Type::Symbol || t == Type::String;
}

// data_only: every command, property, or variable in `a`, located at the array holding it.
// `tags` are the tags down to `a`, made a path only for a refusal.
void find_code(const Array& a, Vec<Symbol>& tags, Vec<Error>& out) {
    for (size_t i = 0; i < a.size(); ++i) {
        const Node& n = a[i];
        if (is_code(n)) {
            Str path;
            for (const Symbol t : tags) {
                if (!path.empty()) path += '.';
                detail::append_path_name(path, t.str());
            }
            const Array* at = n.array() ? n.array() : &a;
            out.push_back(refused(
                "'" + path + "' holds " + detail::preview(n) + "; the layer may hold data only",
                at->file(), at->line()));
        } else if (const Array* child = n.try_array()) {
            const Symbol tag = child->tag();
            if (!tag.empty()) tags.push_back(tag);
            find_code(*child, tags, out);
            if (!tag.empty()) tags.pop_back();
        }
    }
}

// The metadata a data file's arrays carry, as Baker's readers spell them.
bool known_metadata(Symbol tag) noexcept {
    static const Symbol words[] = {"@range", "@step",   "@desc", "@note", "@kind",
                                   "@group", "@one_of", "@was",  "@id",   "@aka"};
    return std::find(std::begin(words), std::end(words), tag) != std::end(words);
}

// The first array, in reading order, that holds a value and a child tagged with one of the
// words metadata had before the sigil, (range 0 10): a field now (bare). And the first
// metadata child whose word Baker does not read, (@rnage 0 10) (unknown). Shapes are left to
// their own reader, which refuses the old words. One walk for both, and the names interned
// once: interning "shapes" at every array was most of check's cost.
struct MetadataFinds {
    const Array* bare = nullptr;
    const Array* unknown = nullptr;
};
void find_metadata(const Array& a, MetadataFinds& f) {
    static const Symbol shapes("shapes");
    static const Symbol words[] = {"range", "step", "desc", "note", "kind", "group", "one_of"};
    if (a.tag() == shapes) return;
    if (!f.unknown)
        for (const Array& m : a.metadata())
            if (!known_metadata(m.tag())) {
                f.unknown = &m;
                break;
            }
    const bool valued = !f.bare && !a.tag().empty() && has_values(a);
    for (size_t i = 0; i < a.size() && !(f.bare && f.unknown); ++i) {  // from 0: a root
        const Array* c = a[i].try_array();
        if (!c) continue;
        if (valued && !f.bare &&
            std::find(std::begin(words), std::end(words), c->tag()) != std::end(words))
            f.bare = c;
        find_metadata(*c, f);
    }
}

bool under(const Path& path, const Path& prefix) noexcept {
    if (prefix.size() > path.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (!(path[i] == prefix[i])) return false;
    return true;
}

}  // namespace

namespace {

// What a check stands on: a Config's stack as it is, or a LayerChecker's copy of one.
struct Basis {
    const Array& root;                   // the merged tree
    std::span<const Ref<Array>> layers;  // the enabled layers' own trees, bottom to top
    std::span<const Symbol> names;       // theirs, parallel
    const LoadSession& session;          // what the stack's files declared: origins
    std::span<const Config::Validator* const> validators;
    detail::MergeScratch& scratch;
    const Allocator& alloc;
};

// The layer's folds of a list of same-tagged records below it, defined below.
void folds(Finder& finder, const Array& below, const Array& layer, size_t first, Vec<Symbol>& tags,
           Vec<Error>& out);

// The candidate `tree` (with a file layer's #autorun `commands`, read with `session`) on top
// of the basis: its effect, the policy's refusals, and the validators' warnings,
// `read_warnings` added.
LayerReport run_check(const Basis& b, std::string_view name, const Ref<Array>& candidate,
                      std::span<const Node> commands, const LoadSession& session,
                      const LayerPolicy& policy, std::span<const Error> read_warnings) {
    const Array& tree = *candidate;
    const Allocator& alloc = b.alloc;
    Vec<LayerEffectEntry> effect(alloc);
    Vec<Error> refusals(alloc);
    Vec<Error> warnings(alloc);
    warnings.assign(read_warnings.data(), read_warnings.data() + read_warnings.size());
    const Symbol layer(name);

    if (policy.data_only) {
        Vec<Symbol> tags(alloc);
        find_code(tree, tags, refusals);
        for (const Node& n : commands) {
            const Array* at = n.array();
            refusals.push_back(refused(
                "#autorun " + detail::preview(n) + " runs a command; the layer may hold data only",
                at ? at->file() : Symbol(), at ? at->line() : 0));
        }
    }
    // A #shape changes how the stack is checked, not a value: an overlay that sent a wider
    // range beside a value would pass a check the value alone fails.
    if (policy.data_only || policy.scalars_only)
        for (const ShapeDecl& d : session.shapes())
            if (d.root == tree.file())
                refusals.push_back(refused("#shape " + detail::quoted(d.form->tag()) +
                                               " declares a shape; the layer may change data only",
                                           d.file, d.line));
    // The forms that declared before #pack, #origin and #shape, now read as data: once
    // each per layer, at the first.
    const Array* first = tree.size() > 0 ? tree[0].try_array() : nullptr;
    auto old_form = [&](const Array* a, const char* form, const char* now) {
        if (a)
            warnings.push_back(make_error(ErrorCode::Syntax,
                                          Str(form) + " is data; a file declares " + now, a->file(),
                                          a->line()));
    };
    old_form(first && first->tag() == Symbol("pack") ? first : nullptr, "(pack ...)",
             "its pack with #pack (NAME (level L) ...)");
    for (size_t i = 0; i < tree.size() && i < 2; ++i)
        if (const Array* a = tree[i].try_array();
            a && a->tag() == Symbol("origin") && a->size() > 1 && (*a)[1].string()) {
            old_form(a, "(origin \"...\")", "its origin with #origin (\"source\" ...)");
            break;
        }
    old_form(tree.try_find("shapes"), "(shapes ...)", "each shape with #shape (NAME ...)");
    // One warning of each per layer, at the first: an unmigrated or misspelt file is
    // noticed at its first check.
    MetadataFinds finds;
    find_metadata(tree, finds);
    if (const Array* m = finds.bare)
        warnings.push_back(make_error(ErrorCode::Syntax,
                                      "(" + Str(m->tag().str()) +
                                          " ...) beside a value reads as a field; if it is "
                                          "metadata, write (@" +
                                          Str(m->tag().str()) + " ...)",
                                      m->file(), m->line()));
    if (const Array* m = finds.unknown)
        warnings.push_back(make_error(ErrorCode::Syntax,
                                      "unknown metadata (" + Str(m->tag().str()) +
                                          " ...): Baker reads @range, @step, @desc, @note, "
                                          "@kind, @group, @one_of and @was",
                                      m->file(), m->line()));
    const Array& stack = b.root;
    Finder finder(alloc);
    // A list of same-tagged records the layer folds: for the tools, not a shipped game.
    if constexpr (!kShipping) {
        Vec<Symbol> tags(alloc);
        folds(finder, stack, tree, 0, tags, warnings);
    }
    if (b.scratch.aliases) finder.merged = &stack;
    // supplier() and source_in(), through the finder: the enabled layer, from the top, whose
    // own tree holds the value, and where. With `list`, the list a verb changes.
    auto below_source = [&](const Path& path, bool list) -> std::optional<Source> {
        const bool index = last_is_index(path);
        for (size_t i = b.layers.size(); i-- > 0;) {
            detail::Resolved r;
            if (!finder.resolve(*b.layers[i], path, r)) continue;
            if (!index && !(r.array && (list ? !r.array->values().empty() : has_values(*r.array)) &&
                            !detail::composes(*r.array)))
                continue;
            const Array& holder = *r.values.source();
            Source s{b.names[i], holder.file(), holder.line()};
            if (const Origin* o = b.session.origin_of(holder.file())) {
                s.origin = o->source;
                s.tool = o->tool;
            }
            return s;
        }
        return std::nullopt;
    };
    Vec<Node> below;
    const Vec<Leaf> leaves = leaves_of(tree);
    size_t chars = 0;
    for (const Leaf& leaf : leaves) chars += leaf.path.size();
    ReportTexts text(leaves.size(), chars);  // the entries' paths, interned never
    for (const Leaf& leaf : leaves) {
        const Path path(leaf.path);
        const bool removal = detail::is_removal(*leaf.holder);
        const bool verb = detail::composes(*leaf.holder);
        const bool exists =
            verb ? finder.list(stack, path, below) : finder.values(stack, path, below);
        LayerEffectEntry e{};
        e.path = text(leaf.path);
        e.values = RowValues({leaf.values.data(), leaf.values.size()});
        e.folded = leaf.folded;
        e.op = op_of(*leaf.holder);
        e.source = Source{layer, leaf.holder->file(), leaf.holder->line()};
        if (exists) {
            e.below = RowValues({below.data(), below.size()});
            e.below_source = below_source(path, verb);
        }
        if (leaf.holder->has_marker()) {  // a verb or a pin
            e.stale = stale(*leaf.holder, exists ? &below : nullptr);
            row_warnings(*leaf.holder, leaf.path, exists ? &below : nullptr, stack, warnings);
        }
        effect.push_back(std::move(e));

        auto refuse = [&](const Str& why) {
            refusals.push_back(
                refused("'" + leaf.path + "' " + why, leaf.holder->file(), leaf.holder->line()));
        };
        if (policy.scalars_only) {
            if (removal)
                refuse("is removed; the layer may only change values");
            else
                for (const Node& v : leaf.values)
                    if (!is_scalar(v)) {
                        refuse("is " + detail::preview(v) +
                               ", not a number, symbol, or string; the layer may only change "
                               "values to those");
                        break;
                    }
        }
        if (policy.existing_only) {
            if (removal)
                refuse("is removed; the layer may only change values the stack has");
            else if (!exists)
                refuse("is new; the layer may only change values the stack has");
        }
        if (!policy.allow.empty() && std::none_of(policy.allow.begin(), policy.allow.end(),
                                                  [&](const Path& p) { return under(path, p); }))
            refuse("is outside the paths the layer may change");
    }

    // The candidate's declarations, each over the stack's of that name.
    Vec<Declaration> decls(alloc);
    declarations_of(session, tree.file(), {}, decls);
    for (const Declaration& d : decls) {
        LayerEffectEntry e{};
        e.path = text(d.name.str());
        e.values = RowValues({&d.form, 1});
        e.declares = d.directive;
        e.source = Source{layer, d.file, d.line};
        if (d.directive == Symbol("#shape"))
            for (const ShapeDecl& under : b.session.shapes())
                if (under.form->tag() == d.name) {
                    const Node form(under.form);
                    e.below = RowValues({&form, 1});
                    e.below_source = Source{Symbol(), under.file, under.line};
                }
        effect.push_back(std::move(e));
    }

    // The validators, over the stack with the candidate on top.
    const Ref<Array> merged = detail::merge(stack, tree, MergePolicy::Overwrite, b.scratch);
    Vec<Ref<Array>> layers(alloc);
    layers.assign(b.layers.data(), b.layers.data() + b.layers.size());
    layers.push_back(candidate);
    const ValidatorStack with{*merged, {layers.data(), layers.size()}, session, &tree};
    for (const Config::Validator* v : b.validators)
        (*v)(with, [&warnings](const Error& e) { warnings.push_back(e); });

    if (policy.warnings_refuse)
        for (const Error& w : warnings)
            refusals.push_back(
                refused("a warning refuses the layer: " + Str(w.message()), w.file(), w.line()));
    return LayerReport{List<LayerEffectEntry>(effect), List<Error>(refusals), List<Error>(warnings),
                       CheckStamp{}};
}

// Reads a candidate's text as push_layer_text does, with `session` (a copy of the stack's
// read session, so its packs meet the stack's), then checks it on `b`.
Expected<LayerReport> check_text_on(const Basis& b, std::string_view name, std::string_view text,
                                    LoadSession& session, const LayerPolicy& policy) {
    // Text only and reaching no other file, as a layer from outside must be.
    session.text_only = session.refuse_includes = true;
    session.clear_autorun();
    const size_t known = session.warnings().size();
    auto tree = read_string(text, name, session);
    if (!tree) return Unexpected(std::move(tree).error());
    const auto all = session.warnings();
    return run_check(b, name, *tree, session.autorun(), session, policy,
                     all.subspan(std::min(known, all.size())));
}

}  // namespace

LayerReport Config::Impl::check_candidate(const Config& c, std::string_view name,
                                          const Ref<Array>& candidate,
                                          std::span<const Node> commands,
                                          const LoadSession& session, const LayerPolicy& policy,
                                          std::span<const Error> read_warnings,
                                          size_t below) const {
    Vec<Symbol> names(alloc);
    for (size_t i = 0; i < std::min(below, layers.size()); ++i)
        if (layers[i].options.enabled) names.emplace_back(layers[i].name);
    Vec<const Validator*> all(alloc);
    for (const Check& v : validators) all.push_back(&v.fn);
    // Below the top, the stack is the layers under the candidate's place, merged anew.
    Ref<Array> under = c.root();
    if (below < layers.size()) {
        under = Array::make(0);
        for (size_t i = 0; i < below; ++i)
            if (layers[i].options.enabled)
                under =
                    detail::merge(*under, *layers[i].root, MergePolicy::Overwrite, merge_scratch);
    }
    const Basis b{*under,  stack_layers(nullptr, below), {names.data(), names.size()},
                  as_read, {all.data(), all.size()},     merge_scratch,
                  alloc};
    return run_check(b, name, candidate, commands, session, policy, read_warnings);
}

LayerReport Config::Impl::check_left_out(const Config& c, const Ref<Array>& candidate,
                                         const LoadSession& session,
                                         const LayerPolicy& policy) const {
    const Ref<Array> merged =
        detail::merge(*c.root(), *candidate, MergePolicy::Overwrite, merge_scratch);
    const ValidatorStack with{*merged, stack_layers(candidate), session, candidate.get()};
    Vec<Error> warnings(alloc);
    for (const Check& v : validators)
        if (!v.options.any_thread)
            v.fn(with, [&warnings](const Error& e) { warnings.push_back(e); });
    Vec<Error> refusals(alloc);
    if (policy.warnings_refuse)
        for (const Error& w : warnings)
            refusals.push_back(
                refused("a warning refuses the layer: " + Str(w.message()), w.file(), w.line()));
    return LayerReport{{}, List<Error>(refusals), List<Error>(warnings), CheckStamp{}};
}

namespace config_internal {

namespace {
uint64_t mix(uint64_t h, uint64_t v) noexcept {
    return h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
}
}  // namespace

uint64_t text_subject(std::string_view name, std::string_view text) noexcept {
    return mix(mix(1, detail::content_hash(name)), detail::content_hash(text));
}

uint64_t tree_subject(std::string_view name, const Array* tree) noexcept {
    return mix(mix(2, detail::content_hash(name)), reinterpret_cast<uintptr_t>(tree));
}

uint64_t policy_hash(const LayerPolicy& p) noexcept {
    uint64_t h = mix(3, (p.data_only ? 1u : 0u) | (p.scalars_only ? 2u : 0u) |
                            (p.existing_only ? 4u : 0u) | (p.warnings_refuse ? 8u : 0u));
    for (const Path& path : p.allow) {
        h = mix(h, path.size());
        for (size_t i = 0; i < path.size(); ++i)
            h = mix(mix(h, reinterpret_cast<uintptr_t>(path[i].name.id())),
                    path[i].is_index ? path[i].index + 1 : 0);
    }
    return h;
}

}  // namespace config_internal

void Config::stamp(LayerReport& report, uint64_t subject,
                   const LayerPolicy& policy) const noexcept {
    report.stamp.config_ = this;
    report.stamp.generation_ = impl_->generation;
    report.stamp.subject_ = subject;
    report.stamp.policy_ = policy_hash(policy);
}

Config::Holding Config::holds(const LayerOptions& options, uint64_t subject,
                              bool on_top) const noexcept {
    const LayerReport* r = options.checked;
    if (!r || !options.policy || !on_top || r->stamp.config_ != this ||
        r->stamp.generation_ != impl_->generation || r->stamp.subject_ != subject ||
        r->stamp.policy_ != policy_hash(*options.policy))
        return Holding::No;
    return r->stamp.left_out_ && r->ok() ? Holding::LeftOut : Holding::Yes;
}

Expected<LayerReport> Config::check_layer(std::string_view name, const Ref<Array>& root,
                                          const LayerPolicy& policy) const {
    if (!root) return Unexpected(make_error(ErrorCode::NotFound, "null layer to check"));
    LayerReport report = impl_->check_candidate(*this, name, root, {}, impl_->as_read, policy, {});
    stamp(report, tree_subject(name, root.get()), policy);
    return report;
}

Expected<LayerReport> Config::check_layer_text(std::string_view name, std::string_view text,
                                               const LayerPolicy& policy) const {
    LoadSession session = impl_->as_read;
    session.clear_autorun();
    const size_t known = session.warnings().size();
    session.text_only = session.refuse_includes = true;
    auto tree = read_string(text, name, session);
    if (!tree) return Unexpected(std::move(tree).error());
    const auto all = session.warnings();
    LayerReport report = impl_->check_candidate(*this, name, *tree, session.autorun(), session,
                                                policy, all.subspan(std::min(known, all.size())));
    stamp(report, text_subject(name, text), policy);
    return report;
}

// --- LayerChecker ---

struct LayerChecker::State {
    explicit State(const Allocator& a) : alloc(with_kind(a, MemoryKind::Config)) {}
    const Allocator alloc;  // the host's: this, and each check's temporaries
    const void* config = nullptr;
    uint64_t generation = 0;
    Ref<Array> root;
    Vec<Ref<Array>> layers{alloc};
    Vec<Symbol> names{alloc};
    LoadSession session{alloc};
    Vec<Config::Validator> validators{alloc};  // the any_thread ones
    bool left_out = false;                     // and there were others
    bool aliases = false;                      // some layer has an (@aka ...)
    std::atomic<uint32_t> refs{1};
};

LayerChecker::LayerChecker(const LayerChecker& other) noexcept : state_(other.state_) {
    if (state_) state_->refs.fetch_add(1, std::memory_order_relaxed);
}

LayerChecker& LayerChecker::operator=(const LayerChecker& other) noexcept {
    if (this == &other) return *this;
    if (other.state_) other.state_->refs.fetch_add(1, std::memory_order_relaxed);
    this->~LayerChecker();
    state_ = other.state_;
    return *this;
}

LayerChecker::~LayerChecker() {
    if (state_ && state_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        const Allocator alloc = state_->alloc;
        detail::destroy(alloc, state_);
    }
    state_ = nullptr;
}

namespace {

// A check on a checker's state, with its own merge scratch and validator list, so that
// checks on several threads share nothing they write.
template <class F>
auto on_state(const LayerChecker::State& st, F&& check) {
    detail::MergeScratch scratch(st.alloc);
    scratch.aliases = st.aliases;
    Vec<const Config::Validator*> validators(st.alloc);
    for (const Config::Validator& v : st.validators) validators.push_back(&v);
    const Basis b{
        *st.root,   {st.layers.data(), st.layers.size()},   {st.names.data(), st.names.size()},
        st.session, {validators.data(), validators.size()}, scratch,
        st.alloc};
    return check(b);
}

}  // namespace

void LayerChecker::stamp(LayerReport& report, uint64_t subject,
                         const LayerPolicy& policy) const noexcept {
    report.stamp.config_ = state_->config;
    report.stamp.generation_ = state_->generation;
    report.stamp.subject_ = subject;
    report.stamp.policy_ = policy_hash(policy);
    report.stamp.left_out_ = state_->left_out;
}

Expected<LayerReport> LayerChecker::check(std::string_view name, const Ref<Array>& root,
                                          const LayerPolicy& policy) const {
    if (!state_) return Unexpected(make_error(ErrorCode::NotFound, "an empty LayerChecker"));
    if (!root) return Unexpected(make_error(ErrorCode::NotFound, "null layer to check"));
    LayerReport report = on_state(*state_, [&](const Basis& b) {
        return run_check(b, name, root, {}, state_->session, policy, {});
    });
    stamp(report, tree_subject(name, root.get()), policy);
    return report;
}

Expected<LayerReport> LayerChecker::check_text(std::string_view name, std::string_view text,
                                               const LayerPolicy& policy) const {
    if (!state_) return Unexpected(make_error(ErrorCode::NotFound, "an empty LayerChecker"));
    auto report = on_state(*state_, [&](const Basis& b) {
        LoadSession session(state_->alloc);
        session = state_->session;
        return check_text_on(b, name, text, session, policy);
    });
    if (report) stamp(*report, text_subject(name, text), policy);
    return report;
}

LayerChecker Config::checker(const Allocator& allocator) const {
    LayerChecker out;
    out.state_ =
        detail::create<LayerChecker::State>(with_kind(allocator, MemoryKind::Config), allocator);
    LayerChecker::State& st = *out.state_;
    st.config = this;
    st.generation = impl_->generation;
    st.root = root_;
    for (const LayerState& l : impl_->layers)
        st.aliases = st.aliases || (l.options.enabled && l.aliases);
    for (const LayerState& l : impl_->layers)
        if (l.options.enabled) {
            st.layers.push_back(l.root);
            st.names.emplace_back(l.name);
        }
    st.session = impl_->as_read;
    st.session.allocator = &st.alloc;  // a candidate's tree is the checker's
    for (const Impl::Check& v : impl_->validators) {
        if (v.options.any_thread)
            st.validators.push_back(v.fn);
        else
            st.left_out = true;
    }
    return out;
}

namespace {

// The children of `a` that are keys a layer merges by: tagged, not metadata, not a row that
// composes or removes.
bool merges_by_tag(const Node& n) {
    const Array* c = n.try_array();
    if (!c || c->tag().empty() || c->tag().is_meta()) return false;
    return !(c->has_marker() && (detail::composes(*c) || detail::is_removal(*c)));
}

// `tags`: the keys down to `layer`, made a path only for a warning.
void folds(Finder& finder, const Array& below, const Array& layer, size_t first, Vec<Symbol>& tags,
           Vec<Error>& out) {
    // Neither array repeating a tag, the usual case, each key needs only its record below.
    // Asked at the first key: a leaf, (price 30), has none and costs nothing.
    int8_t layer_repeats = -1, below_repeats = -1;
    for (size_t i = first; i < layer.size(); ++i) {
        if (!merges_by_tag(layer[i])) continue;
        const Array& row = *layer[i].try_array();
        const Symbol tag = row.tag();
        if (layer_repeats < 0) {
            layer_repeats = finder.repeats(layer) ? 1 : 0;
            below_repeats = finder.repeats(below) ? 1 : 0;
        }
        if (layer_repeats == 1 && finder.find(layer, tag) != &row)
            continue;  // counted at the first
        const Array* under = finder.find(below, tag);
        if (!under) continue;  // new to the stack: added whole
        const uint32_t theirs = below_repeats == 1 ? finder.count(below, tag) : 1;
        const uint32_t mine = layer_repeats == 1 ? finder.count(layer, tag) : 1;
        bool values = false;  // a record of the layer's with values: folded, they overwrite
        if (mine > 1 && theirs < 2)
            for (size_t k = i; k < layer.size() && !values; ++k)
                if (const Array* c = layer[k].try_array(); c && c->tag() == tag)
                    values = has_values(*c);
        tags.push_back(tag);
        if (theirs > 1 || (mine > 1 && values)) {
            Str path, at;
            for (size_t k = 0; k < tags.size(); ++k) {
                if (!at.empty()) at += '.';
                detail::append_path_name(at, tags[k].str());
                if (k + 2 == tags.size()) path = at;
            }
            const Str key = "(" + Str(tag.str()) + " ...)";
            const Str where = path.empty() ? Str("the top level") : "'" + path + "'";
            const Str why = theirs > 1
                                ? where + " below holds " + detail::decimal(theirs) + " " + key +
                                      ", and the layer's merges into the first of them"
                                : "the layer writes " + detail::decimal(mine) + " " + key + " in " +
                                      where + ", and each merges into the same record";
            Error w = make_error(ErrorCode::Syntax,
                                 why +
                                     ": a list a layer changes is written as plain values, "
                                     "restated whole, or extended with #append and #drop",
                                 row.file(), row.line());
            w.set_path(at.view());
            out.push_back(std::move(w));
        }
        if (row.size() > 1 && under->size() > 1) folds(finder, *under, row, 1, tags, out);
        tags.pop_back();
    }
}

}  // namespace

namespace config_internal {
void fold_warnings(const Array& below, const Array& layer, Vec<Error>& out) {
    Finder finder(default_allocator(MemoryKind::Config));
    Vec<Symbol> tags;
    folds(finder, below, layer, 0, tags, out);
}
}  // namespace config_internal

}  // namespace baker
