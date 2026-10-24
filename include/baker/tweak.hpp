#pragma once

// Tweak<T>: a constant that is configured, hot-reloadable, and editable, in one line.
//
//   // Before:
//   const float kBoxWidth = 2.0f;
//   // After:
//   static baker::Tweak<float> kBoxWidth{"box.width", 2.0f,
//                                        {.range = {0, 10}, .step = 1, .desc = "Box width"}};
//
//   float w = kBoxWidth;   // one relaxed atomic load
//
// The constructor registers the tweak with a TweakRegistry (the global one unless another
// is passed). Registration allocates nothing and does not depend on static-initialization
// order. The Config attached to the registry binds the tweak when it loads, and rebinds it
// on every reload or layer change; a tweak registered after loading binds at once. If the
// path is absent the default stands and bound() is false. That is never an error.
//
// Declare tweaks at namespace scope: a function-local static adds a guard check to every
// read. A T whose std::atomic is lock-free is read with one relaxed load; any other T is
// read through get(), whose reference stays valid until the next reload. Values change only
// when the Config does, on the thread that changes it (single-threaded mutation).
//
// Metadata may be given in code (TweakMeta) or in the data file, and the data file wins:
//
//   (box (width 2 (range 0 10) (step 1) (desc "Box width") (note "try 2.5 on handheld")))
//
// `note` is data-only: a remark from whoever tuned the value for whoever tunes it next.
// `kind` is a presentation hint for tools (color, angle, percent, path, ...); when absent,
// a 3- or 4-element value ranged 0..1 is reported as "color".
//
// Under BAKER_SHIPPING a tweak marked dev_only ignores the data and keeps its default, and
// set() is refused: values are frozen at what was loaded.

#include <baker/array.hpp>
#include <baker/path.hpp>

#include <baker/callback.hpp>
#include <baker/list.hpp>
#include <baker/text.hpp>

#include <atomic>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace baker {

class Config;
class TweakRegistry;
namespace detail {
struct ToolsAccess;  // baker::tools' way to the internals (src/tools/access.hpp)
// read_consistent's wait while a change is under way: a spin, then a yield. Out of line, so
// this header needs no <thread>.
void wait_a_moment(unsigned tries) noexcept;
}  // namespace detail
struct Source;

struct TweakRange {
    double min = 0;
    double max = 0;
    [[nodiscard]] bool empty() const noexcept { return !(min < max); }
};

// One value an integer tweak may take, and its name for a panel: {0, "easy"}.
struct TweakChoice {
    int64_t value = 0;
    std::string_view name{};
};

// Every member has its own initializer, so that clang's -Wmissing-field-initializers
// accepts a designated initializer naming only some: {.range = {0, 10}, .step = 1}.
struct TweakMeta {
    TweakRange range{};
    double step = 0;  // 0: none
    std::string_view desc{};
    std::string_view kind{};
    std::string_view group{};  // default: the path without its last segment
    bool dev_only = false;     // under BAKER_SHIPPING, ignore the data and keep the default
    // The symbols a symbol tweak's value may be, for a panel or an editor to offer, in order:
    //   static constexpr std::string_view kButtons[] = {"light", "heavy", "kick"};
    //   Tweak<Symbol> button{"moves.jab.button", Symbol("light"), {.one_of = kButtons}};
    // A string tweak takes a list too, of the strings it may be. The data may give its own,
    // (one_of ...) after the value: symbols, or strings for a string tweak. check_tweaks
    // holds the data to it, and a shape's (one_of ...) is what check() enforces on a field.
    std::span<const std::string_view> one_of{};
    // The values an integer tweak may take, each with a name for a panel:
    //   static constexpr baker::TweakChoice kLevels[] = {{0, "easy"}, {1, "normal"}};
    //   Tweak<int> difficulty{"game.difficulty", 1, {.choices = kLevels}};
    // The data may give its own: (one_of (0 easy) (1 normal)), or bare numbers.
    std::span<const TweakChoice> choices{};
};

class TweakRegistry;

// What check_tweaks found: values the tweaks cannot take (errors), and data the tweaks do not
// match (warnings), each located where the data has a place.
struct TweakFindings {
    List<Error> errors;
    List<Error> warnings;
};

