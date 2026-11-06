#pragma once

// Config: layered, hot-reloadable configuration with provenance and write-back.
//
//   baker::Config cfg;
//   cfg.push_layer("default", "default.dta");
//   cfg.push_layer("platform", "ps5.dta");
//   cfg.push_layer("local", "local/" + user + ".dta", {.optional = true});   // per user
//   cfg.push_layer("patch-2026-10-11", overlay);       // a live-ops overlay array
//   cfg.remove_layer("patch-2026-10-11");              // rollback: leaves no trace
//
//   float c = cfg.get<float>("renderer.clear_color.0");
//   baker::Source s = cfg.source("renderer.clear_color");   // layer, file, line
//
// Layers merge bottom to top. For each tagged array of a layer, the array with the same
// tag below is merged into recursively; the layer's values (everything that is not a tagged
// array) replace the ones below, so (width 3) over (width 2 (@range 0 10)) gives
// (width 3 (@range 0 10)). Each layer is read in its own pass, but macros and defines carry
// upward from the layers below, as from earlier files in one load. load() and merge() are
// shorthands that push a layer named after the file. A layer pushed with
// {.optional = true} whose file is missing is an empty layer, and reload() picks the file
// up once it exists. A disabled layer keeps its place and its trees but is skipped by
// merging, provenance, and write-back; toggling it is how variants are compared A/B.
//
// Every change -- pushing, popping, removing, enabling, or disabling a layer, reload(),
// restore() -- re-merges, rebinds the tweaks of the attached TweakRegistry, fires the
// reload callbacks, and appends a ReloadRecord to the history. Old trees stay alive while
// anyone holds a Ref to them, so a reload never dangles a pointer someone is reading
// through.
//
// Hot reload. reload() re-reads every file each file layer was built from (includes too),
// and re-parses the lowest layer whose files changed and the layers above it. A reload
// that fails to parse changes nothing. Callbacks are registered per group, the subtree that
// code owns: on_reload("renderer", rebuild) fires only when that subtree changed. Punting
// -- tearing the subsystem down and rebuilding it from the new values -- is the default
// and needs no code beyond `rebuild`; a `migrate` callback can move runtime state over
// instead and return Reload::Migrated, or decline with Reload::Punt. By convention a
// subsystem reads only its own group: that is what makes a group the unit of refresh.
//
// Telemetry. Each change is timed in six stages (scan, read, parse, merge, bind, notify)
// with the time source the host gives set_time_source (default: steady_clock), and kept in
// a fixed ring (set_reload_history, default 256). Under BAKER_SHIPPING the timing compiles
// out: last_reload() is empty and each_reload() visits nothing.
//
// Scratch and write-back. set() puts a value tuned in the running program into the
// reserved in-memory layer "scratch", always topmost, so source() reports it as unsaved.
// Setting a path already in scratch is a fast path: no re-merge and no allocation; only
// the tweaks bound to that path are updated. Lookups through Config (get, source) consult
// scratch first; root() includes scratch as of the last full merge. write_back() writes
// the current value into the file of the highest enabled layer below scratch that supplies
// it, replacing only the tokens that produced the old value, so comments, notes, and
// formatting are kept and the diff is one line. A whole float written over an integer
// token keeps the integer form, since hosts tune numbers in float fields: (height 96) tuned
// to 120.0 is written (height 120); 120.5 is written as itself. write_back_to() writes into a
// chosen layer's file, adding the entry as the last child of the deepest ancestor written there
// (creating an optional layer's missing file); promote() writes a whole layer's effect into
// another layer, all or nothing. Write-back refuses, with a located error, a value from a
// macro, an overlay, an ancestor that came from #include or #merge, a missing indexed
// element, or a file that changed on disk since it was read; it never guesses. It needs
// edit mode (ConfigOptions::edit_mode), on by default except under BAKER_SHIPPING.
//
// snapshot() captures the layer stack, each layer's trees (shared, not copied), and
// scratch; restore() returns to it exactly without reading any file.
//
// freeze() ends the initial load: afterwards reload, layer changes, and live edits are
// refused, unless ConfigOptions::live_layers explicitly allows them (the path a shipped
// game uses for live-ops overlays). Under BAKER_SHIPPING, reload(), toggles, set(),
// write-back, and promote() are always inert.
//
// Not thread-safe: change a Config from one thread. Readers on other threads should take
// root() (a Ref) and read that; checks of a candidate layer on another thread take a
// LayerChecker (checker()).

#include <baker/array.hpp>
#include <baker/clock.hpp>
#include <baker/parse.hpp>
#include <baker/path.hpp>
#include <baker/source_map.hpp>
#include <baker/tweak.hpp>
#include <baker/write.hpp>

#include <baker/callback.hpp>
#include <baker/list.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace baker {

enum class MergePolicy : uint8_t {
    Overwrite,    // Config layers: the incoming values replace the existing ones
    FillMissing,  // #merge: existing values stay; only missing keys are added
};

