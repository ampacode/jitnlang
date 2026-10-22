#pragma once
// Дескриптор модуля internet + eval-регистратор.
//
// cpp-путь: net.get("/", h) -> nlinternet_get("/", h),
// хендлер — обычная C++-лямбда ActivationContext(ActivationContext&).
//
// eval-путь: тот же HTTP-сервер, но хендлеры — NObject-функции;
// контекст запроса прокидывается словарём {params, headers, ...}.
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_internet.h"
#include "json.h" // to_jvalue / nljson_export для r.json = {...}
#include <filesystem>

namespace includedlib {

// --- ActivationContext <-> NObject-словарь (только eval-путь) ---

// словарь параметров с самодостаточным акцессором:
// orders.order("q") — затребовать распоряжение по имени
inline NObject order_dict(
        const std::unordered_map<std::string, std::string>& src) {
    auto m = std::make_shared<std::map<std::string, NObject>>();
    for (auto& kv : src) (*m)[kv.first] = NObject(kv.second);
    auto orderfn = std::make_shared<NFunction>();
    orderfn->name = "order";
    orderfn->stdimpl =
        [m](Nvm&, const NArgs& a) -> NObject {
            if (!a.pos.empty()) {
                auto it = m->find(a.pos[0].asStr());
                if (it != m->end()) return it->second;
            }
            return NObject();
        };
    (*m)["order"] = NObject(orderfn);
    return NObject(m);
}

inline NObject ctx_to_nobject(ActivationContext& ac) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    auto params = std::make_shared<std::map<std::string, NObject>>();
    for (auto& kv : ac.params) (*params)[kv.first] = NObject(kv.second);
    (*d)["params"] = NObject(params);
    // req.get("id") -> params["id"]
    auto getfn = std::make_shared<NFunction>();
    getfn->name = "get";
    getfn->stdimpl =
        [params](Nvm&, const NArgs& a) -> NObject {
            if (!a.pos.empty()) {
                auto it = params->find(a.pos[0].asStr());
                if (it != params->end()) return it->second;
            }
            return NObject();
        };
    (*d)["get"] = NObject(getfn);
    // имперские акцессоры:
    //   req.order("q")          — любой параметр (слитно)
    //   req.orders.order("q")   — адресные (path + query)
    //   req.writs.order("q")    — письменные (form-тело)
    auto orderfn = std::make_shared<NFunction>();
    orderfn->name = "order";
    orderfn->stdimpl = getfn->stdimpl;
    (*d)["order"] = NObject(orderfn);
    (*d)["orders"] = order_dict(ac.query_params);
    (*d)["writs"] = order_dict(ac.form_params);
    // vault — хранилище сеанса: данные живут между запросами
    // по куке sid (login/register, корзина, состояние).
    //   req.vault.set("login", u)  req.vault.get("login")
    //   req.vault.erase("login")
    {
        auto vault = std::make_shared<std::map<std::string, NObject>>();
        auto mk = [&](const char* n,
                      std::function<NObject(Nvm&, const NArgs&)> f) {
            auto fn = std::make_shared<NFunction>();
            fn->name = n;
            fn->stdimpl = std::move(f);
            (*vault)[n] = NObject(fn);
        };
        mk("set", [&ac](Nvm&, const NArgs& a) {
            if (a.pos.size() >= 2)
                ac.sessionSet(a.pos[0].asStr(),
                              a.pos[1].asStr());
            return NObject(); });
        mk("get", [&ac](Nvm&, const NArgs& a) {
            if (a.pos.empty()) return NObject();
            std::string v = ac.sessionGet(a.pos[0].asStr());
            return v.empty() ? NObject() : NObject(v); });
        mk("erase", [&ac](Nvm&, const NArgs& a) {
            if (!a.pos.empty())
                ac.sessionErase(a.pos[0].asStr());
            return NObject(); });
        (*d)["vault"] = NObject(vault);
    }
    (*d)["body"] = NObject(ac.body);
    (*d)["html"] = NObject(ac.html);
    (*d)["json"] = NObject(ac.json);
    (*d)["statusCode"] = NObject((long long)ac.statusCode);
    (*d)["contentType"] = NObject(ac.contentType);
    (*d)["redirectUrl"] = NObject(ac.redirectUrl);
    (*d)["headers"] = NObject(std::make_shared<std::map<std::string, NObject>>());
    (*d)["cookies"] = NObject(std::make_shared<std::map<std::string, NObject>>());
    return NObject(d);
}

