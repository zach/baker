#pragma once

// Rules between values, declared in shapes and evaluated here: (rule "message" {expression}
// (warn)) in a shape (<baker/shape.hpp>) holds over each record the shape governs. The shape
// library reads rules and cannot run script; this evaluates them. Part of baker::tools.

#include <baker/config.hpp>
#include <baker/script.hpp>
#include <baker/shape.hpp>

namespace baker::tools {

// Registers what a rule reads of the tree it is checked against, beside the record's own
// fields (each a $variable named by its field):
//   {data SEGMENT ...}       the value at a path, each argument evaluated and a dotted
//                            symbol split: {data tiers $tier ideal}, {data economy.tax}.
//                            One value is itself; several are an array.
//   {data.sum PATH [FIELD]}  the sum of each child's first number at PATH, or of each
//                            child's FIELD: {== {data.sum loot weight} 100}.
// Outside check_rules they fail: they read the checked tree, never a live Config. Calling
// it again does nothing. check_rules calls it; a tool that checks the shapes' rules with
// check() first calls it so that `data` is known.
void declare_rule_functions(Context& ctx);

// Evaluates each rule of `shapes` over each record of `tree` its shape governs, in `ctx`,
// into `report`. A rule whose expression is false is an error, or a warning if (warn),
// located at the record, its path the record's, carrying the rule's message and each
// $variable the expression mentions with the record's value:
//   starts at or below its maximum ($start_hp 12, $max_hp 10)
// A rule that does not evaluate, or not to true or false, is an error naming it and why. A
// field the record lacks leaves its $variable unset. Each $variable is restored after the
// rule, so the Context's own are untouched. A rule calling a function the Context does not
// have fails to evaluate: in a tool without the game, run check() for the demand instead.
void check_rules(const Array& tree, const shape::Set& shapes, Context& ctx, shape::Report& report);

// For Config::add_validator: reads the shapes each layer declares, as shape::validator()
// does, and evaluates their rules over the merged root (check_rules), reporting to the
// warning handler, so a live edit that breaks a rule warns at the next reload. It runs
// script in `ctx`, which must outlive the Config's use of it: add it without
// ValidatorOptions::any_thread. The host kit's Live adds it.
[[nodiscard]] Config::Validator rules_validator(Context& ctx);

}  // namespace baker::tools
