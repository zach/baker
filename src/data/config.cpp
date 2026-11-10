// Config: its layers, reading them and reloading, the merge, scratch and the
// edit journal, and snapshots. Write-back and the layer tools are in config_write.cpp, the
// reload history and loop statistics in config_loop.cpp.

#include <baker/project.hpp>

#include "config_impl.hpp"
#include "image.hpp"

#include <algorithm>
#include <functional>
#include <optional>

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
    if (!path.empty()) {
        leaf_values(*r.array, out);  // a layer's row: its operands, if it composes
        return true;
    }
    for (const Node& n : *r.array)  // the root has no tag
        if (!is_tagged(n)) out.push_back(n);
    return true;
}

bool list_nodes(const Array& root, const Path& path, Vec<Node>& out) {
    out.clear();
    detail::Resolved r;
    if (path.empty() || last_is_index(path) ||
        detail::resolve(root, path, r) != detail::ResolveStatus::Ok || !r.array)
        return false;
    for (const Node& n : r.array->values()) out.push_back(n);
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
    if (state_ && state_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        const Allocator alloc = state_->alloc;  // a copy: the state goes first
        detail::destroy(alloc, state_);
    }
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

// --- The change log ---

void Config::Impl::begin_change(Symbol name) {
    if (changes.open++ == 0) {
        changes.name = name;
        changes.time_ns = now();
        changes.pending.clear();
        changes.pending_written.clear();
    }
}

void Config::Impl::add_change(ChangeRecord record) {
    changes.pending.push_back(std::move(record));
}

void Config::Impl::close_change() {
    if (changes.open == 0) return;
    --changes.open;
    if (changes.open != 0) return;
    if (changes.pending.empty() && changes.pending_written.empty()) return;
    Transaction t;
    t.id = ++changes.count;
    t.name = changes.name;
    t.time_ns = changes.time_ns;
    t.records = List<ChangeRecord>({changes.pending.data(), changes.pending.size()});
    t.written = List<WrittenLine>({changes.pending_written.data(), changes.pending_written.size()});
    changes.pending.clear();
    changes.pending_written.clear();
    // The state before this transaction is the state after the last applied one (every
    // change in between was recorded), or the one the log began at; a new transaction
    // after an undo drops the undone ones.
    ConfigSnapshot before = changes.done > 0       ? changes.ring[changes.done - 1].after
                            : changes.ring.empty() ? changes.origin
                                                   : changes.ring[0].before;
    changes.ring.erase(changes.ring.begin() + static_cast<ptrdiff_t>(changes.done),
                       changes.ring.end());
    changes.ring.push_back({std::move(t), before, snapshot()});
    if (changes.ring.size() > changes.cap) changes.ring.erase(changes.ring.begin());
    changes.done = changes.ring.size();
}

void Config::Impl::record_set(const Path& path, std::span<const Node> before,
                              std::span<const Node> after, const Source& source, Symbol layer) {
    if (!logging()) return;
    ChangeRecord r;
    r.kind = ChangeKind::Set;
    Str dotted(alloc);
    path.write(detail::str_out(dotted), path.size(), '.');
    r.path = Symbol(dotted.view());
    // make_from takes a mutable span: copied, as scratch copies.
    const auto tree = [this](std::span<const Node> nodes) {
        if (nodes.empty()) return Ref<Array>();
        Vec<Node> copy(alloc);
        copy.assign(nodes.data(), nodes.data() + nodes.size());  // EASTL: pointers
        return Array::make_from(copy);
    };
    r.before = tree(before);
    r.after = tree(after);
    r.layer = layer;
    r.source = source;
    r.time_ns = now();
    const bool own = changes.open == 0;
    if (own) begin_change(Symbol("set"));
    add_change(std::move(r));
    if (own) close_change();
}

// The walk of two merged roots: where a subtree is the same object in both (the
// merge shares what it did not change), nothing changed below it. A keyed child in one
// root alone is a record added or removed, with its whole subtree; one in both with other
// values is a Set at its path; a parent whose common keys come in another order is a
// Reorder; a removed and an added child that share an ID, or whose new one's (@aka ...)
// names the old key, are one Rename. A list a layer row grew or shrank with #append or
// #drop is Append or Drop with the row's operand.
namespace {

Symbol dotted_path(const Str& path, Symbol tag) {
    Str p = path;
    if (!p.empty()) p += '.';
    p += tag.str();
    return Symbol(p.view());
}

Ref<Array> whole(const Array& a) {
    return Ref<Array>(const_cast<Array*>(&a));
}

// An array's own values: its nodes that are not keyed children.
Ref<Array> values_tree(const Array& a, const Allocator& alloc) {
    Vec<Node> own(alloc);
    config_internal::leaf_values(a, own);
    if (own.empty()) return nullptr;
    return Array::make_from(own);
}

bool same_values(const Array& a, const Array& b, const Allocator& alloc) {
    Vec<Node> x(alloc), y(alloc);
    config_internal::leaf_values(a, x);
    config_internal::leaf_values(b, y);
    return config_internal::same_nodes({x.data(), x.size()}, {y.data(), y.size()});
}

bool alias_names(const Array& record, Symbol key) {
    if (!record.meta_first()) return false;
    const Array* aka = record.meta(detail::aka_tag());
    if (!aka) return false;
    for (const Node& n : aka->values())
        if (n.is(key)) return true;
    return false;
}

}  // namespace

void Config::Impl::record_commit(const Config& c, const Array& before, const Array& after,
                                 Symbol trigger) {
    const bool own = changes.open == 0;
    if (own) begin_change(trigger);
    walk_changes(c, before, after, Symbol());
    if (own) close_change();
}

// `fixed`: every record's layer, for a layer's own tree; empty for the merged tree, where
// the layer that supplies each value is found.
void Config::Impl::walk_changes(const Config& c, const Array& before, const Array& after,
                                Symbol fixed) {
    struct Half {  // an added or removed child at one parent, for the rename match
        const Array* array;
        bool added;
        size_t record;  // its record's index in pending
    };
    const uint64_t time = now();
    // The layer that wrote a path's value now: the highest enabled one composing it, with
    // its row (for the verb), else the one its source names.
    const auto writer_of = [&](Symbol path, const Array*& row) -> Symbol {
        row = nullptr;
        if (!fixed.empty()) return fixed;
        const Path p(path.str());
        for (size_t i = layers.size(); i-- > 0;)
            if (layers[i].options.enabled && composes_at(i, p)) {
                row = config_internal::row_at(*layers[i].root, p, alias_root());
                return Symbol(layers[i].name.view());
            }
        const auto s = c.source(p);
        return s ? s->layer : Symbol();
    };
    auto walk = [&](auto&& self, const Array& a, const Array& b, const Str& path) -> void {
        if (&a == &b) return;
        Vec<Half> here(alloc);
        Vec<Symbol> order_a(alloc), order_b(alloc);
        const uint32_t fa = a.tag().empty() ? 0 : 1, fb = b.tag().empty() ? 0 : 1;
        for (uint32_t i = fb; i < b.size(); ++i)
            if (const Array* cb = b[i].try_array();
                cb && !cb->tag().empty() && !cb->tag().is_meta())
                order_b.push_back(cb->tag());
        for (uint32_t i = fa; i < a.size(); ++i) {
            const Array* ca = a[i].try_array();
            if (!ca || ca->tag().empty() || ca->tag().is_meta()) continue;
            const Symbol tag = ca->tag();
            if (std::find(order_a.begin(), order_a.end(), tag) != order_a.end()) continue;
            order_a.push_back(tag);
            const Array* cb = b.try_find(tag);
            const Symbol child_path = dotted_path(path, tag);
            if (!cb) {
                ChangeRecord r;
                r.kind = ChangeKind::Remove;
                r.path = child_path;
                r.id = ca->record_id();
                r.before = whole(*ca);
                r.time_ns = time;
                if (!fixed.empty()) r.source = Source{fixed};
                here.push_back({ca, false, changes.pending.size()});
                add_change(std::move(r));
                continue;
            }
            if (ca == cb) continue;
            if (!same_values(*ca, *cb, alloc)) {
                ChangeRecord r;
                r.kind = ChangeKind::Set;
                r.path = child_path;
                r.id = cb->record_id();
                r.before = values_tree(*ca, alloc);
                r.after = values_tree(*cb, alloc);
                r.time_ns = time;
                if (!fixed.empty()) r.source = Source{fixed};
                const Array* row = nullptr;
                r.layer = writer_of(child_path, row);
                if (row) {
                    const detail::Marker m = detail::verb_of(*row);
                    if (m == detail::Marker::Append || m == detail::Marker::Drop) {
                        r.kind =
                            m == detail::Marker::Append ? ChangeKind::Append : ChangeKind::Drop;
                        Vec<Node> operand(alloc);
                        config_internal::leaf_values(*row, operand);
                        r.after = operand.empty() ? Ref<Array>() : Array::make_from(operand);
                    }
                }
                add_change(std::move(r));
            }
            self(self, *ca, *cb, Str(child_path.str()));
        }
        for (uint32_t i = fb; i < b.size(); ++i) {
            const Array* cb = b[i].try_array();
            if (!cb || cb->tag().empty() || cb->tag().is_meta()) continue;
            if (a.try_find(cb->tag())) continue;
            if (std::any_of(here.begin(), here.end(),
                            [&](const Half& p) { return p.added && p.array->tag() == cb->tag(); }))
                continue;
            ChangeRecord r;
            r.kind = ChangeKind::Add;
            r.path = dotted_path(path, cb->tag());
            r.id = cb->record_id();
            r.after = whole(*cb);
            r.time_ns = time;
            const Array* row = nullptr;
            r.layer = writer_of(r.path, row);
            here.push_back({cb, true, changes.pending.size()});
            add_change(std::move(r));
        }
        // A removed and an added child that are one record: by ID, or by the alias.
        for (const Half& gone : here) {
            if (gone.added) continue;
            for (const Half& came : here) {
                if (!came.added) continue;
                const RecordId id = gone.array->record_id();
                const bool same = (!id.empty() && id == came.array->record_id()) ||
                                  alias_names(*came.array, gone.array->tag());
                if (!same) continue;
                ChangeRecord& r = changes.pending[came.record];
                r.kind = ChangeKind::Rename;
                r.renamed_from = gone.array->tag();
                r.before = whole(*gone.array);
                changes.pending[gone.record].kind = ChangeKind::Reorder;  // marked, dropped below
                changes.pending[gone.record].path = Symbol();
                break;
            }
        }
        // The common keys in another order, a renamed one under its new name: a Reorder.
        Vec<Symbol> common_a(alloc), common_b(alloc);
        for (Symbol t : order_a) {
            for (const Half& gone : here)
                if (!gone.added && gone.array->tag() == t) {
                    const ChangeRecord& r = changes.pending[gone.record];
                    if (r.kind == ChangeKind::Reorder && r.path.empty())  // renamed
                        for (const Half& came : here)
                            if (came.added && changes.pending[came.record].renamed_from == t)
                                t = came.array->tag();
                    break;
                }
            if (std::find(order_b.begin(), order_b.end(), t) != order_b.end())
                common_a.push_back(t);
        }
        for (const Symbol t : order_b)
            if (std::find(common_a.begin(), common_a.end(), t) != common_a.end())
                common_b.push_back(t);
        if (common_a.size() == common_b.size() &&
            !std::equal(common_a.begin(), common_a.end(), common_b.begin())) {
            ChangeRecord r;
            r.kind = ChangeKind::Reorder;
            r.path = path.empty() ? Symbol() : Symbol(path.view());
            r.before = whole(a);
            r.after = whole(b);
            r.time_ns = time;
            add_change(std::move(r));
        }
    };
    walk(walk, before, after, Str(alloc));
    // The renames' dropped halves.
    changes.pending.erase(std::remove_if(changes.pending.begin(), changes.pending.end(),
                                         [](const ChangeRecord& r) {
                                             return r.kind == ChangeKind::Reorder &&
                                                    r.path.empty() && r.before && !r.after;
                                         }),
                          changes.pending.end());
}

void Config::set_change_history(size_t capacity) {
    if constexpr (kShipping) return;
    impl_->changes.ring = Vec<Impl::Kept>(impl_->alloc);
    impl_->changes.cap = capacity;
    impl_->changes.count = 0;
    impl_->changes.done = 0;
    impl_->changes.origin = capacity ? impl_->snapshot() : ConfigSnapshot();
    if (capacity) impl_->changes.ring.reserve(capacity + 1);
}

size_t Config::change_history() const noexcept {
    return impl_->changes.cap;
}

size_t Config::transaction_count() const noexcept {
    return impl_->changes.ring.size();
}

const Transaction& Config::transaction_at(size_t i) const noexcept {
    return impl_->changes.ring[i].t;
}

// Undo and redo: a restore to the kept state, not recorded as a change.
Expected<void> Config::undo() {
    if constexpr (kShipping) return Unexpected(shipping("undo()"));
    auto& ch = impl_->changes;
    if (!impl_->logging()) return Unexpected(refused("the change log is off: nothing to undo"));
    if (ch.done == 0) return Unexpected(make_error(ErrorCode::NotFound, "nothing to undo"));
    ch.quiet = true;
    auto r = restore(ch.ring[ch.done - 1].before);
    ch.quiet = false;
    if (!r) return r;
    ch.ring[--ch.done].t.undone = true;
    return {};
}

Expected<void> Config::redo() {
    if constexpr (kShipping) return Unexpected(shipping("redo()"));
    auto& ch = impl_->changes;
    if (!impl_->logging()) return Unexpected(refused("the change log is off: nothing to redo"));
    if (ch.done == ch.ring.size())
        return Unexpected(make_error(ErrorCode::NotFound, "nothing to redo"));
    ch.quiet = true;
    auto r = restore(ch.ring[ch.done].after);
    ch.quiet = false;
    if (!r) return r;
    ch.ring[ch.done++].t.undone = false;
    return {};
}

void Config::begin_transaction(std::string_view name) {
    if (!impl_->logging()) return;
    impl_->begin_change(Symbol(name));
}

void Config::end_transaction() {
    if (!impl_->logging()) return;
    impl_->close_change();
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
        session.drop_metadata = options.cache_drop_metadata;
        const bool written = root && write_cache(**root, detail::str_out(bytes), &session);
        session.drop_metadata = false;
        if (written) (void)writer(cache, bytes);
        return root;
    }
}