inline ActivationContext ctx_from_nobject(const NObject& v) {
    ActivationContext ac;
    if (!v.isDict()) return ac;
    auto d = v.asDict();
    auto get = [&](const char* k) -> std::string {
        auto it = d->find(k);
        if (it == d->end()) return "";
        // dict/list в строковое поле — автосериализация
        if (it->second.isDict() || it->second.isList())
            return nljson_export(to_jvalue(it->second));
        return nstr(it->second).asStr();
    };
    ac.html = get("html");
    ac.body = get("body");
    ac.json = get("json");
    ac.contentType = get("contentType");
    ac.redirectUrl = get("redirectUrl");
    auto st = d->find("statusCode");
    if (st != d->end()) ac.statusCode = (int)st->second.asInt();
    auto hd = d->find("headers");
    if (hd != d->end() && hd->second.isDict())
        for (auto& kv : *hd->second.asDict())
            ac.headers[kv.first] = nstr(kv.second).asStr();
    auto ck = d->find("cookies");
    if (ck != d->end() && ck->second.isDict())
        for (auto& kv : *ck->second.asDict())
            ac.cookies[kv.first] = nstr(kv.second).asStr();
    return ac;
}

// NObject-функция -> NlHandler
inline NlHandler handler_of(Nvm& vm, NObject fn) {
    return [&vm, fn](ActivationContext& req) -> ActivationContext {
        NArgs args;
        args.pos.push_back(ctx_to_nobject(req));
        NObject r = fn.call(vm, args);
        return ctx_from_nobject(r);
    };
}