// Merges `over` into `base`: tagged arrays recurse by tag, anything else is a value.
// Returns `base` itself if nothing changed; never modifies either.
[[nodiscard]] Ref<Array> merge(const Array& base, const Array& over,
                               MergePolicy policy = MergePolicy::Overwrite);

// Where a value came from.
// Every member has its own initializer, so that {layer, file, line} leaves the rest empty
// without -Wmissing-field-initializers.
struct Source {
    Symbol layer{};  // "scratch" for an unsaved live edit
    Symbol file{};   // empty for a layer with no file (an overlay, scratch)
    int32_t line = 0;
    // When the file declares #origin ("economy.xlsx" (tool sheets-export)): what it was
    // generated from, and by what (parse.hpp). Empty otherwise.
    Symbol origin{};
    Symbol tool{};
};

enum class Reload : uint8_t { Migrated, Punt };

struct ConfigOptions {
    bool edit_mode = !kShipping;  // keep source maps, for write-back
    bool live_layers = false;     // allow layer changes and live edits after freeze()
    // The registry whose tweaks this config binds; null for none.
    TweakRegistry* tweaks = &TweakRegistry::global();
    // Outside edit mode, file layers load through binary caches kept here: a fresh
    // cache instead of the text, else the text and a new cache, written through the
    // Config's writer (cache.hpp). Copied by the Config; empty: no caches. The directory
    // must exist. Shipping builds write none: run the same layers through a development
    // build's Config (a build step), and ship the directory.
    std::string_view cache_dir = {};
    // The caches it writes leave out metadata (without_metadata): for the build step's
    // Config whose directory ships to a build that reads no metadata. A tree it reads
    // from such a cache has none either.
    bool cache_drop_metadata = false;
    // A stack image (<baker/image.hpp>): open_project takes the layers the image holds
    // from it, in place of their files, and the registered tables it holds in place of
    // building them. Copied by the Config; empty: none.
    std::string_view stack_image = {};
};

// What a layer may change (Config::check_layer, LayerOptions::policy). Each rule is off by
// default; a refusal names the rule's reason, the path, and the file and line.
struct LayerPolicy {
    // no {commands}, [properties], $variables, #autorun, or #shape
    bool data_only = false;
    // every value an int, float, symbol, or string; no removals, no #shape
    bool scalars_only = false;
    bool existing_only = false;     // change only values the stack already has: no new keys, no
                                    // removals
    bool warnings_refuse = false;   // a validator's or the read's warning refuses too
    std::span<const Path> allow{};  // if not empty, every changed path lies under one of these
};

struct LayerReport;
struct ProjectLayer;  // project.hpp
struct TableSpec;     // table.hpp
class BakedTable;

// Identity (<baker/record_id.hpp>). find_by_id: the array below `root` whose (@id ...)
// is `id`, with its dotted path in `path` if given; null for none. A walk of the tree: for
// tools, not a frame. id_validator: for Config::add_validator, reports two records that
// share an ID (located at the second, naming both) and a malformed (@id ...), as errors to
// the warning handler; the kit's check runs it. A Config pays nothing unless it adds it.
[[nodiscard]] const Array* find_by_id(const Array& root, RecordId id, Path* path = nullptr);

// What a validator judges (Config::add_validator): the merged tree, the trees it was merged
// from, and what their files declared. Valid for the call only.
struct ValidatorStack {
    const Array& root;  // the merged tree
    // The enabled layers' own trees, bottom to top, as Config::layer_root gives them; when
    // a candidate layer is checked, it is last.
    std::span<const Ref<Array>> layers;
    // What the layers' files declared (Config::read_session), with a checked candidate's
    // text read too.
    const LoadSession& session;
};

struct ValidatorOptions {
    // It reads only the stack it is given, and keeps no state, so it may run on any thread
    // at once with the Config's own: a LayerChecker runs it on the thread that checks. A
    // validator without it runs on the Config's thread only.
    bool any_thread = false;
};

struct LayerOptions {
    bool optional = false;  // a missing file is not an error; the layer exists, empty
    bool enabled = true;    // disabled layers keep their position but are skipped by merge
    // A condition on the session's defines, as #ifdef reads them, written as a project file's
    // (when COND) holds it: a define's name, (not C), (and C C...) or (or C C...), read
    // unexpanded. While it does not hold, the layer is skipped by merge as a disabled
    // one is; set_defines and set_layer_enabled apply it again. Read at the push and kept.
    std::string_view when = {};
    // Where push_layer places the layer: directly below or above the named layer, instead of
    // on top. A file layer placed under others re-reads them, as remove_layer does,
    // because they read with the macros below them. Read at the push, never kept.
    std::string_view below = {};
    std::string_view above = {};
    // If given, the layer is checked first (Config::check_layer) and refused, changing
    // nothing, if the policy refuses it. A file or text layer's #autorun commands, and the
    // warnings its read adds (a pack's unmet requires), count too.
    const LayerPolicy* policy = nullptr;
    // With `policy`: a report on this layer from check_layer or check_layer_text, under the
    // same policy. If it still holds, the push takes its verdict instead of checking again:
    // that is, if the Config has not changed since the check (no layer change, reload,
    // set(), validator added or removed, or call of session()), and the push puts the same
    // name with the same text, or the same tree, on top. Otherwise the push checks, as
    // without it. A tree must not change between the check and the push. A validator that
    // reads something outside the Config (check_validator's Context) is trusted not to have
    // changed its verdict meanwhile.
    const LayerReport* checked = nullptr;
};

