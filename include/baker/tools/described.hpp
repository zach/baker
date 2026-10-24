#pragma once

// The game's vocabulary declared from its reference, for a tool that checks data without
// the game: a language server, a CI checker, an agent's pre-flight check. Tweaks, types and
// objects; functions are Context::declare. Part of baker::tools.

#include <baker/script.hpp>
#include <baker/tweak.hpp>

namespace baker::tools {

// A tweak as the reference describes it (baker.tweaks), for a tool that has the game's
// reference but not its code.
struct TweakDescription {
    std::string_view path{};
    std::string_view type{};  // Baker's ("float32", "symbol") or the game's own ("vec2")
    // Baker's type of each value, as the reference's "values" lists them: needed for a type
    // of the game's own, implied for one of Baker's.
    std::span<const std::string_view> values{};
    ValueCount count = ValueCount::Exact;  // how many values it takes, as the reference says
    // A list's element and length, as the reference's "element" and "length" give them: each
    // element is held to them, which a list of the game's own type ("vec2") needs.
    ListElement element{};
    std::string_view default_text{};  // the code's default, as Baker text: "2.5", "(0 1)"
    TweakRange range{};
    double step = 0;
    std::string_view desc{};
    std::string_view kind{};
    std::string_view group{};
    bool dev_only = false;
    std::span<const std::string_view> one_of{};
    std::span<const TweakChoice> choices{};
};

// Tweaks a tool makes from their descriptions, registered with `registry`, so that
// check_tweaks holds data to them as the game holds data to its own: a language server, a CI
// checker, an agent's pre-flight check. No code reads them; they hold copies of what they
// are given, and unregister when the set goes.
class DescribedTweaks {
public:
    explicit DescribedTweaks(TweakRegistry& registry);
    ~DescribedTweaks();
    DescribedTweaks(const DescribedTweaks&) = delete;
    DescribedTweaks& operator=(const DescribedTweaks&) = delete;

    // The same call for every type. An error naming the tweak if its type is not Baker's and
    // it gives no value types, if a value type is not one of Baker's, or if its default does
    // not read as them.
    [[nodiscard]] Expected<void> add(const TweakDescription& tweak);
    [[nodiscard]] size_t size() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

// A method of a type described as data, for declare_type: its name and overloads.
struct MethodDescription {
    Symbol name;
    std::span<const OverloadDescription> overloads;
};

// Declares a type from its description (the reference's `types`), for a tool that checks
// without the game: its bases, each a type `ctx` already knows (native or declared), and its
// methods. check() then holds a call on an object of the type to its methods and its bases',
// as it does a native type's. The Context owns it; its methods have no body. An error naming
// the type, and nothing declared, for a name that is already a type (ErrorCode::Refused), a
// base it does not know, or a method Context::declare() would refuse (ErrorCode::Type).
[[nodiscard]] Expected<void> declare_type(Context& ctx, Symbol name, std::span<const Symbol> bases,
                                          std::span<const MethodDescription> methods);
// Names an object of `type` (the reference's `objects`) for check(): {name method ...} is
// checked against the type. Nothing is added to run: a call on it fails as a call on an
// undefined name does. An error for a type it does not know (ErrorCode::Type), or a name
// that is already a function or an object (ErrorCode::Refused).
[[nodiscard]] Expected<void> declare_object(Context& ctx, Symbol name, Symbol type);
// Whether `ctx` knows a type of that name: native, or declared.
[[nodiscard]] bool has_type(const Context& ctx, Symbol name) noexcept;

}  // namespace baker::tools
