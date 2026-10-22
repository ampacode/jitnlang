// Живой hot-swap: eval-функция -> dll -> stdimpl-thunk.
//
// Ограничение модели: dll не может линковаться к exe, поэтому
// через границу идут только POD (hotabi.h). Это значит, что
// hot-swap работает для функций с "простыми" типами параметров
// и результата (int/double/string/bool) и для самодостаточных
// тел — если тело зовёт другую интерпретируемую функцию,
// dll её не найдёт (линкер упадёт) и мы откатываемся.
#include "hotswap.h"
#include "statements/model.h" // nlang_orm_bind_instance
#include "nvm.h"
#include "module.h"          // find_module: хедеры модулей для dll
#include "statements/function.h"
#include "../runtime/hotabi.h"
#include "../runtime/nljson.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <thread>
#include <cstdlib>

// hot-swap через dlopen возможен и на POSIX, но пока живой
// мост реализован только под Windows (LoadLibrary + dll).
// На остальных платформах — заглушки: hot_state так и
// останется 0, функция идёт интерпретируемым путём.
#ifdef _WIN32
#include <windows.h>

namespace fs = std::filesystem;

// держим загруженные dll до конца процесса
static std::vector<HMODULE> g_hot_modules;

// -------- обратный мост dll -> nlang --------
// dll зовёт host-функцию из exe, та ищет nlang-функцию в vm.vars
// и вызывает её интерпретируемую версию
typedef int (*NLHostCall)(const char* name, const NLArg* args,
                          int nargs, NLRet* ret);
static std::atomic<Nvm*> g_host_vm{nullptr};

static int nl_host_bridge(const char* name, const NLArg* args,
                          int nargs, NLRet* ret) {
    Nvm* vm = g_host_vm.load(std::memory_order_acquire);
    if (!vm) return -1;
    auto it = vm->vars.find(name);
    if (it == vm->vars.end() || !it->second.isFunc()) return -1;
    NArgs a;
    for (int i = 0; i < nargs; ++i) {
        switch (args[i].type) {
            case NL_INT:    a.pos.push_back(NObject(args[i].i)); break;
            case NL_DOUBLE: a.pos.push_back(NObject(args[i].d)); break;
            case NL_BOOL:   a.pos.push_back(NObject(args[i].i != 0)); break;
            case NL_STRING: a.pos.push_back(NObject(std::string(
                                args[i].str ? args[i].str : ""))); break;
            default: break;
        }
    }
    NObject r = it->second.call(*vm, a);
    if (r.isStr()) {
        ret->type = NL_STRING;
        std::string s = r.asStr();
        strncpy(ret->str, s.c_str(), sizeof(ret->str) - 1);
        ret->str[sizeof(ret->str) - 1] = 0;
    } else if (r.isDouble()) {
        ret->type = NL_DOUBLE; ret->d = r.asDouble();
    } else if (r.isBool()) {
        ret->type = NL_BOOL; ret->i = r.asBool() ? 1 : 0;
    } else if (r.isInt()) {
        ret->type = NL_INT; ret->i = r.asInt();
    } else {
        ret->type = NL_VOID;
    }
    return 0;
}

// поддерживаемые типы -> NLType + чтение/запись
struct HSParam { std::string type; std::string name; };

