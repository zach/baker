// Config: its layers, reading them and reloading, the merge, scratch and the
// edit journal, and snapshots. Write-back and the layer tools are in config_write.cpp, the
// reload history and loop statistics in config_loop.cpp.

#include "config_impl.hpp"

#include <algorithm>

#if defined(_MSC_VER)
#define BAKER_INLINE __forceinline
#else
#define BAKER_INLINE [[gnu::always_inline]] inline
#endif

namespace baker {
namespace {

// Whether the subtree at `group` differs between two merged trees.
bool group_changed(const Array& before, const Array& after, const Path& group) {
    detail::Resolved a, b;
    const bool ha = detail::resolve(before, group, a) == detail::ResolveStatus::Ok;
    const bool hb = detail::resolve(after, group, b) == detail::ResolveStatus::Ok;
    if (ha != hb) return true;
    if (!ha) return false;
    if (a.array && b.array) return !structurally_equal(*a.array, *b.array);
    return !same_nodes(a.values.nodes(), b.values.nodes());
}

Vec<ScratchEntry> copy_scratch(const Vec<ScratchEntry>& from) {
    Vec<ScratchEntry> out;
    for (const ScratchEntry& e : from)
        out.push_back({e.key, e.path, Array::make_copy(e.values->nodes()), e.edit});
    return out;
}

// A checked layer's verdict for push_layer: the first refusal, with how many more.
Expected<void> refuse_unless(const LayerReport& report) {
    if (report.ok()) return {};
    const Error& first = report.refusals[0];
    Str message(first.message());
    if (report.refusals.size() > 1)
        message +=
            " (and " + detail::decimal(static_cast<int64_t>(report.refusals.size() - 1)) + " more)";
    return Unexpected(make_error(ErrorCode::Refused, message, first.file(), first.line()));
}

}  // namespace

namespace config_internal {

Node pack(const Vec<Node>& values) {
    if (values.size() == 1) return values[0];
    return Node(Array::make_copy(values));
}

Str quoted(const Path& path) {
    return "'" + detail::text_of(path) + "'";
}

Path parent_of(const Path& path) {
    return Path(detail::text_of(path, path.size() - 1));
}

// The nodes a path's value consists of: the values of the array it names, or the one node
// an index selects. False if the path does not resolve.
bool value_nodes(const Array& root, const Path& path, Vec<Node>& out) {
    out.clear();
    if (last_is_index(path)) {
        detail::Resolved r;
        if (detail::resolve(root, parent_of(path), r) != detail::ResolveStatus::Ok) return false;
        const uint32_t i = path[path.size() - 1].index;
        if (i >= r.values.size()) return false;
        out.push_back(r.values[i]);
        return true;
    }
    detail::Resolved r;
    if (detail::resolve(root, path, r) != detail::ResolveStatus::Ok || !r.array) return false;
    for (size_t i = path.empty() ? 0 : 1; i < r.array->size(); ++i)
        if (!is_tagged((*r.array)[i])) out.push_back((*r.array)[i]);
    return true;
}

bool same_nodes(std::span<const Node> a, std::span<const Node> b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](const Node& x, const Node& y) { return structurally_equal(x, y); });
}

}  // namespace config_internal

