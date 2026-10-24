#pragma once

// Config's internals, shared by its files: config.cpp (layers, reading, reload,
// merging, scratch, snapshots), config_loop.cpp (the reload history and loop statistics)
// and config_write.cpp (write-back, promote, and the layer tools).

#include <baker/cache.hpp>
#include <baker/config.hpp>
#include <baker/write.hpp>
#include "common/containers.hpp"
#include "merge.hpp"
#include "session.hpp"
#include "text.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace baker {
// Named, not anonymous, as three files share them; kept out of baker's own names.
namespace config_internal {

inline const Symbol& scratch_name() {
    static const Symbol s("scratch");
    return s;
}

// A file a layer was built from (or looked for), as it was when the layer was read.
struct Dep {
    Str path;
    bool present = false;
    uint64_t hash = 0;  // of its text; for a directory, of its listing
    bool directory = false;
};

struct LayerState {
    LayerState() = default;
    // A record the Config makes: its own containers from its allocator. A copy keeps
    // its source's.
    explicit LayerState(const Allocator& a)
        : name(a), path(a), file(a), deps(a), tested(a), dir(a), group(a) {}

    Str name;
    Str path;      // as given; empty for an overlay; the name, for text
    Str file;      // the main file, normalized; empty for an overlay
    Rc<Str> text;  // pushed as text (push_layer_text): read from this, never a file; shared
    LayerOptions options;
    Ref<Array> root;
    Rc<SourceMap> map;  // edit mode; shared with snapshots, read-only once built
    Vec<Dep> deps;
    Rc<LoadSession> after;  // macros and defines after this layer; read-only
    bool missing = false;   // optional, and its file is absent (a directory: not listed)
    // The names its read's #ifdef, #ifndef, #define and #undef named: set_defines reads it
    // again only if one changed. Unknown when a cache replayed it, which parses nothing.
    Vec<Symbol> tested;
    bool tested_known = false;
    // A directory of layers (push_layer_dir): its own layer, which holds nothing, is `dir`'s
    // marker, and the layers found in it stand directly above, each with `group` its name.
    // The marker's deps are the directory itself, its order.dta, and the layer.dta files its
    // subdirectories lacked.
    Str dir;
    Str group;
    bool broken = false;  // a found layer that did not read: empty until its files change
};

// One path in scratch. The array is owned by scratch alone, so set() may change it in
// place without allocating.
constexpr uint64_t kNoEdit = UINT64_MAX;

struct ScratchEntry {
    Symbol key;               // the path, '/'-separated, interned once: the journal's name for it
    Path path;                // names only
    Ref<Array> values;        // (last_name value...)
    uint64_t edit = kNoEdit;  // its latest journal entry, by sequence
};

// A planned write: the new text of one file.
struct Plan {
    Symbol file;
    Str text;
    bool changed = false;
};

inline Symbol trigger_name(const char* name) {
    return Symbol(name);
}

inline Error refused(std::string_view message, Symbol file = {}, int32_t line = 0) {
    return make_error(ErrorCode::Refused, message, file, line);
}

[[maybe_unused]] inline Error shipping(std::string_view what) {
    return refused(Str(what) + " is disabled in a shipping build");
}

inline bool is_tagged(const Node& n) noexcept {
    return !detail::tag_of(n).empty();
}

inline bool has_values(const Array& a) noexcept {
    for (size_t i = 1; i < a.size(); ++i)
        if (!is_tagged(a[i])) return true;
    return false;
}

inline bool last_is_index(const Path& path) noexcept {
    return !path.empty() && path[path.size() - 1].is_index;
}

// Whether `path` is `base` plus one index segment.
inline bool is_element_of(const Path& path, const Path& base) noexcept {
    if (path.size() != base.size() + 1 || !last_is_index(path)) return false;
    for (size_t i = 0; i < base.size(); ++i)
        if (!(path[i] == base[i])) return false;
    return true;
}

bool write_disk_file(std::string_view where, std::string_view text);

// One node for a value: itself, or (v1 v2 ...) for several.
Node pack(const Vec<Node>& values);

Str quoted(const Path& path);

Path parent_of(const Path& path);

// The nodes a path's value consists of: the values of the array it names, or the one node
// an index selects. False if the path does not resolve.
bool value_nodes(const Array& root, const Path& path, Vec<Node>& out);

bool same_nodes(std::span<const Node> a, std::span<const Node> b);

// A tagged array with values in some tree, found by walking tags (first occurrence of
// each tag, as find and merge use).
struct Leaf {
    Str path;  // dotted
    const Array* holder;
    Vec<Node> values;
};

// What a check was made against (CheckStamp): a layer's name with its text, or with its
// tree's address; and a policy's rules.
[[nodiscard]] uint64_t text_subject(std::string_view name, std::string_view text) noexcept;
[[nodiscard]] uint64_t tree_subject(std::string_view name, const Array* tree) noexcept;
[[nodiscard]] uint64_t policy_hash(const LayerPolicy& policy) noexcept;

// A tag array's values: its children that are not tagged arrays.
Vec<Node> values_of(const Array& a);
// Each tag's first child only, as lookup sees it, depth first.
Vec<Leaf> leaves_of(const Array& root);

}  // namespace config_internal
using namespace config_internal;

