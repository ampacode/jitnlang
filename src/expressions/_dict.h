#pragma once
// Порт expressions/_dict.py — { "k": v } -> JObj("k", v, ...)
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"
#include "string.h"
#include "variable.h"
#include "getter.h"
#include "list.h"

class DictExpression : public Node {
public:
    std::vector<std::pair<NodePtr, NodePtr>> dictionary;

    std::string _jkey(NodePtr key, Nvm& vm) {
        if (auto g = std::dynamic_pointer_cast<GetterExpression>(key))
            if (g->list.size() == 1) return _jkey(g->list[0], vm);
        if (auto s = std::dynamic_pointer_cast<StringExpression>(key))
            return "\"" + StringExpression::esc(s->value) + "\"";
        if (auto v = std::dynamic_pointer_cast<VariableExpression>(key))
            return "\"" + v->text + "\"";
        return key->cpp(vm).getCpp();
    }
    std::string _jval(NodePtr expr, Nvm& vm) {
        if (auto g = std::dynamic_pointer_cast<GetterExpression>(expr))
            if (g->list.size() == 1) return _jval(g->list[0], vm);
        if (auto d = std::dynamic_pointer_cast<DictExpression>(expr))
            return d->cpp(vm).getCpp();
        if (auto l = std::dynamic_pointer_cast<ListExpression>(expr)) {
            std::string c = "JArr(";
            for (size_t i = 0; i < l->list.size(); ++i) {
                if (i) c += ", ";
                c += _jval(l->list[i], vm);
            }
            return c + ")";
        }
        return expr->cpp(vm).getCpp();
    }

    Emitted cpp(Nvm& vm) override {
        vm.autoModule("json");
        std::string c = "JObj(";
        for (size_t i = 0; i < dictionary.size(); ++i) {
            if (i) c += ", ";
            c += _jkey(dictionary[i].first, vm) + ", " +
                 _jval(dictionary[i].second, vm);
        }
        return Emitted(c + ")");
    }
    Emitted javascript(Nvm& vm) override {
        std::string out = "{";
        for (size_t i = 0; i < dictionary.size(); ++i) {
            if (i) out += ", ";
            out += dictionary[i].first->javascript(vm).getCpp() +
                   ": " + dictionary[i].second->javascript(vm).getCpp();
        }
        return Emitted(out + "}");
    }
    NObject eval(Nvm& vm) override {
        auto d = std::make_shared<std::map<std::string, NObject>>();
        for (auto& kv : dictionary)
            (*d)[nstr(kv.first->eval(vm)).asStr()] = kv.second->eval(vm);
        return NObject(d);
    }
};
