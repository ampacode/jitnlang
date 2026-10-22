#pragma once
// nlang runtime — генерируемый C++ код линкуется с этим хедером.
// nlOut — print, nlStr — str(), JValue — json/dict-тип.
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <variant>
#include <functional>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <stdexcept>

// ================= арифметика с семантикой интерпретатора
// nl_div: eval-деление — всегда true-division (double).
// Скомпилированный (l / r) на int'ах давал бы int-деление,
// а eval возвращает double при неточном результате.
// nl_mod: % для int -> %, для double -> fmod (как eval).
// Перегрузки нужны потому, что (a/b) после nl_div стало
// double — и `(a/b) % c` через обычный % не компилируется.
inline double nl_div(double a, double b) {
    if (b == 0.0)
        throw std::runtime_error("деление на ноль");
    return a / b;
}
// common_type поднимает int/long long/double как eval:
// любой double-операнд -> fmod, иначе int-остаток
template<class A, class B>
inline auto nl_mod(A a, B b) {
    using T = std::common_type_t<A, B>;
    if (b == 0)
        throw std::runtime_error("деление на ноль");
    if constexpr (std::is_floating_point_v<T>)
        return std::fmod((T)a, (T)b);
    else
        return (T)((T)a % (T)b);
}

// ================= JValue — мини-json =================
// Замена Json::Value для dict/json-семантики nlang.
struct JValue {
    using Ptr = std::shared_ptr<JValue>;
    using Object = std::map<std::string, JValue>;
    using Array = std::vector<JValue>;
    std::variant<std::monostate, bool, long long, double,
                 std::string, Array, Object> v;

    JValue() : v(std::monostate{}) {}
    JValue(bool b) : v(b) {}
    JValue(int i) : v((long long)i) {}
    JValue(long i) : v((long long)i) {}
    JValue(long long i) : v(i) {}
    JValue(unsigned i) : v((long long)i) {}
    JValue(unsigned long i) : v((long long)i) {}
    JValue(unsigned long long i) : v((long long)i) {}
    JValue(double d) : v(d) {}
    JValue(const char* s) : v(std::string(s)) {}
    JValue(std::string s) : v(std::move(s)) {}
    JValue(Array a) : v(std::move(a)) {}
    JValue(Object o) : v(std::move(o)) {}

    bool isObject() const { return std::holds_alternative<Object>(v); }
    bool isArray() const { return std::holds_alternative<Array>(v); }

    // for k, v : dict — итерация по Object
    Object::iterator begin() {
        static Object empty_;
        return isObject() ? std::get<Object>(v).begin() : empty_.begin();
    }
    Object::iterator end() {
        static Object empty_;
        return isObject() ? std::get<Object>(v).end() : empty_.end();
    }

    // 0-индексация: j[0] -> array[0]; строковый ключ -> object
    JValue& operator[](long long i) {
        if (!isArray() && !isObject()) v = Array{};
        if (isArray()) {
            auto& a = std::get<Array>(v);
            if (i >= 0 && (size_t)i < a.size()) return a[i];
            static JValue null_;
            return null_;
        }
        return std::get<Object>(v)[std::to_string(i)];
    }
    JValue& operator[](int i) { return (*this)[(long long)i]; }
    JValue& operator[](const std::string& k) {
        if (!isObject()) v = Object{};
        return std::get<Object>(v)[k];
    }
    JValue& operator[](const char* k) { return (*this)[std::string(k)]; }

    long long size() const {
        if (isArray()) return (long long)std::get<Array>(v).size();
        if (isObject()) return (long long)std::get<Object>(v).size();
        if (std::holds_alternative<std::string>(v))
            return (long long)std::get<std::string>(v).size();
        return 0;
    }
    double asDouble() const {
        if (std::holds_alternative<double>(v)) return std::get<double>(v);
        if (std::holds_alternative<long long>(v))
            return (double)std::get<long long>(v);
        if (std::holds_alternative<bool>(v)) return std::get<bool>(v);
        return 0.0;
    }
    long long asInt() const { return (long long)asDouble(); }
    std::string asString() const {
        if (std::holds_alternative<std::string>(v))
            return std::get<std::string>(v);
        if (std::holds_alternative<long long>(v))
            return std::to_string(std::get<long long>(v));
        if (std::holds_alternative<double>(v)) {
            char b[64];
            snprintf(b, 64, "%g", std::get<double>(v));
            return b;
        }
        if (std::holds_alternative<bool>(v))
            return std::get<bool>(v) ? "true" : "false";
        return "";
    }
    explicit operator bool() const {
        if (std::holds_alternative<bool>(v)) return std::get<bool>(v);
        if (std::holds_alternative<long long>(v))
            return std::get<long long>(v) != 0;
        if (std::holds_alternative<double>(v))
            return std::get<double>(v) != 0.0;
        if (std::holds_alternative<std::string>(v))
            return !std::get<std::string>(v).empty();
        if (isArray()) return !std::get<Array>(v).empty();
        if (isObject()) return !std::get<Object>(v).empty();
        return false;
    }
    std::string dump() const {
        if (isObject()) {
            std::string s = "{";
            bool f = true;
            for (auto& kv : std::get<Object>(v)) {
                if (!f) s += ",";
                f = false;
                s += "\"" + kv.first + "\":" + kv.second.dump();
            }
            return s + "}";
        }
        if (isArray()) {
            std::string s = "[";
            bool f = true;
            for (auto& e : std::get<Array>(v)) {
                if (!f) s += ",";
                f = false;
                s += e.dump();
            }
            return s + "]";
        }
        if (std::holds_alternative<std::string>(v))
            return "\"" + std::get<std::string>(v) + "\"";
        return asString().empty() ? "null" : asString();
    }
};