static bool hs_is_str(const std::string& t);
// коллекции через ABI — как JSON-строки (NL_STRING)
static bool hs_is_coll(const std::string& t) {
    return t == "JValue" ||
           t.rfind("vector<", 0) == 0 ||
           t.rfind("std::vector<", 0) == 0 ||
           t.rfind("map<", 0) == 0 ||
           t.rfind("std::map<", 0) == 0 ||
           t.rfind("dict<", 0) == 0 ||
           t.rfind("list<", 0) == 0;
}
// элемент коллекции: vector<X>/list<X> -> X; map<K,V> -> V
static std::string hs_elem(const std::string& t) {
    auto l = t.find('<'), r = t.rfind('>');
    if (l == std::string::npos || r == std::string::npos)
        return "double";
    std::string inside = t.substr(l + 1, r - l - 1);
    if (t.rfind("map<", 0) == 0 || t.rfind("std::map<", 0) == 0 ||
        t.rfind("dict<", 0) == 0) {
        // map<K,V> — второй аргумент (счётчик '<' для вложенных)
        size_t c = inside.find(',');
        return c == std::string::npos ? "JValue"
            : inside.substr(c + 1);
    }
    return inside;
}
static bool hs_is_map(const std::string& t) {
    return t.rfind("map<", 0) == 0 ||
           t.rfind("std::map<", 0) == 0 ||
           t.rfind("dict<", 0) == 0;
}
// nlang-тип -> C++-тип для генерируемого кода (list<X> -> vector<X>)
static std::string hs_ctype(const std::string& t) {
    if (t.rfind("list<", 0) == 0)
        return "std::vector<" + hs_elem(t) + ">";
    if (hs_is_map(t))
        return "std::map<std::string," + hs_elem(t) + ">";
    return t;
}
// kind: 1=int 2=double 3=string 4=bool 5=коллекция(JSON)
static int hs_kind(const std::string& t) {
    if (hs_is_coll(t)) return 5;
    if (hs_is_str(t)) return 3;
    if (t == "double" || t == "float") return 2;
    if (t == "bool") return 4;
    return 1;
}
static bool hs_supported(const std::string& t); // ниже
// модель/класс — тоже коллекция (JSON-объект через ABI)
static bool hs_is_model(Nvm& vm, const std::string& t) {
    return vm.models.count(t) != 0;
}
static bool hs_supported_m(Nvm& vm, const std::string& t) {
    return hs_supported(t) || hs_is_model(vm, t);
}
static int hs_kind_m(Nvm& vm, const std::string& t) {
    return hs_is_model(vm, t) ? 5 : hs_kind(t);
}
// распаковка/упаковка модели через JSON
static std::string hs_model_unpack(const std::string& t,
                                   const std::string& s) {
    return "nl_to_" + t + "(nljs_parse(" + s +
           " ? " + s + " : \"{}\"))";
}
static std::string hs_model_pack(const std::string& /*t*/,
                                 const std::string& v) {
    return "nljs_export(nl_jv(" + v + "))";
}
// struct + nl_jv + nl_to_T для всех моделей — в начало dll
static std::string hs_model_defs(Nvm& vm) {
    std::string out;
    for (auto& kv : vm.models) {
        const std::string& n = kv.first;
        bool is_class = vm.class_data.count(n) != 0;
        out += "struct " + n + " {\n";
        if (!is_class) out += "    long long rowid = 0;\n";
        for (auto& f : kv.second)
            out += "    " + f.first + " " + f.second + "{};\n";
        out += "};\n";
        out += "static JValue nl_jv(const " + n + "& m){\n"
               "    JValue::Object o;\n";
        if (!is_class)
            out += "    o[\"rowid\"] = JValue(m.rowid);\n";
        for (auto& f : kv.second)
            out += "    o[\"" + f.second + "\"] = nl_jv(m." +
                   f.second + ");\n";
        out += "    return JValue(o);\n}\n";
        out += "static " + n + " nl_to_" + n +
               "(const JValue& j){ " + n + " m;\n";
        if (!is_class)
            out += "    m.rowid = nl_to<long long>"
                   "(nl_jget(j,\"rowid\"));\n";
        for (auto& f : kv.second)
            out += "    m." + f.second + " = nl_to<" + f.first +
                   ">(nl_jget(j,\"" + f.second + "\"));\n";
        out += "    return m;\n}\n";
    }
    return out;
}
// emit C++: распаковка коллекции из JSON-строкового выражения
static std::string hs_coll_unpack(const std::string& t,
                                  const std::string& s) {
    if (t == "JValue")
        return "nljs_parse(" + s + " ? " + s + " : \"null\")";
    if (hs_is_map(t))
        return "nl_to<std::map<std::string," + hs_elem(t) +
               ">>(nljs_parse(" + s + " ? " + s + " : \"{}\"))";
    return "nl_jvec<" + hs_elem(t) + ">(nljs_parse(" + s +
           " ? " + s + " : \"[]\"))";
}
// emit C++: упаковка возвращаемого значения в JSON
static std::string hs_coll_pack(const std::string& t,
                                const std::string& v) {
    if (t == "JValue") return "nljs_export(" + v + ")";
    return "nl_jdump(" + v + ")";
}
static bool hs_supported(const std::string& t) {
    return t == "int" || t == "long" || t == "long long" ||
           t == "size_t" || t == "short" || t == "double" ||
           t == "float" || t == "string" || t == "std::string" ||
           t == "bool" || t == "void" || hs_is_coll(t);
}
// NObject <-> JValue конвертеры для exe-стороны
static JValue hs_to_jv(const NObject& v) {
    if (v.isNull()) return JValue();
    if (v.isBool()) return JValue(v.asBool());
    if (v.isInt()) return JValue(v.asInt());
    if (v.isDouble()) return JValue(v.asDouble());
    if (v.isStr()) return JValue(v.asStr());
    if (v.isList()) {
        JValue::Array a;
        for (auto& e : *v.asList()) a.push_back(hs_to_jv(e));
        return JValue(std::move(a));
    }
    if (v.isDict()) {
        JValue::Object o;
        for (auto& kv : *v.asDict()) o[kv.first] = hs_to_jv(kv.second);
        return JValue(std::move(o));
    }
    return JValue(nstr(v).asStr());
}
static NObject hs_from_jv(const JValue& v) {
    if (v.isObject()) {
        auto d = std::make_shared<std::map<std::string, NObject>>();
        for (auto& kv : std::get<JValue::Object>(v.v))
            (*d)[kv.first] = hs_from_jv(kv.second);
        return NObject(d);
    }
    if (v.isArray()) {
        auto l = std::make_shared<std::vector<NObject>>();
        for (auto& e : std::get<JValue::Array>(v.v))
            l->push_back(hs_from_jv(e));
        return NObject(l);
    }
    if (std::holds_alternative<std::string>(v.v))
        return NObject(std::get<std::string>(v.v));
    if (std::holds_alternative<long long>(v.v))
        return NObject(std::get<long long>(v.v));
    if (std::holds_alternative<double>(v.v))
        return NObject(std::get<double>(v.v));
    if (std::holds_alternative<bool>(v.v))
        return NObject(std::get<bool>(v.v));
    return NObject();
}
static bool hs_is_int(const std::string& t) {
    return t == "int" || t == "long" || t == "long long" ||
           t == "size_t" || t == "short" || t == "bool";
}
static bool hs_is_str(const std::string& t) {
    return t == "string" || t == "std::string";
}