// Holds `root` (a Config's merged tree) to the tweaks of `tweaks`, as check() holds data to
// shapes; it changes no tweak. Errors: a value that does not convert to its tweak's type, a
// number outside its range, a symbol or string outside its one_of, an integer outside its
// choices (the data's metadata, else the code's). Warnings: a tweak whose path the data lacks, so
// it keeps its default; a key no tweak reads beside the tweaks of its group, as a misspelled key
// looks. Only groups below the top level are read for unknown keys, so structured data beside them
// stays quiet.
[[nodiscard]] TweakFindings check_tweaks(const Array& root, const TweakRegistry& tweaks);

// A value written with metadata after it, read as a tweak's is: (width 2 (range 0 10)
// (desc "w")) is the value 2 and the metadata (range 0 10) (desc "w"). Only the trailing
// arrays tagged range, step, desc, note, kind, group or one_of are metadata, so in
// (ball (kind ball) (pos 1 2)) everything is value: its last array is not one of them.
// For a tool presenting data as Baker presents tweaks; it allocates nothing.
struct ValueMetadata {
    Values values;
    std::span<const Node> metadata;  // each an array, in the order written
};
[[nodiscard]] ValueMetadata split_metadata(const Values& all) noexcept;
// The kind a value is presented as: its (kind ...) metadata, else `kind`; else "color" for 3
// or 4 numbers whose range, the metadata's (range ...) else `range`, is 0 to 1; else empty.
// The rule TweakBase::kind() applies, for any value.
[[nodiscard]] std::string_view presented_kind(const Values& all, std::string_view kind = {},
                                              TweakRange range = {}) noexcept;

class TweakBase {
public:
    TweakBase(const TweakBase&) = delete;
    TweakBase& operator=(const TweakBase&) = delete;

    [[nodiscard]] std::string_view path() const noexcept { return path_; }
    [[nodiscard]] const TweakMeta& code_meta() const noexcept { return meta_; }
    // True while the config supplies the value; false while the default stands.
    [[nodiscard]] bool bound() const noexcept { return bound_ != nullptr; }
    [[nodiscard]] TweakRegistry& registry() const noexcept { return *registry_; }
    // The value's type, as errors and the reference name it: "float32", "int", "vec2"...
    [[nodiscard]] std::string_view type_name() const noexcept { return info().name; }
    // Baker's type of each of the value's values: {"float32"} for a float, {"float32",
    // "float32"} for a vec2 whose Convert declares `values`; empty when the type does not say.
    [[nodiscard]] std::span<const std::string_view> value_types() const noexcept { return types(); }
    // How many values it takes, beside value_types(): exactly them unless the type says.
    [[nodiscard]] ValueCount value_count() const noexcept { return count(); }
    // A list's element and length (Convert<T>::element); an empty type for any other value.
    [[nodiscard]] ListElement list_element() const noexcept { return element(); }

    // Metadata, the data file's winning over the code's.
    [[nodiscard]] TweakRange range() const;
    [[nodiscard]] double step() const;
    [[nodiscard]] std::string_view desc() const;
    [[nodiscard]] std::string_view note() const;
    [[nodiscard]] std::string_view kind() const;
    [[nodiscard]] std::string_view group() const;
    // The symbols or strings the value may be: the data's (one_of ...), else the code's;
    // empty: any.
    [[nodiscard]] List<Symbol> one_of() const;
    // The integers the value may be, with their names: the data's (one_of ...), else the
    // code's; empty: any. A name is interned, so it lives as long as the program.
    [[nodiscard]] List<TweakChoice> choices() const;

    // Appends the current value as data to `out`, for a generic tuning panel (nothing if T
    // cannot be written).
    virtual void values(ValueList& out) const = 0;
    // Appends the code's default as data, as values() does the current value.
    virtual void default_values(ValueList& out) const = 0;
    // Sets the value from data, as a panel would; see Tweak<T>::set.
    [[nodiscard]] Expected<void> set_values(std::span<const Node> values);

    // Where the current value came from: layer, file, and line (Config::source).
    [[nodiscard]] Expected<Source> source() const;
    // Writes the current value back to the file it came from (Config::write_back).
    [[nodiscard]] Expected<void> write_back();

