#pragma once

// baker::shape: the expected structure of a data domain,
// declared in data, by whichever layer owns the vocabulary.
//
//   (shapes
//     (boss_phase (at bosses * phases)
//       (enter_at float (range 0 1) (kind percent) (desc "HP fraction that starts it"))
//       (attacks  (list symbol))
//       (cadence  float (range 0.2 5) (optional))))
//
// A shape governs every array tagged with its name, or, given (at ...), those whose
// ancestors' tags from the root are the ones listed (`*` matches any). A shape declared
// (keyed) governs every child of the arrays at its (at ...) path, whatever the child's tag:
// keyed data, as (fighters (kestrel (moves (jab ...) (kick ...)))), which layers merge by
// name.
//
//   (shapes (move (at fighters * moves) (keyed) (startup int (range 1 60))))
//
// A field has a type (int, float, bool, symbol, string, any, curve, (list T), another shape's
// name, or (key_of TABLE)) and the tweak metadata; a symbol field may list the
// symbols it allows, (button symbol (one_of light heavy kick special)). A curve field's values
// are a curve (<baker/curve.hpp>), and its (range lo hi) holds every point's y.
//
// (key_of TABLE) holds a symbol to the keys of a keyed table: (weapon (key_of weapons)) in
// a unit names a child of (weapons (laser ...) (rifle ...)). TABLE is a dotted path, looked
// up by its first segment outward from the record that refers to it, the nearest enclosing
// table first and the root last, so (next (key_of moves)) in a fighter's move names that
// fighter's own move.
//
// check() reports a field of the wrong type, out of range or outside its list, a key its
// table lacks, or a required field that is missing, as an error; a field the shape does not
// declare is a warning, so adding data never requires touching its shape first. Commands
// and variables are not checked: their values are unknown until run.
//
//   cfg.add_validator(baker::shape::validator(), {.any_thread = true});  // at every merge
//
// Shapes are opt-in: unshaped data works everywhere. The library depends only on
// baker::data.

#include <baker/array.hpp>
#include <baker/config.hpp>
#include <baker/error.hpp>
#include <baker/list.hpp>
#include <baker/symbol.hpp>
#include <baker/text.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace baker::shape {

enum class Type : uint8_t { Any, Int, Float, Bool, Symbol, String, List, Shape, Key, Curve };

struct Field {
    baker::Symbol name;
    Type type = Type::Any;
    Type element = Type::Any;   // a List's elements
    baker::Symbol shape;        // a Shape's, or a List of shapes', name
    List<baker::Symbol> table;  // a Key's, or a List of keys', table: a path, as written
    bool optional = false;
    bool ranged = false;
    double min = 0, max = 0;
    double step = 0;  // 0: none
    baker::Symbol kind;
    std::string_view desc, note;  // in the declaration, which the Set keeps
    // (one_of light heavy kick): the symbols a symbol field's value may be, in the order
    // written; a field declared again adds to it. Empty: any symbol.
    List<baker::Symbol> one_of;
    baker::Symbol file;
    int32_t line = 0;
};

// (rule "message" {expression} (warn)): what must hold between a record's values. The
// shape library reads rules; baker::tools evaluates them (tools::check_rules), since this
// library cannot run script.
struct Rule {
    std::string_view message;  // in the declaration, which the Set keeps; names the rule
    Node expression;           // as written: $field reads the record's field
    bool warn = false;         // a failure is a warning, not an error
    baker::Symbol file;
    int32_t line = 0;
};

struct Shape {
    baker::Symbol name;
    List<baker::Symbol> at;  // ancestors' tags from the root; empty: anywhere
    bool keyed = false;      // governs every child at `at`, whatever its tag
    List<Field> fields;
    List<Rule> rules;
    baker::Symbol file;
    int32_t line = 0;

    [[nodiscard]] const Field* field(baker::Symbol field_name) const noexcept;
};

struct Report {
    List<Error> errors;
    List<Error> warnings;  // fields a shape does not declare

    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

// The shapes read from trees, for check() and for generated tuning panels (shapes()).
class Set {
public:
    Set();
    ~Set();
    Set(const Set& other);
    Set& operator=(const Set& other);
    Set(Set&& other) noexcept;
    Set& operator=(Set&& other) noexcept;

    // Reads every (shapes ...) at the top of `tree`. A field declared again takes the new
    // type and whatever metadata the new declaration gives, as Config's merge of two layers
    // would. A malformed declaration is an error in `report`.
    //
    // With `packs`, harder packs' declarations hold: a field a softer pack declares
    // again may narrow a range or add metadata, but one that widens or drops a range, changes
    // the type, or makes a required field optional is a warning in `report`, and the harder
    // declaration stays. Files that declare no pack are unconstrained, as in the one-way rule.
    void add(const Array& tree, Report& report, const LoadSession* packs = nullptr);
    [[nodiscard]] std::span<const Shape> shapes() const noexcept;
    [[nodiscard]] const Shape* find(baker::Symbol name) const noexcept;

    struct State;

private:
    State* st_;
};

// Checks every array of `tree` that a shape of `shapes` governs, into `report`. With
// `packs`, a key a harder pack's field names from a softer pack's table is a warning, as the
// one-way rule warns of a call.
void check(const Array& tree, const Set& shapes, Report& report,
           const LoadSession* packs = nullptr);
// Calls `visit` with each array of `tree` that a shape with rules governs (as check() finds
// them, by tag or at a keyed shape's path), the shape, and the array's path from the root:
// what tools::check_rules evaluates each rule over.
void each_ruled(
    const Array& tree, const Set& shapes,
    const Callback<void(const Array& record, const Shape& shape, std::string_view path)>& visit);
// For Config::add_validator: reads the shapes each layer of the stack declares, bottom to
// top, with the stack's session's packs (so harder packs' shapes hold), and checks the
// merged root against them, reporting errors and warnings to the warning handler. It reads
// only its stack, so it may run on any thread:
//   cfg.add_validator(baker::shape::validator(), {.any_thread = true});
// It does not evaluate rules: add tools::rules_validator(ctx) for them. A build without
// baker::script warns once that the shapes hold rules it cannot check.
[[nodiscard]] Config::Validator validator();

// The shapes of `shapes` as JSON (schema baker.shapes/1.4, docs/reference-json.md), sorted by
// name: each shape's place, keyed or not, and its fields with their types, ranges, steps,
// kinds, documentation, and where they are declared, and its rules. For editors and agents.
void write_json(const Set& shapes, TextSink out);

}  // namespace baker::shape
