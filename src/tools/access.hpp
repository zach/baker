#pragma once

// baker::tools' way to the core's internals: the one friend of Config and Context, as
// SessionAccess is LoadSession's. Private to Baker: not installed.

#include "data/config_impl.hpp"
#include "script/internal.hpp"

namespace baker::detail {

struct ToolsAccess {
    static const Config::Impl& impl(const Config& cfg) noexcept { return *cfg.impl_; }
    static const Ref<Array>& root(const Config& cfg) noexcept { return cfg.root_; }
    static const Path& parsed(TweakBase& tweak) { return tweak.parsed(); }
    static void add_type(Context& ctx, const TypeRecord& type) { ctx.add_type(type); }
};

}  // namespace baker::detail