// stack_files(): the layers' dependencies, flattened. A merge that read the same files keeps
// the list, and its epoch, as they were.
void Config::Impl::refresh_files() {
    // Each layer's files and directories, bottom to top.
    auto each = [this](auto&& fn) {
        for (const LayerState& l : layers)
            for (const Dep& d : l.deps) fn(l, d.path.view(), d.present, d.directory);
    };
    size_t i = 0;
    bool same = true;
    each([&](const LayerState& l, std::string_view path, bool present, bool dir) {
        same = same && i < stack_files.size() && stack_files[i].present == present &&
               stack_files[i].directory == dir && stack_files[i].path.str() == path &&
               stack_files[i].layer.str() == l.name.view();
        ++i;
    });
    if (same && i == stack_files.size()) return;
    stack_files.clear();
    each([&](const LayerState& l, std::string_view path, bool present, bool dir) {
        stack_files.push_back({Symbol(l.name.view()), Symbol(path), present, dir});
    });
    ++stack_files_epoch;
}

Expected<void> Config::Impl::read(const ReadContext& io, LayerState& layer,
                                  const LoadSession& before, uint64_t* read_ns,
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
        // A directory a load lists (#include_dir) is one of its files too.
        const LoadSession::Lister& lister = before.lister();
        const LoadSession::Stamper& stamper = before.stamper();
        session->set_lister(
            [&](std::string_view dir, const Callback<void(std::string_view)>& entry) {
                Vec<Str> entries(alloc);
                const uint64_t listing = detail::list_with(lister, dir, entries, alloc);
                deps.push_back({Str(dir, alloc), listing != 0, listing, true});
                for (const Str& e : entries) entry(e);
                return listing != 0;
            });
        session->set_reader([&](std::string_view p, TextSink out) {
            const uint64_t t0 = read_ns ? now() : 0;
            // Into the Config's buffer, which the parser copies from before it reads an
            // include through here again: a file read allocates only the parser's copy.
            const bool ok = detail::read_with(reader, p, io.scan);
            if (read_ns) *read_ns += now() - t0;
            if (p != cache.view())
                deps.push_back({Str(p, alloc), ok, ok ? detail::content_hash(io.scan) : 0, false,
                                stamper ? stamper(p) : 0});
            if (ok) out(io.scan);
            return ok;
        });
        root = cache.empty() ? read_file(layer.path, *session)
                             : read_through_cache(layer.path, cache, *session,
                                                  detail::SessionAccess::reads_disk(before));
        session->set_reader(reader);
        session->set_lister(lister);
    }
    session->source_map = nullptr;
    // The one-way rule's warnings, and a pack's unmet requires, go to the warning
    // handler.
    const auto all = session->warnings();
    for (const Error& w : all.subspan(std::min(before.warnings().size(), all.size()))) {
        tell(io, w);
        if (warnings) warnings->push_back(w);
    }
    session->clear_warnings();
    layer.file = deps.empty() ? layer.path : deps.front().path;
    layer.missing = false;
    if (!root) {
        // An optional layer whose file is absent is an empty layer.
        if (!layer.options.optional || deps.empty() || deps.front().present) {
            layer.deps = std::move(deps);  // a layer found in a directory waits for a change
            return Unexpected(std::move(root).error());
        }
        root = Array::make(0, Symbol(layer.file), 1);
        layer.missing = true;
    }
    layer.root = std::move(*root);
    layer.map = map;
    layer.deps = std::move(deps);
    // What its read tested, if it parsed: a cache's replay records none.
    const auto tested = detail::SessionAccess::tested(*session);
    const size_t from = std::min(detail::SessionAccess::tested(before).size(), tested.size());
    layer.tested.assign(tested.data() + from, tested.data() + tested.size());  // EASTL: pointers
    layer.tested_known =
        detail::SessionAccess::parses(*session) > detail::SessionAccess::parses(before);
    layer.aliases = detail::SessionAccess::aliases(*session) > 0;  // this read's, or replayed
    layer.after = session;
    return {};
}

