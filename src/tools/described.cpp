// Types and objects declared from the reference (<baker/tools/described.hpp>), for a tool that
// checks data without the game.

#include <baker/tools/described.hpp>

#include "access.hpp"

namespace baker::tools {

Expected<void> declare_type(Context& ctx, Symbol name, std::span<const Symbol> bases,
                            std::span<const MethodDescription> methods) {
    Context::Impl& impl = ctx.impl();
    auto refuse = [&](std::string_view why) {
        return Unexpected(
            make_error(ErrorCode::Type, "cannot declare type " + quoted(name) + ": " + Str(why)));
    };
    if (name.empty()) return refuse("it has no name");
    if (impl.type_named(name))
        return Unexpected(make_error(ErrorCode::Refused, "cannot declare type " + quoted(name) +
                                                             ": this Context already has a "
                                                             "type of that name"));
    Vec<const detail::TypeRecord*> found(impl.alloc);
    for (const Symbol b : bases) {
        const detail::TypeRecord* t = impl.type_named(b);
        if (!t) return refuse("its base " + quoted(b) + " is not a type this Context knows");
        found.push_back(t);
    }
    Vec<Vec<detail::Overload>> built(impl.alloc);
    for (const MethodDescription& m : methods) {
        built.emplace_back(impl.alloc);
        if (auto why = detail::build_described(impl, m.name, m.overloads, built.back()))
            return refuse("method " + quoted(m.name) + ": " + *why);
    }
    detail::TypeRecord* type = detail::new_type_record(name.str());
    for (const detail::TypeRecord* b : found) detail::inherit(*type, *b);
    for (size_t i = 0; i < methods.size(); ++i)
        for (detail::Overload& o : built[i])
            (void)detail::add_method(*type, methods[i].name, std::move(o));
    type->sort();
    impl.declared_types.push_back(type);
    detail::ToolsAccess::add_type(ctx, *type);
    return {};
}

bool has_type(const Context& ctx, Symbol name) noexcept {
    return ctx.impl().type_named(name) != nullptr;
}

Expected<void> declare_object(Context& ctx, Symbol name, Symbol type) {
    Context::Impl& impl = ctx.impl();
    auto refuse = [&](std::string_view why) {
        return Unexpected(
            make_error(ErrorCode::Type, "cannot declare object " + quoted(name) + ": " + Str(why)));
    };
    if (!impl.type_named(type))
        return refuse("its type " + quoted(type) + " is not a type this Context knows");
    if (impl.find(name) || impl.slot(impl.named(name)) || !impl.declared_object(name).empty())
        return Unexpected(make_error(ErrorCode::Refused, "cannot declare object " + quoted(name) +
                                                             ": it is already a function or an "
                                                             "object"));
    impl.declared_objects.emplace_back(name, type);
    return {};
}

}  // namespace baker::tools