ConfigSnapshot::ConfigSnapshot(const ConfigSnapshot& other) noexcept : state_(other.state_) {
    if (state_) state_->refs.fetch_add(1, std::memory_order_relaxed);
}
ConfigSnapshot& ConfigSnapshot::operator=(const ConfigSnapshot& other) noexcept {
    ConfigSnapshot copy(other);
    std::swap(state_, copy.state_);
    return *this;
}
ConfigSnapshot::~ConfigSnapshot() {
    if (state_ && state_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        detail::destroy(state_->alloc, state_);
}

void Config::Impl::log_edit(ScratchEntry& owner, const Node* before, uint32_t before_count,
                            std::span<const Node> after, const Source& source) {
    if constexpr (!kShipping) {
        if (!journal_cap) return;
        settle(owner, EditFate::Superseded, {});
        if (journal.capacity() < journal_cap) journal.reserve(journal_cap);
        if (journal.size() < journal_cap) journal.push_back(JournalEntry{});
        // Reused in place: every field is written, and a value slot the previous entry
        // used beyond this one's count is cleared, so nothing it held stays alive.
        JournalEntry& e = journal[journal_count % journal_cap];
        const uint32_t was =
            std::max(std::min<uint32_t>(e.before_count, 4), std::min<uint32_t>(e.after_count, 4));
        owner.edit = journal_count++;
        e.path = owner.key;
        e.time_ns = now();
        e.before_count = before_count;
        e.after_count = static_cast<uint32_t>(after.size());
        const uint32_t nb = std::min<uint32_t>(before_count, 4);
        const uint32_t na = std::min<uint32_t>(e.after_count, 4);
        for (uint32_t i = 0; i < nb; ++i) e.before[i] = before[i];
        for (uint32_t i = 0; i < na; ++i) e.after[i] = after[i];
        for (uint32_t i = nb; i < was; ++i) e.before[i] = Node();
        for (uint32_t i = na; i < was; ++i) e.after[i] = Node();
        e.source = source;
        e.fate = EditFate::Unsaved;
        e.written = {};
    }
}

Expected<Ref<Array>> Config::Impl::read_through_cache(std::string_view path, const Str& cache,
                                                      LoadSession& session, bool from_disk) const {
    if constexpr (kShipping) {  // a cache for this session, else the text; writes none
        return detail::read_shipped(path, cache, session, from_disk);
    } else {
        if (Ref<Array> fresh = detail::read_fresh_cache(cache, session, from_disk)) return fresh;
        session.record_for_cache = true;
        auto root = read_file(path, session);
        session.record_for_cache = false;
        Str bytes(alloc);
        if (root && write_cache(**root, detail::str_out(bytes), &session))
            (void)writer(cache, bytes);
        return root;
    }
}

// stack_files(): the layers' dependencies, flattened. A merge that read the same files keeps
// the list, and its epoch, as they were.
void Config::Impl::refresh_files() {
    size_t i = 0;
    bool same = true;
    for (const LayerState& l : layers)
        for (const Dep& d : l.deps) {
            same = same && i < stack_files.size() && stack_files[i].present == d.present &&
                   stack_files[i].path.str() == d.path.view() &&
                   stack_files[i].layer.str() == l.name.view();
            ++i;
        }
    if (same && i == stack_files.size()) return;
    stack_files.clear();
    for (const LayerState& l : layers)
        for (const Dep& d : l.deps)
            stack_files.push_back({Symbol(l.name.view()), Symbol(d.path.view()), d.present});
    ++stack_files_epoch;
}

Expected<void> Config::Impl::read(LayerState& layer, const LoadSession& before, uint64_t* read_ns,
                                  Vec<Error>* warnings) const {
    auto session = Rc<LoadSession>::make(alloc, before);
    session->clear_autorun();
    auto map = options.edit_mode ? Rc<SourceMap>::make(alloc, alloc) : nullptr;
    session->source_map = map.get();
    Vec<Dep> deps(alloc);
    Expected<Ref<Array>> root = Ref<Array>();
    if (layer.text) {
        // Received: from its text alone, never a cache and never another file.
        session->text_only = session->refuse_includes = true;
        const uint64_t t0 = read_ns ? now() : 0;
        root = read_string(*layer.text, layer.path, *session);
        if (read_ns) *read_ns += now() - t0;
        session->text_only = before.text_only;
        session->refuse_includes = before.refuse_includes;
    } else {
        const LoadSession::Reader& reader = before.reader();
        // Edit mode reads text, which keeps the source maps write-back needs.
        Str cache(alloc);
        if (!options.edit_mode && !cache_dir.empty()) {
            // The cache's name: the hash of the layer's normalized path, in hex, so that the
            // bake and the game agree however the path is spelled.
            char name[17];
            uint64_t h = detail::content_hash(detail::normalize_path(layer.path));
            for (int i = 15; i >= 0; --i, h >>= 4) name[i] = "0123456789abcdef"[h & 15];
            name[16] = '\0';
            cache.append(cache_dir).append("/").append(name).append(".datb");
        }
        session->set_reader([&](std::string_view p, TextSink out) {
            const uint64_t t0 = read_ns ? now() : 0;
            // Into the Config's buffer, which the parser copies from before it reads an
            // include through here again: a file read allocates only the parser's copy.
            const bool ok = detail::read_with(reader, p, scan);
            if (read_ns) *read_ns += now() - t0;
            if (p != cache.view())
                deps.push_back({Str(p, alloc), ok, ok ? detail::content_hash(scan) : 0});
            if (ok) out(scan);
            return ok;
        });
        root = cache.empty() ? read_file(layer.path, *session)
                             : read_through_cache(layer.path, cache, *session,
                                                  detail::SessionAccess::reads_disk(before));
        session->set_reader(reader);
    }
    session->source_map = nullptr;
    // The one-way rule's warnings, and a pack's unmet requires, go to the warning
    // handler.
    const auto all = session->warnings();
    for (const Error& w : all.subspan(std::min(before.warnings().size(), all.size()))) {
        if (warn) warn(w);
        if (warnings) warnings->push_back(w);
    }
    session->clear_warnings();
    layer.file = deps.empty() ? layer.path : deps.front().path;
    layer.missing = false;
    if (!root) {
        // An optional layer whose file is absent is an empty layer.
        if (!layer.options.optional || deps.empty() || deps.front().present)
            return Unexpected(std::move(root).error());
        root = Array::make(0, Symbol(layer.file), 1);
        layer.missing = true;
    }
    layer.root = std::move(*root);
    layer.map = map;
    layer.deps = std::move(deps);
    layer.after = session;
    return {};
}

bool Config::Impl::changed(const LayerState& layer, Symbol& first) const {
    for (const Dep& d : layer.deps) {
        const bool ok = detail::read_with(base.reader(), d.path, scan);
        if (ok != d.present || (ok && detail::content_hash(scan) != d.hash)) {
            first = Symbol(d.path);
            return true;
        }
    }
    return false;
}

Expected<void> Config::Impl::reread(Vec<LayerState>& ls, size_t from, uint64_t* read_ns,
                                    uint32_t* files) const {
    Vec<LayerState> fresh;
    for (size_t i = from; i < ls.size(); ++i) {
        LayerState l = ls[i];
        const auto before = fresh.empty() ? session_below(ls, from) : fresh.back().after;
        if (l.path.empty()) {
            l.after = before;
        } else {
            if (auto r = read(l, *before, read_ns); !r) return r;
            if (files)
                for (const Dep& d : l.deps) *files += d.present ? 1u : 0u;
        }
        fresh.push_back(std::move(l));
    }
    for (size_t i = from; i < ls.size(); ++i) ls[i] = std::move(fresh[i - from]);
    return {};
}

Ref<Array> Config::Impl::scratch_root() const {
    if (scratch.empty()) return nullptr;
    Ref<Array> overlay = Array::make(0);
    for (const ScratchEntry& e : scratch) {
        Ref<Array> nest = Array::make_copy(e.values->nodes());
        for (size_t i = e.path.size() - 1; i-- > 0;) nest = array(e.path[i].name, Node(nest));
        overlay =
            detail::merge(*overlay, *array(Node(nest)), MergePolicy::Overwrite, merge_scratch);
    }
    return overlay;
}

bool Config::Impl::current(const Config& c, const Path& path, Vec<Node>& out) const {
    Values v;
    switch (c.scratch_values(path, v)) {
        case ScratchHit::Found: out.assign(v.begin(), v.end()); return true;
        case ScratchHit::PastEnd: out.clear(); return false;
        case ScratchHit::None: break;
    }
    return value_nodes(*c.root_, path, out);
}

// Inlined into set()'s fast path, as it was when its types had internal linkage.
BAKER_INLINE void Config::Impl::retune(const ScratchEntry& e) const {
    if (!options.tweaks || !scratch_enabled) return;
    TweakBase* few[16];
    size_t n = 0;
    // Lazily: even an empty vector allocates in MSVC's debug library.
    Own<Vec<TweakBase*>> more;
    const Values all = e.values->values();
    options.tweaks->each([&](TweakBase& t) {
        const Path& tp = t.parsed();
        Values v;
        uint32_t first = 1;
        if (tp == e.path) {
            v = all;
        } else if (is_element_of(tp, e.path) && tp[tp.size() - 1].index < all.size()) {
            const uint32_t i = tp[tp.size() - 1].index;
            v = Values(all.nodes().subspan(i, 1), e.values.get());
            first += i;
        } else {
            return;
        }
        if (!t.assign(v, true)) return;  // reported at the next full merge
        t.bound_ = e.values;
        t.first_ = first;
        t.count_ = static_cast<uint32_t>(v.size());
        if (n < 16) {
            few[n++] = &t;
        } else {
            if (!more) more = Own<Vec<TweakBase*>>::make(default_allocator());
            more->push_back(&t);
        }
    });
    for (size_t i = 0; i < n; ++i) few[i]->notify();
    if (more)
        for (TweakBase* t : *more) t->notify();
}

void Config::Impl::put(Config& c, const Path& target, std::span<const Node> values) {
    ScratchEntry* found = nullptr;
    for (ScratchEntry& e : scratch)
        if (e.path == target) found = &e;
    // The journal: the old value, from scratch or else from the stack.
    Node before[4];
    uint32_t before_count = 0;
    Source source{scratch_name()};
    if constexpr (!kShipping) {
        if (found) {
            const Values v = found->values->values();
            before_count = static_cast<uint32_t>(v.size());
            std::copy_n(v.begin(), std::min<uint32_t>(before_count, 4), before);
        } else if (journal_cap) {
            Vec<Node> now;
            if (value_nodes(*c.root_, target, now)) {
                before_count = static_cast<uint32_t>(now.size());
                std::copy_n(now.data(), std::min<uint32_t>(before_count, 4), before);
            }
            auto s = c.source(target);
            source = s ? *s : Source{};
        }
    }
    if (found && found->values->size() == values.size() + 1) {
        for (size_t i = 0; i < values.size(); ++i) found->values->set(i + 1, values[i]);
    } else {
        Vec<Node> nodes{Node(target[target.size() - 1].name)};
        nodes.insert(nodes.end(), values.data(), values.data() + values.size());
        Ref<Array> arr = Array::make_from(nodes);
        if (found)
            found->values = std::move(arr);
        else
            scratch.push_back({Symbol(detail::text_of(target)), target, std::move(arr)});
        found = found ? found : &scratch.back();
    }
    log_edit(*found, before, before_count, values, source);
    c.scratch_dirty_ = true;
    ++c.epoch_;  // a live edit, though not a merge
    retune(*found);
}

Ref<Array> Config::Impl::merged() const {
    Ref<Array> r = Array::make(0);
    for (const LayerState& l : layers)
        if (l.options.enabled)
            r = detail::merge(*r, *l.root, MergePolicy::Overwrite, merge_scratch);
    if (scratch_enabled)
        if (Ref<Array> s = scratch_root())
            r = detail::merge(*r, *s, MergePolicy::Overwrite, merge_scratch);
    return r;
}

void Config::Impl::refresh_read() {
    refresh_files();
    const Rc<LoadSession>* top = nullptr;
    for (const LayerState& l : layers)
        if (!l.path.empty() && l.after) top = &l.after;
    // A layer's session is read-only once built, so the same one needs no copy; the base
    // is the host's to change, so it is copied each time.
    if (top && as_read_from.get() == top->get()) return;
    as_read = top ? **top : base;
    as_read_from = top ? *top : nullptr;
}

void Config::Impl::commit(Config& c, ReloadRecord& rec, uint64_t& t) {
    ++generation;
    const Ref<Array> before = std::move(c.root_);
    c.root_ = merged();
    c.scratch_dirty_ = false;
    refresh_read();
    if (!validators.empty()) {
        const ValidatorStack stack{*c.root_, stack_layers(), as_read};
        for (const Check& v : validators) v.fn(stack, warn);
    }
    lap(rec.merge_ns, t);
    ++c.epoch_;
    Vec<TweakBase*> moved;
    if (options.tweaks)
        options.tweaks->each([&](TweakBase& tw) {
            if (tw.bind(*c.root_, warn)) moved.push_back(&tw);
        });
    rec.tweaks_changed = static_cast<uint32_t>(moved.size());
    lap(rec.bind_ns, t);
    for (TweakBase* tw : moved) tw->notify();
    const auto subs = subscribers;  // callbacks may subscribe or unsubscribe
    for (const Subscriber& s : subs) {
        if (!group_changed(*before, *c.root_, s.group)) continue;
        const Reload outcome = s.migrate ? s.migrate() : Reload::Punt;
        if (outcome == Reload::Punt) {
            ++rec.punted;
            if (s.rebuild) s.rebuild();
        } else {
            ++rec.migrated;
        }
    }
    lap(rec.notify_ns, t);
}

void Config::Impl::change(Config& c, Symbol trigger) {
    ReloadRecord rec = begin(trigger);
    uint64_t t = rec.start_ns;
    commit(c, rec, t);
    finish(c, rec, t);
}

Expected<void> Config::Impl::may_change(const Config& c) const {
    if (c.frozen_ && !options.live_layers)
        return Unexpected(refused("the config is frozen; layers and values can no longer change"));
    return {};
}

// --- Construction ---

Config::Config(ConfigOptions options, const Allocator& allocator)
    : root_(Array::make(0)), impl_(detail::create<Impl>(allocator, allocator)) {
    static std::atomic<uint64_t> configs{0};
    impl_->generation = configs.fetch_add(1, std::memory_order_relaxed) << 32;
    impl_->options = options;
    impl_->cache_dir.assign(options.cache_dir);
    impl_->options.cache_dir = impl_->cache_dir;
    if (TweakRegistry* reg = options.tweaks) {
        reg->config_ = this;
        ReloadRecord unused;
        uint64_t t = 0;
        impl_->commit(*this, unused, t);  // binds the registered tweaks (to nothing yet)
    }
}

Config::~Config() {
    if (TweakRegistry* reg = impl_->options.tweaks; reg && reg->config_ == this)
        reg->config_ = nullptr;
    const Allocator alloc = impl_->alloc;
    detail::destroy(alloc, impl_);
}

LoadSession& Config::session() noexcept {
    ++impl_->generation;  // the caller may change what a check reads with
    return impl_->base;
}

std::span<const StackFile> Config::stack_files() const noexcept {
    return {impl_->stack_files.data(), impl_->stack_files.size()};
}
uint64_t Config::stack_files_epoch() const noexcept {
    return impl_->stack_files_epoch;
}

const LoadSession& Config::read_session() const noexcept {
    return impl_->as_read;
}

void Config::set_writer(Writer writer) {
    impl_->writer = writer ? std::move(writer) : Writer(write_disk_file);
}

void Config::set_warning_handler(Callback<void(const Error&)> handler) {
    impl_->warn = std::move(handler);
}

std::span<const Ref<Array>> Config::Impl::stack_layers(const Ref<Array>& candidate) const {
    enabled_roots.clear();
    for (const LayerState& l : layers)
        if (l.options.enabled) enabled_roots.push_back(l.root);
    if (candidate) enabled_roots.push_back(candidate);
    return {enabled_roots.data(), enabled_roots.size()};
}

Config::ValidatorId Config::add_validator(Validator validator, ValidatorOptions options) {
    const ValidatorId id = impl_->next_id++;
    impl_->validators.push_back({id, std::move(validator), options});
    ++impl_->generation;
    return id;
}

void Config::remove_validator(ValidatorId id) {
    auto& v = impl_->validators;
    v.erase(std::remove_if(v.begin(), v.end(), [id](const auto& x) { return x.id == id; }),
            v.end());
    ++impl_->generation;
}

void Config::bind(TweakBase& t) {
    if (t.bind(*root_, impl_->warn)) t.notify();
}

// --- Layers ---

Expected<size_t> Config::Impl::placement(LayerOptions& opts) const {
    const std::string_view anchor = opts.below.empty() ? opts.above : opts.below;
    const bool below = !opts.below.empty();
    if (below && !opts.above.empty())
        return Unexpected(refused("a layer goes below one layer or above one, not both"));
    opts.below = opts.above = {};  // the views are the caller's; a layer keeps neither
    if (anchor.empty() || anchor == "scratch") return layers.size();
    const auto at = find_layer(anchor);
    if (!at)
        return Unexpected(make_error(ErrorCode::NotFound, "no layer named '" + Str(anchor) + "'"));
    return below ? *at : *at + 1;
}

Expected<void> Config::Impl::admit(const Config& c, const LayerOptions& opts, std::string_view name,
                                   const Ref<Array>& tree, uint64_t subject, bool on_top,
                                   std::span<const Node> commands, const LoadSession& session,
                                   std::span<const Error> read_warnings) const {
    switch (subject ? c.holds(opts, subject, on_top) : Holding::No) {
        case Holding::Yes: return refuse_unless(*opts.checked);  // its verdict
        case Holding::LeftOut: return refuse_unless(check_left_out(c, tree, session, *opts.policy));
        case Holding::No: break;
    }
    return refuse_unless(
        check_candidate(c, name, tree, commands, session, *opts.policy, read_warnings));
}

Expected<void> Config::Impl::push_read(Config& c, LayerState&& layer, LayerOptions opts) {
    if (auto ok = may_change(c); !ok) return ok;
    if (layer.name == "scratch" || find_layer(layer.name))
        return Unexpected(refused("there is already a layer named '" + layer.name + "'"));
    const auto at = placement(opts);
    if (!at) return Unexpected(at.error());
    layer.options = opts;
    const Rc<LoadSession> below = session_below(layers, *at);
    Vec<Error> read_warnings(alloc);
    if (auto r = read(layer, *below, nullptr, &read_warnings); !r) return r;
    if (opts.policy) {
        // A file has no report that holds: no check reads by path.
        const uint64_t subject = layer.text ? text_subject(layer.name, *layer.text) : 0;
        if (auto r = admit(c, opts, layer.name, layer.root, subject, *at == layers.size(),
                           layer.after->autorun(), *layer.after, read_warnings);
            !r)
            return r;
    }
    if (*at == layers.size()) {
        layers.push_back(std::move(layer));
    } else {
        // The layers above read with the macros below them, now this one's too.
        Vec<LayerState> placed = layers;
        placed.insert(placed.begin() + static_cast<ptrdiff_t>(*at), std::move(layer));
        if (auto r = reread(placed, *at + 1); !r) return r;
        layers = std::move(placed);
    }
    change(c, trigger_name("layer"));
    return {};
}

Expected<void> Config::push_layer(std::string_view name, std::string_view path,
                                  LayerOptions options) {
    LayerState layer(impl_->alloc);
    layer.name.assign(name);
    layer.path.assign(path);
    return impl_->push_read(*this, std::move(layer), options);
}

Expected<void> Config::push_layer_text(std::string_view name, std::string_view text,
                                       LayerOptions options) {
    LayerState layer(impl_->alloc);
    layer.name.assign(name);
    layer.path.assign(name);
    layer.text = Rc<Str>::make(impl_->alloc, text, impl_->alloc);
    return impl_->push_read(*this, std::move(layer), options);
}

Expected<void> Config::push_layer(std::string_view name, Ref<Array> overlay, LayerOptions options) {
    if (auto ok = impl_->may_change(*this); !ok) return ok;
    if (!overlay)
        return Unexpected(
            make_error(ErrorCode::NotFound, "null overlay for layer '" + Str(name) + "'"));
    if (name == "scratch" || impl_->find_layer(name))
        return Unexpected(refused("there is already a layer named '" + Str(name) + "'"));
    const auto at = impl_->placement(options);
    if (!at) return Unexpected(at.error());
    if (options.policy)
        if (auto r = impl_->admit(*this, options, name, overlay, tree_subject(name, overlay.get()),
                                  *at == impl_->layers.size(), {}, impl_->as_read, {});
            !r)
            return r;
    LayerState layer(impl_->alloc);
    layer.name.assign(name);
    layer.options = options;
    layer.root = std::move(overlay);
    // An overlay adds no macros, so the layers above need no re-read.
    layer.after = impl_->session_below(impl_->layers, *at);
    impl_->layers.insert(impl_->layers.begin() + static_cast<ptrdiff_t>(*at), std::move(layer));
    impl_->change(*this, trigger_name("layer"));
    return {};
}

Expected<void> Config::remove_layer(std::string_view name) {
    if (auto ok = impl_->may_change(*this); !ok) return ok;
    if (name == "scratch")
        return Unexpected(refused("scratch cannot be removed; clear it instead"));
    const auto at = impl_->find_layer(name);
    if (!at)
        return Unexpected(make_error(ErrorCode::NotFound, "no layer named '" + Str(name) + "'"));
    // The layers above were read with this one's macros; read them again without.
    Vec<LayerState> rest = impl_->layers;
    rest.erase(rest.begin() + static_cast<ptrdiff_t>(*at));
    if (auto r = impl_->reread(rest, *at); !r) return r;
    impl_->layers = std::move(rest);
    impl_->change(*this, trigger_name("layer"));
    return {};
}

Expected<void> Config::pop_layer() {
    if (auto ok = impl_->may_change(*this); !ok) return ok;
    if (impl_->layers.empty())
        return Unexpected(make_error(ErrorCode::NotFound, "no layer to pop"));
    impl_->layers.pop_back();
    impl_->change(*this, trigger_name("layer"));
    return {};
}

List<Symbol> Config::layers() const {
    Vec<Symbol> names;
    for (const auto& l : impl_->layers) names.emplace_back(l.name);
    names.emplace_back("scratch");
    return List<Symbol>(names);
}

List<Node> Config::autorun() const {
    Vec<Node> commands;
    for (const auto& l : impl_->layers) {
        // An overlay's session is the layer's below it: its commands are that layer's.
        if (l.path.empty() || !l.options.enabled || !l.after) continue;
        for (const Node& n : l.after->autorun()) commands.push_back(n);
    }
    return List<Node>(commands);
}

Expected<void> Config::set_layer_enabled(std::string_view name, bool enabled) {
    if constexpr (kShipping) return Unexpected(shipping("toggling a layer"));
    if (auto ok = impl_->may_change(*this); !ok) return ok;
    bool* flag = nullptr;
    if (name == "scratch") {
        flag = &impl_->scratch_enabled;
    } else if (auto at = impl_->find_layer(name)) {
        flag = &impl_->layers[*at].options.enabled;
    } else {
        return Unexpected(make_error(ErrorCode::NotFound, "no layer named '" + Str(name) + "'"));
    }
    if (*flag == enabled) return {};
    *flag = enabled;
    impl_->change(*this, trigger_name("layer"));
    return {};
}

Expected<bool> Config::toggle_layer(std::string_view name) {
    const bool next = !layer_enabled(name);
    if (auto r = set_layer_enabled(name, next); !r) return Unexpected(std::move(r).error());
    return next;
}

bool Config::layer_enabled(std::string_view name) const {
    if (name == "scratch") return impl_->scratch_enabled;
    const auto at = impl_->find_layer(name);
    return at && impl_->layers[*at].options.enabled;
}

Ref<Array> Config::layer_root(std::string_view name) const {
    const auto at = impl_->find_layer(name);
    return at ? impl_->layers[*at].root : nullptr;
}

// --- Reading ---

Config::ScratchHit Config::scratch_values(const Path& path, Values& out) const noexcept {
    const ScratchEntry* e = impl_->scratch_entry(path);
    if (!e) return ScratchHit::None;
    const Values all = e->values->values();
    if (e->path == path) {
        out = all;
        return ScratchHit::Found;
    }
    const uint32_t i = path[path.size() - 1].index;
    if (i >= all.size()) return ScratchHit::PastEnd;
    out = Values(all.nodes().subspan(i, 1), e->values.get());
    return ScratchHit::Found;
}

Error Config::scratch_error(const Path& path, const detail::ConvertInfo* info) {
    if (!info)
        return make_error(ErrorCode::NotFound,
                          quoted(path) + " is past the end of its value in scratch");
    return make_error(ErrorCode::Type, quoted(path) + " in scratch is not " + Str(info->name));
}

Expected<Source> Config::source(const Path& path) const {
    if (!path.valid()) return Unexpected(detail::get_error(*root_, path, nullptr));
    if (path.empty()) return Unexpected(refused("the whole config has no single source"));
    if (impl_->scratch_entry(path)) return Source{Symbol("scratch"), {}, 0};
    std::optional<size_t> at = impl_->supplier(path, impl_->layers.size());
    if (!at) {
        // No layer has values there; name the topmost enabled one that has the path.
        for (size_t i = impl_->layers.size(); i-- > 0 && !at;) {
            detail::Resolved r;
            if (impl_->layers[i].options.enabled &&
                detail::resolve(*impl_->layers[i].root, path, r) == detail::ResolveStatus::Ok)
                at = i;
        }
    }
    if (!at) return Unexpected(detail::get_error(*root_, path, nullptr));
    return impl_->source_in(impl_->layers[*at], path);
}

// --- Reload and telemetry ---

Expected<bool> Config::reload(const ReloadRequest& request) {
    if constexpr (kShipping) return false;
    ReloadRecord rec = impl_->begin(
        request.trigger.empty() ? trigger_name("reload") : request.trigger, request.origin_ns);
    uint64_t t = rec.start_ns;
    if (frozen_ && !impl_->options.live_layers) {
        rec.outcome = ReloadOutcome::Refused;
        impl_->feed(*this, rec, impl_->finish(*this, rec, t));
        return false;
    }
    auto& ls = impl_->layers;
    size_t from = ls.size();
    for (size_t i = 0; i < ls.size() && from == ls.size(); ++i)
        if (!ls[i].path.empty() && impl_->changed(ls[i], rec.first_file)) from = i;
    impl_->lap(rec.scan_ns, t);
    if (from == ls.size()) {
        rec.outcome = ReloadOutcome::Unchanged;
        impl_->feed(*this, rec, impl_->finish(*this, rec, t));
        return false;
    }
    rec.layer = Symbol(ls[from].name);
    if (auto r = impl_->reread_stages(rec, from, t); !r) {
        impl_->feed(*this, rec, impl_->finish(*this, rec, t));
        return Unexpected(std::move(r).error());
    }
    impl_->commit(*this, rec, t);
    impl_->feed(*this, rec, impl_->finish(*this, rec, t));
    return true;
}

Config::ReloadId Config::on_reload(const Path& group, Callback<void()> rebuild,
                                   Callback<Reload()> migrate) {
    const ReloadId id = impl_->next_id++;
    impl_->subscribers.push_back({id, group, std::move(rebuild), std::move(migrate)});
    return id;
}

Config::ReloadId Config::on_reload_complete(Callback<void(const ReloadRecord&)> callback) {
    const ReloadId id = impl_->next_id++;
    impl_->completions.push_back({id, std::move(callback)});
    return id;
}

void Config::remove_on_reload(ReloadId id) {
    auto& s = impl_->subscribers;
    s.erase(std::remove_if(s.begin(), s.end(), [id](const auto& x) { return x.id == id; }),
            s.end());
    auto& c = impl_->completions;
    c.erase(std::remove_if(c.begin(), c.end(), [id](const auto& x) { return x.id == id; }),
            c.end());
}

// --- Scratch ---

Expected<void> Config::set(const Path& path, std::span<const Node> values) {
    if constexpr (kShipping) return Unexpected(shipping("set()"));
    if (auto ok = impl_->may_change(*this); !ok) return ok;
    ++impl_->generation;
    if (!path.valid()) return Unexpected(detail::get_error(*root_, path, nullptr));
    if (path.empty()) return Unexpected(refused("cannot set the whole config"));
    for (size_t i = 0; i + 1 < path.size(); ++i)
        if (path[i].is_index)
            return Unexpected(
                refused("cannot set a value inside an indexed element: " + quoted(path)));
    if (!last_is_index(path)) {
        impl_->put(*this, path, values);
        return {};
    }
    // One element of a value: set the whole value with that element replaced.
    if (values.size() != 1)
        return Unexpected(refused(quoted(path) + " is one element and takes one value"));
    const Path target = parent_of(path);
    Vec<Node> whole;
    if (!impl_->current(*this, target, whole) || path[path.size() - 1].index >= whole.size())
        return Unexpected(detail::get_error(*root_, path, nullptr));
    whole[path[path.size() - 1].index] = values[0];
    impl_->put(*this, target, whole);
    return {};
}

Expected<void> Config::set(std::string_view path, const Node& value) {
    if constexpr (kShipping) return Unexpected(shipping("set()"));
    if (auto ok = impl_->may_change(*this); !ok) return ok;
    // The fast path: a scalar already in scratch changes in place, allocating nothing.
    for (ScratchEntry& e : impl_->scratch) {
        if (e.values->size() == 2 && e.path.matches(path)) {
            const Node before = (*e.values)[1];
            e.values->set(1, value);
            // A scratch value's own source is scratch.
            impl_->log_edit(e, &before, 1, std::span<const Node>(&value, 1),
                            Source{scratch_name()});
            scratch_dirty_ = true;
            ++impl_->generation;
            ++epoch_;
            impl_->retune(e);
            return {};
        }
    }
    return set(Path(path), std::span<const Node>(&value, 1));
}

Expected<void> Config::clear_scratch(std::string_view path) {
    if constexpr (kShipping) return Unexpected(shipping("clear_scratch()"));
    if (auto ok = impl_->may_change(*this); !ok) return ok;
    auto& s = impl_->scratch;
    for (const ScratchEntry& e : s)
        if (path.empty() || e.path.matches(path)) impl_->settle(e, EditFate::Reverted, {});
    if (path.empty())
        s.clear();
    else
        s.erase(std::remove_if(s.begin(), s.end(),
                               [&](const ScratchEntry& e) { return e.path.matches(path); }),
                s.end());
    impl_->change(*this, trigger_name("layer"));
    return {};
}

void Config::set_journal_history(size_t capacity) {
    impl_->journal = Vec<JournalEntry>(impl_->alloc);
    impl_->journal_cap = capacity;
    impl_->journal_count = 0;
    for (ScratchEntry& e : impl_->scratch) e.edit = kNoEdit;
    if (capacity) impl_->journal.reserve(capacity);
}

size_t Config::journal_size() const noexcept {
    return impl_->journal.size();
}

const JournalEntry& Config::journal_at(size_t i) const noexcept {
    const auto& j = impl_->journal;
    const size_t oldest = j.size() < impl_->journal_cap ? 0 : impl_->journal_count % j.size();
    return j[(oldest + i) % j.size()];
}

List<Symbol> Config::unsaved() const {
    Vec<Symbol> keys;
    for (const auto& e : impl_->scratch) keys.emplace_back(e.key);
    return List<Symbol>(keys);
}

// --- Snapshots ---

ConfigSnapshot Config::snapshot() const {
    auto* state = detail::create<ConfigSnapshot::State>(impl_->alloc, impl_->alloc);
    state->layers = impl_->layers;
    state->scratch = copy_scratch(impl_->scratch);
    state->scratch_enabled = impl_->scratch_enabled;
    ConfigSnapshot s;
    s.state_ = state;
    return s;
}

Expected<void> Config::restore(const ConfigSnapshot& snapshot) {
    if (auto ok = impl_->may_change(*this); !ok) return ok;
    if (snapshot.empty()) return Unexpected(make_error(ErrorCode::NotFound, "empty snapshot"));
    impl_->layers = snapshot.state_->layers;
    impl_->scratch = copy_scratch(snapshot.state_->scratch);
    // Unsaved edits the restored scratch does not hold are gone. Each scratch entry
    // finds its own edit, so this is linear in the journal and the scratch.
    Vec<const JournalEntry*> kept;
    kept.reserve(impl_->scratch.size());
    for (const ScratchEntry& e : impl_->scratch)
        if (const JournalEntry* j = impl_->journal_entry(e.edit, e.key)) kept.push_back(j);
    std::sort(kept.begin(), kept.end());
    for (JournalEntry& j : impl_->journal)
        if (j.fate == EditFate::Unsaved && !std::binary_search(kept.begin(), kept.end(), &j))
            j.fate = EditFate::Reverted;
    impl_->scratch_enabled = snapshot.state_->scratch_enabled;
    impl_->change(*this, trigger_name("restore"));
    return {};
}

}  // namespace baker