// --- Telemetry ---

enum class ReloadOutcome : uint8_t { Ok, Unchanged, ParseError, Refused };

struct ReloadRequest {
    Symbol trigger{};        // host tag: "watcher", "console", "livelink", ...
    uint64_t origin_ns = 0;  // when the edit was saved, in the TimeSource domain; 0: unknown
};

struct ReloadRecord {
    uint64_t epoch = 0;  // epoch after this change (unchanged if nothing changed)
    Symbol trigger{};    // the request's, or "reload", "layer", "restore", "write_back"
    uint64_t origin_ns = 0, start_ns = 0, end_ns = 0;
    // The stages, in order, tile start_ns to end_ns. write_ns is write-back's own write of the
    // files it changed, before it reads them back; a reload writes nothing.
    uint64_t scan_ns = 0, write_ns = 0, read_ns = 0, parse_ns = 0;
    uint64_t merge_ns = 0, bind_ns = 0, notify_ns = 0;
    uint32_t files_reloaded = 0, tweaks_changed = 0;
    uint32_t migrated = 0, punted = 0;  // reload-callback outcomes
    Symbol first_file{};                // the first changed file, for display
    Symbol layer{};                     // the layer of first_file; empty if none changed
    uint64_t visible_ns = 0;            // when the host next presented (mark_visible); 0: not yet
    ReloadOutcome outcome = ReloadOutcome::Ok;
};

// Loop statistics: how long edits take to be seen, per file and per
// layer. Only reloads that changed a file feed them, so a host that polls for changes does
// not push edits out. Times are kept to the microsecond.
struct LoopRow {
    Symbol name;            // a file, or a layer
    uint32_t reloads = 0;   // reloads that changed it, failures included, since it got its row
    uint32_t failures = 0;  // of those, the ones that failed to parse
    // Over its last 32 edits that reloaded: edit-to-see, from when the edit was saved
    // (ReloadRequest::origin_ns) to when the host presented it (mark_visible), or to the end
    // of the reload if the host has not; and the reload's own time.
    uint32_t edits = 0;  // edit-to-see samples: only reloads that gave an origin have one
    uint64_t edit_median_ns = 0, edit_worst_ns = 0;
    uint32_t times = 0;  // reload-time samples
    uint64_t reload_median_ns = 0, reload_worst_ns = 0;
};

struct LoopStats {
    uint32_t reloads = 0;    // calls to reload()
    uint32_t unchanged = 0;  // of those, the ones that found nothing changed
    uint32_t failures = 0;   // the ones that failed to parse, or were refused
    // The 32 files and 16 layers changed most recently, most recent first.
    List<LoopRow> files, layers;
};

// --- Layer tooling ---

// One value a layer sets: a path to a tagged array with values. A value of several nodes,
// like (screen_size 1920 1080), is an untagged array of them: (1920 1080).
// A file a layer was read from: the layer's own file, one reached from it by #include,
// #merge or #require, or one looked for there and missing (creating it would change what
// the layer reads).
struct StackFile {
    Symbol layer;
    Symbol path;  // as the session's reader was given it
    bool present = false;
    // A directory of layers (Config::push_layer_dir), as its lister was given it: a file
    // added to it, or removed, changes the stack. `present`: it could be listed.
    bool directory = false;
};

// A row's values, one node each, held as one node so that a single value costs nothing more:
// (aim 0 0) is two values, (aim (0 0)) one, an array. What a layer tool reports for a row
// (LayerEffectEntry, explain, diff).
class RowValues {
public:
    RowValues() noexcept = default;
    explicit RowValues(std::span<const Node> values)
        : node_(values.size() == 1 ? values[0] : Node(Array::make_copy(values))),
          one_(values.size() == 1) {}
    [[nodiscard]] std::span<const Node> nodes() const noexcept {
        if (one_) return {&node_, 1};
        const Array* a = node_.array();
        return a ? a->nodes() : std::span<const Node>();
    }
    [[nodiscard]] size_t size() const noexcept { return nodes().size(); }
    [[nodiscard]] bool empty() const noexcept { return nodes().empty(); }
    [[nodiscard]] const Node& operator[](size_t i) const noexcept { return nodes()[i]; }
    [[nodiscard]] const Node* begin() const noexcept { return nodes().data(); }
    [[nodiscard]] const Node* end() const noexcept { return nodes().data() + nodes().size(); }

private:
    Node node_;         // the one value, or an array of the values
    bool one_ = false;  // node_ is the value itself
};

