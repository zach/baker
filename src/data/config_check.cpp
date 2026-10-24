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
    explicit Finder(const Allocator& a) : wide_(a), keys_(a), found_(a) {}

    [[nodiscard]] const Array* find(const Array& parent, Symbol tag) {
        if (parent.size() < kWide || tag.empty()) return parent.try_find(tag);
        const Wide& w = index(parent);
        for (uint32_t i = static_cast<uint32_t>(tag.hash()) & w.mask;; i = (i + 1) & w.mask) {
            const void* k = keys_[w.first + i];
            if (!k) return nullptr;
            if (k == tag.id()) return found_[w.first + i];
        }
    }
    // detail::resolve, through find().
    [[nodiscard]] bool resolve(const Array& root, const Path& path, detail::Resolved& out) {
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

private:
    static constexpr size_t kWide = 32;  // below this, the tag run's scan is as quick
    struct Wide {
        const Array* array;
        uint32_t first, mask;  // its slots in keys_ and found_
    };

    const Wide& index(const Array& parent) {
        for (const Wide& w : wide_)
            if (w.array == &parent) return w;
        uint32_t cap = 64;
        while (cap < parent.size() * 2) cap *= 2;
        const Wide w{&parent, static_cast<uint32_t>(keys_.size()), cap - 1};
        keys_.resize(keys_.size() + cap, nullptr);
        found_.resize(found_.size() + cap, nullptr);
        for (const Node& n : parent.nodes()) {
            const Array* child = n.try_array();
            const Symbol tag = child ? child->tag() : Symbol();
            if (tag.empty()) continue;
            uint32_t i = static_cast<uint32_t>(tag.hash()) & w.mask;
            while (keys_[w.first + i] && keys_[w.first + i] != tag.id()) i = (i + 1) & w.mask;
            if (keys_[w.first + i]) continue;  // the first child with the tag wins
            keys_[w.first + i] = tag.id();
            found_[w.first + i] = child;
        }
        wide_.push_back(w);
        return wide_.back();
    }

    Vec<Wide> wide_;
    Vec<const void*> keys_;
    Vec<const Array*> found_;
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
    const Array& stack = b.root;
    Finder finder(alloc);
    // supplier() and source_in(), through the finder: the enabled layer, from the top, whose
    // own tree holds the value, and where.
    auto below_source = [&](const Path& path) -> std::optional<Source> {
        const bool index = last_is_index(path);
        for (size_t i = b.layers.size(); i-- > 0;) {
            detail::Resolved r;
            if (!finder.resolve(*b.layers[i], path, r)) continue;
            if (!index && !(r.array && has_values(*r.array))) continue;
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
    for (const Leaf& leaf : leaves_of(tree)) {
        const Path path(leaf.path);
        const bool removal = detail::is_removal(*leaf.holder);
        const bool exists = finder.values(stack, path, below);
        LayerEffectEntry e{};
        e.path = Symbol(leaf.path);
        e.value = pack(leaf.values);
        e.source = Source{layer, leaf.holder->file(), leaf.holder->line()};
        if (exists) {
            e.below = pack(below);
            e.below_source = below_source(path);
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

    // The validators, over the stack with the candidate on top.
    const Ref<Array> merged = detail::merge(stack, tree, MergePolicy::Overwrite, b.scratch);
    Vec<Ref<Array>> layers(alloc);
    layers.assign(b.layers.data(), b.layers.data() + b.layers.size());
    layers.push_back(candidate);
    const ValidatorStack with{*merged, {layers.data(), layers.size()}, session};
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
    const ValidatorStack with{*merged, stack_layers(candidate), session};
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
    explicit State(const Allocator& a) : alloc(a) {}
    const Allocator alloc;  // the host's: this, and each check's temporaries
    const void* config = nullptr;
    uint64_t generation = 0;
    Ref<Array> root;
    Vec<Ref<Array>> layers{alloc};
    Vec<Symbol> names{alloc};
    LoadSession session{alloc};
    Vec<Config::Validator> validators{alloc};  // the any_thread ones
    bool left_out = false;                     // and there were others
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
    out.state_ = detail::create<LayerChecker::State>(allocator, allocator);
    LayerChecker::State& st = *out.state_;
    st.config = this;
    st.generation = impl_->generation;
    st.root = root_;
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

}  // namespace baker
