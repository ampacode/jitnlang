#pragma once
// Дескриптор модуля neuro + eval-регистратор (глобальные N*).
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_neuro.h"
#include "physics.h" // darg

namespace includedlib {

inline void register_neuro(Nvm& vm) {
    auto add = [&](const char* n,
                   std::function<double(const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = [f](Nvm&, const NArgs& a) {
            return NObject(f(a)); };
        vm.set(n, NObject(fn));
    };
    add("NDecay",[](auto& a){ return NDecay(darg(a,0),darg(a,1),darg(a,2)); });
    add("NConc", [](auto& a){ return NConc(darg(a,0),darg(a,1),darg(a,2,1)); });
    add("NConcT",[](auto& a){ return NConcT(darg(a,0),darg(a,1),darg(a,2),darg(a,3),darg(a,4,1)); });
    add("NT12",  [](auto& a){ return NT12(darg(a,0),darg(a,1)); });
    add("NClear",[](auto& a){ return NClear(darg(a,0),darg(a,1),darg(a,2),darg(a,3)); });
    add("NOcc",  [](auto& a){ return NOcc(darg(a,0),darg(a,1),darg(a,2,1)); });
    add("NEff",  [](auto& a){ return NEff(darg(a,0),darg(a,1),darg(a,2),darg(a,3),darg(a,4,1)); });
    add("NCss",  [](auto& a){ return NCss(darg(a,0),darg(a,1),darg(a,2,1)); });
    add("NLoad", [](auto& a){ return NLoad(darg(a,0),darg(a,1),darg(a,2,1)); });
    add("NInf",  [](auto& a){ return NInf(darg(a,0),darg(a,1),darg(a,2,1)); });
    add("NAuc",  [](auto& a){ return NAuc(darg(a,0),darg(a,1),darg(a,2,1)); });
    add("NRisk", [](auto& a){ return NRisk(darg(a,0),darg(a,1)); });
    add("NSchild",[](auto& a){ return NSchild(darg(a,0),darg(a,1),darg(a,2)); });
}

inline const ModuleDescriptor neuro_desc = {
    "neuro", "nlm_neuro.h", {},
    [](Nvm& vm, const std::string&) { register_neuro(vm); },
    "neuro", nullptr
};

} // namespace includedlib