bool Config::Impl::changed(const ReadContext& io, const LayerState& layer, Symbol& first) const {
    for (const Dep& d : layer.deps) {
        if (d.directory) {
            const uint64_t listing = detail::list_with(io.base.lister(), d.path, io.listed, alloc);
            if ((listing != 0) != d.present || listing != d.hash) {
                first = Symbol(d.path);
                return true;
            }
            continue;
        }
        // The same stamp: unchanged, and not opened. Otherwise read and hashed, so a
        // save of the same text changes nothing, and the stamp it has now is kept for the
        // next scan.
        const uint64_t stamp = io.base.stamper() ? io.base.stamper()(d.path) : 0;
        if (stamp != 0 && stamp == d.stamp) continue;
        const bool ok = detail::read_with(io.base.reader(), d.path, io.scan);
        if (ok != d.present || (ok && detail::content_hash(io.scan) != d.hash)) {
            first = Symbol(d.path);
            return true;
        }
        d.stamp = stamp;
    }
    return false;
}

bool Config::Impl::stale(const ReadContext& io, const LayerState& layer, Symbol& first) const {
    return (!layer.path.empty() || !layer.dir.empty()) && changed(io, layer, first);
}

// --- Directories of layers ---

namespace {

// The name of a path's last entry.
std::string_view leaf_of(std::string_view path) noexcept {
    const size_t at = path.find_last_of("/\\");
    return at == std::string_view::npos ? path : path.substr(at + 1);
}

bool is_dta(std::string_view name) noexcept {
    return name.size() > 4 && name.substr(name.size() - 4) == ".dta";
}

}  // namespace

Expected<void> Config::Impl::list_dir(const ReadContext& io, LayerState& marker,
                                      Vec<Found>& found) const {
    found.clear();
    marker.deps.clear();
    Vec<Str> entries(alloc);
    const uint64_t listing = detail::list_with(io.base.lister(), marker.dir, entries, alloc);
    marker.missing = listing == 0;
    marker.deps.push_back({marker.dir, !marker.missing, listing, true});
    if (marker.missing) {
        if (marker.options.optional) return {};
        return Unexpected(make_error(ErrorCode::Io, "cannot list the directory '" + marker.dir +
                                                        "' of layer '" + marker.name + "'"));
    }
    const Str order = detail::join_path(marker.dir, "order.dta");
    Str text(alloc);
    const bool ordered = detail::read_with(io.base.reader(), order, text);
    marker.deps.push_back({order, ordered, ordered ? detail::content_hash(text) : 0});
    Vec<Found> named(alloc);
    for (const Str& e : entries) {
        const std::string_view leaf = leaf_of(e);
        if (leaf.empty() || leaf.front() == '.' || leaf == "order.dta") continue;  // hidden
        if (is_dta(leaf)) {
            named.push_back({Str(leaf.substr(0, leaf.size() - 4), alloc), Str(e, alloc)});
        } else if (leaf.find('.') == std::string_view::npos) {
            // A subdirectory, if it holds a layer.dta; else watched for one. A name with an
            // extension is a file, and not looked into.
            Str inner = detail::join_path(e, "layer.dta");
            if (detail::read_with(io.base.reader(), inner, io.scan))
                named.push_back({Str(leaf, alloc), std::move(inner)});
            else
                marker.deps.push_back({std::move(inner), false, 0});
        }
    }
    std::stable_sort(named.begin(), named.end(),
                     [](const Found& a, const Found& b) { return a.name < b.name; });
    if (!ordered) {
        found = std::move(named);
        return {};
    }
    Vec<std::string_view> names(alloc);
    for (const Found& f : named) names.push_back(f.name.view());
    Vec<uint32_t> order_of(alloc);
    Vec<Error> warnings(alloc);
    if (auto r =
            detail::directory_order(text, order, {names.data(), names.size()}, order_of, warnings);
        !r)
        return r;
    for (const Error& w : warnings) tell(io, w);
    for (const uint32_t i : order_of) found.push_back(std::move(named[i]));
    return {};
}

Expected<size_t> Config::Impl::relist(const ReadContext& io, Vec<LayerState>& ls, size_t at) const {
    Vec<Found> found(alloc);
    if (auto r = list_dir(io, ls[at], found); !r) return Unexpected(std::move(r).error());
    const LayerState& marker = ls[at];
    const size_t old = members(ls, at);
    Vec<LayerState> next(alloc);
    for (const Found& f : found) {
        // A name taken outside the directory, or twice in it (a.dta and a/), is left out.
        bool taken = f.name == "scratch" || f.name == marker.name;
        for (size_t i = 0; i < ls.size() && !taken; ++i)
            taken = (i <= at || i > at + old) && ls[i].name == f.name;
        for (const LayerState& n : next) taken = taken || n.name == f.name;
        if (taken) {
            tell(io, refused("'" + f.path + "' is not read: there is already a layer named '" +
                                 f.name + "'",
                             Symbol(f.path)));
            continue;
        }
        const LayerState* was = nullptr;
        for (size_t i = at + 1; i <= at + old; ++i)
            if (ls[i].name == f.name && ls[i].path == f.path) was = &ls[i];
        if (was) {
            next.push_back(*was);
            continue;
        }
        LayerState l(alloc);
        l.name = f.name;
        l.path = f.path;
        l.group = marker.name;
        l.options.enabled = marker.options.enabled;
        l.when = marker.when;  // the directory's condition is each layer's in it
        l.wanted = marker.wanted;
        next.push_back(std::move(l));
    }
    const auto first = ls.begin() + static_cast<ptrdiff_t>(at + 1);
    ls.erase(first, first + static_cast<ptrdiff_t>(old));
    ls.insert(ls.begin() + static_cast<ptrdiff_t>(at + 1), next.begin(), next.end());
    return next.size();
}