struct ConfigSnapshot::State {
    explicit State(const Allocator& a) : alloc(a) {}
    const Allocator alloc;  // the Config's, which outlives its snapshots
    Vec<LayerState> layers{alloc};
    Vec<ScratchEntry> scratch{alloc};
    bool scratch_enabled = true;
    mutable std::atomic<uint32_t> refs{1};
};

// Loop statistics' samples: a fixed table, allocated by the first reload
// that changes a file. A name gets the row it had, or a free one, or the least recently
// fed one in its half.
struct LoopTable {
    static constexpr uint32_t kFiles = 32, kLayers = 16, kWindow = 32;
    struct Row {
        Symbol name;
        uint64_t used = 0;        // when last fed, by `clock`; 0: free
        uint32_t generation = 0;  // incremented when the row changes hands
        uint32_t reloads = 0, failures = 0;
        uint32_t edits = 0, times = 0;  // samples pushed, ever; the window keeps the last 32
        uint32_t edit_us[kWindow] = {};
        uint32_t time_us[kWindow] = {};
    };
    Row rows[kFiles + kLayers];
    uint64_t clock = 0;

    uint32_t row(Symbol name, bool layer) {
        const uint32_t first = layer ? kFiles : 0, end = first + (layer ? kLayers : kFiles);
        uint32_t pick = first;
        for (uint32_t i = first; i < end; ++i) {
            if (rows[i].used && rows[i].name == name) {
                rows[i].used = ++clock;
                return i;
            }
            if (rows[i].used < rows[pick].used) pick = i;
        }
        const uint32_t generation = rows[pick].generation + 1;
        rows[pick] = Row{};
        rows[pick].name = name;
        rows[pick].generation = generation;
        rows[pick].used = ++clock;
        return pick;
    }
    [[nodiscard]] static uint32_t micros(uint64_t ns) noexcept {
        const uint64_t us = (ns + 500) / 1000;
        return us > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(us);
    }
};

struct Config::Impl {
    struct Subscriber {
        ReloadId id;
        Path group;
        Callback<void()> rebuild;
        Callback<Reload()> migrate;
    };
    struct Completion {
        ReloadId id;
        Callback<void(const ReloadRecord&)> fn;
    };

    explicit Impl(const Allocator& a) : alloc(a) {
        base.allocator = &alloc;  // every layer's tree is the Config's
    }

    const Allocator alloc;  // first: the containers below take it
    ConfigOptions options;
    Str cache_dir{alloc};            // options.cache_dir views this
    mutable Str scan{alloc};         // changed()'s buffer, kept so that a poll allocates nothing
    mutable Vec<Str> listed{alloc};  // the same for a directory's listing
    // Every merge's temporaries, reused: a merge allocates only the arrays it keeps.
    mutable detail::MergeScratch merge_scratch{alloc};
    LoadSession base{alloc};  // the sessions the Config keeps are on its allocator
    LoadSession as_read{
        alloc};  // read_session(): the last file layer's session, as of the last merge
    Rc<LoadSession> as_read_from;       // the session as_read copies; held, so it is not reused
    Vec<StackFile> stack_files{alloc};  // stack_files(), as of the last merge
    uint64_t stack_files_epoch = 0;
    Writer writer = write_disk_file;
    Callback<void(const Error&)> warn;
    struct Check {
        ValidatorId id;
        Validator fn;
        ValidatorOptions options;
    };
    Vec<Check> validators{alloc};
    // The enabled layers' trees, for the validators' stack (ValidatorStack::layers): refilled
    // at each merge, its capacity kept.
    mutable Vec<Ref<Array>> enabled_roots{alloc};
    Vec<LayerState> layers{alloc};
    Vec<ScratchEntry> scratch{alloc};
    bool scratch_enabled = true;
    Vec<Subscriber> subscribers{alloc};
    Vec<Completion> completions{alloc};
    ReloadId next_id = 1;
    // Incremented on every change that can change a check's verdict (CheckStamp): a merge, a
    // validator added or removed, a set(), a call of session(). Each Config starts its own
    // range, so a report never holds for another Config at the same address.
    uint64_t generation = 0;