struct LayerEffectEntry {
    Symbol path;  // dotted, as get<T>() takes it
    // This layer's values, one node each: (aim (0 0)) is one, an array; (aim 0 0) is two. A
    // verb's operands, a record one operand; a removal's #remove.
    RowValues values;
    // The stack's values without this layer, if it has the path. Under a verb, the list the
    // verb changes: every element, records too, (moves (jab ...) (heavy ...)) two.
    std::optional<RowValues> below;
    Source source;  // provenance, as source() returns it
    // Where `below` was written: under a verb, the row of the list it changes.
    std::optional<Source> below_source;
    bool shadowed = false;  // a higher enabled layer (or scratch) overrides this path
    Symbol op;              // the row's verb: #append, #drop or #remove; empty for a value
    bool stale = false;     // the row's (@was ...) pin is not `below`
    // The layer's children after this row, in its array, with its tag: each merges into this
    // row's record rather than adding to a list, and a check warns. 0 for a key written
    // once.
    uint32_t folded = 0;
    // A declaration the layer's read made, not a value: #shape, #pack or #origin. `path` is
    // then the shape's or pack's name, or the file an origin describes; `values` its form,
    // alone; `below` the same declaration from a layer below, if any.
    Symbol declares;
};

// What a LayerReport was made against, so that a push can tell whether it still holds
// (LayerOptions::checked). Opaque.
class CheckStamp {
private:
    friend class Config;
    friend class LayerChecker;
    const void* config_ = nullptr;
    uint64_t generation_ = 0;
    uint64_t subject_ = 0;  // the layer's name with its text, or with its tree's address
    uint64_t policy_ = 0;
    bool left_out_ = false;  // by a LayerChecker: validators not any_thread did not run
};

// Config::check_layer: a candidate layer's effect, and the verdicts on it, before it takes
// effect.
struct LayerReport {
    List<LayerEffectEntry> effect;  // what it would change, as layer_effect reports it
    List<Error> refusals;           // the policy's, located; any refuses the layer
    List<Error> warnings;           // the validators' over the stack with it, and the read's
    CheckStamp stamp;
    [[nodiscard]] bool ok() const noexcept { return refusals.empty(); }
};

struct WrittenLine {
    Symbol file;
    int32_t line = 0;
};

// The edit journal: every live change made through the Config, and
// what became of it. A value of more than four nodes keeps its first four, and its count.
enum class EditFate : uint8_t {
    Unsaved,     // still only in scratch
    Written,     // written to a file by write_back, write_back_to, or promote from scratch
    Reverted,    // dropped from scratch by clear_scratch or restore
    Superseded,  // edited again before it was saved: the later entry has the fate
};

struct JournalEntry {
    Symbol path;           // as unsaved() names it: box/width
    uint64_t time_ns = 0;  // from the Config's time source
    Node before[4], after[4];
    uint32_t before_count = 0;  // 0: the path had no value
    uint32_t after_count = 0;
    Source source;  // where the old value came from
    EditFate fate = EditFate::Unsaved;
    WrittenLine written;  // for Written: the file and line
};

struct WriteReport {
    List<WrittenLine> written;  // every file and line written
    List<Error> refused;        // promote(): every refusal; then nothing was written
    [[nodiscard]] bool ok() const noexcept { return refused.empty(); }
};

// A configuration state to return to: the layer stack, each layer's trees (shared by
// reference, not copied), the files' load-time content, and a copy of scratch. Copyable.
class ConfigSnapshot {
public:
    ConfigSnapshot() noexcept = default;
    ConfigSnapshot(const ConfigSnapshot& other) noexcept;
    ConfigSnapshot& operator=(const ConfigSnapshot& other) noexcept;
    ~ConfigSnapshot();
    [[nodiscard]] bool empty() const noexcept { return !state_; }

private:
    friend class Config;
    struct State;  // refcounted; defined in config.cpp
    State* state_ = nullptr;
};

// A fixed state of a Config to check candidate layers against, on any thread
// (Config::checker). It shares the Config's trees and copies the rest, and never touches the
// Config after it is made: the Config may change, or be destroyed, meanwhile. Its checks
// report as Config::check_layer and check_layer_text do, against the state it was made
// from, and run only the validators added with ValidatorOptions::any_thread; a push with
// such a report, if it still holds (LayerOptions::checked), runs the others first, on the
// Config's thread. Copyable; copies share one state, and checks with it may run on several
// threads at once. Needs atomic reference counts (the default) to be used off the
// Config's thread. The Config's allocator must outlive it, as for a snapshot.
class LayerChecker {
public:
    LayerChecker() noexcept = default;
    LayerChecker(const LayerChecker& other) noexcept;
    LayerChecker& operator=(const LayerChecker& other) noexcept;
    ~LayerChecker();
    [[nodiscard]] bool empty() const noexcept { return !state_; }

    [[nodiscard]] Expected<LayerReport> check(std::string_view name, const Ref<Array>& root,
                                              const LayerPolicy& policy = {}) const;
    [[nodiscard]] Expected<LayerReport> check_text(std::string_view name, std::string_view text,
                                                   const LayerPolicy& policy = {}) const;