    // Called after the value changes, on the thread that changed it. A callback may set
    // other tweaks, whose callbacks run in turn; a change it makes to this tweak, or to one
    // whose callback sets this tweak back, does not notify this tweak again, so callbacks
    // cannot loop. A callback added from inside a callback runs from the next change.
    void on_change(Callback<void()> callback);

protected:
    TweakBase(const char* path, const TweakMeta& meta, TweakRegistry& registry) noexcept
        : path_(path), meta_(meta), registry_(&registry) {}
    ~TweakBase() = default;

    // Registers with the registry, binding at once if its config is loaded. Called by
    // Tweak<T> once its value exists.
    void attach();
    void detach() noexcept;

private:
    friend class TweakRegistry;
    friend class Config;
    friend struct detail::ToolsAccess;
    friend TweakFindings check_tweaks(const Array& root, const TweakRegistry& tweaks);

    // Reads the values as T and, if `apply`, stores them; false if they do not convert.
    virtual bool assign(const Values& values, bool apply) = 0;
    virtual void reset() = 0;  // back to the default
    [[nodiscard]] virtual detail::ConvertInfo info() const noexcept = 0;
    [[nodiscard]] virtual std::span<const std::string_view> types() const noexcept = 0;
    [[nodiscard]] virtual ValueCount count() const noexcept = 0;
    [[nodiscard]] virtual ListElement element() const noexcept = 0;

    // Rebinds against `root` (a Config's merged tree); true if the value changed. Reports
    // a value that does not convert through `warn`.
    bool bind(const Array& root, const Callback<void(const Error&)>& warn);
    bool unbind();
    void notify();
    const Path& parsed();
    [[nodiscard]] const Array* meta_array(Symbol key) const;

    const char* path_;
    TweakMeta meta_;
    TweakRegistry* registry_;
    TweakBase* prev_ = nullptr;
    TweakBase* next_ = nullptr;
    // Made on first use, not at registration, so registering allocates nothing; freed by
    // detach(). Their types are defined in tweak.cpp.
    struct Extra;
    Extra* extra_ = nullptr;
    Ref<Array> bound_;                // the array holding the value; null when unbound
    uint32_t first_ = 0, count_ = 0;  // the value's nodes within bound_
};

// The set of live tweaks, as an intrusive list: adding one allocates nothing. The global
// registry is constant-initialized, so tweaks may register from any static initializer.
class TweakRegistry {
public:
    constexpr TweakRegistry() noexcept = default;
    TweakRegistry(const TweakRegistry&) = delete;
    TweakRegistry& operator=(const TweakRegistry&) = delete;

    [[nodiscard]] static TweakRegistry& global() noexcept;

    // Visits every registered tweak. `f` must not create or destroy tweaks.
    template <class F>
    void each(F&& f) const {
        lock();
        for (TweakBase* t = head_; t; t = t->next_) f(*t);
        unlock();
    }
    [[nodiscard]] size_t size() const noexcept;
    // The Config that binds these tweaks, or null.
    [[nodiscard]] Config* config() const noexcept { return config_; }

