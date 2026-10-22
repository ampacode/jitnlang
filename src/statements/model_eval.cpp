// eval-сторона ORM: model X / database db
// В этой сборке ORM работает in-memory, без внешних СУБД.
// PostgreSQL/SQLite-бекенд отключён, чтобы не требовать libpq/sqlite3.
#include "model.h"
#include <memory>

namespace {
    std::map<std::string,
             std::vector<std::pair<std::string, std::string>>>
        g_orm_fields;
}

NObject ModelStatement::eval(Nvm& vm) {
    vm.models[name] = fields;
    g_orm_fields[name] = fields;
    auto f = std::make_shared<NFunction>();
    f->name = name;
    f->stdimpl = [this](Nvm&, const NArgs&) -> NObject {
        auto inst =
            std::make_shared<std::map<std::string, NObject>>();
        (*inst)["__model"] = NObject(name);
        (*inst)["rowid"] = NObject(0LL);
        for (auto& fd : fields) {
            const std::string& t = fd.first;
            if (t == "int" || t == "long" || t == "bool")
                (*inst)[fd.second] = NObject(0LL);
            else if (t == "float" || t == "double")
                (*inst)[fd.second] = NObject(0.0);
            else
                (*inst)[fd.second] = NObject(std::string());
        }
        return NObject(inst);
    };
    vm.set(name, NObject(f));
    return NObject();
}

NObject DatabaseStatement::eval(Nvm& vm) {
    vm.databases[name] = "in-memory";
    vm.set(name, NObject(std::string(
        "database '" + name +
        "': в этой сборке ORM только in-memory")));
    return NObject();
}

void nlang_orm_bind_instance(const std::string&, NObject) {}