    TimeSource clock = steady_now;
    void* clock_user = nullptr;
    Vec<ReloadRecord> ring{alloc};  // reserved once, at its capacity
    size_t ring_cap = 256;
    size_t ring_next = 0;

    // Loop statistics. The reloads that wait for mark_visible: at most kPending; more
    // between two frames keep the edit-to-see their reload's end gave them.
    struct Pending {
        size_t slot;  // in the ring; SIZE_MAX: none
        uint64_t start_ns, epoch, origin_ns;
        uint32_t rows[2];  // file, layer; UINT32_MAX: none
        uint32_t generation[2];
        uint32_t seq[2];  // the edit-to-see sample each row was given; UINT32_MAX: none
    };
    static constexpr uint32_t kPending = 8;
    Own<LoopTable> loop;
    Pending pending[kPending] = {};
    uint32_t pending_n = 0;
    uint32_t loop_reloads = 0, loop_unchanged = 0, loop_failures = 0;

    // The edit journal: a ring, reserved by the first edit, written in place after.
    // Entry n of the session is in slot n % journal_cap while the ring holds it. A scratch
    // entry knows its latest entry, so giving one its fate never searches the ring.
    Vec<JournalEntry> journal{alloc};
    size_t journal_cap = 256;
    uint64_t journal_count = 0;  // entries ever made

    [[nodiscard]] JournalEntry* journal_entry(uint64_t n, Symbol key) noexcept {
        if (n == kNoEdit || n >= journal_count || journal_count - n > journal.size())
            return nullptr;
        JournalEntry& e = journal[n % journal_cap];
        return e.path == key ? &e : nullptr;
    }
    // Records a live change of `owner`'s value, from `before` (its first four nodes of
    // `before_count`, sourced at `source`) to `after`; its previous entry, if still unsaved,
    // is superseded. Allocates only the ring, once.
    void log_edit(ScratchEntry& owner, const Node* before, uint32_t before_count,
                  std::span<const Node> after, const Source& source);
    // Gives `owner`'s latest entry its fate, if it is still unsaved.
    void settle(const ScratchEntry& owner, EditFate fate, WrittenLine written) noexcept {
        if (JournalEntry* e = journal_entry(owner.edit, owner.key);
            e && e->fate == EditFate::Unsaved) {
            e->fate = fate;
            e->written = written;
        }
    }

    // --- Telemetry ---

    [[nodiscard]] uint64_t now() const {
        if constexpr (kShipping)
            return 0;
        else
            return clock(clock_user);
    }
    [[nodiscard]] ReloadRecord begin(Symbol trigger, uint64_t origin = 0) const {
        ReloadRecord r;
        r.trigger = trigger;
        r.origin_ns = origin;
        r.start_ns = now();
        return r;
    }
    void lap(uint64_t& field, uint64_t& t) const {
        const uint64_t n = now();
        field += n - t;
        t = n;
    }
    // Ends the record at `end` (the end of its last stage, so the stages tile the whole
    // change), keeps it in the history, and hands it to the completion callbacks; returns
    // its slot in the history (SIZE_MAX: none). No allocation once the ring is reserved.
    size_t finish(const Config& c, ReloadRecord& r, uint64_t end);

    // Feeds a reload's record to the loop statistics.
    void feed(Config& c, const ReloadRecord& r, size_t slot);

    // --- Reading layers ---

    [[nodiscard]] Rc<LoadSession> session_below(const Vec<LayerState>& ls, size_t i) const {
        return i == 0 ? Rc<LoadSession>::make(alloc, base) : ls[i - 1].after;
    }

    // A fresh cache of `path`, or the text and a new cache, written through the writer: a
    // cache that cannot be written costs the next read a parse, not this one its tree.
    // `from_disk`: the Config reads through the disk's reader, so the cache file is read
    // straight into its arena rather than copied through the session's reader.
    Expected<Ref<Array>> read_through_cache(std::string_view path, const Str& cache,
                                            LoadSession& session, bool from_disk) const;