// -------- общий прелоад dll: nl_arg + JSON-конвертеры ----------
static std::string hs_preamble() {
    std::string tramps =
        "typedef int (*NLHostCall)(const char*, const NLArg*, int,"
        " NLRet*);\n"
        "static NLHostCall g_host = nullptr;\n"
        "extern \"C\" __declspec(dllexport)\n"
        "void nl_set_host(NLHostCall h) { g_host = h; }\n"
        "static NLArg nl_arg(long long v){NLArg a{};a.type=NL_INT;a.i=v;return a;}\n"
        "static NLArg nl_arg(int v){return nl_arg((long long)v);}\n"
        "static NLArg nl_arg(short v){return nl_arg((long long)v);}\n"
        "static NLArg nl_arg(size_t v){return nl_arg((long long)v);}\n"
        "static NLArg nl_arg(long v){return nl_arg((long long)v);}\n"
        "static NLArg nl_arg(double v){NLArg a{};a.type=NL_DOUBLE;a.d=v;return a;}\n"
        "static NLArg nl_arg(float v){return nl_arg((double)v);}\n"
        "static NLArg nl_arg(bool v){NLArg a{};a.type=NL_BOOL;a.i=v?1:0;return a;}\n"
        "static NLArg nl_arg(const char* s){NLArg a{};a.type=NL_STRING;a.str=s;return a;}\n"
        "static NLArg nl_arg(const std::string& s){return nl_arg(s.c_str());}\n"
        // коллекции <-> JSON: nl_to<T> через tag-dispatch —
        // nl_to_impl(j,(T*)nullptr), перегрузки для скаляров/vector/map
        "static long long nl_to_impl(const JValue& j, long long*){return j.asInt();}\n"
        "static int nl_to_impl(const JValue& j, int*){return (int)j.asInt();}\n"
        "static short nl_to_impl(const JValue& j, short*){return (short)j.asInt();}\n"
        "static long nl_to_impl(const JValue& j, long*){return (long)j.asInt();}\n"
        "static size_t nl_to_impl(const JValue& j, size_t*){return (size_t)j.asInt();}\n"
        "static double nl_to_impl(const JValue& j, double*){return j.asDouble();}\n"
        "static float nl_to_impl(const JValue& j, float*){return (float)j.asDouble();}\n"
        "static bool nl_to_impl(const JValue& j, bool*){return j.asInt()!=0;}\n"
        "static std::string nl_to_impl(const JValue& j, std::string*){return j.asString();}\n"
        "template<typename T> std::vector<T> nl_jvec(const JValue& j);\n"
        "template<typename T> std::vector<T> nl_to_impl(const JValue& j, std::vector<T>*){return nl_jvec<T>(j);}\n"
        "template<typename K,typename T> std::map<K,T> nl_to_impl(const JValue& j, std::map<K,T>*){\n"
        "    std::map<K,T> o;\n"
        "    if (j.isObject()) for (auto& kv : std::get<JValue::Object>(j.v)) o[kv.first] = nl_to_impl(kv.second,(T*)nullptr);\n"
        "    return o;\n}\n"
        "template<typename T> T nl_to(const JValue& j){return nl_to_impl(j,(T*)nullptr);}\n"
        "template<typename T> std::vector<T> nl_jvec(const JValue& j){\n"
        "    std::vector<T> o;\n"
        "    if (j.isArray()) for (auto& e : std::get<JValue::Array>(j.v)) o.push_back(nl_to<T>(e));\n"
        "    return o;\n}\n"
        "static JValue nl_jv(const JValue& j){return j;}\n"
        "template<typename T> JValue nl_jv(const T& v){return JValue(v);}\n"
        "template<typename T> JValue nl_jv(const std::vector<T>& v){\n"
        "    JValue::Array a; for (auto& e : v) a.push_back(nl_jv(e));\n"
        "    return JValue(a);\n}\n"
        "template<typename K,typename T> JValue nl_jv(const std::map<K,T>& m){\n"
        "    JValue::Object o; for (auto& kv : m) o[kv.first] = nl_jv(kv.second);\n"
        "    return JValue(o);\n}\n"
        "template<typename T> std::string nl_jdump(const std::vector<T>& v){\n"
        "    return nljs_export(nl_jv(v));\n}\n"
        "template<typename K,typename T> std::string nl_jdump(const std::map<K,T>& m){\n"
        "    return nljs_export(nl_jv(m));\n}\n"
        "static std::string nl_jdump(const JValue& j){return nljs_export(j);}\n"
        // чтение поля объекта: nl_jget(j,"x")
        "static const JValue& nl_jget(const JValue& j, const char* k){\n"
        "    static JValue empty_;\n"
        "    if (j.isObject()) {\n"
        "        auto& o = std::get<JValue::Object>(j.v);\n"
        "        auto it = o.find(k);\n"
        "        if (it != o.end()) return it->second;\n"
        "    }\n"
        "    return empty_;\n}\n";
    return tramps;
}