    struct State;  // refcounted; defined in config_check.cpp

private:
    friend class Config;
    void stamp(LayerReport& report, uint64_t subject, const LayerPolicy& policy) const noexcept;
    State* state_ = nullptr;
};

namespace detail {
struct ToolsAccess;  // baker::tools' way to the internals (src/tools/access.hpp)
struct TableSlot;    // a registered table (src/data/table_build.hpp)
}  // namespace detail
class Config;
namespace detail {
[[nodiscard]] std::span<const TableSlot> table_slots(const Config& cfg);  // the image writer's
}

class Config {
public:
    using Writer = Callback<bool(std::string_view path, std::string_view text)>;
    using ReloadId = uint32_t;

    // Everything the Config keeps comes from `allocator`, which must outlive it and its
    // snapshots.
    explicit Config(ConfigOptions options = {}, const Allocator& allocator = default_allocator());
    ~Config();
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    // --- Layers. ---
    [[nodiscard]] Expected<void> push_layer(std::string_view name, std::string_view path,
                                            LayerOptions options = {});
    [[nodiscard]] Expected<void> push_layer(std::string_view name, Ref<Array> overlay,
                                            LayerOptions options = {});
    // A layer from text the host received (over the network, from a mod store): read as a
    // file layer is, against the session below it, so its packs, origin, and #autorun are
    // recorded; but from `text` alone, never through a cache, and an #include or #merge in
    // it is refused. `name` is also the file its sources name, such as
    // "received/balance@3.dta". The Config keeps the text, to read it again when a layer
    // below changes; reload() never does. Write-back refuses its values: it has no file.
    [[nodiscard]] Expected<void> push_layer_text(std::string_view name, std::string_view text,
                                                 LayerOptions options = {});
    // A text layer's text replaced in place, for a peer that receives the layer again (a
    // server resending a match's settings, a lobby host's edits): the layer keeps its
    // place and options, and the layers above are read again with its macros. The same
    // bytes as it holds change nothing: no read, no merge, no reload record. With
    // `policy`, the new text is checked against the layers below it, without the version
    // it replaces, and refused, changing nothing, if the policy refuses it. NotFound for
    // an unknown layer; Refused for a file layer (reload() reads one again) or scratch.
    [[nodiscard]] Expected<void> set_layer_text(std::string_view name, std::string_view text,
                                                const LayerPolicy* policy = nullptr);
    // The host's own flags, for #ifdef, replaced after the stack is open: a server switching
    // mode, a demo unlocked. Each layer whose read tested, defined or undefined a name that
    // changed is read again, in place, with the sessions below it; every other layer keeps
    // its tree. One merge follows, recorded as a reload whose trigger is "defines". The same
    // set changes nothing. Works in a shipping build: a mode is decided there too. A layer
    // a cache replayed, which records no tests, is read again. If a layer does not read, the
    // defines and every layer stay as they were.
    [[nodiscard]] Expected<void> set_defines(std::span<const std::string_view> names);

