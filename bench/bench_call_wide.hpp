#pragma once

// The definitions behind bench_call_types.hpp, for the units that instantiate one part.
#include "bench_call_types.hpp"

#include <string>
#include <utility>

// One of 64 types with M methods, m0 to m(M-1), each a float argument, and a HitMsg handler.
template <int N, int M>
struct Wide : baker::Object<Wide<N, M>> {
    float v = 0;
    int hp = 1'000'000;
    int on_hit(const HitMsg& m) { return hp -= m.points; }
    static void bind(baker::Binder<Wide>& b) {
        b.name((M == 1 ? "Narrow" : "Wide") + std::to_string(N));
        for (int k = 0; k < M; ++k)
            b.method(baker::Symbol("m" + std::to_string(k)),
                     [](Wide& w, float a) { return w.v += a; });
        b.template on<HitMsg>(&Wide::on_hit);
    }
};

template <int Part, int M, int... K>
void push_types(Objects& out, std::integer_sequence<int, K...>) {
    (out.push_back(std::make_unique<Wide<Part * kTypesPerPart + K, M>>()), ...);
}

template <int Part>
void push_part(int methods, Objects& out) {
    if (methods == 1)
        push_types<Part, 1>(out, std::make_integer_sequence<int, kTypesPerPart>());
    else
        push_types<Part, 32>(out, std::make_integer_sequence<int, kTypesPerPart>());
}
