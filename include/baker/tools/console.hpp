#pragma once

// The console's config.* commands. Part of baker::tools, which a game links only with the
// host kit; the kit's Live binds them.

#include <baker/config.hpp>
#include <baker/script.hpp>

namespace baker::tools {

// The Config's tooling as script functions (config.get, curve.sample, config.source,
// config.explain, config.layers, config.effect, config.diff; in development builds also config.set,
// config.clear_scratch, config.toggle, config.reload, config.unsaved, config.journal, config.loop,
// and config.snapshot and config.restore by name), documented, so the reference lists them and
// check() checks calls to them. config.write_back and config.promote write files, so they are bound
// only when allowed. Results are data: through eval_request, a tool reads them as JSON. `cfg` must
// outlive the Context's use of them.
struct ConfigToolOptions {
    bool allow_write = false;  // also bind config.write_back and config.promote
};
[[nodiscard]] Expected<void> bind_config_tools(Context& ctx, Config& cfg,
                                               const ConfigToolOptions& options = {});

}  // namespace baker::tools
