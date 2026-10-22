#pragma once
// Дескриптор модуля math + eval-регистратор.
// cpp-путь: math.floor(x) -> nlmath_floor(x) из libnlmodules.
// eval-путь: словарь math с нативными обёртками над теми же функциями.
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_math.h"

namespace includedlib {

inline double num(const NArgs& a, size_t i, double def = 0.0) {
    return i < a.pos.size() ? a.pos[i].asDouble() : def;
}
inline long long inum(const NArgs& a, size_t i, long long def = 0) {
    return i < a.pos.size() ? a.pos[i].asInt() : def;
}

// math. — словарь функций
inline void register_math(Nvm& vm, const std::string& alias) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    auto add = [&](const char* n, std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        (*d)[n] = NObject(fn);
    };
    add("floor", [](Nvm&, const NArgs& a) { return NObject(nlmath_floor(num(a,0))); });
    add("ceil",  [](Nvm&, const NArgs& a) { return NObject(nlmath_ceil(num(a,0))); });
    add("round", [](Nvm&, const NArgs& a) { return NObject(nlmath_round(num(a,0))); });
    add("sqrt",  [](Nvm&, const NArgs& a) { return NObject(nlmath_sqrt(num(a,0))); });
    add("abs",   [](Nvm&, const NArgs& a) { return NObject(nlmath_abs(num(a,0))); });
    add("sin",   [](Nvm&, const NArgs& a) { return NObject(nlmath_sin(num(a,0))); });
    add("cos",   [](Nvm&, const NArgs& a) { return NObject(nlmath_cos(num(a,0))); });
    add("tan",   [](Nvm&, const NArgs& a) { return NObject(nlmath_tan(num(a,0))); });
    add("log",   [](Nvm&, const NArgs& a) { return NObject(nlmath_log(num(a,0))); });
    add("exp",   [](Nvm&, const NArgs& a) { return NObject(nlmath_exp(num(a,0))); });
    add("pow",   [](Nvm&, const NArgs& a) { return NObject(nlmath_pow(num(a,0), num(a,1))); });
    add("min",   [](Nvm&, const NArgs& a) { return NObject(nlmath_min(num(a,0), num(a,1))); });
    add("max",   [](Nvm&, const NArgs& a) { return NObject(nlmath_max(num(a,0), num(a,1))); });
    (*d)["pi"] = NObject(nlmath_pi());
    (*d)["e"] = NObject(nlmath_e());
    vm.set(alias, NObject(d));
}

inline const ModuleDescriptor math_desc = {
    "math", "nlm_math.h", {"nlmodules"}, &register_math, "math", nullptr
};

} // namespace includedlib
