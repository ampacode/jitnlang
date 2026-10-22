#pragma once
// Дескриптор модуля pg + eval-регистратор.
// pg.connect(conninfo) -> handle (int); pg.* принимают handle.
#include "../module.h"
#include "../nvm.h"
#include "str.h"
#include "physics.h" // darg
#include <unordered_map>

#ifdef __ANDROID__
// libpq на телефоне нет — заглушка под ORM-мост.
// В ГЛОБАЛЬНОМ пространстве — как настоящий PgDb
// из nlm_pg.h, иначе model_eval его не найдёт.
struct PgDb {
    PgDb() = default;
    PgDb(const std::string&) {}
    bool ok() { return false; }
    std::string err() { return "no libpq"; }
    long long count(const std::string&) { return 0; }
    std::vector<std::vector<std::string>> rows(
        const std::string&) { return {}; }
    long long execRows(const std::string&) { return -1; }
    std::string one(const std::string&) { return {}; }
    std::string esc(const std::string& s) { return s; }
};
inline void nlpg_close(PgDb&) {}
#else
#include "../../modules/nlm_pg.h"
#endif

namespace includedlib {

inline std::unordered_map<long long, std::unique_ptr<PgDb>> g_pgdbs;
inline long long g_pgnext = 1;

inline PgDb* pgget(const NArgs& a, size_t i = 0) {
    auto it = g_pgdbs.find(i < a.pos.size() ? a.pos[i].asInt() : 0);
    return it == g_pgdbs.end() ? nullptr : it->second.get();
}

inline void register_pg(Nvm& vm, const std::string& alias) {
#ifdef __ANDROID__
    vm.set(alias, NObject(
        std::make_shared<std::map<std::string, NObject>>()));
#else
    auto pg = std::make_shared<std::map<std::string, NObject>>();
    auto add = [&](const char* n,
                   std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        (*pg)[n] = NObject(fn);
    };
    add("connect", [](Nvm&, const NArgs& a) {
        auto db = std::make_unique<PgDb>(sarg(a, 0));
        PgDb* p = db.get();
        g_pgdbs[g_pgnext] = std::move(db);
        if (!g_defaultDb) g_defaultDb = p;
        return NObject(g_pgnext++);
    });
    add("ok", [](Nvm&, const NArgs& a) {
        auto* p = pgget(a); return NObject(p && p->ok()); });
    add("err", [](Nvm&, const NArgs& a) {
        auto* p = pgget(a);
        return NObject(p ? p->err() : std::string("no db")); });
    add("exec", [](Nvm&, const NArgs& a) {
        auto* p = pgget(a);
        return NObject(p ? p->execRows(sarg(a, 1)) : -1LL); });
    add("one", [](Nvm&, const NArgs& a) {
        auto* p = pgget(a);
        return NObject(p ? p->one(sarg(a, 1)) : std::string()); });
    add("count", [](Nvm&, const NArgs& a) {
        auto* p = pgget(a);
        return NObject(p ? p->count(sarg(a, 1)) : 0LL); });
    add("query", [](Nvm&, const NArgs& a) {
        auto* p = pgget(a);
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
    add("esc", [](Nvm&, const NArgs& a) {
        auto* p = pgget(a);
        return NObject(p ? p->esc(sarg(a, 1)) : sarg(a, 1)); });
    add("close", [](Nvm&, const NArgs& a) {
        if (auto* p = pgget(a)) nlpg_close(*p);
        return NObject(); });
    vm.set(alias, NObject(pg));
#endif
}

inline const ModuleDescriptor pg_desc = {
    "pg", "nlm_pg.h",
    // libpq.dll.a — import lib из ucrt64
    {"nlmodules", "pq"},
    [](Nvm& vm, const std::string& a) { register_pg(vm, a); },
    "pg", nullptr
};

} // namespace includedlib
