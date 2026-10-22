// Модуль internet — мини HTTP/1.1 сервер.
//
// Устройство простое:
//   socket() -> bind() -> listen() -> accept() в цикле;
//   каждое соединение — свой поток (std::thread::detach);
//   запрос разбирается вручную: первая строка "GET /p?a=1 HTTP/1.1",
//   заголовки до пустой строки, остальное — body;
//   маршруты — список {метод, шаблон, handler}; шаблон "/u/{id}"
//   сравнивается сегментами, {id} захватывает сегмент в params.
#include "nlm_internet.h"
#include <vector>
#include <thread>
#include <mutex>
#include <map>
#include <iostream>
#include <cstring>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
  #include <chrono>
  using socket_t = SOCKET;
  #define NL_INVALID_SOCK INVALID_SOCKET
  #define nl_close closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <unistd.h>
  using socket_t = int;
  #define NL_INVALID_SOCK (-1)
  #define nl_close close
#endif

namespace {

struct Route {
    std::string method, path;
    NlHandler handler;
    // сегменты пути; "{x}" = захват
    std::vector<std::pair<std::string, bool>> segs; // (текст или имя, захват?)
};

std::vector<Route> routes;
std::mutex routes_mtx;
std::vector<std::pair<std::string,
    std::function<bool(ActivationContext&)>>> filters;
std::string static_dir;
// сессии: sid -> {key: value}
std::unordered_map<std::string,
    std::unordered_map<std::string, std::string>> sessions;
std::mutex sessions_mtx;
long long session_counter = 1;
void* bridge_ctx = nullptr;
NlBridge bridge_fn = nullptr;

std::vector<std::pair<std::string, bool>> split_path(const std::string& p) {
    std::vector<std::pair<std::string, bool>> r;
    size_t i = 0;
    while (i < p.size()) {
        size_t j = p.find('/', i);
        std::string seg = p.substr(i, j == std::string::npos
                                        ? j : j - i);
        if (!seg.empty()) {
            if (seg.front() == '{' && seg.back() == '}')
                r.push_back({seg.substr(1, seg.size() - 2), true});
            else
                r.push_back({seg, false});
        }
        i = (j == std::string::npos) ? p.size() : j + 1;
    }
    return r;
}

void url_decode_inplace(std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') out += ' ';
        else out += s[i];
    }
    s = std::move(out);
}

void parse_query(const std::string& q,
                 std::unordered_map<std::string, std::string>& target) {
    size_t i = 0;
    while (i < q.size()) {
        size_t amp = q.find('&', i);
        std::string pair = q.substr(i, amp == std::string::npos
                                        ? amp : amp - i);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string k = pair.substr(0, eq);
            std::string v = pair.substr(eq + 1);
            url_decode_inplace(k);
            url_decode_inplace(v);
            target[k] = v;
        }
        i = (amp == std::string::npos) ? q.size() : amp + 1;
    }
}

