#pragma once
// Дескриптор модуля sqlite + eval-регистратор.
// sqlite.open(path) -> handle (int); sqlite.* принимают handle.
#include "../module.h"
#include "../nvm.h"
#include "str.h"
#include "physics.h" // darg
#include <unordered_map>

#ifdef __ANDROID__
// libsqlite3 на телефоне нет — заглушка под ORM-мост
// (model_eval обращается к g_dbs напрямую).
// Глобальное пространство — как настоящий Db.
struct Db {
    Db() = default;
    Db(const std::string&) {}
    void* h = nullptr;
    bool ok() { return false; }
    std::string err() { return "no sqlite"; }
    long long count(const std::string&) { return 0; }
    std::vector<std::vector<std::string>> rows(
        const std::string&) { return {}; }
    long long execRows(const std::string&) { return -1; }
    std::string one(const std::string&) { return {}; }
    static std::string esc(const std::string& s) { return s; }
};
#else
#include "../../modules/nlm_sqlite.h"
#endif

namespace includedlib {

inline std::unordered_map<long long, std::unique_ptr<Db>> g_dbs;
inline long long g_dbnext = 1;

inline Db* dbget(const NArgs& a, size_t i = 0) {
    auto it = g_dbs.find(i < a.pos.size() ? a.pos[i].asInt() : 0);
    return it == g_dbs.end() ? nullptr : it->second.get();
}

inline void register_sqlite(Nvm& vm, const std::string& alias) {
#ifdef __ANDROID__
    // движка нет — пустой dict, чтобы `using <sqlite>` не падал
    vm.set(alias, NObject(
        std::make_shared<std::map<std::string, NObject>>()));
#else
    auto sq = std::make_shared<std::map<std::string, NObject>>();
    auto add = [&](const char* n,
                   std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        (*sq)[n] = NObject(fn);
    };
    add("open", [](Nvm&, const NArgs& a) {
        auto db = std::make_unique<Db>(sarg(a, 0));
        g_dbs[g_dbnext] = std::move(db);
        return NObject(g_dbnext++);
    });
    add("ok", [](Nvm&, const NArgs& a) {
        auto* p = dbget(a); return NObject(p && p->ok()); });
    add("err", [](Nvm&, const NArgs& a) {
        auto* p = dbget(a);
        return NObject(p ? p->err() : std::string("no db")); });
    add("exec", [](Nvm&, const NArgs& a) {
        auto* p = dbget(a);
        return NObject(p ? p->execRows(sarg(a, 1)) : -1LL); });
    add("one", [](Nvm&, const NArgs& a) {
        auto* p = dbget(a);
        return NObject(p ? p->one(sarg(a, 1)) : std::string()); });
    add("count", [](Nvm&, const NArgs& a) {
        auto* p = dbget(a);
        return NObject(p ? p->count(sarg(a, 1)) : 0LL); });
    add("query", [](Nvm&, const NArgs& a) {
        auto* p = dbget(a);
        auto l = std::make_shared<std::vector<NObject>>();
        if (p)
            for (auto& row : p->rows(sarg(a, 1))) {
                std::string s;
                for (size_t j = 0; j < row.size(); j++) {
                    if (j) s += '\x1f';
                    s += row[j];
                }
                l->push_back(NObject(s));
            }
        return NObject(l);
    });
    add("column", [](Nvm&, const NArgs& a) {
        auto* p = dbget(a);
        auto l = std::make_shared<std::vector<NObject>>();
        if (p)
            for (auto& s : p->column(sarg(a, 1)))
                l->push_back(NObject(s));
        return NObject(l);
    });
    add("esc", [](Nvm&, const NArgs& a) {
        return NObject(Db::esc(sarg(a, 1))); });
    vm.set(alias, NObject(sq));
#endif
}

inline const ModuleDescriptor sqlite_desc = {
    "sqlite", "nlm_sqlite.h", {"nlmodules", "sqlite3"},
    [](Nvm& vm, const std::string& a) { register_sqlite(vm, a); },
    "sqlite", nullptr
};

} // namespace includedlib
