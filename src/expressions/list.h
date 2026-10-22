#pragma once
// Порт expressions/list.py
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"

class ListExpression : public Node {
public:
    std::vector<NodePtr> list;
    explicit ListExpression(std::vector<NodePtr> l) : list(std::move(l)) {}

    Emitted javascript(Nvm& vm) override {
        std::string out = "[";
        for (size_t i = 0; i < list.size(); ++i) {
            if (i) out += ", ";
            out += list[i]->javascript(vm).getCpp();
        }
        return Emitted(out + "]");
    }

    Emitted cpp(Nvm& vm) override {
        std::string c = "{";
        for (size_t i = 0; i < list.size(); ++i) {
            if (i) c += ",";
            c += list[i]->cpp(vm).getCpp();
        }
        Emitted e(c + "}");
        e.is_list_expression = true;
        return e;
    }
    NObject eval(Nvm& vm) override {
        auto l = std::make_shared<std::vector<NObject>>();
        for (auto& v : list) l->push_back(v->eval(vm));
        return NObject(l);
    }
};