// Content-Type по расширению для статики
std::string mime_of(const std::string& path) {
    auto dot = path.rfind('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    static const std::map<std::string, std::string> m = {
        {"html", "text/html"}, {"htm", "text/html"}, {"css", "text/css"},
        {"js", "text/javascript"}, {"json", "application/json"},
        {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
        {"gif", "image/gif"}, {"svg", "image/svg+xml"}, {"ico", "image/x-icon"},
        {"txt", "text/plain"}, {"pdf", "application/pdf"},
        {"wasm", "application/wasm"},
        {"nlb", "text/plain"}, {"nlc", "text/plain"},
        {"nlg", "text/plain"},
    };
    auto it = m.find(ext);
    return it == m.end() ? "application/octet-stream" : it->second;
}

bool read_file(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    if (n > 0) std::fread(&out[0], 1, n, f);
    std::fclose(f);
    return true;
}

std::string status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

void send_all(socket_t s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = (int)::send(s, data.data() + sent,
                            (int)(data.size() - sent), 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }
}

void send_response(socket_t s, const ActivationContext& ac) {
    std::string body;
    std::string ctype = ac.contentType;
    if (!ac.redirectUrl.empty()) {
        body = "";
    } else if (!ac.json.empty()) {
        body = ac.json;
        if (ctype.empty()) ctype = "application/json";
    } else if (!ac.html.empty()) {
        body = ac.html;
        if (ctype.empty()) ctype = "text/html; charset=utf-8";
    } else {
        body = ac.body;
    }

    int status = ac.statusCode;
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " +
                       status_text(status) + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (!ctype.empty()) head += "Content-Type: " + ctype + "\r\n";
    if (!ac.redirectUrl.empty())
        head += "Location: " + ac.redirectUrl + "\r\n";
    for (auto& kv : ac.headers)
        head += kv.first + ": " + kv.second + "\r\n";
    for (auto& kv : ac.cookies)
        head += "Set-Cookie: " + kv.first + "=" + kv.second +
                "; Path=/\r\n";
    head += "Connection: close\r\n\r\n";
    send_all(s, head + body);
}

// найти маршрут и заполнить path-параметры.
// литеральные сегменты приоритетнее {params}: "/login" важнее "/{file}"
// независимо от порядка регистрации.
Route* find_route(const std::string& method, const std::string& path,
                  ActivationContext& ctx) {
    auto pathsegs = split_path(path);
    Route* best = nullptr;
    std::unordered_map<std::string, std::string> best_captured;
    size_t best_literals = 0;
    for (auto& r : routes) {
        if (r.method != method) continue;
        if (r.segs.size() != pathsegs.size()) continue;
        bool ok = true;
        size_t literals = 0;
        std::unordered_map<std::string, std::string> captured;
        for (size_t i = 0; i < r.segs.size(); ++i) {
            if (r.segs[i].second) {
                std::string v = pathsegs[i].first;
                url_decode_inplace(v);
                captured[r.segs[i].first] = v;
            } else if (r.segs[i].first != pathsegs[i].first) {
                ok = false;
                break;
            } else {
                ++literals;
            }
        }
        // строго больше: при равенстве побеждает раньше зарегистрированный
        if (ok && (!best || literals > best_literals)) {
            best = &r;
            best_captured = std::move(captured);
            best_literals = literals;
        }
    }
    if (best)
        for (auto& kv : best_captured) {
            ctx.params[kv.first] = kv.second;
            // захваты из адресной строки — это orders
            ctx.query_params[kv.first] = kv.second;
        }
    return best;
}

void handle_connection(socket_t client) {
    // читаем запрос (до ~1 МБ — достаточно для простых форм)
    std::string raw;
    char buf[8192];
    for (;;) {
        int n = (int)::recv(client, buf, sizeof(buf), 0);
        if (n <= 0) { nl_close(client); return; }
        raw.append(buf, n);
        // конец заголовков
        if (raw.find("\r\n\r\n") != std::string::npos) {
            // добираем body по Content-Length если есть
            size_t hpos = raw.find("\r\n\r\n");
            std::string head = raw.substr(0, hpos);
            long long want = 0;
            auto p = head.find("Content-Length:");
            if (p != std::string::npos)
                want = std::stoll(head.substr(p + 15));
            if ((long long)(raw.size() - hpos - 4) >= want) break;
        }
        if (raw.size() > (1 << 20)) break;
    }

    size_t eol = raw.find("\r\n");
    if (eol == std::string::npos) { nl_close(client); return; }
    std::string first = raw.substr(0, eol);
    std::string method = first.substr(0, first.find(' '));
    std::string target = first.substr(first.find(' ') + 1);
    target = target.substr(0, target.find(' '));
    std::string path = target, query;
    size_t qm = target.find('?');
    if (qm != std::string::npos) {
        path = target.substr(0, qm);
        query = target.substr(qm + 1);
    }

    ActivationContext ctx;
    // заголовки запроса
    size_t pos = eol + 2;
    for (;;) {
        size_t e = raw.find("\r\n", pos);
        if (e == std::string::npos || e == pos) break;
        std::string line = raw.substr(pos, e - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = line.substr(0, colon);
            std::string v = line.substr(colon + 1);
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            ctx.requestHeaders[k] = v;
            if (k == "Cookie") {
                size_t ci = 0;
                while (ci < v.size()) {
                    size_t sc = v.find(';', ci);
                    std::string c = v.substr(ci,
                        sc == std::string::npos ? sc : sc - ci);
                    size_t ce = c.find('=');
                    if (ce != std::string::npos)
                        ctx.requestCookies[
                            c.substr(0, ce + (c[ce-1]==' '?0:0))] =
                            c.substr(ce + 1);
                    ci = sc == std::string::npos ? v.size() : sc + 2;
                }
            }
        }
        pos = e + 2;
    }
    size_t bodypos = raw.find("\r\n\r\n");
    if (bodypos != std::string::npos)
        ctx.body = raw.substr(bodypos + 4);

    // адресная строка -> orders; слитный params тоже наполняем
    parse_query(query, ctx.query_params);
    for (auto& kv : ctx.query_params) ctx.params[kv.first] = kv.second;
    // application/x-www-form-urlencoded body -> writs (+ в params)
    auto ct = ctx.requestHeaders.find("Content-Type");
    if (ct != ctx.requestHeaders.end() &&
        ct->second.find("x-www-form-urlencoded") != std::string::npos) {
        parse_query(ctx.body, ctx.form_params);
        for (auto& kv : ctx.form_params) ctx.params[kv.first] = kv.second;
    }

    // сессия по куке sid
    auto sid = ctx.requestCookies.find("sid");
    if (sid != ctx.requestCookies.end())
        ctx._sessionId = sid->second;
    else {
        std::lock_guard<std::mutex> lk(sessions_mtx);
        ctx._sessionId = "s" + std::to_string(session_counter++);
        ctx.cookies["sid"] = ctx._sessionId;
    }

    // фильтры
    for (auto& f : filters) {
        if (path.rfind(f.first, 0) == 0 && !f.second(ctx)) {
            ActivationContext denied;
            denied.statusCode = 403;
            denied.body = "403 Forbidden";
            send_response(client, denied);
            nl_close(client);
            return;
        }
    }

    // маршрут
    bool handled = false;
    ActivationContext resp;
    {
        std::lock_guard<std::mutex> lk(routes_mtx);
        if (Route* r = find_route(method, path, ctx)) {
            resp = r->handler(ctx);
            handled = true;
        }
    }
    if (!handled && bridge_fn) {
        bridge_fn(bridge_ctx, method, path, ctx, resp, handled);
    }
    if (!handled && !static_dir.empty()) {
        // статика: /file -> static_dir/file (без выхода за каталог)
        if (path.find("..") == std::string::npos) {
            std::string content;
            std::string fp = static_dir +
                (path == "/" ? "/index.html" : path);
            if (read_file(fp, content)) {
                resp.body = content;
                resp.contentType = mime_of(fp);
                handled = true;
            }
        }
    }
    if (!handled) {
        resp.statusCode = 404;
        resp.body = "404 Not Found";
    }
    // sid живёт в куке ответа — иначе хранилище сеанса
    // никогда не доедет до клиента
    if (resp.cookies.find("sid") == resp.cookies.end())
        resp.cookies["sid"] = ctx._sessionId;
    send_response(client, resp);
    nl_close(client);
}

struct WinsockInit {
    WinsockInit() {
#ifdef _WIN32
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
#endif
    }
};

#ifdef _WIN32
// PID процесса, который слушает порт, через GetExtendedTcpTable
static void kill_port_owner(u_short port) {
    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                        TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<char> buf(size);
    auto* table = (MIB_TCPTABLE_OWNER_PID*)buf.data();
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_LISTENER,
                            0) != NO_ERROR)
        return;
    DWORD me = GetCurrentProcessId();
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        auto& e = table->table[i];
        if (e.dwState != MIB_TCP_STATE_LISTEN) continue;
        if (ntohs((u_short)e.dwLocalPort) != port) continue;
        DWORD pid = e.dwOwningPid;
        if (pid == me || pid == 0) continue;
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) {
            TerminateProcess(h, 0);
            CloseHandle(h);
            std::cerr << "internet: kill-last pid " << pid
                      << " (порт " << port << ")\n";
            // сокет освобождается не мгновенно
            std::this_thread::sleep_for(
                std::chrono::milliseconds(300));
        }
    }
}
#else
static void kill_port_owner(u_short) {}
#endif

} // namespace

