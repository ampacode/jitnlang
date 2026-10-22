#pragma once
// Дескриптор модуля http + eval-регистратор.
//
// cpp-путь: http.get(url) -> nlhttp_get(url) — libcurl.
// eval-путь: глобальная сессия = хендл 0; session() -> dict
// с методами (get/post/put/patch/del/header/status/err/timeout).
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_http.h"
#include <unordered_map>

namespace includedlib {

// сессии для eval: handle -> NHSession (0 — глобальная)
inline std::unordered_map<long long, std::unique_ptr<NHSession>>
    g_httpsess;
inline long long g_httpnext = 1;

inline NHSession* http_get_sess(long long h) {
    if (h == 0) return _nlhttp_global();
    auto it = g_httpsess.find(h);
    return it == g_httpsess.end() ? nullptr : it->second.get();
}

// dict-обёртка для eval-сессии: s.get(url) и т.д.
inline NObject http_session_obj(long long h) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    (*d)["__handle"] = NObject(h);
    auto add = [&](const char* n,
                   std::function<NObject(Nvm&, const NArgs&)> f) {
        auto g = std::make_shared<NFunction>();
        g->name = n;
        g->stdimpl = std::move(f);
        (*d)[n] = NObject(g);
    };
    add("get", [h](Nvm&, const NArgs& a) {
        auto* s = http_get_sess(h);
        return NObject(s ? s->get(nstr(a.pos[0]).asStr(),
            a.pos.size() > 1 ? nstr(a.pos[1]).asStr() : "")
            : std::string()); });
    add("post", [h](Nvm&, const NArgs& a) {
        auto* s = http_get_sess(h);
        return NObject(s ? s->post(nstr(a.pos[0]).asStr(),
            nstr(a.pos[1]).asStr(),
            a.pos.size() > 2 ? nstr(a.pos[2]).asStr() : "")
            : std::string()); });
    add("put", [h](Nvm&, const NArgs& a) {
        auto* s = http_get_sess(h);
        return NObject(s ? s->put(nstr(a.pos[0]).asStr(),
            nstr(a.pos[1]).asStr(),
            a.pos.size() > 2 ? nstr(a.pos[2]).asStr() : "")
            : std::string()); });
    add("patch", [h](Nvm&, const NArgs& a) {
        auto* s = http_get_sess(h);
        return NObject(s ? s->patch(nstr(a.pos[0]).asStr(),
            nstr(a.pos[1]).asStr(),
            a.pos.size() > 2 ? nstr(a.pos[2]).asStr() : "")
            : std::string()); });
    add("del", [h](Nvm&, const NArgs& a) {
        auto* s = http_get_sess(h);
        return NObject(s ? s->del(nstr(a.pos[0]).asStr(),
            a.pos.size() > 1 ? nstr(a.pos[1]).asStr() : "")
            : std::string()); });
    add("header", [h](Nvm&, const NArgs& a) {
        auto* s = http_get_sess(h);
        if (s) s->header(nstr(a.pos[0]).asStr(),
                         nstr(a.pos[1]).asStr());
        return NObject(); });
    add("timeout", [h](Nvm&, const NArgs& a) {
        auto* s = http_get_sess(h);
        if (s) s->timeout(a.pos.empty()
            ? 0.0 : a.pos[0].asDouble());
        return NObject(); });
    add("status", [h](Nvm&, const NArgs&) {
        auto* s = http_get_sess(h);
        return NObject(s ? (long long)s->status() : 0LL); });
    add("err", [h](Nvm&, const NArgs&) {
        auto* s = http_get_sess(h);
        return NObject(s ? s->err() : std::string()); });
    return NObject(d);
}

