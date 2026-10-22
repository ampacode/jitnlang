#pragma once
// Порт expressions/unary.py
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"

class UnaryExpression : public Node {
public:
    int type;
    NodePtr expr;
    UnaryExpression(int t, NodePtr e) : type(t), expr(std::move(e)) {}

    Emitted javascript(Nvm& vm) override {
        return Emitted("(" + js::op(type) +
                       expr->javascript(vm).getCpp() + ")");
    }

    Emitted cpp(Nvm& vm) override {
        std::string op = type == MINUS ? "-" : "-";
        return Emitted("(" + op + expr->cpp(vm).getCpp() + ")");
    }
    NObject eval(Nvm& vm) override {
        NObject v = expr->eval(vm);
        if (type == MINUS)
            return v.isDouble() ? NObject(-v.asDouble()) : NObject(-v.asInt());
        return v;
    }
};