void ActivationContext::sessionSet(const std::string& k,
                                   const std::string& v) {
    if (_sessionId.empty()) return;
    std::lock_guard<std::mutex> lk(sessions_mtx);
    sessions[_sessionId][k] = v;
}
std::string ActivationContext::sessionGet(const std::string& k) const {
    if (_sessionId.empty()) return "";
    std::lock_guard<std::mutex> lk(sessions_mtx);
    auto s = sessions.find(_sessionId);
    if (s == sessions.end()) return "";
    auto it = s->second.find(k);
    return it == s->second.end() ? "" : it->second;
}
void ActivationContext::sessionErase(const std::string& k) {
    if (_sessionId.empty()) return;
    std::lock_guard<std::mutex> lk(sessions_mtx);
    sessions[_sessionId].erase(k);
}

void nlinternet_route(const std::string& method, const std::string& path,
                      NlHandler handler) {
    std::lock_guard<std::mutex> lk(routes_mtx);
    Route r{method, path, std::move(handler), {}};
    r.segs = split_path(path);
    routes.push_back(std::move(r));
}
void nlinternet_get(const std::string& p, NlHandler h) {
    nlinternet_route("GET", p, std::move(h));
}
void nlinternet_post(const std::string& p, NlHandler h) {
    nlinternet_route("POST", p, std::move(h));
}
void nlinternet_put(const std::string& p, NlHandler h) {
    nlinternet_route("PUT", p, std::move(h));
}
void nlinternet_del(const std::string& p, NlHandler h) {
    nlinternet_route("DELETE", p, std::move(h));
}
void nlinternet_patch(const std::string& p, NlHandler h) {
    nlinternet_route("PATCH", p, std::move(h));
}
void nlinternet_head(const std::string& p, NlHandler h) {
    nlinternet_route("HEAD", p, std::move(h));
}
void nlinternet_options(const std::string& p, NlHandler h) {
    nlinternet_route("OPTIONS", p, std::move(h));
}
// рецепторные алиасы
void nlinternet_ampa(const std::string& p, NlHandler h) {
    nlinternet_route("GET", p, std::move(h));
}
void nlinternet_nmda(const std::string& p, NlHandler h) {
    nlinternet_route("POST", p, std::move(h));
}
void nlinternet_d2(const std::string& p, NlHandler h) {
    nlinternet_route("PUT", p, std::move(h));
}
void nlinternet_gabab(const std::string& p, NlHandler h) {
    nlinternet_route("DELETE", p, std::move(h));
}
void nlinternet_gabaa(const std::string& p, NlHandler h) {
    nlinternet_route("PATCH", p, std::move(h));
}
void nlinternet_m1(const std::string& p, NlHandler h) {
    nlinternet_route("HEAD", p, std::move(h));
}
void nlinternet_m2(const std::string& p, NlHandler h) {
    nlinternet_route("OPTIONS", p, std::move(h));
}
void nlinternet_staticDir(const std::string& dir) { static_dir = dir; }
// wire(["ampa","d2l"], "/p", h) — набор рецепторов -> method +
// content-type (как checkWiresProtocolds в py):
//   d2l+d2s -> json, d1+d2s -> form, d1 -> octet, d2l -> multipart
void nlinternet_wire(const std::vector<std::string>& protos,
                     const std::string& path, NlHandler handler) {
    static const std::map<std::string, std::string> methods = {
        {"ampa","GET"}, {"nmda","POST"}, {"d2","PUT"},
        {"gabab","DELETE"}, {"gabaa","PATCH"},
        {"m1","HEAD"}, {"m2","OPTIONS"},
    };
    std::string method = "GET";
    bool hasD1 = false, hasD2l = false, hasD2s = false;
    for (auto& p : protos) {
        auto it = methods.find(p);
        if (it != methods.end()) method = it->second;
        else if (p == "d1") hasD1 = true;
        else if (p == "d2l") hasD2l = true;
        else if (p == "d2s") hasD2s = true;
    }
    std::string ctype;
    if (hasD2l && hasD2s) ctype = "application/json";
    else if (hasD1 && hasD2s) ctype = "application/x-www-form-urlencoded";
    else if (hasD1) ctype = "application/octet-stream";
    else if (hasD2l) ctype = "multipart/form-data";
    nlinternet_route(method, path,
        [ctype, handler = std::move(handler)](ActivationContext& r) {
            ActivationContext resp = handler(r);
            if (!ctype.empty() && resp.contentType.empty())
                resp.contentType = ctype;
            return resp;
        });
}
void nlinternet_filter(const std::string& prefix,
                       std::function<bool(ActivationContext&)> g) {
    filters.push_back({prefix, std::move(g)});
}
void nlinternet_set_bridge(void* ctx, NlBridge fn) {
    bridge_ctx = ctx;
    bridge_fn = fn;
}

