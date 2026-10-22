#pragma once
// Порт expressions/conditional.py — ConditionalExpression + ExcludeExpression
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"

class ConditionalExpression : public Node {
public:
    int type;
    NodePtr first, second;
    ConditionalExpression(int t, NodePtr a, NodePtr b)
        : type(t), first(std::move(a)), second(std::move(b)) {}

    Emitted javascript(Nvm& vm) override {
        return Emitted("(" + first->javascript(vm).getCpp() + " " +
                       js::op(type) + " " +
                       second->javascript(vm).getCpp() + ")");
    }

    Emitted cpp(Nvm& vm) override {
        std::string f = first->cpp(vm).getCpp();
        std::string s = second->cpp(vm).getCpp();
        const char* op = "?";
        switch (type) {
            case LT: op = "<"; break;
            case GT: op = ">"; break;
            case GTEQ: op = ">="; break;
            case LTEQ: op = "<="; break;
            case EXCLEQ: op = "!="; break;
            case EQEQ: op = "=="; break;
            case AMPAMP: op = "&&"; break;
            case BARBAR: op = "||"; break;
        }
        return Emitted("(" + f + " " + op + " " + s + ")");
    }
    NObject eval(Nvm& vm) override {
        NObject f = first->eval(vm);
        if (type == AMPAMP)
            return NObject(f.asBool() && second->eval(vm).asBool());
        if (type == BARBAR)
            return NObject(f.asBool() || second->eval(vm).asBool());
        NObject s = second->eval(vm);
        // числовое/строковое сравнение
        if (f.isNum() && s.isNum()) {
            double a = f.asDouble(), b = s.asDouble();
            switch (type) {
                case LT: return NObject(a < b);
                case GT: return NObject(a > b);
                case GTEQ: return NObject(a >= b);
                case LTEQ: return NObject(a <= b);
                case EQEQ: return NObject(a == b);
                case EXCLEQ: return NObject(a != b);
            }
        }
        if (f.isStr() && s.isStr()) {
            const std::string& a = f.asStr(); const std::string& b = s.asStr();
            switch (type) {
                case LT: return NObject(a < b);
                case GT: return NObject(a > b);
                case GTEQ: return NObject(a >= b);
                case LTEQ: return NObject(a <= b);
                case EQEQ: return NObject(a == b);
                case EXCLEQ: return NObject(a != b);
            }
        }
        if (type == EQEQ) return NObject(false);
        if (type == EXCLEQ) return NObject(true);
        return NObject(false);
    }
};

class ExcludeExpression : public Node {
public:
    NodePtr expr;
    explicit ExcludeExpression(NodePtr e) : expr(std::move(e)) {}
    Emitted cpp(Nvm& vm) override {
        return Emitted("(!" + expr->cpp(vm).getCpp() + ")");
    }
    Emitted javascript(Nvm& vm) override {
        return Emitted("!" + expr->javascript(vm).getCpp());
    }
    NObject eval(Nvm& vm) override {
        return NObject(!expr->eval(vm).asBool());
    }
};