    // Reads a file layer, recording every file it reads (or looks for) for reload, and the
    // time spent reading them into `read_ns`; or a layer pushed as text, from its text only.
    // The read's warnings go to the warning handler, and to `warnings` if given.
    Expected<void> read(LayerState& layer, const LoadSession& before, uint64_t* read_ns = nullptr,
                        Vec<Error>* warnings = nullptr) const;
    // A layer pushed under a policy: the verdict of the report it brings if that still holds
    // (`subject` names it; 0 for a file), with the validators a LayerChecker left out run
    // now; else of a check.
    [[nodiscard]] Expected<void> admit(const Config& c, const LayerOptions& opts,
                                       std::string_view name, const Ref<Array>& tree,
                                       uint64_t subject, bool on_top,
                                       std::span<const Node> commands, const LoadSession& session,
                                       std::span<const Error> read_warnings) const;
    // push_layer by path or text, once the layer is named.
    Expected<void> push_read(Config& c, LayerState&& layer, LayerOptions opts);

    bool changed(const LayerState& layer, Symbol& first) const;
    // Whether reload() must read `layer` again: a file of it changed, or, for a directory of
    // layers, its listing or order.dta.
    bool stale(const LayerState& layer, Symbol& first) const;

    // --- Directories of layers ---

    // A layer the directory holds: its name, and the file it is read from.
    struct Found {
        Str name;
        Str path;
    };
    // Lists the directory of `marker`, filling its deps, and `found` with its
    // layers in order.dta's order. Warns of a name order.dta lists that the directory lacks.
    [[nodiscard]] Expected<void> list_dir(LayerState& marker, Vec<Found>& found) const;
    // Lists the directory of the marker at `ls[at]` again and puts the layers it holds above
    // it, in place of those that were: one that stays keeps its state (its enabled flag).
    // Returns how many layers the group now has above the marker.
    [[nodiscard]] Expected<size_t> relist(Vec<LayerState>& ls, size_t at) const;
    // How many layers of `ls[at]`'s directory stand above it (0 for a layer that is not one).
    [[nodiscard]] static size_t members(const Vec<LayerState>& ls, size_t at) noexcept {
        if (ls[at].dir.empty()) return 0;
        size_t n = 0;
        while (at + 1 + n < ls.size() && ls[at + 1 + n].group == ls[at].name) ++n;
        return n;
    }

    // Re-reads `layers[from..]` as the read and parse stages of `rec`, from `t`, which it
    // moves to their end; a failure marks the record a parse error.
    Expected<void> reread_stages(ReloadRecord& rec, size_t from, uint64_t& t) {
        return reread_stages(rec, layers, from, t);
    }
    // The same over `ls`, a stack to replace `layers` with.
    Expected<void> reread_stages(ReloadRecord& rec, Vec<LayerState>& ls, size_t from,
                                 uint64_t& t) const {
        auto r = reread(ls, from, &rec.read_ns, &rec.files_reloaded);
        const uint64_t parsed = now();
        rec.parse_ns = parsed - t - rec.read_ns;
        t = parsed;
        if (!r) rec.outcome = ReloadOutcome::ParseError;
        return r;
    }

    // Re-reads `ls[from..]` in order (each sees the macros of the ones below), all or
    // nothing.
    Expected<void> reread(Vec<LayerState>& ls, size_t from, uint64_t* read_ns = nullptr,
                          uint32_t* files = nullptr) const;

    // Where push_layer puts a layer: its index in the stack, from opts.below or .above, which
    // it clears.
    [[nodiscard]] Expected<size_t> placement(LayerOptions& opts) const;

    [[nodiscard]] std::optional<size_t> find_layer(std::string_view name) const {
        for (size_t i = 0; i < layers.size(); ++i)
            if (layers[i].name == name) return i;
        return std::nullopt;
    }

    // --- Scratch ---

    // (a (b (c values...))) for every scratch path, or null when scratch is empty.
    [[nodiscard]] Ref<Array> scratch_root() const;

    // The scratch entry for `path`, or for the value `path` is an element of.
    [[nodiscard]] const ScratchEntry* scratch_entry(const Path& path) const noexcept {
        if (!scratch_enabled) return nullptr;
        for (const ScratchEntry& e : scratch)
            if (e.path == path || is_element_of(path, e.path)) return &e;
        return nullptr;
    }

    // Whether a live edit waits in scratch at a path below `path`: a record or table read
    // there would miss it in the last full merge.
    [[nodiscard]] bool edits_beneath(const Path& path) const noexcept {
        if (!scratch_enabled) return false;
        for (const ScratchEntry& e : scratch) {
            if (e.path.size() <= path.size()) continue;
            bool under = true;
            for (size_t i = 0; under && i < path.size(); ++i) under = e.path[i] == path[i];
            if (under) return true;
        }
        return false;
    }

