#pragma once
// Дескриптор модуля random + eval-регистратор.
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_random.h"
#include "math.h" // num, inum

namespace includedlib {

inline void register_random(Nvm& vm, const std::string& alias) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    auto add = [&](const char* n, std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        (*d)[n] = NObject(fn);
    };
    add("seed",     [](Nvm&, const NArgs& a) { nlrandom_seed(inum(a,0)); return NObject(); });
    add("seedTime", [](Nvm&, const NArgs&) { nlrandom_seedTime(); return NObject(); });
    add("int",      [](Nvm&, const NArgs& a) { return NObject(nlrandom_int(inum(a,0), inum(a,1))); });
    add("float",    [](Nvm&, const NArgs&) { return NObject(nlrandom_float()); });
    add("bool",     [](Nvm&, const NArgs&) { return NObject(nlrandom_bool()); });
    vm.set(alias, NObject(d));
}

inline const ModuleDescriptor random_desc = {
    "random", "nlm_random.h", {"nlmodules"}, &register_random, "random", nullptr
};

} // namespace includedlib