// -------- трамплины: вызовы из dll в интерпретируемые функции --
// для каждой nlang-функции с POD-сигнатурой генерируем обёртку:
// marshal args -> g_host(name, ...) -> unmarshal ret
static std::string hs_tramps(Nvm& vm, const std::string& self_name) {
    std::string tramps;
    for (auto& kv : vm.vars) {
        if (kv.first == self_name || !kv.second.isFunc()) continue;
        FuncPtr other = kv.second.asFunc();
        auto odecl = std::dynamic_pointer_cast<FunctionDeclaration>(
            other->decl_node);
        if (!odecl) continue; // нативы — не мостим
        std::string osig = odecl->signature(vm);
        size_t olp = osig.find('('), orp = osig.rfind(')');
        if (olp == std::string::npos) continue;
        std::string oret = osig.substr(0, osig.find(' '));
        if (!hs_supported_m(vm, oret) || oret == "auto" ||
            oret == "anti")
            continue;
        std::string inside = osig.substr(olp + 1, orp - olp - 1);
        std::vector<std::string> ptypes;
        bool bad = false;
        size_t p = 0;
        while (p < inside.size()) {
            size_t c = inside.find(',', p);
            std::string one = inside.substr(p, c ==
                std::string::npos ? std::string::npos : c - p);
            size_t b = one.find_first_not_of(" \t");
            if (b == std::string::npos) break;
            size_t e = one.find_last_not_of(" \t");
            one = one.substr(b, e - b + 1);
            size_t sp = one.rfind(' ');
            std::string t = sp == std::string::npos
                ? "auto" : one.substr(0, sp);
            if (!hs_supported_m(vm, t) || t == "auto" ||
                t == "anti") {
                bad = true; break;
            }
            ptypes.push_back(t);
            if (c == std::string::npos) break;
            p = c + 1;
        }
        if (bad) continue;
        std::string tdef = hs_ctype(oret) + " " + kv.first + "(";
        for (size_t i = 0; i < ptypes.size(); ++i) {
            if (i) tdef += ",";
            tdef += hs_ctype(ptypes[i]) + " p" + std::to_string(i);
        }
        size_t n = ptypes.size();
        tdef += ") {\n    NLArg _a[" +
            std::to_string(std::max<size_t>(1, n)) + "] = {};\n";
        for (size_t i = 0; i < n; ++i) {
            std::string I = std::to_string(i);
            if (hs_is_coll(ptypes[i]) ||
                hs_is_model(vm, ptypes[i])) {
                // коллекция/модель -> JSON-строка (до конца вызова)
                tdef += "    std::string _js" + I + " = " +
                        (hs_is_model(vm, ptypes[i])
                            ? hs_model_pack(ptypes[i], "p" + I)
                            : hs_coll_pack(ptypes[i], "p" + I)) + ";\n";
                tdef += "    _a[" + I + "].type=NL_STRING; _a[" + I +
                        "].str=_js" + I + ".c_str();\n";
            } else if (hs_is_str(ptypes[i]))
                tdef += "    _a[" + I +
                        "].type=NL_STRING; _a[" + I +
                        "].str=p" + I + ".c_str();\n";
            else
                tdef += "    _a[" + I +
                        "]=nl_arg(p" + I + ");\n";
        }
        tdef += "    NLRet _r{};\n"
                "    if (!g_host || g_host(\"" + kv.first + "\", _a, " +
                std::to_string(n) + ", &_r)) return " +
                (oret == "void" ? std::string() :
                 hs_ctype(oret) + "{}") + ";\n";
        if (oret == "void") {}
        else if (hs_is_model(vm, oret))
            tdef += "    return " + hs_model_unpack(oret, "_r.str") +
                    ";\n";
        else if (hs_is_coll(oret))
            tdef += "    return " + hs_coll_unpack(oret, "_r.str") +
                    ";\n";
        else if (hs_is_str(oret))
            tdef += "    return std::string(_r.str);\n";
        else if (oret == "double" || oret == "float")
            tdef += "    return (" + oret + ")_r.d;\n";
        else if (oret == "bool")
            tdef += "    return _r.i != 0;\n";
        else
            tdef += "    return (" + oret + ")_r.i;\n";
        tdef += "}\n";
        tramps += tdef;
    }
    return tramps + "\n";
}