Expected<void> Config::Impl::reread(const ReadContext& io, Vec<LayerState>& ls, size_t from,
                                    uint64_t* read_ns, uint32_t* files) const {
    Vec<LayerState> fresh;
    for (size_t i = from; i < ls.size(); ++i) {
        LayerState l = ls[i];
        const auto before = fresh.empty() ? session_below(io, ls, from) : fresh.back().after;
        if (l.path.empty()) {
            l.after = before;
        } else {
            l.broken = false;
            if (auto r = read(io, l, *before, read_ns); !r) {
                if (l.group.empty()) return r;
                // A layer found in a directory that does not read is empty, and the rest
                // stand; it is read again when its files change.
                tell(io, r.error());
                l.root = Array::make(0, Symbol(l.file), 1);
                l.map = nullptr;
                l.after = before;
                l.broken = true;
            }
            if (files)
                for (const Dep& d : l.deps) *files += d.present ? 1u : 0u;
        }
        fresh.push_back(std::move(l));
    }
    for (size_t i = from; i < ls.size(); ++i) ls[i] = std::move(fresh[i - from]);
    return {};
}

namespace {

// Whether merging `e` as (p0 (p1 ... (last values...))) only sets the values at its path: no
// value is a key (a tagged array), nothing holds a verb or a pin, and no segment is
// metadata. Then the merges of scratch's entries, one after another, give a tree that
// one pass builds (ScratchTree).
bool sets_values_only(const ScratchEntry& e) noexcept {
    if (e.values->has_marker()) return false;  // sticky in set(): at worst, the fold
    for (size_t i = 1; i < e.values->size(); ++i)
        if (is_tagged((*e.values)[i])) return false;
    for (size_t i = 0; i < e.path.size(); ++i)
        if (e.path[i].name.is_meta()) return false;
    return true;
}

// Scratch's overlay as the merges of its entries in order give it, when every entry sets
// values only, in one pass. An array's children are its values, then its keys in
// the order their paths first appear in scratch; its values are those of the last entry for
// its path that has any, as Overwrite replaces values and an empty run changes nothing.
// The entries sorted by path group each array's; each key is placed by the first entry
// below it. The arrays are scratch's copies: set() changes an entry's own in place.
class ScratchTree {
public:
    ScratchTree(const Vec<ScratchEntry>& scratch, const Allocator& alloc)
        : s_(scratch), order_(alloc), keys_(alloc), nodes_(alloc) {}

    Ref<Array> build() {
        order_.resize(s_.size());
        for (uint32_t i = 0; i < order_.size(); ++i) order_[i] = i;
        std::sort(order_.begin(), order_.end(), [this](uint32_t a, uint32_t b) {
            const Path& x = s_[a].path;
            const Path& y = s_[b].path;
            for (size_t i = 0; i < x.size() && i < y.size(); ++i)
                if (x[i].name != y[i].name) return std::less<>()(x[i].name.id(), y[i].name.id());
            return x.size() != y.size() ? x.size() < y.size() : a < b;  // a parent first
        });
        return array_at(0, static_cast<uint32_t>(order_.size()), 0);
    }

private:
    struct Key {
        uint32_t lo, hi;  // its entries, in order_
        uint32_t first;   // the earliest of them in scratch
    };

    // The array at depth `depth` of the entries order_[lo, hi), which share their first
    // `depth` segments: the overlay itself at depth 0.
    Ref<Array> array_at(uint32_t lo, uint32_t hi, size_t depth) {
        const size_t at = nodes_.size();  // this array's nodes, past its parents'
        const ScratchEntry& any = s_[order_[lo]];
        if (depth > 0) nodes_.emplace_back(any.path[depth - 1].name);
        // The entries for this path itself sort first, in scratch's order. Paths are
        // distinct, but a later entry with values would win over an earlier one, as in the
        // fold.
        const ScratchEntry* own = nullptr;
        uint32_t i = lo;
        for (; i < hi && s_[order_[i]].path.size() == depth; ++i)
            if (!own || s_[order_[i]].values->size() > 1) own = &s_[order_[i]];
        if (own)
            for (size_t v = 1; v < own->values->size(); ++v) nodes_.push_back((*own->values)[v]);
        // The keys below, each a run of the entries sharing its segment.
        const size_t keys_at = keys_.size();
        while (i < hi) {
            const Symbol name = s_[order_[i]].path[depth].name;
            Key k{i, i, order_[i]};
            for (; k.hi < hi && s_[order_[k.hi]].path[depth].name == name; ++k.hi)
                k.first = std::min(k.first, order_[k.hi]);
            keys_.push_back(k);
            i = k.hi;
        }
        std::sort(keys_.begin() + static_cast<ptrdiff_t>(keys_at), keys_.end(),
                  [](const Key& a, const Key& b) { return a.first < b.first; });
        for (size_t k = keys_at; k < keys_.size(); ++k) {
            const Key key = keys_[k];  // by value: the call below grows keys_
            Ref<Array> child = array_at(key.lo, key.hi, depth + 1);
            nodes_.emplace_back(std::move(child));
        }
        keys_.resize(keys_at);
        Ref<Array> made = Array::make_from(std::span<Node>(nodes_.data() + at, nodes_.size() - at));
        nodes_.resize(at);
        return made;
    }

    const Vec<ScratchEntry>& s_;
    Vec<uint32_t> order_;
    Vec<Key> keys_;
    Vec<Node> nodes_;
};

}  // namespace

Ref<Array> Config::Impl::scratch_root() const {
    if (scratch.empty()) return nullptr;
    if (std::all_of(scratch.begin(), scratch.end(), sets_values_only))
        return ScratchTree(scratch, alloc).build();
    // A value that is a key, a verb or a pin merges as a layer's would: the entries merge
    // one at a time, quadratic in them; set() makes such an entry only for a tagged value.
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
    options.tweaks->begin_change();  // read_consistent waits until every tweak is set
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
        tweak_index.touched(*options.tweaks, t);  // the next commit binds it to the merge
        if (n < 16) {
            few[n++] = &t;
        } else {
            if (!more) more = Own<Vec<TweakBase*>>::make(default_allocator(MemoryKind::Tweaks));
            more->push_back(&t);
        }
    });
    options.tweaks->end_change();
    for (size_t i = 0; i < n; ++i) few[i]->notify();
    if (more)
        for (TweakBase* t : *more) t->notify();
}

