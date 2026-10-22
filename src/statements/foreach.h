#pragma once
// Порт statements/foreach.py — for v : xs / for k, v : xs
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"
#include "block.h"
#include "../expressions/getter.h"
#include "../expressions/variable.h"

class ForeachStatement : public Node {
public:
    std::string key, value; // key пуст — только value
    NodePtr expr, block;
    Emitted javascript(Nvm& vm) override {
        // for (x : c) -> for (const x of c)
        return Emitted("for (const " + value + " of " +
                       expr->javascript(vm).getCpp() + ") " +
                       block->javascript(vm).getCpp());
    }
    ForeachStatement(std::string k, std::string v, NodePtr e, NodePtr b)
        : key(std::move(k)), value(std::move(v)),
          expr(std::move(e)), block(std::move(b)) {}

    std::string _container_kind(Nvm& vm) {
        NodePtr head = expr;
        if (auto g = std::dynamic_pointer_cast<GetterExpression>(expr))
            if (!g->list.empty()) head = g->list[0];
        std::string name;
        if (auto v = std::dynamic_pointer_cast<VariableExpression>(head))
            name = v->text;
        std::string t = vm.getVariableType(name);
        if (t.empty()) t = vm.inferType(expr);
        if (t == "json" || t == "JValue" || t == "Json::Value") return "json";
        if (t == "map" || t == "dict" ||
            t.find("map<") != std::string::npos) return "map";
        if (t == "std::string" || t == "string") return "string";
        return "list";
    }

    Emitted cpp(Nvm& vm) override {
        vm.pushScope();
        try {
            std::string kind = _container_kind(vm);
            std::string e = expr->cpp(vm).getCpp();
            vm.setVariableType(value, kind == "json" ? "JValue" : "auto");
            if (!key.empty())
                vm.setVariableType(key,
                    kind == "json" ? "std::string" : "long long");
            std::string code;
            if (key.empty()) {
                code = "for (auto " + value + " : " + e + ")";
                code += " {\n" + block->cpp(vm).getCpp();
            } else if (kind == "map" || kind == "json") {
                code = "for (auto&& __kv : " + e + ")";
                code += " {\n" + cpp_pad(vm, 1) +
                        "auto " + key + " = __kv.first;\n" + cpp_pad(vm, 1) +
                        "auto " + value + " = __kv.second;\n" +
                        block->cpp(vm).getCpp();
            } else {
                // list: for i, v : xs -> i = 0..n-1 (как enumerate в py)
                code = "for (long long " + key + " = 0; auto " +
                       value + " : " + e + ")";
                code += " {\n" + block->cpp(vm).getCpp() +
                        cpp_pad(vm, 1) + "++" + key + ";\n";
            }
            code += cpp_pad(vm) + "}";
            vm.popScope();
            return Emitted(code);
        } catch (...) {
            vm.popScope();
            throw;
        }
    }
    NObject eval(Nvm& vm) override;
};