    // A directory of layers, for mods, DLC and plugin packs: a layer named `name`, which holds
    // nothing, with a layer above it for each *.dta file directly in `dir` (named by its file
    // name, without .dta) and each subdirectory that holds a layer.dta (named by the
    // subdirectory, read from its layer.dta). They stand in the order `dir`/order.dta lists,
    // (order a b c); the rest follow in name order. A name order.dta lists that the directory
    // lacks is a warning, located there; order.dta is not a layer. A layer whose name is
    // taken is left out, with a warning.
    //
    // The directory is listed through the session's lister and its files read through its
    // reader. Every reload() lists it again: a new file adds a layer, a removed one removes
    // it, and an order.dta changed reorders them. A layer keeps its enabled state across that
    // for as long as it is there; a new one takes the directory's (set_layer_enabled on
    // `name` sets them all). A layer that does not read is left empty, its error passed to
    // the warning handler, and read again when its files change; the others stand. With
    // `options.optional`, a directory that cannot be listed is empty. A policy is refused:
    // check each layer's text instead. remove_layer(name) removes them all; a layer found
    // in it cannot be removed alone (remove its file, or disable it), nor be a placement's
    // anchor (place below or above `name`).
    [[nodiscard]] Expected<void> push_layer_dir(std::string_view name, std::string_view dir,
                                                LayerOptions options = {});
    [[nodiscard]] Expected<void> remove_layer(std::string_view name);
    // Removes the topmost layer below scratch.
    [[nodiscard]] Expected<void> pop_layer();
    [[nodiscard]] Expected<void> load(std::string_view path) { return push_layer(path, path); }
    [[nodiscard]] Expected<void> merge(std::string_view path) { return push_layer(path, path); }
    // Layer names bottom to top, ending with "scratch".
    [[nodiscard]] List<Symbol> layers() const;
    // Every file the file layers were read from, layer by layer, bottom to top: for a host
    // that watches files itself (an engine's file notifications, an editor's change
    // tracking, a write-time check each frame) and calls reload() with the save's time as
    // origin_ns only when one changes. Valid until the Config next changes; refreshed at
    // every merge. stack_files_epoch() moves whenever the list does, so a host compares one
    // number a frame. A layer pushed as text, and a layer replayed from a cache, list what
    // their reads opened.
    [[nodiscard]] std::span<const StackFile> stack_files() const noexcept;
    [[nodiscard]] uint64_t stack_files_epoch() const noexcept;
    // The #autorun commands of the enabled file layers, bottom to top, as their last reads
    // collected them. Each layer is read with a session of its own, so they are not
    // in session().autorun(); a host runs them after a push, reload, or toggle, so that a
    // data file can define script functions, with Context::run_autorun, which also undoes
    // what a removed layer's commands defined. Allocates its result.
    [[nodiscard]] List<Node> autorun() const;
    [[nodiscard]] Expected<void> set_layer_enabled(std::string_view name, bool enabled);
    // Returns the new state.
    [[nodiscard]] Expected<bool> toggle_layer(std::string_view name);
    // Whether the layer is in the merge: enabled, and its condition (LayerOptions::when)
    // holding. False for an unknown layer.
    [[nodiscard]] bool layer_enabled(std::string_view name) const;
    // The layer's condition as read, `(and PC (not DEMO))`, or nothing for a layer without
    // one or an unknown layer.
    [[nodiscard]] std::optional<Node> layer_condition(std::string_view name) const;
    // The layer's own tree, as read, before any merge; null for an unknown layer and for
    // scratch. For tools such as shape::validator, which read it; changing it changes
    // nothing until the next merge, and is not supported.
    [[nodiscard]] Ref<Array> layer_root(std::string_view name) const;
    // The layer's own tree as text, for a peer that must hold the same layer: a server
    // sending a match's settings to its clients, which push it with push_layer_text under
    // their own LayerPolicy. The tree is as read (includes spliced in, macros expanded,
    // #remove kept) and reads back to an equal tree; comments and layout are not kept, and
    // the layer's file is not touched. "scratch" writes the live edits as a layer, each
    // edited path with its value. The same tree writes the same bytes, so text sent again
    // unchanged is a no-op on a receiver's reload. NotFound for an unknown layer. Allocates
    // only for the text; <baker/std_interop.hpp> has a std::string form.
    [[nodiscard]] Expected<void> layer_text(std::string_view name, TextSink out) const;
    // The layout of the text this Config writes: layer_text, and the forms write-back and
    // promote add to a layer's file. WriteOptions' house layout until set;
    // open_project sets the project's (format ...).
    void set_layout(const WriteOptions& layout) noexcept;
    [[nodiscard]] const WriteOptions& layout() const noexcept;

    // A digest of the data, for hosts that must agree on it: lockstep peers, a replay, a
    // save loaded under other data. 64 bits, the same on every platform and build for the
    // same data and different when any value differs (detail in baker::digest). It finds a
    // difference; it is not a cryptographic hash, and no defence against tampering.
    // digest(): of the merged tree as the game reads it, live edits included, whatever
    // layers produced it. stack_digest(): of each enabled layer's name and tree, in order,
    // then scratch's: it moves when a layer is toggled or edited even if the values agree.
    // Linear in the data; asked for, never on a frame's path.
    [[nodiscard]] uint64_t digest() const;
    [[nodiscard]] uint64_t stack_digest() const;

    // --- Reading. ---
    // The merged tree, including scratch as of the last full merge or apply_edits(): a live
    // edit since is in get() and not here until then. Holding the Ref keeps this version
    // alive across reloads.
    [[nodiscard]] Ref<Array> root() const noexcept { return root_; }
    // Merges the live edits made since the last full merge into root(), so every reader of
    // the tree sees them: between frames, before the game reads its structured data (Live
    // does, before `changed`). Nothing to do when there are none.
    void apply_edits();
    // A value as the game reads it, live edits included. A record or a table above a live
    // edit not yet applied is read from a fresh merge, so it holds the edit; a T that keeps
    // views into the tree (std::string_view, Values) must not be read so, and should be read
    // after apply_edits().
    template <class T>
    [[nodiscard]] Expected<T> get(const Path& path) const {
        Values v;
        if (scratch_dirty_) {
            if (const ScratchHit hit = scratch_values(path, v); hit != ScratchHit::None)
                return from_scratch<T>(path, hit, v);
            if (edits_beneath(path)) return with_edits()->get<T>(path);
        }
        return root_->get<T>(path);
    }
    template <class T>
    [[nodiscard]] Expected<T> get(const Path& path, T fallback) const {
        Values v;
        if (scratch_dirty_) {
            if (const ScratchHit hit = scratch_values(path, v); hit != ScratchHit::None)
                return hit == ScratchHit::PastEnd ? std::move(fallback)
                                                  : from_scratch<T>(path, hit, v);
            if (edits_beneath(path)) return with_edits()->get<T>(path, std::move(fallback));
        }
        return root_->get<T>(path, std::move(fallback));
    }
    [[nodiscard]] Expected<Source> source(const Path& path) const;
    // Increments on every change: a reload, a layer change, every live edit (a set(), the
    // fast path's included, a clear_scratch(), a write-back, a restore). A host compares it
    // once a frame to know that what it shows or caches is stale. Under BAKER_SHIPPING only
    // the changes a shipping build makes move it.
    [[nodiscard]] uint64_t epoch() const noexcept { return epoch_; }

