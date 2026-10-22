#pragma once
// Модуль internet — лёгкий HTTP-сервер без внешних зависимостей
// (приближенная замена drogon).
//
// Простыми словами: программа регистрирует «обработчики» —
// функции вида ctx = handler(ctx). Сервер слушает порт,
// разбирает входящий HTTP-запрос в ActivationContext (params,
// headers, body), вызывает нужный обработчик и отправляет
// ответ обратно. Это однопоточный-per-connection сервер:
// для нагруженного продакшена не подходит — нет пула потоков,
// TLS, HTTP/2. Для учебных и внутренних сервисов — достаточно.
#include <string>
#include <unordered_map>
#include <functional>

// Контекст запроса/ответа — то, что видит handler.
// Заполняется из входящего запроса; handler меняет поля,
// сервер превращает их в HTTP-ответ.
class ActivationContext {
public:
    std::unordered_map<std::string, std::string> params;  // query + path + form (слитно)
    // раздельно: адресные распоряжения (path+query) и
    // письменные прошения (urlencoded-тело)
    std::unordered_map<std::string, std::string> query_params;
    std::unordered_map<std::string, std::string> form_params;
    std::unordered_map<std::string, std::string> headers; // заголовки ОТВЕТА
    std::unordered_map<std::string, std::string> cookies; // куки ОТВЕТА
    std::unordered_map<std::string, std::string> requestHeaders;
    std::unordered_map<std::string, std::string> requestCookies;
    std::string html;        // ответ как HTML
    std::string body;        // тело запроса (для POST) или ответа
    std::string json;        // ответ как JSON (ставит Content-Type)
    std::string redirectUrl; // если задано — 302 редирект
    std::string contentType;
    int statusCode = 200;

    ActivationContext() {}
    explicit ActivationContext(std::unordered_map<std::string, std::string> p)
        : params(std::move(p)) {}
    explicit ActivationContext(std::string html_) : html(std::move(html_)) {}

    std::string get(const std::string& key) const {
        auto it = params.find(key);
        return it == params.end() ? "" : it->second;
    }
    void set(const std::string& k, const std::string& v) { params[k] = v; }
    void setHeader(const std::string& k, const std::string& v) { headers[k] = v; }
    void setCookie(const std::string& k, const std::string& v) { cookies[k] = v; }
    std::string getHeader(const std::string& k) const {
        auto it = requestHeaders.find(k);
        return it == requestHeaders.end() ? "" : it->second;
    }
    std::string getCookie(const std::string& k) const {
        auto it = requestCookies.find(k);
        return it == requestCookies.end() ? "" : it->second;
    }
    // сессии: server-side хранилище по куке sid
    std::string sessionGet(const std::string& k) const;
    void sessionSet(const std::string& k, const std::string& v);
    void sessionErase(const std::string& k);
    // внутреннее: id сессии текущего запроса
    std::string _sessionId;
};

using NlHandler = std::function<ActivationContext(ActivationContext&)>;

// регистрация маршрутов: path может содержать {имя} —
// например "/user/{id}", значение попадёт в params["id"].
void nlinternet_route(const std::string& method, const std::string& path,
                      NlHandler handler);
void nlinternet_get(const std::string& path, NlHandler h);
void nlinternet_post(const std::string& path, NlHandler h);
void nlinternet_put(const std::string& path, NlHandler h);
void nlinternet_del(const std::string& path, NlHandler h);
void nlinternet_patch(const std::string& path, NlHandler h);
void nlinternet_head(const std::string& path, NlHandler h);
void nlinternet_options(const std::string& path, NlHandler h);

// Рецепторные имена — то же, что в py-версии:
// метод = рецептор нейромедиатора. net.ampa("/", h) == net.get("/", h).
//   ampa -> GET    (быстрый вход, чтение)
//   nmda -> POST   (запись через усиление связи)
//   d2   -> PUT    (тормозной контур: замена)
//   gabab-> DELETE (GABA-B: подавление)
//   gabaa-> PATCH  (GABA-A: точечная правка)
//   m1   -> HEAD   (мускариновый: только заголовки)
//   m2   -> OPTIONS
void nlinternet_ampa(const std::string& path, NlHandler h);
void nlinternet_nmda(const std::string& path, NlHandler h);
void nlinternet_d2(const std::string& path, NlHandler h);
void nlinternet_gabab(const std::string& path, NlHandler h);
void nlinternet_gabaa(const std::string& path, NlHandler h);
void nlinternet_m1(const std::string& path, NlHandler h);
void nlinternet_m2(const std::string& path, NlHandler h);

// статика: раздавать файлы из каталога по /<имя файла>
void nlinternet_staticDir(const std::string& dir);
void nlinternet_wire(const std::vector<std::string>& protos,
                     const std::string& path, NlHandler handler);

// pre-routing guard: если вернёт false — 403. prefix — начало пути.
void nlinternet_filter(const std::string& prefix,
                       std::function<bool(ActivationContext&)> guard);

// запуск сервера (блокирующий) — как attention в py-версии
void nlinternet_listen(long long port);
void nlinternet_attention(const std::string& host, long long port);

// «поднять сектор» одной командой: оболочка и машинерия из
// core_dir, nlang-файлы сектора — из sector_dir
void nlinternet_sector(const std::string& sector_dir,
                       const std::string& core_dir,
                       long long port);

// helpers для обработчиков: mime по расширению, чтение файла
std::string nlinternet_mime(const std::string& path);
bool nlinternet_read_file(const std::string& path,
                          std::string& out);

// мост для интерпретатора: если установлен bridge, разбор запроса
// идёт через него (eval-режим вызывает NObject-хендлеры)
using NlBridge = void(*)(void* ctx, const std::string& method,
                         const std::string& path,
                         ActivationContext& req, ActivationContext& resp,
                         bool& handled);
void nlinternet_set_bridge(void* ctx, NlBridge bridge);
