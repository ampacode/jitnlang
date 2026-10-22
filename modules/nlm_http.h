#pragma once
// Модуль http — клиент на libcurl (порт includedlib/http.py).
//
//   nlhttp_get(url[, hdrs])          -> string тело
//   nlhttp_post(url, body[, hdrs])
//   nlhttp_put/patch/delete
//   nlhttp_status()                  -> long код ответа
//   nlhttp_err()                     -> string ошибка (status==0)
//   nlhttp_timeout(sec)
//   nlhttp_header(name, value)       -> заголовок на след. запросы
//   nlhttp_clearHeaders()
//
//   s = nlhttp_session()             -> persistent сессия
//     s.get/post/put/patch/del/header/status/err/timeout
#include <string>
#include <sstream>
#include <unordered_map>
#include <curl/curl.h>

inline size_t _nlhttp_write(char* ptr, size_t sz, size_t n,
                            void* ud) {
    ((std::string*)ud)->append(ptr, sz * n);
    return sz * n;
}

// захват заголовков ОТВЕТА: "Name: value" -> map
inline size_t _nlhttp_header_write(char* ptr, size_t sz, size_t n,
                                   void* ud) {
    std::string line(ptr, sz * n);
    auto* m = (std::unordered_map<std::string, std::string>*)ud;
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string k = line.substr(0, colon);
        std::string v = line.substr(colon + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
            v.erase(v.begin());
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n'))
            v.pop_back();
        (*m)[k] = v;
    }
    return sz * n;
}

struct NHSession {
    CURL* c = nullptr;
    struct curl_slist* hdrs = nullptr;
    long st = 0;
    std::string er;
    // заголовки последнего ответа
    std::unordered_map<std::string, std::string> resp_headers;

    NHSession() {
        static bool init = false;
        if (!init) {
            curl_global_init(CURL_GLOBAL_DEFAULT);
            init = true;
        }
        c = curl_easy_init();
        curl_easy_setopt(c, CURLOPT_USERAGENT, "nlang-http/1.0");
        // cookie jar в памяти: Set-Cookie сервера едет дальше —
        // это и есть «сессия» исходящего клиента
        curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");
    }
    NHSession(const NHSession&) = delete;
    NHSession(NHSession&& o) noexcept
        : c(o.c), hdrs(o.hdrs), st(o.st), er(std::move(o.er)),
          resp_headers(std::move(o.resp_headers)) {
        o.c = nullptr;
        o.hdrs = nullptr;
    }
    ~NHSession() {
        if (hdrs) curl_slist_free_all(hdrs);
        if (c) curl_easy_cleanup(c);
    }

    void header(const std::string& n, const std::string& v) {
        hdrs = curl_slist_append(hdrs, (n + ": " + v).c_str());
    }
    void clearHeaders() {
        if (hdrs) curl_slist_free_all(hdrs);
        hdrs = nullptr;
    }
    void timeout(double sec) {
        curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)sec);
    }
    long status() { return st; }
    std::string err() { return er; }

    std::string _do(const std::string& method,
                    const std::string& url,
                    const std::string& extraHdrs,
                    const std::string& body, bool hasBody) {
        std::string out;
        st = 0;
        er.clear();
        resp_headers.clear();
        curl_easy_reset(c);
        // reset стирает cookie-движок — включаем заново
        curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "nlang-http/1.0");
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
#ifdef _WIN32
        // корни из системного хранилища Windows
        curl_easy_setopt(c, CURLOPT_SSL_OPTIONS,
                         (long)CURLSSLOPT_NATIVE_CA);
#endif
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, _nlhttp_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION,
                         _nlhttp_header_write);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &resp_headers);
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
        struct curl_slist* h = nullptr;
        for (auto* p = hdrs; p; p = p->next)
            h = curl_slist_append(h, p->data);
        if (!extraHdrs.empty()) {
            std::istringstream ss(extraHdrs);
            std::string line;
            while (std::getline(ss, line)) {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty())
                    h = curl_slist_append(h, line.c_str());
            }
        }
        if (hasBody) {
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE,
                             (long)body.size());
        }
        if (h) curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        CURLcode rc = curl_easy_perform(c);
        if (h) curl_slist_free_all(h);
        if (rc != CURLE_OK) {
            er = curl_easy_strerror(rc);
            return std::string();
        }
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st);
        return out;
    }

    std::string get(const std::string& url,
                    const std::string& h = "") {
        return _do("GET", url, h, "", false);
    }
    std::string post(const std::string& url,
                     const std::string& body,
                     const std::string& h = "") {
        return _do("POST", url, h, body, true);
    }
    std::string put(const std::string& url,
                    const std::string& body,
                    const std::string& h = "") {
        return _do("PUT", url, h, body, true);
    }
    std::string patch(const std::string& url,
                      const std::string& body,
                      const std::string& h = "") {
        return _do("PATCH", url, h, body, true);
    }
    std::string del(const std::string& url,
                    const std::string& h = "") {
        return _do("DELETE", url, h, "", false);
    }
};

inline NHSession* _nlhttp_global() {
    static NHSession s;
    return &s;
}
inline NHSession nlhttp_session() { return NHSession(); }
inline std::string nlhttp_get(const std::string& url,
                              const std::string& h = "") {
    return _nlhttp_global()->get(url, h);
}
inline std::string nlhttp_post(const std::string& url,
                               const std::string& body,
                               const std::string& h = "") {
    return _nlhttp_global()->post(url, body, h);
}
inline std::string nlhttp_put(const std::string& url,
                              const std::string& body,
                              const std::string& h = "") {
    return _nlhttp_global()->put(url, body, h);
}
inline std::string nlhttp_patch(const std::string& url,
                                const std::string& body,
                                const std::string& h = "") {
    return _nlhttp_global()->patch(url, body, h);
}
inline std::string nlhttp_delete(const std::string& url,
                                 const std::string& h = "") {
    return _nlhttp_global()->del(url, h);
}
inline void nlhttp_header(const std::string& n,
                          const std::string& v) {
    _nlhttp_global()->header(n, v);
}
inline void nlhttp_clearHeaders() {
    _nlhttp_global()->clearHeaders();
}
inline void nlhttp_timeout(double sec) {
    _nlhttp_global()->timeout(sec);
}
inline long long nlhttp_status() {
    return _nlhttp_global()->status();
}
inline std::string nlhttp_err() {
    return _nlhttp_global()->err();
}
