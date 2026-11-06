// Content tables: records designers author, read into structs the build generated
// from their shape. The build ran `baker_bake --views` over data/baker-project.dta
// (examples/CMakeLists.txt, baker_generate_views), which wrote game::Move, its reader and a
// digest of the shape's layout into the build tree.
//
//   baker_example_content_tables data/baker-project.dta
#include <baker/config.hpp>
#include <baker/image.hpp>
#include <baker/project.hpp>
#include <baker/shape.hpp>
#include <baker/table.hpp>
#include <baker/view_read.hpp>

#include "move_read.hpp"  // generated: the struct and its reader
#include "tables.hpp"     // generated: register_tables, a baked table per keyed view
#include "views.hpp"      // generated: every view, and kViews for the drift check

#include <baker/std_interop.hpp>

#include <cstdio>
#include <string>
#include <string_view>

namespace {

void print(std::string_view s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr, "usage: %s data/baker-project.dta [game.datb]\n", argv[0]);
        return 2;
    }
    auto project = baker::read_project(argv[1]);
    if (!project) {
        print(baker::to_string(project.error()));
        return 1;
    }
    baker::Config cfg({.tweaks = nullptr});
    if (auto r = game::register_tables(cfg); !r) {  // before the load: a table per keyed view
        print(baker::to_string(r.error()));
        return 1;
    }
    if (auto r = baker::open_project(cfg, *project); !r) {
        print(baker::to_string(r.error()));
        return 1;
    }

    // In development: are the compiled views the shapes the stack declares? A designer who
    // changes a field's type sees this before the programmer regenerates.
    {
        baker::shape::Report report;
        const auto roots = baker::shape::contract_roots(cfg, project->views.layers.items());
        const baker::shape::Set contract =
            baker::shape::contract_set(cfg.read_session(), roots.items(), report);
        baker::shape::check_views(contract, game::kViews, report);
        if (report.warnings.empty())
            std::printf("views: %zu, up to date\n", std::size(game::kViews));
        for (const baker::Error& w : report.warnings) {
            print(baker::to_string(w));
            print("\n");
        }
    }

    // One record, by path: every field typed, the absent ones their defaults.
    auto jab = baker::read_view<game::Move>(cfg, "fighters.kestrel.moves.jab");
    if (!jab) {
        print(baker::to_string(jab.error()));
        return 1;
    }
    std::printf("jab: startup %d, damage %.1f, button %s, cancels into", jab->startup, jab->damage,
                jab->button.c_str());
    for (const baker::Symbol into : jab->cancel_into) std::printf(" %s", into.c_str());
    std::printf("; icon %.*s\n", static_cast<int>(jab->icon.size()), jab->icon.data());

    // The whole table: one pass over each fighter's moves, as a game fills its own arrays
    // at load. The content layer's field is a member; a field marked (@view no) stays in
    // the tree, for the script that runs it.
    const baker::Ref<baker::Array> root = cfg.root();  // held: the views borrow from it
    int moves = 0;
    for (const baker::Node& f : root->try_find("fighters")->nodes().subspan(1)) {
        const baker::Array* fighter = f.try_array();
        const baker::Array* table = fighter ? fighter->try_find("moves") : nullptr;
        if (!table) continue;
        for (const baker::Node& m : table->nodes().subspan(1)) {
            const baker::Array* record = m.try_array();
            if (!record) continue;
            auto move = baker::read_view<game::Move>(*record);
            if (!move) {
                print(baker::to_string(move.error()));
                return 1;
            }
            ++moves;
            // A renamed record keeps its ID; a field spelled by its old name reads as the
            // field (wren's poke says windup).
            if (const baker::RecordId id = record->record_id(); !id.empty()) {
                char hex[baker::RecordId::kDigits];
                id.write(hex);
                std::printf("%s.%s is %.32s, startup %d\n", fighter->tag().c_str(),
                            record->tag().c_str(), hex, move->startup);
            }
            if (!move->hint.empty())
                std::printf("%s.%s: \"%.*s\"\n", fighter->tag().c_str(), record->tag().c_str(),
                            static_cast<int>(move->hint.size()), move->hint.data());
            if (record->try_find("on_hit"))
                std::printf("%s.%s runs a command on a hit\n", fighter->tag().c_str(),
                            record->tag().c_str());
        }
    }
    std::printf("%d moves read\n", moves);

    // The same records as a baked table: one block, read by index, by path and by ID, built
    // once per change instead of walked per read. Its rows are the structs above.
    const baker::Table<game::Move> table = baker::table<game::Move>(cfg);
    if (const auto i = table.index_of("kestrel.slam"))
        std::printf("table: %u moves; slam is row %u, damage %.0f, id %s\n", table.size(), *i,
                    table[*i].damage, table.id_of(*i).empty() ? "none" : "kept");
    if (const auto i = table.index_of("kestrel.heavy"))  // the alias still finds it
        std::printf("table: heavy is slam, row %u\n", *i);

    // Shipping: the contract layers and the tables baked into one image (the host kit's
    // `bake --baker-stack` does this on the game's binary), which a Config opens in place
    // of the layers' text; the local layer, above the contract, loads as today.
    if (argc == 3) {
        std::string image;
        if (auto r = baker::write_stack_image(cfg, project->views.layers.items(),
                                              baker::string_out(image));
            !r) {
            print(baker::to_string(r.error()));
            return 1;
        }
        if (std::FILE* f = std::fopen(argv[2], "wb")) {
            std::fwrite(image.data(), 1, image.size(), f);
            std::fclose(f);
        }
        baker::Config shipped({.tweaks = nullptr, .stack_image = argv[2]});
        if (auto r = game::register_tables(shipped); !r) {
            print(baker::to_string(r.error()));
            return 1;
        }
        if (auto r = baker::open_project(shipped, *project); !r) {
            print(baker::to_string(r.error()));
            return 1;
        }
        const baker::Table<game::Move> baked = baker::table<game::Move>(shipped);
        std::printf("image: %zu bytes, %u moves, jab cancels into %s\n", image.size(), baked.size(),
                    baked[0].cancel_into[0].c_str());
    }

    // A value of the wrong type is a located error naming the field, not a wrong struct.
    if (auto r = cfg.push_layer_text("agent", "(fighters (kestrel (moves (jab (startup fast)))))");
        !r) {
        print(baker::to_string(r.error()));
        return 1;
    }
    auto bad = baker::read_view<game::Move>(cfg, "fighters.kestrel.moves.jab");
    if (!bad) {
        print(baker::to_string(bad.error()));
        print("\n");
    }
    return 0;
}