void Config::Impl::put(Config& c, const Path& target, std::span<const Node> values) {
    ScratchEntry* found = nullptr;
    for (ScratchEntry& e : scratch)
        if (e.path == target) found = &e;
    // The journal: the old value, from scratch or else from the stack; the change
    // log keeps the whole of it.
    Node before[4];
    uint32_t before_count = 0;
    Source source{scratch_name()};
    std::optional<Vec<Node>> whole_before;  // made only with the log on: the fast path's cost
    if constexpr (!kShipping) {
        if (found) {
            const Values v = found->values->values();
            before_count = static_cast<uint32_t>(v.size());
            std::copy_n(v.begin(), std::min<uint32_t>(before_count, 4), before);
            if (logging()) whole_before.emplace(alloc).assign(v.begin(), v.end());
        } else if (journal_cap || logging()) {
            Vec<Node> now;
            if (value_nodes(*c.root_, target, now)) {
                before_count = static_cast<uint32_t>(now.size());
                std::copy_n(now.data(), std::min<uint32_t>(before_count, 4), before);
                if (logging()) whole_before.emplace(alloc) = now;
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
    if (logging()) {
        record_set(target,
                   whole_before ? std::span<const Node>(*whole_before) : std::span<const Node>(),
                   values, source, scratch_name());
        // The root holds the edit at once, so the next commit's walk starts from what the
        // stack read.
        c.apply_edits();
    }
}

Ref<Array> Config::Impl::merged() const {
    Ref<Array> r = Array::make(0);
    merge_scratch.aliases = false;
    for (const LayerState& l : layers)
        merge_scratch.aliases = merge_scratch.aliases || (l.options.enabled && l.aliases);
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
    ++commits;
    const Ref<Array> before = std::move(c.root_);
    c.root_ = merged();
    pending_merge = nullptr;  // the full merge has the edits
    merged_root = c.root_;
    c.scratch_dirty_ = false;
    refresh_read();
    if (!tables.empty() && !hold_tables) build_tables(*c.root_, false);
    if (logging() && before) record_commit(c, *before, *c.root_, rec.trigger);
    if (!validators.empty()) {
        const ValidatorStack stack{*c.root_, stack_layers(), as_read, pushed};
        for (const Check& v : validators)
            // A held report's check ran the any_thread ones over this stack.
            if (!v.options.checks_only && !(pushed_held && v.options.any_thread)) v.fn(stack, warn);
    }
    lap(rec.merge_ns, t);
    ++c.epoch_;
    Vec<TweakBase*> moved;
    if (options.tweaks) {
        options.tweaks->begin_change();  // read_consistent waits until every tweak is set
        tweak_index.rebind(*options.tweaks, c.root_, warn, moved);  // what changed
        options.tweaks->end_change();
    }
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

void Config::Impl::change_pushed(Config& c, const Array* tree, bool held) {
    pushed = tree;
    pushed_held = held;
    change(c, trigger_name("layer"));
    pushed = nullptr;
    pushed_held = false;
}

Expected<void> Config::Impl::may_change(const Config& c) const {
    if (c.frozen_ && !options.live_layers)
        return Unexpected(refused("the config is frozen; layers and values can no longer change"));
    return {};
}

// --- Construction ---

Config::Config(ConfigOptions options, const Allocator& allocator)
    : root_(Array::make(0)),
      impl_(detail::create<Impl>(with_kind(allocator, MemoryKind::Config), allocator)) {
    static std::atomic<uint64_t> configs{0};
    impl_->generation = configs.fetch_add(1, std::memory_order_relaxed) << 32;
    impl_->options = options;
    impl_->cache_dir.assign(options.cache_dir);
    impl_->options.cache_dir = impl_->cache_dir;
    impl_->stack_image.assign(options.stack_image);
    impl_->options.stack_image = impl_->stack_image;
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

const TweakRegistry* Config::tweak_registry() const noexcept {
    return impl_->options.tweaks;
}

void Config::set_writer(Writer writer) {
    impl_->writer = writer ? std::move(writer) : Writer(write_disk_file);
}

void Config::set_warning_handler(Callback<void(const Error&)> handler) {
    impl_->warn = std::move(handler);
}

std::span<const Ref<Array>> Config::Impl::stack_layers(const Ref<Array>& candidate,
                                                       size_t count) const {
    enabled_roots.clear();
    for (size_t i = 0; i < std::min(count, layers.size()); ++i)
        if (layers[i].options.enabled) enabled_roots.push_back(layers[i].root);
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
    t.registry_->begin_change();
    const bool moved = t.bind(*root_, impl_->warn);
    t.registry_->end_change();
    if (moved) t.notify();
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
    if (!layers[*at].group.empty())
        return Unexpected(refused("'" + Str(anchor) + "' is in the directory of layer '" +
                                  layers[*at].group + "': place a layer below or above '" +
                                  layers[*at].group + "'"));
    // Above a directory is above the layers found in it.
    return below ? *at : *at + 1 + members(layers, *at);
}

Expected<void> Config::Impl::admit(const Config& c, const LayerOptions& opts, std::string_view name,
                                   const Ref<Array>& tree, uint64_t subject, bool on_top,
                                   std::span<const Node> commands, const LoadSession& session,
                                   std::span<const Error> read_warnings, bool* held) const {
    const Holding holding = subject ? c.holds(opts, subject, on_top) : Holding::No;
    if (held) *held = holding != Holding::No;
    switch (holding) {
        case Holding::Yes: return refuse_unless(*opts.checked);  // its verdict
        case Holding::LeftOut: return refuse_unless(check_left_out(c, tree, session, *opts.policy));
        case Holding::No: break;
    }
    return refuse_unless(
        check_candidate(c, name, tree, commands, session, *opts.policy, read_warnings));
}

namespace {
// A layer's (when COND): a define, (not C), (and C C...) or (or C C...).
bool condition(const Node& c) noexcept {
    if (c.type() == Type::Symbol) return !c.try_sym()->empty();
    const Array* a = c.type() == Type::Array ? c.try_array() : nullptr;
    if (!a || a->size() < 2) return false;
    const Symbol op = a->tag();
    if (op != Symbol("not") && op != Symbol("and") && op != Symbol("or")) return false;
    if (op == Symbol("not") && a->size() != 2) return false;
    return std::all_of(a->begin() + 1, a->end(), condition);
}
bool condition_holds(const Node& c, const LoadSession& s) {
    if (const auto name = c.try_sym()) return s.is_defined(*name);
    const Array& a = *c.try_array();
    if (a.tag() == Symbol("not")) return !condition_holds(a[1], s);
    const bool all = a.tag() == Symbol("and");
    for (size_t i = 1; i < a.size(); ++i)
        if (condition_holds(a[i], s) != all) return !all;
    return all;
}
}  // namespace

Expected<void> Config::Impl::take_options(LayerState& layer, LayerOptions opts) const {
    if (!opts.when.empty()) {
        // Read unexpanded: a session of its own, which defines no macro.
        LoadSession plain(alloc);
        auto cond = read_string(opts.when, "when", plain);
        if (!cond || (*cond)->size() != 1 || !condition((**cond)[0]))
            return Unexpected(
                make_error(ErrorCode::Syntax, "layer '" + layer.name +
                                                  "': its condition is a define, "
                                                  "(not C), (and C C...) or (or C C...), not '" +
                                                  Str(opts.when) + "'"));
        layer.when = std::move(*cond);
    }
    layer.wanted = opts.enabled;
    opts.when = {};  // a view: read here, never kept
    layer.options = opts;
    apply_condition(layer);
    return {};
}

bool Config::Impl::apply_condition(LayerState& l) const {
    const bool on = l.wanted && (!l.when || condition_holds((*l.when)[0], base));
    const bool changed = on != l.options.enabled;
    l.options.enabled = on;
    return changed;
}

Expected<void> Config::Impl::push_read(Config& c, LayerState&& layer, LayerOptions opts) {
    if (auto ok = may_change(c); !ok) return ok;
    if (layer.name == "scratch" || find_layer(layer.name))
        return Unexpected(refused("there is already a layer named '" + layer.name + "'"));
    const auto at = placement(opts);
    if (!at) return Unexpected(at.error());
    if (auto r = take_options(layer, opts); !r) return r;
    const Rc<LoadSession> below = session_below(layers, *at);
    Vec<Error> read_warnings(alloc);
    if (auto r = read(layer, *below, nullptr, &read_warnings); !r) return r;
    // What a check would say of its verbs and pins, over the layers below it: one merge, only
    // for a layer that holds either. Not in shipping, where nobody reads them: the merge, and
    // the write-back code it reaches, stay out of a shipped game.
    if constexpr (!kShipping) {
        if (!opts.policy && warn && layer.root->has_marker()) {
            Ref<Array> stack = Array::make(0);
            for (size_t i = 0; i < *at; ++i)
                if (layers[i].options.enabled)
                    stack = detail::merge(*stack, *layers[i].root, MergePolicy::Overwrite,
                                          merge_scratch);
            Vec<Node> v(alloc);
            Vec<Error> found(alloc);
            for (const Leaf& leaf : leaves_of(*layer.root))
                row_warnings(*leaf.holder, leaf.path,
                             below_nodes(*stack, Path(leaf.path), *leaf.holder, v) ? &v : nullptr,
                             *stack, found);
            for (const Error& w : found) warn(w);
        }
    }
    bool held = false;
    if (opts.policy) {
        // A file has no report that holds: no check reads by path.
        const uint64_t subject = layer.text ? text_subject(layer.name, *layer.text) : 0;
        if (auto r = admit(c, opts, layer.name, layer.root, subject, *at == layers.size(),
                           layer.after->autorun(), *layer.after, read_warnings, &held);
            !r)
            return r;
    }
    // On top, what changed is the layer's own tree; placed lower, the layers above
    // read again, and may change anywhere.
    const Array* tree = layer.root.get();
    if (*at == layers.size()) {
        layers.push_back(std::move(layer));
    } else {
        // The layers above read with the macros below them, now this one's too.
        Vec<LayerState> placed = layers;
        placed.insert(placed.begin() + static_cast<ptrdiff_t>(*at), std::move(layer));
        if (auto r = reread(placed, *at + 1); !r) return r;
        layers = std::move(placed);
        tree = nullptr;
    }
    change_pushed(c, tree, held);
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

Expected<void> Config::set_layer_text(std::string_view name, std::string_view text,
                                      const LayerPolicy* policy) {
    if (auto ok = impl_->may_change(*this); !ok) return ok;
    if (name == "scratch") return Unexpected(refused("scratch is edited with set(), not as text"));
    const auto at = impl_->find_layer(name);
    if (!at)
        return Unexpected(make_error(ErrorCode::NotFound, "no layer named '" + Str(name) + "'"));
    if (!impl_->layers[*at].text)
        return Unexpected(
            refused("'" + Str(name) + "' is not a text layer; reload() reads a file layer again"));
    if (impl_->layers[*at].text->view() == text) return {};  // the same layer: nothing to do
    LayerState layer = impl_->layers[*at];
    layer.text = Rc<Str>::make(impl_->alloc, text, impl_->alloc);
    Vec<Error> read_warnings(impl_->alloc);
    if (auto r =
            impl_->read(layer, *impl_->session_below(impl_->layers, *at), nullptr, &read_warnings);
        !r)
        return r;
    // Checked against the layers below it, as a push on top was (and as one placed lower
    // was not), without the version it replaces.
    if (policy)
        if (auto r = refuse_unless(impl_->check_candidate(*this, name, layer.root,
                                                          layer.after->autorun(), *layer.after,
                                                          *policy, read_warnings, *at));
            !r)
            return r;
    // The layers above read with its macros: read them again with the new text's.
    Vec<LayerState> placed = impl_->layers;
    placed[*at] = std::move(layer);
    if (auto r = impl_->reread(placed, *at + 1); !r) return r;
    impl_->layers = std::move(placed);
    impl_->change(*this, trigger_name("layer"));
    return {};
}

Expected<void> Config::push_layer_dir(std::string_view name, std::string_view dir,
                                      LayerOptions options) {
    if (auto ok = impl_->may_change(*this); !ok) return ok;
    if (options.policy)
        return Unexpected(
            refused("a directory of layers is pushed without a policy; check each "
                    "layer's text with check_layer_text instead"));
    if (name == "scratch" || impl_->find_layer(name))
        return Unexpected(refused("there is already a layer named '" + Str(name) + "'"));
    const auto at = impl_->placement(options);
    if (!at) return Unexpected(at.error());
    LayerState marker(impl_->alloc);
    marker.name.assign(name);
    marker.dir.assign(dir);
    if (auto r = impl_->take_options(marker, options); !r) return r;
    marker.root = Array::make(0);  // it holds nothing
    Vec<LayerState> placed = impl_->layers;
    placed.insert(placed.begin() + static_cast<ptrdiff_t>(*at), std::move(marker));
    if (auto n = impl_->relist(placed, *at); !n) return Unexpected(std::move(n).error());
    // The layers above read with the macros of the ones found, as above a file layer.
    if (auto r = impl_->reread(placed, *at); !r) return r;
    impl_->layers = std::move(placed);
    impl_->change(*this, trigger_name("layer"));
    return {};
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
    bool held = false;
    if (options.policy)
        if (auto r = impl_->admit(*this, options, name, overlay, tree_subject(name, overlay.get()),
                                  *at == impl_->layers.size(), {}, impl_->as_read, {}, &held);
            !r)
            return r;
    LayerState layer(impl_->alloc);
    layer.name.assign(name);
    if (auto r = impl_->take_options(layer, options); !r) return r;
    // A tree was not parsed here, so nothing counted its aliases and IDs: one walk, with an
    // early exit, says whether the merge must resolve by them; an image's
    // section carries the count, so its load walks nothing.
    layer.aliases = !impl_->from_image && detail::has_identity(*overlay);
    layer.root = std::move(overlay);
    // An overlay adds no macros, so the layers above need no re-read.
    layer.after = impl_->session_below(impl_->layers, *at);
    const Array* tree = layer.root.get();
    impl_->layers.insert(impl_->layers.begin() + static_cast<ptrdiff_t>(*at), std::move(layer));
    impl_->change_pushed(*this, tree, held);
    return {};
}

Expected<void> Config::set_defines(std::span<const std::string_view> names) {
    if (auto ok = impl_->may_change(*this); !ok) return ok;
    LoadSession& base = impl_->base;
    auto has = [](const Vec<Symbol>& v, Symbol s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    };
    Vec<Symbol> was(impl_->alloc), now(impl_->alloc), changed(impl_->alloc);
    for (const ActiveDefine& d : base.active_defines())
        if (d.from == DefineFrom::Host) was.push_back(d.name);
    for (const std::string_view n : names)
        if (!has(now, Symbol(n))) now.emplace_back(n);
    for (const Symbol s : was)
        if (!has(now, s)) changed.push_back(s);
    for (const Symbol s : now)
        if (!has(was, s)) changed.push_back(s);
    if (changed.empty()) return {};
    const LoadSession kept = base;  // to put back if a layer does not read
    for (const Symbol s : was)
        if (!has(now, s)) base.undefine(s);
    for (const Symbol s : now)
        if (!has(was, s)) base.define(s, DefineFrom::Host);
    ++impl_->generation;
    // Each layer that touched a changed name, read again; every other keeps its tree, and
    // its session takes each changed name as the session below it now has it, since its own
    // read neither tested nor set one.
    Vec<LayerState> fresh = impl_->layers;
    for (size_t i = 0; i < fresh.size(); ++i) {
        LayerState& l = fresh[i];
        const Rc<LoadSession> before = impl_->session_below(fresh, i);
        if (l.path.empty()) {  // an overlay reads nothing: its session is the one below
            l.after = before;
            continue;
        }
        const bool touched =
            !l.tested_known || std::any_of(l.tested.begin(), l.tested.end(),
                                           [&](Symbol s) { return has(changed, s); });
        if (touched) {
            if (auto r = impl_->read(l, *before); !r) {
                base = kept;
                return r;
            }
            continue;
        }
        auto patched = Rc<LoadSession>::make(impl_->alloc, *l.after);
        for (const Symbol s : changed) {
            const auto stands =
                std::find_if(before->active_defines().begin(), before->active_defines().end(),
                             [&](const ActiveDefine& d) { return d.name == s; });
            if (stands == before->active_defines().end())
                patched->undefine(s);
            else
                patched->define(s, stands->from);
        }
        l.after = patched;
    }
    for (LayerState& l : fresh) impl_->apply_condition(l);  // conditions see the new defines
    impl_->layers = std::move(fresh);
    impl_->change(*this, trigger_name("defines"));
    return {};
}

Expected<void> Config::remove_layer(std::string_view name) {
    if (auto ok = impl_->may_change(*this); !ok) return ok;
    if (name == "scratch")
        return Unexpected(refused("scratch cannot be removed; clear it instead"));
    const auto at = impl_->find_layer(name);
    if (!at)
        return Unexpected(make_error(ErrorCode::NotFound, "no layer named '" + Str(name) + "'"));
    if (!impl_->layers[*at].group.empty())
        return Unexpected(refused("'" + Str(name) + "' is in the directory of layer '" +
                                  impl_->layers[*at].group + "': remove its file, or disable it"));
    // The layers above were read with this one's macros; read them again without. A
    // directory goes with the layers found in it.
    Vec<LayerState> rest = impl_->layers;
    const auto first = rest.begin() + static_cast<ptrdiff_t>(*at);
    rest.erase(first, first + 1 + static_cast<ptrdiff_t>(Impl::members(rest, *at)));
    if (auto r = impl_->reread(rest, *at); !r) return r;
    impl_->layers = std::move(rest);
    impl_->change(*this, trigger_name("layer"));
    return {};
}

Expected<void> Config::pop_layer() {
    if (auto ok = impl_->may_change(*this); !ok) return ok;
    auto& ls = impl_->layers;
    if (ls.empty()) return Unexpected(make_error(ErrorCode::NotFound, "no layer to pop"));
    // A directory's layers go with it.
    const auto group = ls.back().group.empty() ? std::nullopt : impl_->find_layer(ls.back().group);
    ls.erase(ls.begin() + static_cast<ptrdiff_t>(group ? *group : ls.size() - 1), ls.end());
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
        if (l.path.empty() || l.broken || !l.options.enabled || !l.after) continue;
        for (const Node& n : l.after->autorun()) commands.push_back(n);
    }
    return List<Node>(commands);
}

Expected<void> Config::set_layer_enabled(std::string_view name, bool enabled) {
    if constexpr (kShipping) return Unexpected(shipping("toggling a layer"));
    if (auto ok = impl_->may_change(*this); !ok) return ok;
    bool changed = false;
    if (name == "scratch") {
        changed = impl_->scratch_enabled != enabled;
        impl_->scratch_enabled = enabled;
    } else if (auto at = impl_->find_layer(name)) {
        // A directory, and every layer found in it; each still under its condition.
        for (size_t i = *at; i <= *at + Impl::members(impl_->layers, *at); ++i) {
            impl_->layers[i].wanted = enabled;
            changed = impl_->apply_condition(impl_->layers[i]) || changed;
        }
    } else {
        return Unexpected(make_error(ErrorCode::NotFound, "no layer named '" + Str(name) + "'"));
    }
    if (!changed) return {};
    impl_->change(*this, trigger_name("layer"));
    return {};
}

Expected<bool> Config::toggle_layer(std::string_view name) {
    const bool next = !layer_enabled(name);
    if (auto r = set_layer_enabled(name, next); !r) return Unexpected(std::move(r).error());
    return layer_enabled(name);  // a condition that does not hold keeps it out
}

bool Config::layer_enabled(std::string_view name) const {
    if (name == "scratch") return impl_->scratch_enabled;
    const auto at = impl_->find_layer(name);
    return at && impl_->layers[*at].options.enabled;
}

std::optional<Node> Config::layer_condition(std::string_view name) const {
    const auto at = impl_->find_layer(name);
    if (!at || !impl_->layers[*at].when) return std::nullopt;
    return (*impl_->layers[*at].when)[0];
}

uint64_t Config::digest() const {
    // A live edit is in scratch until the next full merge: merge afresh then.
    return baker::digest(scratch_dirty_ ? *with_edits() : *root_);
}

uint64_t Config::stack_digest() const {
    uint64_t d = 0;
    auto layer = [&d](std::string_view name, const Array& tree) {
        const Node n{Symbol(name)};
        d = baker::digest(tree, baker::digest(*Array::make_copy(std::span<const Node>(&n, 1)), d));
    };
    for (const LayerState& l : impl_->layers)
        if (l.options.enabled) layer(l.name.view(), *l.root);
    if (impl_->scratch_enabled)
        if (const Ref<Array> s = impl_->scratch_root()) layer("scratch", *s);
    return d;
}

Ref<Array> Config::layer_root(std::string_view name) const {
    const auto at = impl_->find_layer(name);
    return at ? impl_->layers[*at].root : nullptr;
}

void Config::set_layout(const WriteOptions& layout) noexcept {
    impl_->layout = layout;
}
const WriteOptions& Config::layout() const noexcept {
    return impl_->layout;
}

Expected<void> Config::layer_text(std::string_view name, TextSink out) const {
    Ref<Array> tree;
    if (name == "scratch")
        tree = impl_->scratch_root();
    else if (const auto at = impl_->find_layer(name))
        tree = impl_->layers[*at].root;
    else
        return Unexpected(make_error(ErrorCode::NotFound, "no layer named '" + Str(name) + "'"));
    if (tree) write_dta(*tree, out, impl_->layout);  // empty scratch is an empty layer
    return {};
}

// --- Reading ---

bool Config::edits_beneath(const Path& path) const noexcept {
    return impl_->edits_beneath(path);
}

Ref<Array> Config::with_edits() const {
    Impl& im = *impl_;
    if (!im.pending_merge || im.pending_epoch != epoch_) {
        im.pending_merge = im.merged();
        im.pending_epoch = epoch_;
    }
    return im.pending_merge;
}

void Config::apply_edits() {
    if (!scratch_dirty_) return;
    root_ = with_edits();  // the reads' merge, if they made one since the last edit
    impl_->pending_merge = nullptr;
    impl_->merged_root = root_;
    scratch_dirty_ = false;
    if (!impl_->tables.empty() && !impl_->hold_tables) impl_->build_tables(*root_, false);
}

std::string_view Config::stack_image() const noexcept {
    return impl_->stack_image.view();
}

Expected<uint32_t> Config::push_image_layers(std::span<const ProjectLayer> layers) {
    auto image = detail::read_image(impl_->stack_image.view(), impl_->base, impl_->alloc);
    if (!image) return Unexpected(std::move(image).error());
    const auto section_named = [&](detail::ImageSection::Kind kind,
                                   std::string_view name) -> const detail::ImageSection* {
        for (const detail::ImageSection& s : image->sections)
            if (s.kind == kind && s.name == name) return &s;
        return nullptr;
    };
    // The leading layers the image holds, pushed from it.
    impl_->hold_tables = true;
    impl_->from_image = true;
    uint32_t taken = 0;
    for (const ProjectLayer& l : layers) {
        const detail::ImageSection* s =
            section_named(detail::ImageSection::Kind::Layer, l.name.str());
        if (!s) break;
        auto tree = detail::read_image_layer(s->bytes, l.name.str(), impl_->base);
        if (!tree) {
            impl_->hold_tables = false;
            return Unexpected(std::move(tree).error());
        }
        const LayerOptions o{.optional = l.optional, .enabled = l.enabled, .when = l.when.str()};
        if (auto r = push_layer(l.name.str(), std::move(*tree), o); !r) {
            impl_->hold_tables = false;
            impl_->from_image = false;
            return Unexpected(std::move(r).error());
        }
        // The section's record says whether the layer carries aliases or IDs:
        // the merge resolves by them only when some layer does.
        if (const auto li = impl_->find_layer(l.name.str()))
            impl_->layers[*li].aliases = detail::SessionAccess::aliases(impl_->base) > 0;
        ++taken;
    }
    impl_->hold_tables = false;
    impl_->from_image = false;
    // The tables: the image's block where it matches the registered view, else built.
    for (detail::TableSlot& slot : impl_->tables) {
        Ref<const BakedTable> block;
        Error why;
        for (const detail::ImageSection& s : image->sections) {
            if (s.kind != detail::ImageSection::Kind::Table) continue;
            block = detail::read_table(s.bytes, slot.spec, impl_->alloc, why);
            if (block) break;
        }
        if (block) {
            slot.built = block;
            slot.errors.clear();
            detail::record_table_sources(slot, *root_, impl_->alloc);
        } else {
            if (impl_->warn)
                impl_->warn(make_error(
                    ErrorCode::Refused,
                    "the stack image has no table for " +
                        Str(slot.spec.name.empty() ? std::string_view("a view") : slot.spec.name) +
                        " as this build compiles it (" + Str(why.message()) +
                        "); it is built from the tree"));
            slot.built = detail::build_table(slot, *root_, impl_->alloc, impl_->warn);
        }
    }
    return taken;
}

namespace detail {
std::span<const TableSlot> table_slots(const Config& cfg) {
    return {cfg.impl_->tables.data(), cfg.impl_->tables.size()};
}
}  // namespace detail

// --- Baked tables ---

void Config::Impl::build_tables(const Array& root, bool all) {
    for (detail::TableSlot& slot : tables) {
        if (!all && slot.built && detail::table_sources_unchanged(slot, root, alloc)) continue;
        slot.built = detail::build_table(slot, root, alloc, warn);
    }
}

Expected<void> Config::add_table(const TableSpec& spec) {
    for (const detail::TableSlot& s : impl_->tables)
        if (s.spec.digest == spec.digest)
            return Unexpected(refused("a table of shape " +
                                      Str(spec.name.empty() ? "?" : spec.name) +
                                      " is registered already"));
    detail::TableSlot slot(impl_->alloc);
    slot.spec = spec;
    if (auto r = detail::parse_table_at(spec, slot); !r) return r;
    impl_->tables.push_back(std::move(slot));
    impl_->tables.back().built =
        detail::build_table(impl_->tables.back(), *root_, impl_->alloc, impl_->warn);
    return {};
}

List<Error> Config::table_errors() const {
    Vec<Error> all;
    for (const detail::TableSlot& s : impl_->tables)
        for (const Error& e : s.errors) all.push_back(e);
    return List<Error>({all.data(), all.size()});
}

Ref<const BakedTable> Config::table_block(uint64_t digest) const noexcept {
    for (const detail::TableSlot& s : impl_->tables)
        if (s.spec.digest == digest) return s.built;
    return nullptr;
}

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
                detail::resolve_in_layer(*impl_->layers[i].root, path, impl_->alias_root(), r) ==
                    detail::ResolveStatus::Ok)
                at = i;
        }
    }
    if (!at) return Unexpected(detail::get_error(*root_, path, nullptr));
    return impl_->source_in(impl_->layers[*at], path);
}

// --- Reload and telemetry ---

// --- Reload in parts ---

struct ReloadJob::State {
    State(const Config::Impl& im, const ReloadRecord& r)
        : impl(&im),
          layers(im.layers),
          base(im.base),
          scan(im.alloc),
          listed(im.alloc),
          warnings(im.alloc),
          rec(r),
          commits(im.commits) {}
    const Config::Impl* impl;
    Vec<LayerState> layers;  // the stack at begin_reload, its trees shared; re-read in place
    LoadSession base;        // the session at begin_reload
    Str scan;
    Vec<Str> listed;
    Vec<Error> warnings;  // given to the warning handler at finish_reload
    ReloadRecord rec;
    uint64_t commits;  // the Config's at begin_reload
    uint64_t t = 0;    // the end of the last stage run
    bool ran = false;
    bool any = false;  // a layer's files changed
    std::optional<Error> error;
};

ReloadJob& ReloadJob::operator=(ReloadJob&& other) noexcept {
    if (this != &other) {
        ReloadJob dropped(std::move(*this));
        state_ = other.state_;
        other.state_ = nullptr;
    }
    return *this;
}

ReloadJob::~ReloadJob() {
    if (state_) detail::destroy(state_->impl->alloc, state_);
}

bool ReloadJob::ran() const noexcept {
    return state_ && state_->ran;
}

void ReloadJob::run() {
    if (!state_ || state_->ran) return;
    State& j = *state_;
    const Config::Impl& im = *j.impl;
    j.ran = true;
    const ReadContext io{j.base, j.scan, j.listed, &j.warnings};
    Vec<LayerState>& ls = j.layers;
    uint64_t& t = j.t;
    t = j.rec.start_ns;
    size_t from = ls.size();
    for (size_t i = 0; i < ls.size() && from == ls.size(); ++i)
        if (im.stale(io, ls[i], j.rec.first_file)) from = i;
    // A directory at or above the change is listed again if it changed.
    for (size_t i = from; i < ls.size(); ++i) {
        Symbol unused;
        if (ls[i].dir.empty() || (i != from && !im.stale(io, ls[i], unused))) continue;
        if (auto n = im.relist(io, ls, i); !n) {
            im.lap(j.rec.scan_ns, t);
            j.rec.layer = Symbol(ls[i].name);
            j.rec.outcome = ReloadOutcome::ParseError;
            j.error = std::move(n).error();
            return;
        }
    }
    im.lap(j.rec.scan_ns, t);
    if (from == ls.size()) return;  // nothing changed
    j.any = true;
    j.rec.layer = Symbol(ls[from].name);
    if (auto r = im.reread_stages(io, j.rec, ls, from, t); !r) j.error = std::move(r).error();
}

ReloadJob Config::begin_reload(const ReloadRequest& request) {
    ReloadJob job;
    if constexpr (kShipping) return job;
    const ReloadRecord rec = impl_->begin(
        request.trigger.empty() ? trigger_name("reload") : request.trigger, request.origin_ns);
    job.state_ = detail::create<ReloadJob::State>(impl_->alloc, *impl_, rec);
    return job;
}

Expected<ReloadOutcome> Config::finish_reload(ReloadJob job) {
    if constexpr (kShipping) return ReloadOutcome::Refused;
    if (job.empty()) return ReloadOutcome::Refused;
    ReloadJob::State& j = *job.state_;
    Impl& im = *impl_;
    // Another Config's job, or one begun before a change to the stack: its reads are of
    // a stack that is gone.
    if (j.impl != impl_ || j.commits != im.commits) {
        j.rec.outcome = ReloadOutcome::Refused;
        if (j.impl == impl_) im.feed(*this, j.rec, im.finish(*this, j.rec, im.now()));
        return ReloadOutcome::Refused;
    }
    job.run();
    for (const Error& w : j.warnings)
        if (im.warn) im.warn(w);
    uint64_t t = j.t;
    if (frozen_ && !im.options.live_layers) {
        j.rec.outcome = ReloadOutcome::Refused;
        im.feed(*this, j.rec, im.finish(*this, j.rec, t));
        return ReloadOutcome::Refused;
    }
    if (j.error) {
        im.feed(*this, j.rec, im.finish(*this, j.rec, t));
        return Unexpected(std::move(*j.error));
    }
    if (!j.any) {
        j.rec.outcome = ReloadOutcome::Unchanged;
        im.feed(*this, j.rec, im.finish(*this, j.rec, t));
        return ReloadOutcome::Unchanged;
    }
    im.layers = std::move(j.layers);
    im.commit(*this, j.rec, t);
    im.feed(*this, j.rec, im.finish(*this, j.rec, t));
    return ReloadOutcome::Ok;
}

// In place, on this thread: a reload that finds nothing changed allocates nothing, where a
// ReloadJob copies the stack it reads against.
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
        if (impl_->stale(ls[i], rec.first_file)) from = i;
    // A directory at or above the change is listed again if it changed, into a copy of the
    // stack that replaces it only if every layer reads.
    // Made only then: even an empty vector allocates in MSVC's debug library.
    std::optional<Vec<LayerState>> listed;
    for (size_t i = from; i < ls.size(); ++i) {
        Symbol unused;
        if (ls[i].dir.empty() || (i != from && !impl_->stale(ls[i], unused))) continue;
        if (!listed) listed.emplace(ls);
        const size_t at = i + listed->size() - ls.size();  // where it is in the copy
        if (auto n = impl_->relist(*listed, at); !n) {
            impl_->lap(rec.scan_ns, t);
            rec.layer = Symbol(ls[i].name);
            rec.outcome = ReloadOutcome::ParseError;
            impl_->feed(*this, rec, impl_->finish(*this, rec, t));
            return Unexpected(std::move(n).error());
        }
    }
    impl_->lap(rec.scan_ns, t);
    if (from == ls.size()) {
        rec.outcome = ReloadOutcome::Unchanged;
        impl_->feed(*this, rec, impl_->finish(*this, rec, t));
        return false;
    }
    rec.layer = Symbol(ls[from].name);
    Vec<LayerState>& next = listed ? *listed : ls;
    if (auto r = impl_->reread_stages(rec, next, from, t); !r) {
        impl_->feed(*this, rec, impl_->finish(*this, rec, t));
        return Unexpected(std::move(r).error());
    }
    if (listed) ls = std::move(*listed);
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
            if (impl_->logging()) {  // one test with the log off
                impl_->record_set(e.path, std::span<const Node>(&before, 1),
                                  std::span<const Node>(&value, 1), Source{scratch_name()},
                                  scratch_name());
                apply_edits();
            }
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
    // The log: the commit's walk records each dropped edit's value giving way
    // to the files'; the old values were scratch's own.
    const bool own = impl_->logging() && impl_->changes.open == 0;
    if (own) impl_->begin_change(Symbol("clear_scratch"));
    const size_t first = impl_->changes.pending.size();
    if (path.empty())
        s.clear();
    else
        s.erase(std::remove_if(s.begin(), s.end(),
                               [&](const ScratchEntry& e) { return e.path.matches(path); }),
                s.end());
    impl_->change(*this, trigger_name("layer"));
    for (size_t i = first; i < impl_->changes.pending.size(); ++i)
        impl_->changes.pending[i].source = Source{scratch_name()};
    if (own) impl_->close_change();
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

ConfigSnapshot Config::Impl::snapshot() const {
    auto* state = detail::create<ConfigSnapshot::State>(alloc, alloc);
    state->layers = layers;
    state->scratch = copy_scratch(scratch);
    state->scratch_enabled = scratch_enabled;
    ConfigSnapshot s;
    s.state_ = state;
    return s;
}

ConfigSnapshot Config::snapshot() const {
    return impl_->snapshot();
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