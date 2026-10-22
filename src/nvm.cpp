#include "nvm.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>

// ==================== встроенные функции ====================

static NObject bi_print(Nvm&, const NArgs& a) {
    for (auto& v : a.pos) std::cout << nstr(v).asStr();
    std::cout << "\n";
    std::cout.flush();
    return NObject();
}
static NObject bi_str(Nvm&, const NArgs& a) {
    return a.pos.empty() ? NObject(std::string()) : nstr(a.pos[0]);
}
static NObject bi_now(Nvm&, const NArgs&) {
    using namespace std::chrono;
    return NObject((double)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count());
}
static NObject bi_size(Nvm&, const NArgs& a) {
    if (a.pos.empty()) return NObject(0);
    const NObject& v = a.pos[0];
    if (v.isList()) return NObject((long long)v.asList()->size());
    if (v.isStr()) return NObject((long long)v.asStr().size());
    if (v.isDict()) return NObject((long long)v.asDict()->size());
    return NObject(0);
}

static FuncPtr mknative(const std::string& name, NativeFn fn) {
    auto f = std::make_shared<NFunction>();
    f->name = name;
    f->impl.store(fn, std::memory_order_release);
    return f;
}

Nvm::Nvm() {
    vars["print"] = NObject(mknative("print", &bi_print));
    vars["transmit"] = NObject(mknative("transmit", &bi_print));
    vars["str"] = NObject(mknative("str", &bi_str));
    vars["now"] = NObject(mknative("now", &bi_now));
    vars["size"] = NObject(mknative("size", &bi_size));
    vars["true"] = NObject(true);
    vars["false"] = NObject(false);
    vars["null"] = NObject();
}