inline void register_http(Nvm& vm, const std::string& alias) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    auto add = [&](const char* n,
                   std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        (*d)[n] = NObject(fn);
    };
    add("get", [](Nvm&, const NArgs& a) {
        return NObject(nlhttp_get(nstr(a.pos[0]).asStr(),
            a.pos.size() > 1 ? nstr(a.pos[1]).asStr() : "")); });
    add("post", [](Nvm&, const NArgs& a) {
        return NObject(nlhttp_post(nstr(a.pos[0]).asStr(),
            nstr(a.pos[1]).asStr(),
            a.pos.size() > 2 ? nstr(a.pos[2]).asStr() : "")); });
    add("put", [](Nvm&, const NArgs& a) {
        return NObject(nlhttp_put(nstr(a.pos[0]).asStr(),
            nstr(a.pos[1]).asStr(),
            a.pos.size() > 2 ? nstr(a.pos[2]).asStr() : "")); });
    add("patch", [](Nvm&, const NArgs& a) {
        return NObject(nlhttp_patch(nstr(a.pos[0]).asStr(),
            nstr(a.pos[1]).asStr(),
            a.pos.size() > 2 ? nstr(a.pos[2]).asStr() : "")); });
    add("delete", [](Nvm&, const NArgs& a) {
        return NObject(nlhttp_delete(nstr(a.pos[0]).asStr(),
            a.pos.size() > 1 ? nstr(a.pos[1]).asStr() : "")); });
    add("del", [](Nvm& vm, const NArgs& a) {
        NObject o;
        o = http_session_obj(0);
        return o.asDict()->at("del").call(vm, a); });
    add("status", [](Nvm&, const NArgs&) {
        return NObject(nlhttp_status()); });
    add("err", [](Nvm&, const NArgs&) {
        return NObject(nlhttp_err()); });
    add("timeout", [](Nvm&, const NArgs& a) {
        nlhttp_timeout(a.pos.empty() ? 0.0 : a.pos[0].asDouble());
        return NObject(); });
    add("header", [](Nvm&, const NArgs& a) {
        nlhttp_header(nstr(a.pos[0]).asStr(),
                      nstr(a.pos[1]).asStr());
        return NObject(); });
    add("clearHeaders", [](Nvm&, const NArgs&) {
        nlhttp_clearHeaders();
        return NObject(); });
    add("session", [](Nvm&, const NArgs&) {
        g_httpsess[g_httpnext] = std::make_unique<NHSession>();
        return http_session_obj(g_httpnext++); });
    vm.set(alias, NObject(d));
}

inline const ModuleDescriptor http_desc = {
    "http", "nlm_http.h", {"nlmodules", "curl"},
    &register_http, "http", nullptr
};

// ---- канцелярский диалект: courier ----
// ответ = депеша (dispatch): {body, status, error, headers, ok}
//   courier.order(url)      — GET, затребовать
//   courier.deliver(u, b)   — POST, доставить груз
//   courier.supplant(u, b)  — PUT, замена
//   courier.amend(u, b)     — PATCH, точечная правка
//   courier.purge(url)      — DELETE
//   courier.session()       — курьер-сессия: cookie jar,
//                             свои заголовки на все запросы

// завернуть результат поездки курьера в dispatch-словарь
inline NObject courier_dispatch(NHSession* s,
                                const std::string& body) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    if (!s) {
        (*d)["body"] = NObject(std::string());
        (*d)["status"] = NObject(0LL);
        (*d)["error"] = NObject(std::string("no courier"));
        (*d)["ok"] = NObject(false);
    } else {
        (*d)["body"] = NObject(body);
        (*d)["status"] = NObject((long long)s->status());
        (*d)["error"] = NObject(s->err());
        (*d)["ok"] = NObject(s->status() >= 200 &&
                            s->status() < 400);
        auto hd = std::make_shared<std::map<std::string, NObject>>();
        for (auto& kv : s->resp_headers)
            (*hd)[kv.first] = NObject(kv.second);
        (*d)["headers"] = NObject(hd);
    }
    return NObject(d);
}

