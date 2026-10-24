// Tweaks made from their descriptions (<baker/tools/described.hpp>), for a tool that checks
// data without the game.

#include <baker/config.hpp>
#include <baker/curve.hpp>
#include <baker/parse.hpp>
#include <baker/tools/described.hpp>
#include <baker/write.hpp>

#include "common/containers.hpp"
#include "data/text.hpp"

#include <algorithm>
#include <optional>

namespace baker::tools {

// --- Described tweaks ---

namespace {

// What a described tweak keeps: copies of its description, which TweakBase points into.
struct DescribedParts {
    explicit DescribedParts(const TweakDescription& d, std::span<const std::string_view> types,
                            std::span<const std::string_view> element_types)
        : path_text(d.path),
          type_text(d.type),
          element_text(d.element.type),
          desc_text(d.desc),
          kind_text(d.kind),
          group_text(d.group),
          counted(d.count),
          curve(d.type == "curve") {
        for (const std::string_view t : types) type_names.emplace_back(t);
        for (const std::string_view o : d.one_of) one_of_names.emplace_back(o);
        for (const TweakChoice& c : d.choices) choices.push_back({c.value, Symbol(c.name).str()});
        for (const std::string_view t : element_types) element_names.emplace_back(t);
        for (const Str& t : type_names) type_views.push_back(t.view());
        for (const Str& t : element_names) element_views.push_back(t.view());
        listed = {element_text.view(),
                  {element_views.data(), element_views.size()},
                  d.element.count,
                  d.element.length};
        for (const Str& o : one_of_names) one_of_views.push_back(o.view());
        described = {.range = d.range,
                     .step = d.step,
                     .desc = desc_text.view(),
                     .kind = kind_text.view(),
                     .group = group_text.view(),
                     .dev_only = d.dev_only,
                     .one_of = {one_of_views.data(), one_of_views.size()},
                     .choices = {choices.data(), choices.size()}};
    }
    Str path_text, type_text, element_text, desc_text, kind_text, group_text;
    Vec<Str> type_names, element_names, one_of_names;
    Vec<std::string_view> type_views, element_views, one_of_views;
    ListElement listed;        // a list's element, its type empty for any other value
    Vec<TweakChoice> choices;  // names interned
    TweakMeta described;
    ValueCount counted;  // how many values it takes
    bool curve;          // a curve (<baker/curve.hpp>): read as one, with no value types
    ValueList fallback, current;
    bool has_current = false;
};

class DescribedTweak final : private DescribedParts, public TweakBase {
public:
    DescribedTweak(const TweakDescription& d, std::span<const std::string_view> types,
                   std::span<const std::string_view> element_types, TweakRegistry& registry)
        : DescribedParts(d, types, element_types),
          TweakBase(path_text.c_str(), described, registry) {}
    ~DescribedTweak() { detach(); }
    DescribedTweak(const DescribedTweak&) = delete;
    DescribedTweak& operator=(const DescribedTweak&) = delete;