long long Nvm::exec_now_ms() {
    using namespace std::chrono;
    return (long long)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

NObject* Nvm::get(const std::string& nm) {
    auto it = vars.find(nm);
    return it != vars.end() ? &it->second : nullptr;
}
void Nvm::set(const std::string& nm, NObject v) {
    lock_record(nm);
    vars[nm] = std::move(v);
}

void Nvm::lock() {
    if (_lock_depth == _lock_pool.size())
        _lock_pool.push_back(std::make_unique<LockFrame>());
    else
        _lock_pool[_lock_depth]->old.clear();
    ++_lock_depth;
    ++lock_gen;
}
void Nvm::unlock() {
    // как в py: восстанавливаем старые ключи; новые остаются.
    // но только те, что реально писались под этим кадром.
    auto& old = _lock_pool[--_lock_depth]->old;
    for (auto& kv : old) vars[kv.first] = kv.second;
}
void Nvm::lock_record(const std::string& nm) {
    if (!_lock_depth) return;
    auto& old = _lock_pool[_lock_depth - 1]->old;
    if (old.find(nm) != old.end()) return; // уже записано
    auto it = vars.find(nm);
    if (it != vars.end()) old.emplace(nm, it->second);
}

void Nvm::pushScope() { _scope_stack.push_back(var_types); }
void Nvm::popScope() {
    if (!_scope_stack.empty()) {
        var_types = _scope_stack.back();
        _scope_stack.pop_back();
    }
}

std::string Nvm::getVariableType(const std::string& n) const {
    auto it = var_types.find(n);
    return it != var_types.end() ? it->second : "";
}
std::string Nvm::getDimension(const std::string& n) const {
    auto it = dimensions.find(n);
    return it != dimensions.end() ? it->second : "";
}

NObject* Nvm::antiGet(const std::string& nm) {
    auto it = antiassigned.find(nm);
    return it != antiassigned.end() ? &it->second : nullptr;
}
void Nvm::antiAssign(const std::string& t, const std::string& n, NObject v) {
    antiassigned[n] = std::move(v);
    antiassigned_types[n] = t;
}
bool Nvm::checkIsFunctionCpp(const std::string& n) const {
    for (auto& f : cppfunctions) if (f == n) return true;
    return false;
}

void Nvm::autoModule(const std::string& name) {
    if (_auto_modules.count(name)) return;
    _auto_modules.insert(name);
    // загрузка модуля — когда появятся модули
}

// имя -> C++ символ (как getCppVariableAddress в nlcnvm)
std::string Nvm::getCppVariableAddress(const std::string& n) const {
    static const std::unordered_map<std::string, std::string> aliases = {
        {"print", "nlOut"}, {"str", "nlStr"}, {"fileStr", "nlStr"},
        {"itostr", "nlStr"}, {"now", "nlNow"},
        {"csvNumbers", "nlCsvNumbers"},
        {"readFile", "nlReadFile"}, {"writeFile", "nlWriteFile"},
    };
    auto it = aliases.find(n);
    return it != aliases.end() ? it->second : n;
}

// math.floor(x) -> nlmath_floor(x): модульная функция.
// mod — имя в коде (может быть алиасом: net -> internet).
std::string Nvm::module_func(const std::string& mod,
                             const std::string& fn) const {
    std::string real = mod;
    auto a = alias_to_module.find(mod);
    if (a != alias_to_module.end()) real = a->second;
    else if (!used_modules.count(mod)) return "";
    return "nl" + real + "_" + fn;
}

bool Nvm::isModule(const std::string& name) const {
    return used_modules.count(name) ||
           alias_to_module.count(name);
}

// using <x>: регистрация дескриптора модуля
#include "module.h"
void Nvm::useModule(const std::string& name) {
    if (used_modules.count(name)) return;
    const ModuleDescriptor* d = find_module(name);
    if (!d)
        throw NLangError{"неизвестный модуль '" + name + "'"};
    used_modules.insert(name);
    alias_to_module[d->alias] = name;
    if (d->header[0]) requireInclude(d->header);
    for (auto& l : d->libs) link_libs.insert(l);
    if (d->cpp_declarations) module_declarations += d->cpp_declarations;
    if (d->register_eval) d->register_eval(*this, d->alias);
}

// UFCS: obj.method(args) -> шаблон C++-хелпера (порт _UFCS)
std::string Nvm::ufcs(const std::string& obj_type, const std::string& method) const {
    static const std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> UFCS = {
        {"sort",   {{"vector", "std::sort({o}.begin(), {o}.end(){comma_args})"},
                    {"string", "std::sort({o}.begin(), {o}.end())"}}},
        {"reverse",{{"vector", "std::reverse({o}.begin(), {o}.end())"},
                    {"string", "std::reverse({o}.begin(), {o}.end())"}}},
        {"min",    {{"vector", "*std::min_element({o}.begin(), {o}.end())"}}},
        {"max",    {{"vector", "*std::max_element({o}.begin(), {o}.end())"}}},
        {"sum",    {{"vector", "std::accumulate({o}.begin(), {o}.end(), {elem}())"}}},
        {"size",   {{"vector", "(long long){o}.size()"},
                    {"string", "(long long){o}.size()"},
                    {"JValue", "(long long){o}.size()"},
                    {"json",   "(long long){o}.size()"}}},
        {"append", {{"vector", "({o}.push_back({args}), {o})"}}},
        {"push",   {{"vector", "({o}.push_back({args}), {o})"}}},
        {"pop",    {{"vector", "nlPop({o})"}}},
        {"strip",  {{"string", "nlStrip({o})"}}},
        {"replace",{{"string", "nlReplace({o}, {args})"}}},
        {"split",  {{"string", "nlSplit({o}, {args})"}}},
        {"get",    {{"json", "({o}[{args}])"},
                    {"JValue", "({o}[{args}])"}}},
        {"times",  {{"int", "nlTimes({o}, {args})"},
                    {"long", "nlTimes({o}, {args})"}}},
        {"str",    {{"int", "nlStr({o})"}, {"double", "nlStr({o})"},
                    {"long", "nlStr({o})"}, {"bool", "nlStr({o})"}}},
    };
    auto m = UFCS.find(method);
    if (m == UFCS.end()) return "";
    // точное попадание
    auto t = m->second.find(obj_type);
    if (t != m->second.end()) return t->second;
    // базовый тип без шаблонов: vector<int> -> vector
    std::string base = obj_type.substr(0, obj_type.find('<'));
    base.erase(base.find_last_not_of(" *&") + 1);
    // алиасы имён: nlang-типы -> ключи таблицы (C++-типы)
    static const std::unordered_map<std::string, std::string> ALIAS = {
        {"list", "vector"}, {"std::vector", "vector"},
        {"str", "string"}, {"std::string", "string"},
        {"dict", "JValue"}, {"map", "JValue"}, {"json", "JValue"},
        {"set", "JArr"},
    };
    auto al = ALIAS.find(base);
    if (al != ALIAS.end()) base = al->second;
    t = m->second.find(base);
    if (t != m->second.end()) return t->second;
    // уникальный метод — fallback только если тип неизвестен
    if (obj_type.empty() && m->second.size() == 1)
        return m->second.begin()->second;
    return "";
}
// категория типа для UFCS-таблицы (порт _type_category)
static std::string type_category(const std::string& t) {
    if (t.empty()) return "";
    if (t.find("vector<") != std::string::npos ||
        t.find("list<") != std::string::npos || t == "list")
        return "vector";
    if (t == "json" || t == "JValue" || t == "dict") return "json";
    if (t.find("map<") != std::string::npos || t == "map")
        return "map";
    if (t == "string" || t == "std::string") return "string";
    return "";
}
std::string Nvm::ufcs_rtype(const std::string& obj_type,
                            const std::string& method) const {
    // порт _UFCS_RTYPE из nlcnvm.py (Json::Value -> JValue)
    static const std::unordered_map<std::string, std::string>
        RTYPE = {
        {"json:keys",    "std::vector<std::string>"},
        {"json:values",  "JValue"},
        {"json:items",   "JValue"},
        {"json:has",     "bool"},
        {"json:export",  "std::string"},
        {"json:sortkeys","std::vector<std::string>"},
        {"vector:min",   "elem"},
        {"vector:max",   "elem"},
        {"vector:sum",   "elem"},
        {"string:min",   "char"},
        {"string:max",   "char"},
        {"string:sort",  "void"},
        {"vector:sort",  "void"},
        {"vector:reverse","void"},
        {"vector:unique","void"},
    };
    std::string cat = type_category(obj_type);
    if (!cat.empty()) {
        auto it = RTYPE.find(cat + ":" + method);
        if (it != RTYPE.end())
            return it->second == "elem" ? elem_type(obj_type)
                                        : it->second;
    }
    if (method == "size" && !obj_type.empty()) return "size_t";
    return "";
}
std::string Nvm::elem_type(const std::string& t) const {
    // vector<X> / list<X> -> X
    auto l = t.find('<');
    auto r = t.rfind('>');
    if (l != std::string::npos && r != std::string::npos && r > l + 1)
        return t.substr(l + 1, r - l - 1);
    return "double";
}
