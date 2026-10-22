#pragma once
// Дескриптор модуля regex + eval-регистратор.
// В сгенерированном C++ имена функций — прямые: reMatch() и т.д.
// (регистратор для eval — re.match()/re.search()).
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_regex.h"

namespace includedlib {

static inline NObject re_list(const std::vector<std::string>& v) {
    auto l = std::make_shared<std::vector<NObject>>();
    for (auto& s : v) l->push_back(NObject(s));
    return NObject(l);
}

inline void register_regex(Nvm& vm, const std::string& alias) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    auto add = [&](const char* n,
                   std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        (*d)[n] = NObject(fn);
    };
    add("match", [](Nvm&, const NArgs& a) {
        return NObject(nlregex_match(sarg(a, 0), sarg(a, 1))); });
    add("search", [](Nvm&, const NArgs& a) {
        return NObject(nlregex_search(sarg(a, 0), sarg(a, 1))); });
    add("group", [](Nvm&, const NArgs& a) {
        return NObject(nlregex_group(sarg(a, 0), sarg(a, 1),
            a.pos.size() > 2 ? a.pos[2].asInt() : 0)); });
    add("groups", [](Nvm&, const NArgs& a) {
        return re_list(nlregex_groups(sarg(a, 0), sarg(a, 1))); });
    add("findAll", [](Nvm&, const NArgs& a) {
        return re_list(nlregex_findAll(sarg(a, 0), sarg(a, 1))); });
    add("replace", [](Nvm&, const NArgs& a) {
        return NObject(nlregex_replace(sarg(a, 0), sarg(a, 1),
                                       sarg(a, 2))); });
    add("split", [](Nvm&, const NArgs& a) {
        return re_list(nlregex_split(sarg(a, 0), sarg(a, 1))); });
    add("test", [](Nvm&, const NArgs& a) {
        return NObject(nlregex_test(sarg(a, 0))); });
    vm.set(alias, NObject(d));
}

inline const ModuleDescriptor regex_desc = {
    "regex", "nlm_regex.h", {"nlmodules"},
    &register_regex, "re", nullptr
};

} // namespace includedlib
