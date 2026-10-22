#pragma once
// Порт expressions/new.py — `new X(args)` -> `new X(args)`
#include "../node.h"
#include "../nvm.h"

class NewExpression : public Node {
public:
    NodePtr getter;
    std::vector<NodePtr> pos_args;
    explicit NewExpression(NodePtr g) : getter(std::move(g)) {}

    Emitted javascript(Nvm& vm) override {
        // `new X` / `new X(a)` — getter парсится как полное
        // выражение (может уже нести `(args)`); pos_args
        // добавляем только если реально есть
        std::string out = "new " + getter->javascript(vm).getCpp();
        if (!pos_args.empty()) {
            out += "(";
            for (size_t i = 0; i < pos_args.size(); ++i) {
                if (i) out += ", ";
                out += pos_args[i]->javascript(vm).getCpp();
            }
            out += ")";
        }
        return Emitted(out);
    }

    Emitted cpp(Nvm& vm) override {
        return Emitted("new " + getter->cpp(vm).getCpp());
    }
    NObject eval(Nvm& vm) override {
        return getter->eval(vm); // классы позже
    }
};