    // --- Hot reload and telemetry. ---
    // Re-reads changed files; true if anything changed.
    [[nodiscard]] Expected<bool> reload(const ReloadRequest& request = {});
    ReloadId on_reload(const Path& group, Callback<void()> rebuild,
                       Callback<Reload()> migrate = {});
    // Called after every change with its finished record (not under BAKER_SHIPPING).
    ReloadId on_reload_complete(Callback<void(const ReloadRecord&)> callback);
    void remove_on_reload(ReloadId id);
    void set_time_source(TimeSource fn, void* user = nullptr);
    // Allocates the history ring once; 0 turns history off. Default 256.
    void set_reload_history(size_t capacity);
    [[nodiscard]] std::optional<ReloadRecord> last_reload() const noexcept;
    // Called by the host when a frame presents: stamps visible_ns on the reloads completed
    // since the last call, and counts their edit-to-see to now. With nothing to stamp, one
    // branch. Does nothing under BAKER_SHIPPING.
    void mark_visible() noexcept {
        if (visible_pending_) stamp_visible();
    }
    // Per file and per layer (above); empty under BAKER_SHIPPING. Allocates its result.
    [[nodiscard]] LoopStats loop_stats() const;
    // Visits the history, oldest to newest.
    template <class F>
    void each_reload(F&& fn) const {
        for (size_t i = 0, n = history_size(); i < n; ++i) fn(history_at(i));
    }
    // Called with a located error for each tweak whose value does not convert (the tweak
    // keeps its default) and each bad tweak path. Default: ignored.
    void set_warning_handler(Callback<void(const Error&)> handler);
    // Called after every merge (a layer pushed, popped, removed, or toggled; a reload; a
    // restore) with the stack and the warning handler, to report on the data, and by each
    // check of a candidate layer: baker::shape::validator() checks shapes this way,
    // and baker::check_validator() script calls. Validators run in the order they
    // were added.
    using Validator =
        Callback<void(const ValidatorStack& stack, const Callback<void(const Error&)>& warn)>;
    using ValidatorId = uint32_t;
    ValidatorId add_validator(Validator validator, ValidatorOptions options = {});
    void remove_validator(ValidatorId id);
    // Baked tables (<baker/table.hpp>): add_table<T>(cfg) registers one, built from
    // the merged tree now and at every change that touches its records; table<T>(cfg) reads
    // it. Refused: a spec registered twice (by digest), or one whose records' place has no
    // `*`-free keyed parent to find.
    Expected<void> add_table(const TableSpec& spec);
    [[nodiscard]] Ref<const BakedTable> table_block(uint64_t digest) const noexcept;
    // The errors the last build of each table reported (a field that is not a literal, a
    // wrong type), located, as the warning handler saw them: for a tool's report.
    [[nodiscard]] List<Error> table_errors() const;
    // ConfigOptions::stack_image, as copied. open_project calls push_image_layers with the
    // project's layers when it is set: the leading layers the image holds are pushed from
    // it, bottom to top, each with its project options, and the tables the image holds
    // are installed for the registered specs they match (a mismatch is a warning, and that
    // table is built from the tree). Returns how many layers were taken from the image.
    [[nodiscard]] std::string_view stack_image() const noexcept;
    [[nodiscard]] Expected<uint32_t> push_image_layers(std::span<const ProjectLayer> layers);
    friend std::span<const detail::TableSlot> detail::table_slots(const Config& cfg);
    void freeze() noexcept { frozen_ = true; }
    [[nodiscard]] bool frozen() const noexcept { return frozen_; }

    // The session each layer is read with: include_dirs, defines, limits, reader, pack
    // levels. Each layer reads a copy of it, so what the files declare (packs, origins) is
    // not here; read_session() has that.
    [[nodiscard]] LoadSession& session() noexcept;
    // What the layers' files declared, as read: a copy of the session after the last
    // file layer was read, with every layer's packs, origins, macros, and defines (a
    // disabled layer's too: layers read in order, each over the one below). Refreshed at
    // every merge, before the validators run; its address is stable for the Config's life,
    // so it is the session to give check, check_validator, to_json, and reference:
    //   cfg.add_validator(baker::check_validator(ctx, {.packs = &cfg.read_session()}));
    [[nodiscard]] const LoadSession& read_session() const noexcept;
    // The registry whose tweaks this Config binds (ConfigOptions::tweaks); null for none.
    [[nodiscard]] const TweakRegistry* tweak_registry() const noexcept;
    // Replaces file writing for write-back (default: the local filesystem).
    void set_writer(Writer writer);

