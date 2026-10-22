#pragma once
// Порт statements/assignment.py —
// AssignmentStatement + DeclarationStatement (+ AntiDeclaration)
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"
#include "../ninclude.h"
#include "../expressions/getter.h"
#include "../expressions/variable.h"
#include "../expressions/_dict.h"
#include "../expressions/list.h"
#include "../includedlib/json.h"

class AssignmentStatement : public Node {
public:
    NodePtr getter; // GetterExpression
    NodePtr value;
    AssignmentStatement(NodePtr g, NodePtr v)
        : getter(std::move(g)), value(std::move(v)) {}

    Emitted javascript(Nvm& vm) override {
        return Emitted(getter->javascript(vm).getCpp() + " = " +
                       value->javascript(vm).getCpp() + ";");
    }

    std::string _plain_var_name() {
        auto g = std::dynamic_pointer_cast<GetterExpression>(getter);
        if (!g || g->list.size() != 1) return "";
        auto v = std::dynamic_pointer_cast<VariableExpression>(g->list[0]);
        return v ? v->text : "";
    }

    // `r.json = {...}` / `r.body = [...]` — авто-сериализация:
    // dict/list литерал в поле ответа сервера -> JValue::dump()
    bool _member_assign_of_dict() {
        auto g = std::dynamic_pointer_cast<GetterExpression>(getter);
        if (!g || g->list.size() < 2) return false;
        // только поля ответа сервера — не любой dict-член
        auto last = std::dynamic_pointer_cast<VariableExpression>(
            g->list.back());
        if (!last) return false;
        const std::string& m = last->text;
        if (m != "json" && m != "body" && m != "html") return false;
        NodePtr v = nlang_unwrap_getter(value);
        return std::dynamic_pointer_cast<DictExpression>(v) ||
               std::dynamic_pointer_cast<ListExpression>(v);
    }

    Emitted cpp(Nvm& vm) override {
        std::string left = getter->cpp(vm).getCpp();
        std::string name = _plain_var_name();
        if (!name.empty() && !vm.var_types.count(name)) {
            // неявная декларация: `x = expr` на новом имени
            std::string inferred = vm.inferType(value);
            std::string resolved = inferred.empty() ? "auto" : inferred;
            vm.setVariableType(name, resolved);
            return Emitted(resolved + " " + left + " = " +
                           value->cpp(vm).getCpp());
        }
        std::string rhs = value->cpp(vm).getCpp();
        // `obj.field = {...}` — сериализуем JValue в std::string
        if (_member_assign_of_dict())
            rhs = "(" + rhs + ").dump()";
        return Emitted(left + " = " + rhs);
    }
    NObject eval(Nvm& vm) override {
        auto g = std::dynamic_pointer_cast<GetterExpression>(getter);
        NObject v = value->eval(vm);
        // `r.json = {...}` — авто-экспорт в строку
        if (_member_assign_of_dict() && (v.isDict() || v.isList()))
            v = NObject(nljson_export(
                includedlib::to_jvalue(v)));
        g->assign(vm, v);
        return v;
    }
};

class DeclarationStatement : public Node {
public:
    std::string type, name;
    NodePtr value; // может быть nullptr
    std::vector<std::string> template_declaration_list;
    std::string dimension;       // "m/s" и т.п. — текстом
    NodePtr array_size;          // C-массив type name[N]

    Emitted javascript(Nvm& vm) override {
        // типы отбрасываются: int/string/auto -> let (как py)
        return Emitted("let " + name + " = " +
                       (value ? value->javascript(vm).getCpp()
                              : "undefined") + ";");
    }

    Emitted cpp(Nvm& vm) override {
        // C-массив
        if (array_size) {
            std::string sz = array_size->cpp(vm).getCpp();
            vm.setVariableType(name, type + "[]");
            if (!value)
                return Emitted("alignas(64) " + type + " " + name +
                               "[" + sz + "] = {}");
            return Emitted("alignas(64) " + type + " " + name +
                           "[" + sz + "] = " + value->cpp(vm).getCpp());
        }
        if (!dimension.empty()) vm.setDimension(name, dimension);

        std::string resolved = type;
        std::string inferred = value ? vm.inferType(value) : "";
        if (type == "auto" || type == "anti") {
            resolved = !inferred.empty() ? inferred : "auto";
        }
        vm.setVariableType(name, resolved);

        std::string code;
        if (resolved == "list" || resolved == "vector") {
            code += "vector";
            if (!template_declaration_list.empty()) {
                code += "<";
                for (size_t i = 0; i < template_declaration_list.size(); ++i) {
                    if (i) code += ",";
                    std::string a = template_declaration_list[i];
                    // алиасы внутри шаблонов
                    if (a == "json") a = "JValue";
                    auto p = a.find("list<");
                    if (p != std::string::npos) a.replace(p, 4, "vector");
                    code += a;
                }
                code += ">";
            }
        } else if (resolved == "json") {
            code += "JValue";
        } else {
            code += resolved;
        }
        code += " " + name;
        if (value) code += " = " + value->cpp(vm).getCpp();
        return Emitted(code);
    }
    NObject eval(Nvm& vm) override {
        NObject v;
        if (value) {
            v = value->eval(vm);
        } else {
            // Type x — если Type это класс, вызываем конструктор
            auto it = vm.vars.find(type);
            if (it != vm.vars.end() && it->second.isFunc()) {
                NArgs empty;
                v = it->second.call(vm, empty);
            }
        }
        vm.set(name, v);
        vm.setVariableType(name, type);
        return v;
    }
};
