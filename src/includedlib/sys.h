#pragma once
// Дескриптор модуля sys + eval-регистратор.
//
//   sys.args()       -> list<string> argv
//   sys.argc()       -> long
//   sys.env(name)    -> string
//   sys.envSet(n,v)
//   sys.exec(cmd)    -> stdout
//   sys.execCode(cmd)-> код возврата
//   sys.exit(code)
//   sys.cwd()
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_sys.h"

namespace includedlib {

inline void register_sys(Nvm& vm, const std::string& alias) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    auto add = [&](const char* n,
                   std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        (*d)[n] = NObject(fn);
    };
    add("args", [](Nvm&, const NArgs&) {
        auto l = std::make_shared<std::vector<NObject>>();
        for (auto& s : nlsys_args()) l->push_back(NObject(s));
        return NObject(l); });
    add("argc", [](Nvm&, const NArgs&) {
        return NObject(nlsys_argc()); });
    add("arg", [](Nvm&, const NArgs& a) {
        return NObject(nlsys_arg(a.pos.empty()
            ? 0LL : a.pos[0].asInt())); });
    add("env", [](Nvm&, const NArgs& a) {
        return NObject(nlsys_env(nstr(a.pos[0]).asStr())); });
    add("envSet", [](Nvm&, const NArgs& a) {
        nlsys_envSet(nstr(a.pos[0]).asStr(),
                     nstr(a.pos[1]).asStr());
        return NObject(); });
    add("exec", [](Nvm&, const NArgs& a) {
        return NObject(nlsys_exec(nstr(a.pos[0]).asStr())); });
    add("execCode", [](Nvm&, const NArgs& a) {
        return NObject(nlsys_execCode(nstr(a.pos[0]).asStr())); });
    add("exit", [](Nvm&, const NArgs& a) {
        nlsys_exit(a.pos.empty() ? 0LL : a.pos[0].asInt());
        return NObject(); });
    add("cwd", [](Nvm&, const NArgs&) {
        return NObject(nlsys_cwd()); });
    vm.set(alias, NObject(d));
}

inline const ModuleDescriptor sys_desc = {
    "sys", "nlm_sys.h", {"nlmodules"}, &register_sys, "sys",
    nullptr // nlsys_capture вставляет run.cpp ($SYS_INIT$)
};

} // namespace includedlib