void nlang_maybe_hotswap(Nvm& vm, FuncPtr fn) {
    fn->hot_state = 1;
    auto bail = [&] { fn->hot_state = -1; };

    auto decl = std::dynamic_pointer_cast<FunctionDeclaration>(fn->decl_node);
    if (!decl || fn->args.empty() && decl->args.empty()) { bail(); return; }
    if (vm.runtime_dir.empty() || vm.compiler_dir.empty()) { bail(); return; }

    // сигнатура с проставленными (выведенными) типами
    std::string sig = decl->signature(vm); // "double half(double a)"
    size_t lp = sig.find('('), rp = sig.rfind(')');
    if (lp == std::string::npos || rp == std::string::npos) { bail(); return; }

    std::string ret_type = sig.substr(0, sig.find(' '));
    if (!hs_supported_m(vm, ret_type) || ret_type == "auto" ||
        ret_type == "anti") { bail(); return; }

    // разбор параметров из сигнатуры
    std::vector<HSParam> params;
    {
        std::string inside = sig.substr(lp + 1, rp - lp - 1);
        size_t p = 0;
        while (p < inside.size()) {
            size_t c = inside.find(',', p);
            std::string one = inside.substr(
                p, c == std::string::npos ? std::string::npos : c - p);
            // trim
            size_t b = one.find_first_not_of(" \t");
            if (b != std::string::npos) {
                size_t e = one.find_last_not_of(" \t");
                one = one.substr(b, e - b + 1);
                size_t sp = one.rfind(' ');
                std::string t = sp == std::string::npos
                    ? "auto" : one.substr(0, sp);
                std::string n = sp == std::string::npos
                    ? one : one.substr(sp + 1);
                if (!hs_supported_m(vm, t) || t == "auto" ||
                    t == "anti") {
                    bail(); return;
                }
                params.push_back({t, n});
            }
            if (c == std::string::npos) break;
            p = c + 1;
        }
    }

    // тело — через cpp() декларации; signature уже вызван,
    // cpp() переиспользует выведенные типы
    Emitted body = decl->cpp(vm);
    if (body.getCpp().empty()) { bail(); return; }

    // тело, читающее глобальные переменные, в dll не
    // скомпилируется — их символов там нет. Ловим заранее
    // по границе слова, иначе каждая такая функция сыпала
    // бы в консоль дампом ошибок g++.
    {
        const std::string& bs = body.getCpp();
        for (auto& kv : vm.vars) {
            const std::string& vn = kv.first;
            if (vn.size() < 2) continue;
            char c0 = vn[0];
            if (!isalpha((unsigned char)c0) && c0 != '_') continue;
            if (kv.second.isFunc()) continue;   // функции мостятся трамплинами
            // слово-граница: «st» не должно матчиться в «first»
            size_t p = bs.find(vn);
            while (p != std::string::npos) {
                bool lo = p == 0 || !(isalnum((unsigned char)bs[p - 1])
                                      || bs[p - 1] == '_');
                size_t e = p + vn.size();
                bool hi = e >= bs.size() ||
                          !(isalnum((unsigned char)bs[e]) || bs[e] == '_');
                if (lo && hi) { bail(); return; }
                p = bs.find(vn, p + 1);
            }
        }
    }

    // -------- генерируем dll-исходник --------
    // modules/ рядом с runtime/ — там nlm_*.h и libnlmodules.a
    std::string modules_dir =
        (fs::path(vm.runtime_dir).parent_path() / "modules").string();
    // инклуды использованных модулей: nl<mod>_* функции
    // объявлены в modules/nlm_*.h и линкуются из libnlmodules.a —
    // без этого горячая функция, зовущая math./gl./file.*,
    // падала на компиляции («nlmath_floor was not declared»).
    // Для eval-only модулей (qt/gl) header пуст — берём конвенцию
    // nlm_<name>.h, если такой файл есть.
    std::string mod_includes;
    for (auto& m : vm.used_modules)
        if (const ModuleDescriptor* d = find_module(m)) {
            std::string h = d->header[0] ? d->header
                                         : "nlm_" + m + ".h";
            if (fs::exists(fs::path(modules_dir) / h))
                mod_includes += "#include \"" + h + "\"\n";
        }
    std::string src =
        "#include \"nlrt.h\"\n"
        "#include \"hotabi.h\"\n"
        "#include \"nljson.h\"\n" +
        mod_includes +
        "#include <cstring>\n"
        "using namespace std;\n\n" +
        vm.module_declarations +
        hs_preamble() +
        hs_model_defs(vm) +
        hs_tramps(vm, fn->name) +
        body.getCpp() + "\n\n"
        "extern \"C\" __declspec(dllexport)\n"
        "int nl_hot_call(const NLArg* args, int nargs, NLRet* ret) {\n"
        "    if (nargs < " + std::to_string(params.size()) +
        ") return -1;\n";
    // распаковка параметров
    for (size_t i = 0; i < params.size(); ++i) {
        const std::string& t = params[i].type;
        src += "    " + hs_ctype(t) + " p" + std::to_string(i) + " = ";
        if (hs_is_coll(t) || hs_is_model(vm, t)) {
            std::string arg = "args[" + std::to_string(i) + "].str";
            src += hs_is_model(vm, t)
                ? hs_model_unpack(t, arg)
                : hs_coll_unpack(t, arg);
        }
        else if (hs_is_str(t))
            src += "args[" + std::to_string(i) +
                   "].str ? string(args[" + std::to_string(i) +
                   "].str) : string()";
        else if (t == "double" || t == "float")
            src += "(" + t + ")args[" + std::to_string(i) + "].d";
        else
            src += "(" + t + ")args[" + std::to_string(i) + "].i";
        src += ";\n";
    }
    src += "    ";
    std::string callArgs;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) callArgs += ",";
        callArgs += "p" + std::to_string(i);
    }
    if (ret_type == "void") {
        src += fn->name + "(" + callArgs + ");\n";
        src += "    ret->type = NL_VOID;\n";
    } else {
        src += "auto r = " + fn->name + "(" + callArgs + ");\n";
        if (hs_is_model(vm, ret_type))
            src += "    ret->type = NL_STRING; "
                   "strncpy(ret->str, " +
                   hs_model_pack(ret_type, "r") +
                   ".c_str(), sizeof(ret->str)-1);"
                   " ret->str[sizeof(ret->str)-1] = 0;\n";
        else if (hs_is_coll(ret_type))
            src += "    ret->type = NL_STRING; "
                   "strncpy(ret->str, " +
                   hs_coll_pack(ret_type, "r") +
                   ".c_str(), sizeof(ret->str)-1);"
                   " ret->str[sizeof(ret->str)-1] = 0;\n";
        else if (ret_type == "bool")
            src += "    ret->type = NL_BOOL; ret->i = r ? 1 : 0;\n";
        else if (hs_is_str(ret_type))
            src += "    ret->type = NL_STRING; "
                   "strncpy(ret->str, r.c_str(), sizeof(ret->str)-1);"
                   " ret->str[sizeof(ret->str)-1] = 0;\n";
        else if (ret_type == "double" || ret_type == "float")
            src += "    ret->type = NL_DOUBLE; ret->d = (double)r;\n";
        else
            src += "    ret->type = NL_INT; ret->i = (long long)r;\n";
    }
    src += "    return 0;\n}\n";

    // -------- компиляция + загрузка В ФОНЕ --------
    // пока g++ работает, вызовы продолжают идти через
    // интерпретатор без паузы; готовая dll публикуется атомикой
    std::vector<int> kinds;
    for (auto& p : params)
        kinds.push_back(hs_kind_m(vm, p.type));
    kinds.push_back(hs_kind_m(vm, ret_type)); // последний — тип результата
    // если ret — модель: invoke привяжет save/remove к dict
    if (hs_is_model(vm, ret_type))
        fn->hot_ret_model = ret_type;
    else
        fn->hot_ret_model.clear();
    auto kinds_ptr =
        std::make_shared<const std::vector<int>>(std::move(kinds));
    std::string runtime_dir = vm.runtime_dir;
    // линковочные флаги: из дескрипторов модулей (nlmodules,
    // ws2_32, ...) + Qt — если заюзаны qt/gl
    std::vector<std::string> link_libs;
    for (auto& l : vm.link_libs) link_libs.push_back("-l" + l);
    if (vm.used_modules.count("qt") || vm.used_modules.count("gl")) {
        link_libs.push_back("-lQt6OpenGLWidgets");
        link_libs.push_back("-lQt6Widgets");
        link_libs.push_back("-lQt6Gui");
        link_libs.push_back("-lQt6Core");
    }
    std::string name = fn->name;
    // vm для обратного моста (dll -> eval-функции)
    g_host_vm.store(&vm, std::memory_order_release);

    std::thread([fn, src = std::move(src), runtime_dir, modules_dir,
                 link_libs, kinds_ptr, name] {
        fs::path tmpdir = fs::temp_directory_path();
        fs::path cppPath = tmpdir / ("nlhot_" + name + ".cpp");
        fs::path dllPath = tmpdir / ("nlhot_" + name + ".dll");
        {
            std::ofstream o(cppPath);
            o << src;
        }
        // тот же выбор компилятора, что и в main.cpp
        const char* cxx = std::getenv("NLANG_CXX");
        std::string cxxs = (cxx && *cxx) ? cxx
            : fs::exists("C:/msys64/ucrt64/bin/g++.exe")
                ? "C:/msys64/ucrt64/bin/g++.exe" : "g++";
        // ucrt64/bin нужен в PATH для DLLs самого компилятора
        std::string pfx;
        if (cxxs != "g++") {
            auto b = cxxs.substr(0, cxxs.find_last_of("/\\"));
            pfx = "set \"PATH=" + b + ";%PATH%\" && ";
        }
        std::string cmd = pfx + "\"" + cxxs +
            "\" -O2 -shared -std=c++20 -I\"" +
            runtime_dir + "\" -I\"" + modules_dir +
            "\" \"" + cppPath.string() +
            "\" -o \"" + dllPath.string() + "\"";
        // модули: libnlmodules.a + системные флаги —
        // иначе вызовы nl<mod>_* не резолвятся при линковке
        if (!link_libs.empty()) {
            cmd += " -L\"" + modules_dir + "\"";
            for (auto& l : link_libs) cmd += " " + l;
        }
        if (std::system(cmd.c_str()) != 0 || !fs::exists(dllPath)) {
            fn->hot_state.store(-1, std::memory_order_release);
            return;
        }
        HMODULE h = LoadLibraryW(dllPath.wstring().c_str());
        if (!h) {
            fn->hot_state.store(-1, std::memory_order_release);
            return;
        }
        auto callee = (NLHotCall)GetProcAddress(h, "nl_hot_call");
        if (!callee) {
            FreeLibrary(h);
            fn->hot_state.store(-1, std::memory_order_release);
            return;
        }
        // регистрируем обратный мост: dll сможет звать eval-функции
        typedef void (*NLSetHost)(NLHostCall);
        if (auto setter = (NLSetHost)GetProcAddress(h, "nl_set_host"))
            setter(&nl_host_bridge);
        g_hot_modules.push_back(h);
        // публикуем: сначала данные, потом флаг (release/acquire)
        fn->hot_kinds = kinds_ptr;
        fn->hot_abi.store((void*)callee, std::memory_order_release);
        fn->hot_state.store(2, std::memory_order_release);
    }).detach();
}