    // --- Scratch, write-back, and layer tooling. ---
    [[nodiscard]] Expected<void> set(const Path& path, std::span<const Node> values);
    [[nodiscard]] Expected<void> set(std::string_view path, const Node& value);
    template <class T>
    [[nodiscard]] Expected<void> set(const Path& path, const T& value) {
        ValueList list;
        Convert<T>::to(value, list);
        return set(path, std::span<const Node>(list.nodes()));
    }
    // One path, or all of scratch.
    [[nodiscard]] Expected<void> clear_scratch(std::string_view path = {});
    // Paths in scratch: live edits not yet written back.
    [[nodiscard]] List<Symbol> unsaved() const;
    // Visits the edit journal, oldest first: up to the last 256 live changes; empty
    // under BAKER_SHIPPING. set_journal_history allocates the ring once; 0 turns it off.
    template <class F>
    void journal(F&& fn) const {
        for (size_t i = 0, n = journal_size(); i < n; ++i) fn(journal_at(i));
    }
    void set_journal_history(size_t capacity);

    [[nodiscard]] Expected<WriteReport> write_back(const Path& path);
    // `pin`: the row also carries `(@was v...)`, the stack's value below `layer` as it is now,
    // so that a check of the layer warns once that value moves.
    [[nodiscard]] Expected<WriteReport> write_back_to(const Path& path, std::string_view layer,
                                                      bool pin = false);
    // Writes every entry of tools::layer_effect(from) into `to`'s file, or nothing: on any
    // refusal the report lists them all and no file changes. `from` stays in the stack.
    [[nodiscard]] Expected<WriteReport> promote(std::string_view from, std::string_view to);
    // Checks a candidate layer, changing nothing: its effect on top of the stack as it
    // stands, the policy's refusals, and the validators' warnings over the stack with it.
    // Available in shipping builds: it is what makes live_layers safe there.
    [[nodiscard]] Expected<LayerReport> check_layer(std::string_view name, const Ref<Array>& root,
                                                    const LayerPolicy& policy = {}) const;
    // The same for text a host received, read as push_layer_text reads it (never a cache,
    // and no #include), with the session the stack's layers read with, so its packs'
    // requires and versions are checked against the stack's packs; the read's warnings join
    // the report's.
    [[nodiscard]] Expected<LayerReport> check_layer_text(std::string_view name,
                                                         std::string_view text,
                                                         const LayerPolicy& policy = {}) const;

    // A LayerChecker of the stack as it stands, for checks on another thread. What it keeps,
    // and each of its checks' temporaries and candidate trees, come from `allocator`, which
    // must outlive it and its reports. Allocates.
    [[nodiscard]] LayerChecker checker(const Allocator& allocator = default_allocator()) const;

    [[nodiscard]] ConfigSnapshot snapshot() const;
    [[nodiscard]] Expected<void> restore(const ConfigSnapshot& snapshot);

private:
    struct Impl;
    friend class TweakBase;
    friend struct detail::ToolsAccess;

    void bind(TweakBase& t);
    // LayerOptions::checked: stamps a report on the layer `subject` names, and tells whether
    // one still holds for a push of it on top.
    void stamp(LayerReport& report, uint64_t subject, const LayerPolicy& policy) const noexcept;
    enum class Holding : uint8_t { No, Yes, LeftOut };  // LeftOut: and validators left to run
    [[nodiscard]] Holding holds(const LayerOptions& options, uint64_t subject,
                                bool on_top) const noexcept;
    // The scratch values `path` names (exactly, or one element of them): none, found, or
    // an element past the end of a scratch value, which hides the layers' longer one.
    enum class ScratchHit : uint8_t { None, Found, PastEnd };
    [[nodiscard]] ScratchHit scratch_values(const Path& path, Values& out) const noexcept;
    [[nodiscard]] bool edits_beneath(const Path& path) const noexcept;
    [[nodiscard]] Ref<Array> with_edits() const;  // a fresh merge, live edits included
    // `info` null: past the end.
    [[nodiscard]] static Error scratch_error(const Path& path, const detail::ConvertInfo* info);
    template <class T>
    [[nodiscard]] static Expected<T> from_scratch(const Path& path, ScratchHit hit,
                                                  const Values& v) {
        static constexpr detail::ConvertInfo kInfo = detail::convert_info<T>();
        T out{};
        if (hit == ScratchHit::Found && Convert<T>::from(v, out)) return out;
        return Unexpected(scratch_error(path, hit == ScratchHit::Found ? &kInfo : nullptr));
    }
    void stamp_visible() noexcept;
    [[nodiscard]] size_t history_size() const noexcept;
    [[nodiscard]] const ReloadRecord& history_at(size_t i) const noexcept;
    [[nodiscard]] size_t journal_size() const noexcept;
    [[nodiscard]] const JournalEntry& journal_at(size_t i) const noexcept;

    Ref<Array> root_;
    uint64_t epoch_ = 0;
    bool frozen_ = false;
    bool scratch_dirty_ = false;    // scratch changed since the last full merge
    bool visible_pending_ = false;  // a reload waits for mark_visible
    Impl* impl_;
};

[[nodiscard]] Config::Validator id_validator();

}  // namespace baker
