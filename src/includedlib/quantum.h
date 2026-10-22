#pragma once
// Дескриптор модуля quantum + eval-регистратор.
// QState/QGate — комплексные типы, в eval не мостятся:
// регистрируем только скалярные S*-функции.
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_quantum.h"
#include "physics.h" // darg

namespace includedlib {

inline void register_quantum(Nvm& vm) {
    auto add = [&](const char* n,
                   std::function<double(const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = [f](Nvm&, const NArgs& a) {
            return NObject(f(a)); };
        vm.set(n, NObject(fn));
    };
    add("SDim",   [](auto& a) {
        return (double)(a.pos.empty() || !a.pos[0].asBool() ? 10 : 26); });
    add("STension",[](auto& a){ return STension(darg(a,0)); });
    add("SLen",   [](auto& a) { return SLen(darg(a,0)); });
    add("SRegge", [](auto& a) {
        return SRegge((int)darg(a,0), darg(a,1), darg(a,2,1)); });
    add("SReggeMass",[](auto& a) {
        return SReggeMass((int)darg(a,0), darg(a,1), darg(a,2,1)); });
    add("SKK",    [](auto& a) {
        return SKK((int)darg(a,0), darg(a,1)); });
    add("SMom",   [](auto& a) {
        return SMom((int)darg(a,0), darg(a,1)); });
    vm.set("SPlanckLen", NObject(SPlanckLen));
    vm.set("SPlanckMass", NObject(SPlanckMass));
    vm.set("SHbar", NObject(SHbar));
    vm.set("SC", NObject(SC));
}

inline const ModuleDescriptor quantum_desc = {
    "quantum", "nlm_quantum.h", {},
    [](Nvm& vm, const std::string&) { register_quantum(vm); },
    "quantum", nullptr
};

} // namespace includedlib
