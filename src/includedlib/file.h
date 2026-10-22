#pragma once
// Дескриптор модуля file + eval-регистратор.
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_file.h"
#include "str.h" // sarg
#include <filesystem>

namespace includedlib {

// относительный путь в скрипте = относительно каталога
// скрипта (vm.source_dir — ставит nlang-команда и cli).
// Без этого на Android file.exists("player.nlc") идёт
// в cwd процесса (/), а не в каталог сессии.
inline std::string file_rel(Nvm& vm, const std::string& p) {
    if (vm.source_dir.empty() || p.empty()) return p;
    std::filesystem::path fp(p);
    if (fp.is_absolute()) return p;
    return (std::filesystem::path(vm.source_dir) / fp)
        .lexically_normal().string();
}

inline void register_file(Nvm& vm, const std::string& alias) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    auto add = [&](const char* n, std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        (*d)[n] = NObject(fn);
    };
    add("read",   [](Nvm& v, const NArgs& a) { return NObject(nlfile_read(file_rel(v, sarg(a,0)))); });
    add("write",  [](Nvm& v, const NArgs& a) { nlfile_write(file_rel(v, sarg(a,0)), sarg(a,1)); return NObject(); });
    add("append", [](Nvm& v, const NArgs& a) { nlfile_append(file_rel(v, sarg(a,0)), sarg(a,1)); return NObject(); });
    add("exists", [](Nvm& v, const NArgs& a) { return NObject(nlfile_exists(file_rel(v, sarg(a,0)))); });
    add("lines",  [](Nvm& v, const NArgs& a) {
        auto l = std::make_shared<std::vector<NObject>>();
        for (auto& s : nlfile_lines(file_rel(v, sarg(a,0)))) l->push_back(NObject(s));
        return NObject(l); });
    add("size",   [](Nvm& v, const NArgs& a) { return NObject(nlfile_size(file_rel(v, sarg(a,0)))); });
    add("remove", [](Nvm& v, const NArgs& a) { return NObject(nlfile_remove(file_rel(v, sarg(a,0)))); });
    add("list",   [](Nvm& v, const NArgs& a) {
        auto l = std::make_shared<std::vector<NObject>>();
        for (auto& s : nlfile_list(file_rel(v, sarg(a,0)))) l->push_back(NObject(s));
        return NObject(l); });
    add("name",   [](Nvm&, const NArgs& a) {
        return NObject(nlfile_name(sarg(a,0))); });
    vm.set(alias, NObject(d));
}

inline const ModuleDescriptor file_desc = {
    "file", "nlm_file.h", {"nlmodules"}, &register_file, "file", nullptr
};

} // namespace includedlib
