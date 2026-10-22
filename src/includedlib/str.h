#pragma once
// Дескриптор модуля str + eval-регистратор.
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_str.h"

namespace includedlib {

inline std::string sarg(const NArgs& a, size_t i) {
    return i < a.pos.size() ? nstr(a.pos[i]).asStr() : "";
}

inline void register_str(Nvm& vm, const std::string& alias) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    auto add = [&](const char* n, std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        (*d)[n] = NObject(fn);
    };
    add("upper", [](Nvm&, const NArgs& a) {
        return NObject(nlstr_upper(sarg(a, 0))); });
    add("lower", [](Nvm&, const NArgs& a) {
        return NObject(nlstr_lower(sarg(a, 0))); });
    add("trim", [](Nvm&, const NArgs& a) {
        return NObject(nlstr_trim(sarg(a, 0))); });
    add("split", [](Nvm&, const NArgs& a) {
        auto l = std::make_shared<std::vector<NObject>>();
        for (auto& s : nlstr_split(sarg(a, 0), sarg(a, 1)))
            l->push_back(NObject(s));
        return NObject(l); });
    add("join", [](Nvm&, const NArgs& a) {
        std::vector<std::string> parts;
        if (!a.pos.empty() && a.pos[0].isList())
            for (auto& e : *a.pos[0].asList())
                parts.push_back(nstr(e).asStr());
        return NObject(nlstr_join(parts, sarg(a, 1))); });
    add("contains", [](Nvm&, const NArgs& a) {
        return NObject(nlstr_contains(sarg(a, 0), sarg(a, 1))); });
    add("startsWith", [](Nvm&, const NArgs& a) {
        return NObject(nlstr_startsWith(sarg(a, 0), sarg(a, 1))); });
    add("endsWith", [](Nvm&, const NArgs& a) {
        return NObject(nlstr_endsWith(sarg(a, 0), sarg(a, 1))); });
    add("replace", [](Nvm&, const NArgs& a) {
        return NObject(nlstr_replace(sarg(a, 0), sarg(a, 1), sarg(a, 2))); });
    add("repeat", [](Nvm&, const NArgs& a) {
        return NObject(nlstr_repeat(sarg(a, 0),
            a.pos.size() > 1 ? a.pos[1].asInt() : 0)); });
    add("indexOf", [](Nvm&, const NArgs& a) {
        return NObject(nlstr_indexOf(sarg(a, 0), sarg(a, 1),
            a.pos.size() > 2 ? a.pos[2].asInt() : 0)); });
    add("substr", [](Nvm&, const NArgs& a) {
        return NObject(nlstr_substr(sarg(a, 0),
            a.pos.size() > 1 ? a.pos[1].asInt() : 0,
            a.pos.size() > 2 ? a.pos[2].asInt() : -1)); });
    add("at", [](Nvm&, const NArgs& a) {
        return NObject(nlstr_at(sarg(a, 0),
            a.pos.size() > 1 ? a.pos[1].asInt() : 0)); });
    add("toInt",    [](Nvm&, const NArgs& a) {
        return NObject(nlstr_toInt(sarg(a, 0))); });
    add("toDouble", [](Nvm&, const NArgs& a) {
        return NObject(nlstr_toDouble(sarg(a, 0))); });
    // модуль занимает имя "str" — встроенный str(x)
    // остаётся доступен через __call__ (см. NObject::call)
    auto builtin = vm.vars.find("str");
    if (builtin != vm.vars.end())
        (*d)["__call__"] = builtin->second;
    vm.set(alias, NObject(d));
}

inline const ModuleDescriptor str_desc = {
    "str", "nlm_str.h", {"nlmodules"}, &register_str, "str", nullptr
};

} // namespace includedlib