inline JValue JObj() { return JValue(JValue::Object{}); }
template<typename... Args>
inline JValue JObj(const std::string& k, JValue v, Args... rest) {
    JValue o = JObj(rest...);
    std::get<JValue::Object>(o.v)[k] = std::move(v);
    return o;
}
inline JValue JArr() { return JValue(JValue::Array{}); }
template<typename... Args>
inline JValue JArr(JValue v, Args... rest) {
    JValue a = JArr(rest...);
    auto& arr = std::get<JValue::Array>(a.v);
    arr.insert(arr.begin(), std::move(v));
    return a;
}

// ================= вывод =================
inline void nlPrintOne(const JValue& j) { std::cout << j.dump(); }
inline void nlPrintOne(const std::string& s) { std::cout << s; }
inline void nlPrintOne(const char* s) { std::cout << s; }
inline void nlPrintOne(bool b) { std::cout << (b ? "true" : "false"); }
template<typename T>
inline void nlPrintOne(const T& v) { std::cout << v; }
template<typename T>
inline void nlPrintOne(const std::vector<T>& v) {
    std::cout << "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) std::cout << ", ";
        nlPrintOne(v[i]);
    }
    std::cout << "]";
}
inline void nlOut() { std::cout << "\n"; std::cout.flush(); }
template<typename T, typename... Rest>
inline void nlOut(const T& v, Rest... rest) {
    nlPrintOne(v);
    nlOut(rest...);
}

// ================= nlStr =================
inline std::string nlStr(const std::string& s) { return s; }
inline std::string nlStr(const char* s) { return s; }
inline std::string nlStr(bool b) { return b ? "true" : "false"; }
inline std::string nlStr(const JValue& j) { return j.dump(); }
inline std::string nlStr(double d) {
    char b[64];
    snprintf(b, 64, "%g", d);
    return b;
}
inline std::string nlStr(float d) { return nlStr((double)d); }
template<typename T>
inline std::string nlStr(const T& v) { return std::to_string(v); }
template<typename T>
inline std::string nlStr(const std::vector<T>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += nlStr(v[i]);
    }
    return s + "]";
}

// ================= helpers =================
// 0-индексация как в C++ + проверка границ (для watch/reveal)
template<typename T>
inline T& nlAt(std::vector<T>& v, long long i) {
    if (i < 0 || (size_t)i >= v.size())
        throw std::out_of_range("индекс вне диапазона: " +
                                std::to_string(i));
    return v[i];
}
template<typename T>
inline const T& nlAt(const std::vector<T>& v, long long i) {
    if (i < 0 || (size_t)i >= v.size())
        throw std::out_of_range("индекс вне диапазона: " +
                                std::to_string(i));
    return v[i];
}
inline JValue& nlAt(JValue& j, long long i) { return j[i]; }
inline JValue& nlAt(JValue& j, const std::string& k) { return j[k]; }
inline char nlAt(const std::string& s, long long i) {
    if (i < 0 || (size_t)i >= s.size())
        throw std::out_of_range("индекс вне диапазона: " +
                                std::to_string(i));
    return s[i];
}

inline double nlNow() {
    using namespace std::chrono;
    return (double)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}
template<typename T>
inline T nlPop(std::vector<T>& v) {
    T r = v.back();
    v.pop_back();
    return r;
}
inline std::string nlStrip(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    size_t b = s.find_last_not_of(" \t\n\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
inline std::string nlReplace(std::string s, const std::string& from,
                             const std::string& to) {
    if (from.empty()) return s;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
    return s;
}
inline std::vector<std::string> nlSplit(const std::string& s,
                                        const std::string& d) {
    std::vector<std::string> r;
    size_t p = 0, f;
    while ((f = s.find(d, p)) != std::string::npos) {
        r.push_back(s.substr(p, f - p));
        p = f + d.size();
    }
    r.push_back(s.substr(p));
    return r;
}
template<typename F>
inline void nlTimes(long long n, F&& f) {
    for (long long i = 0; i < n; ++i) f(i);
}
inline std::vector<double> nlCsvNumbers(const std::string& path,
                                        const std::string& delim,
                                        long long col) {
    std::vector<double> r;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string cell;
        long long i = 0;
        while (std::getline(ss, cell, delim.empty() ? ',' : delim[0])) {
            if (i++ == col - 1) {
                try { r.push_back(std::stod(cell)); }
                catch (...) { r.push_back(0.0); }
                break;
            }
        }
    }
    return r;
}
inline std::string nlReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
inline void nlWriteFile(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    f << data;
}