// общий вызов метода сессии -> dispatch
inline NObject courier_call(long long h, const char* method,
                            const NArgs& a) {
    NHSession* s = http_get_sess(h);
    if (!s) return courier_dispatch(nullptr, "");
    std::string url = nstr(a.pos[0]).asStr();
    // для методов с телом: (url, body[, hdrs]); без тела: (url[, hdrs])
    if (!strcmp(method, "order")) {
        std::string hdrs = a.pos.size() > 1
            ? nstr(a.pos[1]).asStr() : "";
        return courier_dispatch(s, s->get(url, hdrs));
    }
    if (!strcmp(method, "purge")) {
        std::string hdrs = a.pos.size() > 1
            ? nstr(a.pos[1]).asStr() : "";
        return courier_dispatch(s, s->del(url, hdrs));
    }
    std::string body = a.pos.size() > 1 ? nstr(a.pos[1]).asStr() : "";
    std::string hdrs = a.pos.size() > 2 ? nstr(a.pos[2]).asStr() : "";
    if (!strcmp(method, "deliver"))  return courier_dispatch(s, s->post(url, body, hdrs));
    if (!strcmp(method, "supplant")) return courier_dispatch(s, s->put(url, body, hdrs));
    if (!strcmp(method, "amend"))    return courier_dispatch(s, s->patch(url, body, hdrs));
    return courier_dispatch(nullptr, "");
}

// сессия курьера как dict с имперскими методами
inline NObject courier_session_obj(long long h) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    (*d)["__handle"] = NObject(h);
    auto add = [&](const char* n,
                   std::function<NObject(Nvm&, const NArgs&)> f) {
        auto g = std::make_shared<NFunction>();
        g->name = n;
        g->stdimpl = std::move(f);
        (*d)[n] = NObject(g);
    };
    for (const char* m : {"order", "deliver", "supplant",
                          "amend", "purge"})
        add(m, [h, m](Nvm&, const NArgs& a) {
            return courier_call(h, m, a); });
    add("header", [h](Nvm&, const NArgs& a) {
        auto* s = http_get_sess(h);
        if (s) s->header(nstr(a.pos[0]).asStr(),
                         nstr(a.pos[1]).asStr());
        return NObject(); });
    add("timeout", [h](Nvm&, const NArgs& a) {
        auto* s = http_get_sess(h);
        if (s) s->timeout(a.pos.empty()
            ? 0.0 : a.pos[0].asDouble());
        return NObject(); });
    add("clearHeaders", [h](Nvm&, const NArgs&) {
        auto* s = http_get_sess(h);
        if (s) s->clearHeaders();
        return NObject(); });
    return NObject(d);
}

inline void register_courier(Nvm& vm, const std::string& alias) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    auto add = [&](const char* n,
                   std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        (*d)[n] = NObject(fn);
    };
    // методы работают на глобальной сессии (handle 0)
    for (const char* m : {"order", "deliver", "supplant",
                          "amend", "purge"})
        add(m, [m](Nvm&, const NArgs& a) {
            return courier_call(0, m, a); });
    add("session", [](Nvm&, const NArgs&) {
        g_httpsess[g_httpnext] = std::make_unique<NHSession>();
        return courier_session_obj(g_httpnext++); });
    add("header", [](Nvm&, const NArgs& a) {
        nlhttp_header(nstr(a.pos[0]).asStr(),
                      nstr(a.pos[1]).asStr());
        return NObject(); });
    add("clearHeaders", [](Nvm&, const NArgs&) {
        nlhttp_clearHeaders();
        return NObject(); });
    add("timeout", [](Nvm&, const NArgs& a) {
        nlhttp_timeout(a.pos.empty() ? 0.0 : a.pos[0].asDouble());
        return NObject(); });
    vm.set(alias, NObject(d));
}

inline const ModuleDescriptor courier_desc = {
    "courier", "nlm_http.h", {"nlmodules", "curl"},
    &register_courier, "courier", nullptr
};

} // namespace includedlib