// -------- thunk: NObject -> NLArg -> dll -> NObject --------
NObject nlang_hot_invoke(NLHotCall callee,
                         const std::vector<int>& kinds,
                         const NArgs& a,
                         const std::string& ret_model) {
    if (kinds.empty()) return NObject();
    int ret_kind = kinds.back(); // последний элемент — тип результата
    size_t nparams = kinds.size() - 1;
    NLArg arr[16] = {};
    std::vector<std::string> store;
    store.reserve(nparams);
    int n = 0;
    for (size_t i = 0; i < nparams && i < a.pos.size() &&
         n < 16; ++i, ++n) {
        switch (kinds[i]) {
            case 5: // коллекция/модель -> JSON-строка
                store.push_back(nljs_export(hs_to_jv(a.pos[i])));
                arr[n].type = NL_STRING;
                arr[n].str = store.back().c_str();
                break;
            case 3:
                store.push_back(a.pos[i].isStr() ? a.pos[i].asStr()
                                               : nstr(a.pos[i]).asStr());
                arr[n].type = NL_STRING;
                arr[n].str = store.back().c_str();
                break;
            case 2:
                arr[n].type = NL_DOUBLE;
                arr[n].d = a.pos[i].asDouble();
                break;
            case 4:
                arr[n].type = NL_BOOL;
                arr[n].i = a.pos[i].asBool() ? 1 : 0;
                break;
            default:
                arr[n].type = NL_INT;
                arr[n].i = a.pos[i].asInt();
        }
    }
    NLRet ret{};
    if (callee(arr, n, &ret) != 0) return NObject();
    if (ret_kind == 5 && ret.type == NL_STRING) {
        NObject res = hs_from_jv(nljs_parse(ret.str));
        // модель: привязать save/remove к полученному dict
        if (!ret_model.empty() && res.isDict()) {
            Nvm* vm = g_host_vm.load(std::memory_order_acquire);
            if (vm && vm->class_data.count(ret_model)) {
                // класс: свежий экземпляр фабрики (методы) +
                // перезапись полей данными из dll
                auto it = vm->vars.find(ret_model);
                if (it != vm->vars.end() && it->second.isFunc()) {
                    try {
                        NObject inst = it->second.call(*vm, NArgs{});
                        if (inst.isDict())
                            for (auto& kv : *res.asDict())
                                (*inst.asDict())[kv.first] =
                                    kv.second;
                        res = inst;
                    } catch (...) {
                        // ctor требует аргументы — вернуть dict как есть
                    }
                }
            } else {
                nlang_orm_bind_instance(ret_model, res);
            }
        }
        return res;
    }
    switch (ret.type) {
        case NL_INT:    return NObject(ret.i);
        case NL_DOUBLE: return NObject(ret.d);
        case NL_BOOL:   return NObject(ret.i != 0);
        case NL_STRING: return NObject(std::string(ret.str));
        default:        return NObject();
    }
}

#else // !_WIN32 � hot-swap ��������, eval-���� ��� dll

void nlang_maybe_hotswap(Nvm&, std::shared_ptr<NFunction>) {}
NObject nlang_hot_invoke(NLHotCall, const std::vector<int>&,
                         const NArgs&, const std::string&) {
    return NObject();
}

#endif