inline void register_internet(Nvm& vm, const std::string& alias) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    auto add = [&](const char* n, std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        (*d)[n] = NObject(fn);
    };
    auto route = [](const char* method) {
        return [method](Nvm& vm, const NArgs& a) {
            if (a.pos.size() >= 2)
                nlinternet_route(method, a.pos[0].asStr(),
                                 handler_of(vm, a.pos[1]));
            return NObject();
        };
    };
    static auto get_ = route("GET");
    static auto post_ = route("POST");
    static auto put_ = route("PUT");
    static auto del_ = route("DELETE");
    static auto patch_ = route("PATCH");
    static auto head_ = route("HEAD");
    static auto options_ = route("OPTIONS");
    add("get", get_);
    add("post", post_);
    add("put", put_);
    add("del", del_);
    add("patch", patch_);
    add("head", head_);
    add("options", options_);
    // рецепторные алиасы (ampa=GET, nmda=POST, ...)
    add("ampa", get_);
    add("nmda", post_);
    add("d2", put_);
    add("gabab", del_);
    add("gabaa", patch_);
    add("m1", head_);
    add("m2", options_);
    // wire(["ampa","d2l"], "/p", h) — метод + content-type из
    // набора рецепторов (checkWiresProtocolds из py)
    add("wire", [](Nvm& vm, const NArgs& a) {
        if (a.pos.size() >= 3 && a.pos[0].isList()) {
            std::vector<std::string> protos;
            for (auto& p : *a.pos[0].asList())
                protos.push_back(nstr(p).asStr());
            nlinternet_wire(protos, a.pos[1].asStr(),
                            handler_of(vm, a.pos[2]));
        }
        return NObject(); });
    add("route", [](Nvm& vm, const NArgs& a) {
        if (a.pos.size() >= 3)
            nlinternet_route(a.pos[0].asStr(), a.pos[1].asStr(),
                             handler_of(vm, a.pos[2]));
        return NObject(); });
    add("staticDir", [](Nvm&, const NArgs& a) {
        if (!a.pos.empty()) nlinternet_staticDir(a.pos[0].asStr());
        return NObject(); });
    add("listen", [](Nvm&, const NArgs& a) {
        nlinternet_listen(a.pos.empty() ? 8080 : a.pos[0].asInt());
        return NObject(); });
    // каталог ядра станции: <nlang-cpp>/wasm/core —
    // runtime_dir это <...>/build-ucrt/runtime.
    // exe может жить по хардлинку/копии в другом месте —
    // тогда ищем wasm/core относительно каталога скрипта
    // (проекты типа mwa/ лежат рядом с nlang-cpp/).
    auto core_dir_of = [](Nvm& vm) {
        namespace fs = std::filesystem;
        std::vector<fs::path> cands;
        fs::path d = vm.runtime_dir;
        cands.push_back(d.parent_path().parent_path() / "wasm/core");
        if (!vm.source_dir.empty()) {
            fs::path sd = vm.source_dir;
            cands.push_back(sd / "../nlang-cpp/wasm/core");
            cands.push_back(sd / "../../nlang-cpp/wasm/core");
            cands.push_back(sd.parent_path() / "nlang-cpp/wasm/core");
        }
        for (auto& c : cands)
            if (fs::exists(c / "boot.js")) return c.string();
        return cands[0].string();
    };
    // файл машинерии ядра по имени: "index.html" ->
    // <nlang-cpp>/wasm/core/index.html
    auto core_file = [core_dir_of](Nvm& vm,
            const std::string& name) {
        return (std::filesystem::path(core_dir_of(vm)) / name)
            .string();
    };
    // ответ файлом (общий хелпер для shell/core ниже).
    // ядро (index.html/boot.js/nlang.js/nlang.wasm) отдаём
    // с no-store: браузерный module/fetch-кеш иначе держит
    // старую версию между пересборками wasm.
    auto file_answer = [](const std::string& path) {
        auto d = std::make_shared<std::map<std::string, NObject>>();
        (*d)["body"] = NObject(std::string());
        (*d)["statusCode"] = NObject((long long)200);
        auto hd = std::make_shared<std::map<std::string, NObject>>();
        (*hd)["Cache-Control"] = NObject("no-store");
        (*d)["headers"] = NObject(hd);
        std::string body;
        if (nlinternet_read_file(path, body)) {
            (*d)["body"] = NObject(body);
            (*d)["contentType"] = NObject(nlinternet_mime(path));
        } else {
            (*d)["body"] = NObject(std::string("404: ") + path);
            (*d)["statusCode"] = NObject((long long)404);
        }
        return NObject(d);
    };
    // net.shell — готовый обработчик: оболочка сектора
    // (входная страница ядра)
    add("shell", [core_file, file_answer]
        (Nvm& vm, const NArgs& a) -> NObject {
        return file_answer(core_file(vm, "index.html")); });
    // net.core — готовый обработчик для /core/{file}:
    // отдаёт файлы машинерии ядра (boot.js, nlang.*)
    add("core", [core_file, file_answer]
        (Nvm& vm, const NArgs& a) -> NObject {
        std::string name;
        // хендлеру передаётся ctx-словарь — берём params.file
        if (!a.pos.empty() && a.pos[0].isDict()) {
            auto& req = *a.pos[0].asDict();
            auto p = req.find("params");
            if (p != req.end() && p->second.isDict()) {
                auto& ps = *p->second.asDict();
                auto f = ps.find("file");
                if (f != ps.end()) name = f->second.asStr();
            }
        }
        return file_answer(core_file(vm, name)); });
    // «поднять сектор» одной командой: оболочка и машинерия
    // из <nlang-cpp>/wasm/core, .nlb/.nlc/.nlg — из каталога
    // скрипта. nlang-код вообще не видит техдеталей ядра.
    add("sector", [core_dir_of](Nvm& vm, const NArgs& a) {
        nlinternet_sector(vm.source_dir, core_dir_of(vm),
            a.pos.empty() ? 8765 : a.pos[0].asInt());
        return NObject(); });

    // --- строительство ответов: net.reply / net.json /
    // net.redirect / net.error / net.file ---
    auto reply_dict = [](std::function<void(
            std::map<std::string, NObject>&)> fill) {
        auto d = std::make_shared<std::map<std::string, NObject>>();
        (*d)["body"] = NObject(std::string());
        (*d)["html"] = NObject(std::string());
        (*d)["json"] = NObject(std::string());
        (*d)["contentType"] = NObject(std::string());
        (*d)["redirectUrl"] = NObject(std::string());
        (*d)["statusCode"] = NObject((long long)200);
        auto hd = std::make_shared<std::map<std::string, NObject>>();
        (*hd)["Cache-Control"] = NObject("no-store");
        (*d)["headers"] = NObject(hd);
        fill(*d);
        return NObject(d);
    };
    // net.reply("текст" [, "text/plain"] [, 200])
    add("reply", [reply_dict](Nvm&, const NArgs& a) {
        return reply_dict([&](auto& d) {
            if (!a.pos.empty()) d["body"] = NObject(a.pos[0].asStr());
            if (a.pos.size() > 1) d["contentType"] =
                NObject(a.pos[1].asStr());
            if (a.pos.size() > 2) d["statusCode"] =
                NObject(a.pos[2].asInt());
        }); });
    // net.json(data) — dict/list в json-ответ
    add("json", [reply_dict](Nvm&, const NArgs& a) {
        return reply_dict([&](auto& d) {
            if (!a.pos.empty())
                d["json"] = NObject(
                    nljson_export(to_jvalue(a.pos[0])));
        }); });
    // net.redirect("/sector" [, 302])
    add("redirect", [reply_dict](Nvm&, const NArgs& a) {
        return reply_dict([&](auto& d) {
            if (!a.pos.empty()) d["redirectUrl"] =
                NObject(a.pos[0].asStr());
            if (a.pos.size() > 1) d["statusCode"] =
                NObject(a.pos[1].asInt());
            else d["statusCode"] = NObject((long long)302);
        }); });
    // net.error(404 [, "нет такого"])
    add("error", [reply_dict](Nvm&, const NArgs& a) {
        return reply_dict([&](auto& d) {
            if (!a.pos.empty()) d["statusCode"] =
                NObject(a.pos[0].asInt());
            if (a.pos.size() > 1) d["body"] =
                NObject(a.pos[1].asStr());
            else d["body"] = NObject(
                std::to_string(d["statusCode"].asInt()));
        }); });
    // net.file("app.nlb") — файл относительно каталога скрипта
    add("file", [reply_dict](Nvm& vm, const NArgs& a) {
        std::string path = vm.source_dir.empty() || a.pos.empty()
            ? (a.pos.empty() ? "" : a.pos[0].asStr())
            : (std::filesystem::path(vm.source_dir) /
               a.pos[0].asStr()).string();
        return reply_dict([&](auto& d) {
            std::string body;
            if (!path.empty() && nlinternet_read_file(path, body)) {
                d["body"] = NObject(body);
                d["contentType"] = NObject(nlinternet_mime(path));
            } else {
                d["body"] = NObject(std::string("404: ") + path);
                d["statusCode"] = NObject((long long)404);
            }
        }); });
    // net.filter("/api", fn) — guard: fn(req)=false -> 403
    add("filter", [](Nvm& vm, const NArgs& a) {
        if (a.pos.size() >= 2) {
            NObject fn = a.pos[1];
            nlinternet_filter(a.pos[0].asStr(),
                [&vm, fn](ActivationContext& req) -> bool {
                    NArgs args;
                    args.pos.push_back(ctx_to_nobject(req));
                    return fn.call(vm, args).asBool();
                });
        }
        return NObject(); });
    add("attention", [](Nvm&, const NArgs& a) {
        std::string host = a.pos.empty() ? "0.0.0.0" : a.pos[0].asStr();
        long long port = a.pos.size() > 1 ? a.pos[1].asInt() : 8080;
        nlinternet_attention(host, port);
        return NObject(); });
    // --- канцелярский диалект (holonet) ---
    // то же самое, имперскими именами:
    //   holonet.directive("GET", "/x", officer)
    //   holonet.transmit("текст")  holonet.report({...})
    //   holonet.archive("f.nlb")   holonet.exile("/x")
    //   holonet.denounce(404)      holonet.censor("/api",fn)
    add("directive", [](Nvm& vm, const NArgs& a) {
        if (a.pos.size() >= 3)
            nlinternet_route(a.pos[0].asStr(), a.pos[1].asStr(),
                             handler_of(vm, a.pos[2]));
        return NObject(); });
    add("transmit", [reply_dict](Nvm&, const NArgs& a) {
        return reply_dict([&](auto& d) {
            if (!a.pos.empty()) d["body"] = NObject(a.pos[0].asStr());
            if (a.pos.size() > 1) d["contentType"] =
                NObject(a.pos[1].asStr());
            if (a.pos.size() > 2) d["statusCode"] =
                NObject(a.pos[2].asInt());
        }); });
    add("report", [reply_dict](Nvm&, const NArgs& a) {
        return reply_dict([&](auto& d) {
            if (!a.pos.empty())
                d["json"] = NObject(
                    nljson_export(to_jvalue(a.pos[0])));
        }); });
    add("archive", [reply_dict](Nvm& vm, const NArgs& a) {
        std::string path = vm.source_dir.empty() || a.pos.empty()
            ? (a.pos.empty() ? "" : a.pos[0].asStr())
            : (std::filesystem::path(vm.source_dir) /
               a.pos[0].asStr()).string();
        return reply_dict([&](auto& d) {
            std::string body;
            if (!path.empty() && nlinternet_read_file(path, body)) {
                d["body"] = NObject(body);
                d["contentType"] = NObject(nlinternet_mime(path));
            } else {
                d["body"] = NObject(std::string("404: ") + path);
                d["statusCode"] = NObject((long long)404);
            }
        }); });
    add("exile", [reply_dict](Nvm&, const NArgs& a) {
        return reply_dict([&](auto& d) {
            if (!a.pos.empty()) d["redirectUrl"] =
                NObject(a.pos[0].asStr());
            if (a.pos.size() > 1) d["statusCode"] =
                NObject(a.pos[1].asInt());
            else d["statusCode"] = NObject((long long)302);
        }); });
    add("denounce", [reply_dict](Nvm&, const NArgs& a) {
        return reply_dict([&](auto& d) {
            if (!a.pos.empty()) d["statusCode"] =
                NObject(a.pos[0].asInt());
            if (a.pos.size() > 1) d["body"] =
                NObject(a.pos[1].asStr());
            else d["body"] = NObject(
                std::to_string(d["statusCode"].asInt()));
        }); });
    add("censor", [](Nvm& vm, const NArgs& a) {
        if (a.pos.size() >= 2) {
            NObject fn = a.pos[1];
            nlinternet_filter(a.pos[0].asStr(),
                [&vm, fn](ActivationContext& req) -> bool {
                    NArgs args;
                    args.pos.push_back(ctx_to_nobject(req));
                    return fn.call(vm, args).asBool();
                });
        }
        return NObject(); });

    // ActivationContext(html) — конструктор ответа
    auto mkctx = std::make_shared<NFunction>();
    mkctx->name = "ActivationContext";
    mkctx->impl.store(
        +[](Nvm&, const NArgs& a) -> NObject {
            auto dd = std::make_shared<std::map<std::string, NObject>>();
            if (!a.pos.empty()) (*dd)["html"] = NObject(a.pos[0].asStr());
            (*dd)["statusCode"] = NObject((long long)200);
            (*dd)["body"] = NObject(std::string());
            (*dd)["json"] = NObject(std::string());
            (*dd)["contentType"] = NObject(std::string());
            (*dd)["redirectUrl"] = NObject(std::string());
            (*dd)["headers"] = NObject(std::make_shared<
                std::map<std::string, NObject>>());
            (*dd)["cookies"] = NObject(std::make_shared<
                std::map<std::string, NObject>>());
            return NObject(dd);
        }, std::memory_order_release);
    vm.set("ActivationContext", NObject(mkctx));
    vm.set(alias, NObject(d));
}

inline const ModuleDescriptor internet_desc = {
    "internet", "nlm_internet.h",
#ifdef _WIN32
    {"nlmodules", "ws2_32", "iphlpapi"},
#else
    {"nlmodules", "pthread"},
#endif
    &register_internet, "net", nullptr
};

// тот же модуль под канцелярским именем:
// using <holonet> -> holonet.sector(...) и прочий бюрократический
// диалект (transmit/report/archive/exile/denounce/censor)
inline const ModuleDescriptor holonet_desc = {
    "holonet", "nlm_internet.h",
#ifdef _WIN32
    {"nlmodules", "ws2_32", "iphlpapi"},
#else
    {"nlmodules", "pthread"},
#endif
    &register_internet, "holonet", nullptr
};

} // namespace includedlib