void nlinternet_listen(long long port) {
    nlinternet_attention("0.0.0.0", port);
}

// обёртки над внутренними helpers — для eval-слоёв
std::string nlinternet_mime(const std::string& path) {
    return mime_of(path);
}
bool nlinternet_read_file(const std::string& path,
                          std::string& out) {
    return read_file(path, out);
}

// «поднять сектор» одной командой: оболочка и машинерия из
// core_dir, nlang-файлы (.nlb/.nlc/.nlg) — из sector_dir.
// nlang-код не видит ни оболочки, ни путей ядра.
void nlinternet_sector(const std::string& sector_dir,
                       const std::string& core_dir,
                       long long port) {
    nlinternet_get("/", [core_dir](ActivationContext&) {
        ActivationContext r;
        std::string body;
        if (read_file(core_dir + "/index.html", body)) {
            r.body = body;
            r.contentType = "text/html; charset=utf-8";
        } else {
            r.body = "404: оболочка ядра не найдена";
            r.statusCode = 404;
        }
        return r; });
    nlinternet_get("/{file}",
        [sector_dir, core_dir](ActivationContext& ac) {
            std::string name = ac.get("file");
            bool is_sector = name.ends_with(".nlb") ||
                             name.ends_with(".nlc") ||
                             name.ends_with(".nlg");
            std::string path = (is_sector ? sector_dir : core_dir)
                               + "/" + name;
            ActivationContext r;
            std::string body;
            if (read_file(path, body)) {
                r.body = body;
                r.contentType = mime_of(path);
            } else {
                r.body = "404: " + path;
                r.statusCode = 404;
            }
            return r; });
    nlinternet_listen(port);
}

