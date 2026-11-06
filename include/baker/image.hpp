#pragma once

// A stack image: one file holding a project's contract layers as trees and the
// baked tables built over them, for a shipped game to load in place of the layers' text.
//
//   // the build, on the game's own binary (the host kit's `bake --baker-stack game.datb`):
//   baker::write_stack_image(cfg, contract_layers, sink);
//   // the game:
//   baker::Config cfg({.stack_image = "game.datb"});
//   game::register_tables(cfg);
//   baker::open_project(cfg, project);   // the image's layers from the image, the rest as today
//
// The image holds each contract layer's tree as its cache would (cache.hpp), in the
// project's order, and each registered table's block with what fixes it up: pointers as
// offsets, symbols by name. A table whose view the game no longer compiles alike (another
// digest, size or fix-up list) is skipped with a warning to the Config's handler, and that
// table is built from the tree as a development build builds it. Layers above the
// contract stack (mods, overlays) push as today, and rebuild the tables they touch.

#include <baker/config.hpp>
#include <baker/expected.hpp>
#include <baker/symbol.hpp>
#include <baker/text.hpp>

#include <span>

namespace baker {

// Writes the image of `cfg`'s layers named in `layers`, bottom to top, and of every table
// registered with it, to `out`. A named layer the Config lacks, or one with no tree, is an
// error.
[[nodiscard]] Expected<void> write_stack_image(const Config& cfg, std::span<const Symbol> layers,
                                               TextSink out);

}  // namespace baker
