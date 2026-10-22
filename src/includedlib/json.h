#pragma once
// Дескриптор модуля json + eval-регистратор.
// JValue (скомпилированный мир) и dict-NObject (интерпретатор) —
// две разные вещи, поэтому eval-версия пока работает с текстом
// через вложенные NObject-структуры упрощённо:
// json.export(dict) сериализует dict/list напрямую.
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_json.h"
#include "str.h" // sarg

namespace includedlib {

// NObject -> JValue (для json.export в интерпретаторе)
inline JValue to_jvalue(const NObject& v) {
    if (v.isNull()) return JValue();
    if (v.isBool()) return JValue(v.asBool());
    if (v.isInt()) return JValue(v.asInt());
    if (v.isDouble()) return JValue(v.asDouble());
    if (v.isStr()) return JValue(v.asStr());
    if (v.isList()) {
        JValue::Array a;
        for (auto& e : *v.asList()) a.push_back(to_jvalue(e));
        return JValue(std::move(a));
    }
    if (v.isDict()) {
        JValue::Object o;
        for (auto& kv : *v.asDict()) o[kv.first] = to_jvalue(kv.second);
        return JValue(std::move(o));
    }
    return JValue(nstr(v).asStr());
}

// JValue -> NObject (для json.import в интерпретаторе)
inline NObject from_jvalue(const JValue& v) {
    if (v.isObject()) {
        auto d = std::make_shared<std::map<std::string, NObject>>();
        for (auto& kv : std::get<JValue::Object>(v.v))
            (*d)[kv.first] = from_jvalue(kv.second);
        return NObject(d);
    }
    if (v.isArray()) {
        auto l = std::make_shared<std::vector<NObject>>();
        for (auto& e : std::get<JValue::Array>(v.v))
            l->push_back(from_jvalue(e));
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

inline void register_json(Nvm& vm, const std::string& alias) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    auto add = [&](const char* n, std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        (*d)[n] = NObject(fn);
    };
    add("import", [](Nvm&, const NArgs& a) {
        try {
            return from_jvalue(nljson_import(sarg(a, 0)));
        } catch (std::exception& e) {
            throw NLangError{e.what()};
        } });
    add("export", [](Nvm&, const NArgs& a) {
        return NObject(nljson_export(
            a.pos.empty() ? JValue() : to_jvalue(a.pos[0]))); });
    add("exportPretty", [](Nvm&, const NArgs& a) {
        long long ind = a.pos.size() > 1 ? a.pos[1].asInt() : 2;
        return NObject(nljson_exportPretty(
            a.pos.empty() ? JValue() : to_jvalue(a.pos[0]), ind)); });
    vm.set(alias, NObject(d));
}

inline const ModuleDescriptor json_desc = {
    "json", "nlm_json.h", {"nlmodules"}, &register_json, "json", nullptr
};

} // namespace includedlib