void nlinternet_attention(const std::string& host, long long port) {
    static WinsockInit wsi;
    // --kill-last (env NLANG_KILL_LAST): освободить порт,
    // убив его слушателя — dev-рестарт сервера без ручной
    // зачистки
    if (std::getenv("NLANG_KILL_LAST"))
        kill_port_owner((u_short)port);
    socket_t srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv == NL_INVALID_SOCK) {
        std::cerr << "internet: не удалось создать сокет\n";
        return;
    }
    int one = 1;
    // на Windows SO_REUSEADDR НЕ мешает второму bind на
    // занятый порт — нужен EXCLUSIVEADDRUSE, иначе два
    // сервера молча живут на одном порту
#ifdef _WIN32
    setsockopt(srv, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               (char*)&one, sizeof(one));
#else
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&one, sizeof(one));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = host == "localhost"
        ? htonl(INADDR_LOOPBACK) : INADDR_ANY;
    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::cerr << "internet: порт " << port << " занят\n";
        nl_close(srv);
        return;
    }
    if (::listen(srv, 64) != 0) {
        nl_close(srv);
        return;
    }
    std::cout << "internet: слушаю http://" << host << ":" << port << "\n";
    std::cout.flush();
    for (;;) {
        socket_t client = ::accept(srv, nullptr, nullptr);
        if (client == NL_INVALID_SOCK) continue;
        std::thread(handle_connection, client).detach();
    }
}