    // Runs `fn`, which reads tweaks, so that what it read agrees: a reload, a live edit or a
    // layer change updates tweaks one at a time on the game's thread, and a worker reading
    // several mid-update could see the new gravity with the old jump speed. `fn` runs again
    // if the tweaks changed while it ran, so it must only read:
    //   float g, j;
    //   baker::TweakRegistry::global().read_consistent([&] { g = gravity; j = jump_speed; });
    // It covers atomic tweaks only, those whose T is trivially copyable and lock-free (float,
    // int, bool, Symbol, a small struct): a std::string or std::vector tweak is never safe to
    // read off the game's thread, inside it or not. While a change is under way it spins
    // briefly, then yields. Without a change it costs two loads and a fence around `fn`.
    template <class F>
    void read_consistent(F&& fn) const {
        for (unsigned tries = 0;; ++tries) {
            const uint64_t before = generation_.load(std::memory_order_acquire);
            if (before & 1u) {  // a change is under way
                detail::wait_a_moment(tries);
                continue;
            }
            fn();
            std::atomic_thread_fence(std::memory_order_acquire);
            if (generation_.load(std::memory_order_relaxed) == before) return;
        }
    }

private:
    friend class TweakBase;
    friend class Config;
    void lock() const noexcept;
    void unlock() const noexcept;
    // A change to its tweaks, on the game's thread, for read_consistent: the generation is
    // odd from begin_change() to end_change(). A seqlock's writer: the stores between are
    // the tweaks' own relaxed stores. Changes never nest: notifications run after end.
    void begin_change() noexcept {
        generation_.store(generation_.load(std::memory_order_relaxed) + 1,
                          std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
    }
    void end_change() noexcept {
        generation_.store(generation_.load(std::memory_order_relaxed) + 1,
                          std::memory_order_release);
    }

    mutable std::atomic_flag lock_;
    std::atomic<uint64_t> generation_{0};  // odd while its tweaks are changing
    TweakBase* head_ = nullptr;
    Config* config_ = nullptr;
};

// Every tweak in `tweaks` as JSON (schema baker.tweaks/1.5, docs/reference-json.md), sorted by
// path: its type and value types, default and current value, metadata (the data's winning
// over the code's), and, while a config supplies it, where the value came from. For tools
// and agents.
void write_json(const TweakRegistry& tweaks, TextSink out);

namespace detail {

template <class T>
[[nodiscard]] constexpr bool atomic_tweak() noexcept {
    if constexpr (std::is_trivially_copyable_v<T>)
        return std::atomic<T>::is_always_lock_free;
    else
        return false;
}
}  // namespace detail

template <class T>
class Tweak final : public TweakBase {
    static constexpr bool kAtomic = detail::atomic_tweak<T>();

public:
    // `path` must outlive the tweak: a string literal, as tweaks in code are written.
    Tweak(const char* path, T fallback, const TweakMeta& meta = {},
          TweakRegistry& registry = TweakRegistry::global())
        : TweakBase(path, meta, registry), default_(fallback), value_(default_) {
        attach();
    }
    // A path made at runtime, by a scripting host or an editor: a symbol is interned for
    // the program's life, so it outlives the tweak. Each new path stays interned, so make
    // such tweaks once, at startup or at a load, not in a loop. `meta`'s strings must
    // outlive the tweak too; a symbol's str() does.
    Tweak(Symbol path, T fallback, const TweakMeta& meta = {},
          TweakRegistry& registry = TweakRegistry::global())
        : Tweak(path.c_str(), std::move(fallback), meta, registry) {}
    ~Tweak() { detach(); }

    operator T() const noexcept {  // NOLINT(google-explicit-constructor)
        if constexpr (kAtomic)
            return value_.load(std::memory_order_relaxed);
        else
            return value_;
    }
    [[nodiscard]] T value() const noexcept { return *this; }
    // For a T that is not read atomically: valid until the next reload.
    [[nodiscard]] const T& get() const noexcept
        requires(!kAtomic)
    {
        return value_;
    }
    [[nodiscard]] const T& default_value() const noexcept { return default_; }

    // A live edit: through the Config, so it shows in source() as layer "live" and can be
    // written back; without a Config, the value changes directly. Refused under
    // BAKER_SHIPPING. Needs Convert<T>::to.
    [[nodiscard]] Expected<void> set(const T& v) {
        ValueList list;
        Convert<T>::to(v, list);
        return set_values(list.nodes());
    }

    void values(ValueList& out) const override {
        if constexpr (requires {
                          Convert<T>::to(std::declval<const T&>(), std::declval<ValueList&>());
                      })
            Convert<T>::to(value(), out);
    }
    void default_values(ValueList& out) const override {
        if constexpr (requires {
                          Convert<T>::to(std::declval<const T&>(), std::declval<ValueList&>());
                      })
            Convert<T>::to(default_, out);
    }

private:
    bool assign(const Values& values, bool apply) override {
        T v{};
        if (!Convert<T>::from(values, v)) return false;
        if (apply) store(std::move(v));
        return true;
    }
    void reset() override { store(default_); }
    [[nodiscard]] detail::ConvertInfo info() const noexcept override {
        return detail::convert_info<T>();
    }
    [[nodiscard]] std::span<const std::string_view> types() const noexcept override {
        return detail::value_types<T>();
    }
    [[nodiscard]] ValueCount count() const noexcept override { return detail::value_count<T>(); }
    [[nodiscard]] ListElement element() const noexcept override {
        return detail::list_element<T>();
    }
    void store(T v) {
        if constexpr (kAtomic)
            value_.store(v, std::memory_order_relaxed);
        else
            value_ = std::move(v);
    }

    T default_;
    std::conditional_t<kAtomic, std::atomic<T>, T> value_;
};

}  // namespace baker