    // The current value at `path`: scratch first, then the merged tree.
    bool current(const Config& c, const Path& path, Vec<Node>& out) const;

    // Updates the tweaks bound to `e`'s path (or to one of its elements) from scratch,
    // without a merge, and fires only their on_change. Allocates nothing for up to 16
    // affected tweaks. A disabled scratch layer waits: the merge that enables it retunes.
    void retune(const ScratchEntry& e) const;

    // Sets `target`'s value in scratch (a names-only path), in place when the entry exists
    // with as many values.
    void put(Config& c, const Path& target, std::span<const Node> values);

    // --- Merging ---

    [[nodiscard]] Ref<Array> merged() const;

    // read_session(): the session after the last file layer, which carries every layer's
    // packs and origins (each layer reads over a copy of the one below).
    void refresh_read();
    void refresh_files();

    // Installs a new merged tree, rebinds tweaks, then fires the reload callbacks, timing
    // the merge, bind, and notify stages from `t`.
    void commit(Config& c, ReloadRecord& rec, uint64_t& t);

    // A change with no files to read (a layer change, a restore): merge, bind, notify,
    // record.
    void change(Config& c, Symbol trigger);

    [[nodiscard]] Expected<void> may_change(const Config& c) const;

    // The enabled layer below `top` (skipping `skip`) whose own tree has `path`'s value:
    // overwrite merging means its values won.
    [[nodiscard]] std::optional<size_t> supplier(const Path& path, size_t top,
                                                 size_t skip = SIZE_MAX) const {
        const bool index = last_is_index(path);
        for (size_t i = top; i-- > 0;) {
            if (i == skip || !layers[i].options.enabled) continue;
            detail::Resolved r;
            if (detail::resolve(*layers[i].root, path, r) != detail::ResolveStatus::Ok) continue;
            if (index || (r.array && has_values(*r.array))) return i;
        }
        return std::nullopt;
    }

    [[nodiscard]] Source source_in(const LayerState& l, const Path& path) const {
        detail::Resolved r;
        if (detail::resolve(*l.root, path, r) != detail::ResolveStatus::Ok)
            return {Symbol(l.name), {}, 0};
        return located(l, *r.values.source());
    }
    // The source of a value `holder` holds in layer `l`: with its file's origin, if the
    // file declared one.
    [[nodiscard]] static Source located(const LayerState& l, const Array& holder) {
        Source s{Symbol(l.name), holder.file(), holder.line()};
        if (const Origin* o = l.after ? l.after->origin_of(holder.file()) : nullptr) {
            s.origin = o->source;
            s.tool = o->tool;
        }
        return s;
    }
    [[nodiscard]] Source located(std::string_view layer, const Array& holder) const {
        if (const auto i = find_layer(layer)) return located(layers[*i], holder);
        return {Symbol(layer), holder.file(), holder.line()};
    }

    // --- Checking a candidate layer (Config::check_layer) ---

    // The candidate `tree` (with a file layer's #autorun `commands`, read with `session`) on
    // top of the stack, or of its first `below` layers (set_layer_text's replacement, without
    // the layer it replaces and scratch): its effect, the policy's refusals, and the
    // validators' warnings, `read_warnings` added.
    [[nodiscard]] LayerReport check_candidate(const Config& c, std::string_view name,
                                              const Ref<Array>& tree,
                                              std::span<const Node> commands,
                                              const LoadSession& session, const LayerPolicy& policy,
                                              std::span<const Error> read_warnings,
                                              size_t below = SIZE_MAX) const;
    // A held report from a LayerChecker: the validators it left out (not any_thread), over
    // the stack with `candidate` on top, read with `session`; their warnings refuse under
    // the policy's warnings_refuse.
    [[nodiscard]] LayerReport check_left_out(const Config& c, const Ref<Array>& candidate,
                                             const LoadSession& session,
                                             const LayerPolicy& policy) const;
    // Fills enabled_roots: the enabled layers' trees among the first `count`, then
    // `candidate` if given.
    std::span<const Ref<Array>> stack_layers(const Ref<Array>& candidate = nullptr,
                                             size_t count = SIZE_MAX) const;

    // --- Write-back ---

    [[nodiscard]] Expected<Plan> plan(const LayerState& layer, const Path& path,
                                      const Vec<Node>& values, bool insert,
                                      const LoadSession::Reader& read_file) const;
    Expected<WriteReport> write_into(Config& c, size_t li, const Path& path,
                                     const Vec<Node>& values, bool insert);
};

}  // namespace baker
