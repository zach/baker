#pragma once

// bench_call's message and its 128 bound types. Binding a type costs
// the compiler about 0.17 s, so the types are split across kParts translation units that
// build in parallel; bench_call_wide.hpp defines them, and only those units include
// it.
#include <baker/object.hpp>

#include <memory>
#include <vector>

struct HitMsg {
    static constexpr auto name = "hit";
    int points;
};

using Objects = std::vector<std::unique_ptr<baker::ObjectBase>>;
constexpr int kTypes = 64, kParts = 4, kTypesPerPart = kTypes / kParts;

// Pushes one object of each type in `Part`, in order: types Part * kTypesPerPart on, with
// `methods` methods each (32 or 1).
template <int Part>
void push_part(int methods, Objects& out);
