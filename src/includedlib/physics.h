#pragma once
// Дескриптор модуля physics + eval-регистратор.
// Функции глобальные, как в py: PT(...), PF(...) — не physics.PT.
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_physics.h"

namespace includedlib {

inline double darg(const NArgs& a, size_t i, double def = 0) {
    return i < a.pos.size() ? a.pos[i].asDouble() : def;
}
inline void addd(std::function<NObject(Nvm&, const NArgs&)> f) {}

inline void register_physics(Nvm& vm) {
    auto add = [&](const char* n,
                   std::function<double(const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = [f](Nvm&, const NArgs& a) {
            return NObject(f(a)); };
        vm.set(n, NObject(fn));
    };
    add("PV",   [](auto& a) { return PV(darg(a,0),darg(a,1),darg(a,2)); });
    add("PS",   [](auto& a) { return PS(darg(a,0),darg(a,1),darg(a,2)); });
    add("PT",   [](auto& a) { return PT(darg(a,0),darg(a,1),darg(a,2)); });
    add("PV2",  [](auto& a) { return PV2(darg(a,0),darg(a,1),darg(a,2)); });
    add("PFall",[](auto& a) { return PFall(darg(a,0),darg(a,1,PG)); });
    add("PF",   [](auto& a) { return PF(darg(a,0),darg(a,1)); });
    add("PA",   [](auto& a) { return PA(darg(a,0),darg(a,1)); });
    add("PMom", [](auto& a) { return PMom(darg(a,0),darg(a,1)); });
    add("PWork",[](auto& a) { return PWork(darg(a,0),darg(a,1),darg(a,2)); });
    add("PPower",[](auto& a){ return PPower(darg(a,0),darg(a,1)); });
    add("PKin", [](auto& a) { return PKin(darg(a,0),darg(a,1)); });
    add("PPot", [](auto& a) { return PPot(darg(a,0),darg(a,1),darg(a,2,PG)); });
    add("PVEnergy",[](auto& a){ return PVEnergy(darg(a,0),darg(a,1)); });
    add("PGravForce",[](auto& a){ return PGravForce(darg(a,0),darg(a,1),darg(a,2)); });
    add("POrbitV",[](auto& a){ return POrbitV(darg(a,0),darg(a,1)); });
    add("PEscape",[](auto& a){ return PEscape(darg(a,0),darg(a,1)); });
    add("POrbitT",[](auto& a){ return POrbitT(darg(a,0),darg(a,1)); });
    add("PRange",[](auto& a){ return PRange(darg(a,0),darg(a,1),darg(a,2,PG)); });
    add("PHeight",[](auto& a){ return PHeight(darg(a,0),darg(a,1),darg(a,2,PG)); });
    add("PFlight",[](auto& a){ return PFlight(darg(a,0),darg(a,1),darg(a,2,PG)); });
    add("PPendulum",[](auto& a){ return PPendulum(darg(a,0),darg(a,1,PG)); });
    add("PSpringT",[](auto& a){ return PSpringT(darg(a,0),darg(a,1)); });
    add("PHarm",[](auto& a){ return PHarm(darg(a,0),darg(a,1),darg(a,2),darg(a,3)); });
    // PPos -> список [x, y]
    auto fpos = std::make_shared<NFunction>();
    fpos->name = "PPos";
    fpos->stdimpl = [](Nvm&, const NArgs& a) {
        auto v = PPos(darg(a,0),darg(a,1),darg(a,2),darg(a,3,PG));
        auto l = std::make_shared<std::vector<NObject>>();
        for (auto x : v) l->push_back(NObject(x));
        return NObject(l);
    };
    vm.set("PPos", NObject(fpos));
    // константы как числа
    vm.set("PG", NObject(PG));
    vm.set("PGrav", NObject(PGrav));
    vm.set("PC", NObject(PC));
    vm.set("PH", NObject(PH));
    vm.set("PK", NObject(PK));
    vm.set("PNA", NObject(PNA));
}

inline const ModuleDescriptor physics_desc = {
    "physics", "nlm_physics.h", {},
    [](Nvm& vm, const std::string&) { register_physics(vm); },
    "physics", nullptr
};

} // namespace includedlib