    // Reads the default; false if it does not read as the value types.
    bool start(std::span<const Node> nodes) {
        if (!assign(Values(nodes, nullptr), false)) return false;
        for (const Node& n : nodes) fallback.push(n);
        attach();
        return true;
    }
    void values(ValueList& out) const override {
        for (const Node& n : (has_current ? current : fallback).nodes()) out.push(n);
    }
    void default_values(ValueList& out) const override {
        for (const Node& n : fallback.nodes()) out.push(n);
    }

private:
    bool assign(const Values& v, bool apply) override {
        // A curve as one; a list by its elements; any other value by its value types.
        Curve c;
        if (curve ? !!read_curve(v, c)
            : !listed.type.empty()
                ? !detail::reads_list(listed, v)
                : !detail::reads_as({type_views.data(), type_views.size()}, counted, v))
            return false;
        if (apply) {
            current = ValueList();
            const size_t n = counted == ValueCount::Exact && listed.type.empty() && !curve
                                 ? type_views.size()
                                 : v.size();
            for (size_t i = 0; i < n; ++i) current.push(v[i]);
            has_current = true;
        }
        return true;
    }
    void reset() override { has_current = false; }
    [[nodiscard]] detail::ConvertInfo info() const noexcept override {
        using K = detail::ConvertKind;
        if (curve || type_views.size() != 1 || counted != ValueCount::Exact || !listed.type.empty())
            return {type_text.view(), K::Compound};
        const std::string_view t = type_views[0];
        if (t == "float32" || t == "float") return {type_text.view(), K::Float};
        if (t.find("int") != std::string_view::npos) return {type_text.view(), K::Int};
        return {type_text.view(), K::Scalar};
    }
    [[nodiscard]] std::span<const std::string_view> types() const noexcept override {
        return {type_views.data(), type_views.size()};
    }
    [[nodiscard]] ValueCount count() const noexcept override { return counted; }
    [[nodiscard]] ListElement element() const noexcept override { return listed; }
};

}  // namespace

struct DescribedTweaks::Impl {
    TweakRegistry* registry;
    Vec<DescribedTweak*> tweaks;
};

DescribedTweaks::DescribedTweaks(TweakRegistry& registry)
    : impl_(detail::create<Impl>(default_allocator(), Impl{&registry, {}})) {}

DescribedTweaks::~DescribedTweaks() {
    for (DescribedTweak* t : impl_->tweaks) detail::destroy(default_allocator(), t);
    detail::destroy(default_allocator(), impl_);
}

size_t DescribedTweaks::size() const noexcept {
    return impl_->tweaks.size();
}

Expected<void> DescribedTweaks::add(const TweakDescription& d) {
    auto refuse = [&](const Str& why) {
        return Unexpected(make_error(ErrorCode::Type, "tweak '" + Str(d.path) + "': " + why));
    };
    if (d.path.empty()) return refuse("it has no path");
    // A type of Baker's is its own one value type; a game's names its own.
    const std::string_view own[1] = {d.type};
    const std::span<const std::string_view> types = !d.values.empty() ? d.values
                                                    : detail::value_type_known(d.type)
                                                        ? std::span(own)
                                                        : std::span<const std::string_view>();
    const bool list = !d.element.type.empty();
    // A list's element likewise: of Baker's type, or of the game's with its value types.
    const std::string_view element_own[1] = {d.element.type};
    const std::span<const std::string_view> element_types =
        !d.element.values.empty()                          ? d.element.values
        : list && detail::value_type_known(d.element.type) ? std::span(element_own)
                                                           : std::span<const std::string_view>();
    if (list && element_types.empty())
        return refuse("its element type '" + Str(d.element.type) +
                      "' is not one of Baker's, and its description gives no value types for it");
    const bool curve = d.type == "curve";
    if (types.empty() && !list && !curve)
        return refuse("its type '" + Str(d.type) +
                      "' is not one of Baker's, and its description gives no value types");
    for (const auto set : {types, element_types})
        for (const std::string_view t : set)
            if (!detail::value_type_known(t))
                return refuse("value type '" + Str(t) + "' is not one of Baker's");
    // The default, as Baker text: one value bare, several in parentheses.
    auto tree =
        read_string(d.default_text.empty() ? std::string_view("0") : d.default_text, "default");
    if (!tree) return refuse("its default does not read: " + Str(tree.error().message()));
    std::span<const Node> nodes = (*tree)->nodes();
    // Several values, or a list, written in parentheses: (1 2), (fast loud), ((0 0) (1 0)).
    if (nodes.size() == 1 && (types.size() > 1 || d.count != ValueCount::Exact || list || curve))
        if (const Array* a = nodes[0].try_array()) nodes = a->nodes();
    auto* t = detail::create<DescribedTweak>(default_allocator(), d, types, element_types,
                                             *impl_->registry);
    if (!t->start(nodes)) {
        detail::destroy(default_allocator(), t);
        return refuse("its default (" + Str(d.default_text) + ") is not " + Str(d.type));
    }
    impl_->tweaks.push_back(t);
    return {};
}

}  // namespace baker::tools
